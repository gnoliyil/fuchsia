// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use anyhow::Context;
use byteorder::{BigEndian, ByteOrder, LittleEndian, WriteBytesExt};
use core::hash::Hash;
use std::collections::HashSet;
use std::convert::TryInto;

macro_rules! impl_try_array_unpack_sized(
    ($self:ty, $lifetime:lifetime) => {
        fn try_array_unpack(iter: &mut std::slice::Iter<$lifetime, u8>) -> anyhow::Result<<$self>::Unpacked> {
            let data: &[u8] = TryUnpackAs::<SpinelDataWlen>::try_unpack_as(iter)?;
            Self::try_unpack_from_slice(data)
        }
    }
);

macro_rules! impl_try_array_owned_unpack_sized(
    ($self:ty) => {
        fn try_array_owned_unpack(iter: &mut std::slice::Iter<'_, u8>) -> anyhow::Result<<$self>::Unpacked> {
            let data: &[u8] = TryUnpackAs::<SpinelDataWlen>::try_unpack_as(iter)?;
            Self::try_owned_unpack_from_slice(data)
        }
    }
);

macro_rules! impl_try_pack_unpack_as_data(
    (u8) => {
        // We skip u8.
    };
    ($self:ty) => {
        impl<'a> TryUnpackAs<'a, [u8]> for $self {
            fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
                Self::try_unpack(iter)
            }
        }
        impl<'a> TryUnpackAs<'a, SpinelDataWlen> for $self {
            fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
                // Get a reference to the buffer.
                let buffer: &'a[u8] = TryUnpackAs::<SpinelDataWlen>::try_unpack_as(iter)?;

                // Unpack using that buffer.
                TryUnpackAs::<[u8]>::try_unpack_as(&mut buffer.iter())
            }
        }
        impl TryPackAs<[u8]> for $self {
            fn pack_as_len(&self) -> io::Result<usize> {
                self.pack_len()
            }

            fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
                self.try_pack(buffer)
            }
        }
        impl TryPackAs<SpinelDataWlen> for $self {
            fn pack_as_len(&self) -> io::Result<usize> {
                self.pack_len().map(|x|x+2)
            }

            fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
                // Start with the length of the encoding
                let mut len = TryPackAs::<[u8]>::pack_as_len(self)?;

                // Encode the length of the buffer and add the
                // length of that to our total length.
                len += TryPackAs::<u16>::try_pack_as(&(len as u16), buffer)?;

                // Encode the rest of the object into the buffer.
                TryPackAs::<[u8]>::try_pack_as(self, buffer)?;

                Ok(len)
            }
        }
    };
    ($self:ty $(,$next:ty)*,) => {
        impl_try_pack_unpack_as_data!($self $(,$next)*);
    };
    ($self:ty $(,$next:ty)*) => {
        impl_try_pack_unpack_as_data!($self);
        impl_try_pack_unpack_as_data!($($next,)*);
    };
);

/// Private convenience macro for defining the appropriate
/// traits for primitive types with fixed encoding lengths.
macro_rules! def_fixed_len(
    ($t:ty, $len:expr, |$pack_buf:ident, $pack_var:ident| $pack_block:expr, | $unpack_buf:ident | $unpack_block:expr) => {
        def_fixed_len!($t, $len, $t, |$pack_buf, $pack_var|$pack_block, | $unpack_buf | $unpack_block);
    };
    ($t:ty, $len:expr, $pack_as:ty,  |$pack_buf:ident, $pack_var:ident| $pack_block:expr, | $unpack_buf:ident | $unpack_block:expr) => {
        impl_try_pack_unpack_as_data!($t);
        impl TryPackAs<$pack_as> for $t {
            fn pack_as_len(&self) -> io::Result<usize> {
                Ok(<$t>::FIXED_LEN)
            }

            fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
                let pack = | $pack_buf: &mut T, $pack_var: $t | $pack_block;
                //let pack: dyn Fn(&mut T, $t) -> io::Result<()> = $pack_block;

                pack (buffer, *self).map(|_|<$t>::FIXED_LEN)
            }
        }

        impl SpinelFixedLen for $t {
            const FIXED_LEN: usize = $len;
        }

        impl TryPack for $t {
            fn pack_len(&self) -> io::Result<usize> {
                TryPackAs::<$pack_as>::pack_as_len(self)
            }

            fn try_pack<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
                TryPackAs::<$pack_as>::try_pack_as(self, buffer)
            }
        }

        impl<'a> TryUnpackAs<'a, $pack_as> for $t {
            fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
                let unpack_fn = | $unpack_buf: &[u8] | $unpack_block;

                if iter.len() < <$t>::FIXED_LEN {
                    Err(UnpackingError::InvalidInternalLength)?
                }

                let result: Result<$t,UnpackingError> = unpack_fn(iter.as_slice());
                *iter = iter.as_slice()[<$t>::FIXED_LEN..].iter();
                Ok(result?)
            }
        }

        impl_try_unpack_for_owned! {
            impl TryOwnedUnpack for $t {
                type Unpacked = Self;
                fn try_owned_unpack(iter: &mut std::slice::Iter<'_, u8>) -> anyhow::Result<Self::Unpacked> {
                    TryUnpackAs::<$pack_as>::try_unpack_as(iter)
                }
            }
        }
    };
);

def_fixed_len!(u8, 1, |b, v| b.write_u8(v), |buffer| Ok(buffer[0]));

def_fixed_len!(i8, 1, |b, v| b.write_i8(v), |buffer| Ok(buffer[0] as i8));

def_fixed_len!(u16, 2, |b, v| b.write_u16::<LittleEndian>(v), |buffer| Ok(LittleEndian::read_u16(
    buffer
)));

def_fixed_len!(i16, 2, |b, v| b.write_i16::<LittleEndian>(v), |buffer| Ok(LittleEndian::read_i16(
    buffer
)));

def_fixed_len!(u32, 4, |b, v| b.write_u32::<LittleEndian>(v), |buffer| Ok(LittleEndian::read_u32(
    buffer
)));

def_fixed_len!(i32, 4, |b, v| b.write_i32::<LittleEndian>(v), |buffer| Ok(LittleEndian::read_i32(
    buffer
)));

def_fixed_len!(u64, 8, |b, v| b.write_u64::<LittleEndian>(v), |buffer| Ok(LittleEndian::read_u64(
    buffer
)));

def_fixed_len!(i64, 8, |b, v| b.write_i64::<LittleEndian>(v), |buffer| Ok(LittleEndian::read_i64(
    buffer
)));

def_fixed_len!(bool, 1, |b, v| b.write_u8(v as u8), |buffer| match buffer[0] {
    0 => Ok(false),
    1 => Ok(true),
    _ => Err(UnpackingError::InvalidValue),
});

def_fixed_len!((), 0, |_b, _v| { Ok(()) }, |_b| Ok(()));

def_fixed_len!(std::net::Ipv6Addr, 16, |b, v| b.write_u128::<BigEndian>(v.into()), |b| Ok(
    BigEndian::read_u128(b).into()
));

def_fixed_len!(EUI64, 8, |b, v| b.write_all((&v).into()), |b| Ok(EUI64([
    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]
])));

def_fixed_len!(EUI48, 6, |b, v| b.write_all((&v).into()), |b| Ok(EUI48([
    b[0], b[1], b[2], b[3], b[4], b[5]
])));

impl TryPack for str {
    fn pack_len(&self) -> io::Result<usize> {
        TryPackAs::<str>::pack_as_len(self)
    }

    fn try_pack<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
        TryPackAs::<str>::try_pack_as(self, buffer)
    }
}

impl TryPack for &str {
    fn pack_len(&self) -> io::Result<usize> {
        TryPackAs::<str>::pack_as_len(*self)
    }

    fn try_pack<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
        TryPackAs::<str>::try_pack_as(*self, buffer)
    }
}

impl TryPack for String {
    fn pack_len(&self) -> io::Result<usize> {
        TryPackAs::<str>::pack_as_len(self.as_str())
    }

    fn try_pack<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
        TryPackAs::<str>::try_pack_as(self.as_str(), buffer)
    }
}

impl TryPackAs<str> for str {
    fn pack_as_len(&self) -> io::Result<usize> {
        Ok(self.as_bytes().len() + 1)
    }

    fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
        let bytes = self.as_bytes();
        let len = bytes.len() + 1;

        if len > std::u16::MAX as usize {
            Err(io::ErrorKind::InvalidInput.into())
        } else if buffer.write(bytes)? != bytes.len() || buffer.write(&[0u8; 1])? != 1 {
            Err(io::ErrorKind::Other.into())
        } else {
            Ok(len)
        }
    }
}

impl TryPackAs<str> for &str {
    fn pack_as_len(&self) -> io::Result<usize> {
        TryPackAs::<str>::pack_as_len(*self)
    }

    fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
        TryPackAs::<str>::try_pack_as(*self, buffer)
    }
}

impl TryPackAs<str> for String {
    fn pack_as_len(&self) -> io::Result<usize> {
        TryPackAs::<str>::pack_as_len(self.as_str())
    }

    fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
        TryPackAs::<str>::try_pack_as(self.as_str(), buffer)
    }
}

impl TryPackAs<str> for &[u8] {
    fn pack_as_len(&self) -> io::Result<usize> {
        Ok(self.len() + 1)
    }

    fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
        let string = std::str::from_utf8(self)
            .map_err(|err| std::io::Error::new(std::io::ErrorKind::InvalidData, err))?;
        TryPackAs::<str>::try_pack_as(string, buffer)
    }
}

impl TryPackAs<str> for Vec<u8> {
    fn pack_as_len(&self) -> io::Result<usize> {
        TryPackAs::<str>::pack_as_len(&self.as_slice())
    }

    fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> io::Result<usize> {
        TryPackAs::<str>::try_pack_as(&self.as_slice(), buffer)
    }
}

impl<T: TryPack> TryPack for Vec<T>
where
    for<'a> &'a [T]: TryPack,
{
    fn pack_len(&self) -> io::Result<usize> {
        self.as_slice().pack_len()
    }

    fn try_pack<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        self.as_slice().try_pack(buffer)
    }

    fn array_pack_len(&self) -> io::Result<usize> {
        self.as_slice().array_pack_len()
    }

    fn try_array_pack<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        self.as_slice().try_array_pack(buffer)
    }
}

impl<T: TryPack> TryPack for &[T] {
    fn pack_len(&self) -> io::Result<usize> {
        let mut len: usize = 0;
        for iter in self.iter() {
            len += iter.array_pack_len()?;
        }
        Ok(len)
    }

    fn try_pack<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        let mut len: usize = 0;

        // Write out the elements of the data
        for iter in self.iter() {
            len += iter.try_array_pack(buffer)?;
        }

        Ok(len)
    }

    fn array_pack_len(&self) -> io::Result<usize> {
        self.pack_len().map(|len| len + 2)
    }

    fn try_array_pack<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        let pack_len = self.pack_len()?;

        // Write out the length of the data
        let pack_len: u16 =
            pack_len.try_into().map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;

        TryPackAs::<u16>::try_pack_as(&pack_len, buffer)?;

        self.try_pack(buffer).map(|len| len + 2)
    }
}

impl<T: TryPack> TryPack for HashSet<T> {
    fn pack_len(&self) -> io::Result<usize> {
        let mut len: usize = 0;
        for iter in self.iter() {
            len += iter.array_pack_len()?;
        }
        Ok(len)
    }

    fn try_pack<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        let mut len: usize = 0;

        // Write out the elements of the data
        for iter in self.iter() {
            len += iter.try_array_pack(buffer)?;
        }

        Ok(len)
    }

    fn array_pack_len(&self) -> io::Result<usize> {
        self.pack_len().map(|len| len + 2)
    }

    fn try_array_pack<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        let pack_len = self.pack_len()?;

        // Write out the length of the data
        let pack_len: u16 =
            pack_len.try_into().map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;

        TryPackAs::<u16>::try_pack_as(&pack_len, buffer)?;

        self.try_pack(buffer).map(|len| len + 2)
    }
}

/// Borrowed unpack for EUI64
impl<'a> TryUnpack<'a> for &'a EUI64 {
    type Unpacked = Self;
    fn try_unpack(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self::Unpacked> {
        if iter.len() < std::mem::size_of::<EUI64>() {
            Err(UnpackingError::InvalidInternalLength)?
        }

        // Convert the iterator into a slice.
        let ret = &iter.as_slice()[..std::mem::size_of::<EUI64>()];

        // Move the iterator to the end.
        *iter = ret[ret.len()..].iter();

        Ok(ret.try_into().unwrap())
    }
}

/// Borrowed unpack for EUI48
impl<'a> TryUnpack<'a> for &'a EUI48 {
    type Unpacked = Self;
    fn try_unpack(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self::Unpacked> {
        if iter.len() < std::mem::size_of::<EUI48>() {
            Err(UnpackingError::InvalidInternalLength)?
        }

        // Convert the iterator into a slice.
        let ret = &iter.as_slice()[..std::mem::size_of::<EUI48>()];

        // Move the iterator to the end.
        *iter = ret[ret.len()..].iter();

        Ok(ret.try_into().unwrap())
    }
}

impl<'a> TryUnpack<'a> for &'a [u8] {
    type Unpacked = Self;
    fn try_unpack(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self::Unpacked> {
        // Convert the iterator into a slice.
        let ret = iter.as_slice();

        // Move the iterator to the end.
        *iter = ret[ret.len()..].iter();

        Ok(ret)
    }
    impl_try_array_unpack_sized!(Self, 'a);
}

impl<'a> TryUnpackAs<'a, [u8]> for &'a [u8] {
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        Self::try_unpack(iter)
    }
}

impl<'a> TryUnpackAs<'a, SpinelDataWlen> for &'a [u8] {
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        let len = u16::try_unpack(iter)? as usize;

        let ret = iter.as_slice();

        if ret.len() < len {
            Err(UnpackingError::InvalidInternalLength)?
        }

        // Move the iterator to the end.
        *iter = ret[len..].iter();

        Ok(&ret[..len])
    }
}

impl<'a> TryUnpack<'a> for &'a str {
    type Unpacked = Self;
    fn try_unpack(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self::Unpacked> {
        TryUnpackAs::<str>::try_unpack_as(iter)
    }
}

impl<'a> TryUnpackAs<'a, str> for &'a str {
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        let mut split = iter.as_slice().splitn(2, |c| *c == 0);

        let str_bytes: &[u8] = split.next().ok_or(UnpackingError::UnterminatedString)?;

        *iter = split.next().ok_or(UnpackingError::UnterminatedString)?.iter();

        std::str::from_utf8(str_bytes).context(UnpackingError::InvalidValue)
    }
}

impl<'a> TryUnpackAs<'a, str> for String {
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        <&str>::try_unpack(iter).map(ToString::to_string)
    }
}

impl<'a> TryUnpackAs<'a, str> for &'a [u8] {
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        <&str as TryUnpackAs<str>>::try_unpack_as(iter).map(str::as_bytes)
    }
}

impl<'a> TryUnpackAs<'a, str> for Vec<u8> {
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        <&[u8] as TryUnpackAs<str>>::try_unpack_as(iter).map(<[u8]>::to_vec)
    }
}

impl_try_unpack_for_owned! {
    impl TryOwnedUnpack for String {
        type Unpacked = Self;
        fn try_owned_unpack(iter: &mut std::slice::Iter<'_, u8>) -> anyhow::Result<Self::Unpacked> {
            <&str>::try_unpack(iter).map(ToString::to_string)
        }
    }
}

impl TryPackAs<SpinelUint> for u32 {
    fn pack_as_len(&self) -> std::io::Result<usize> {
        if *self < (1 << 7) {
            Ok(1)
        } else if *self < (1 << 14) {
            Ok(2)
        } else if *self < (1 << 21) {
            Ok(3)
        } else if *self < (1 << 28) {
            Ok(4)
        } else {
            Ok(5)
        }
    }

    fn try_pack_as<T: std::io::Write + ?Sized>(&self, buffer: &mut T) -> std::io::Result<usize> {
        let len = TryPackAs::<SpinelUint>::pack_as_len(self)?;

        let mut value = *self;
        let mut inner_buffer = [0u8; 5];

        #[allow(clippy::needless_range_loop)]
        for i in 0..(len - 1) {
            inner_buffer[i] = ((value & 0x7F) | 0x80) as u8;
            value >>= 7;
        }

        inner_buffer[len - 1] = (value & 0x7F) as u8;

        buffer.write_all(&inner_buffer[..len])?;

        Ok(len)
    }
}

impl<'a> TryUnpackAs<'a, SpinelUint> for u32 {
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        let mut len: usize = 0;
        let mut value: u32 = 0;
        let mut i = 0;

        loop {
            if len >= 5 {
                return Err(UnpackingError::InvalidValue.into());
            }

            let byte = iter.next().ok_or(UnpackingError::InvalidInternalLength)?;

            len += 1;

            value |= ((byte & 0x7F) as u32) << i;

            if byte & 0x80 != 0x80 {
                break;
            }

            i += 7;
        }

        Ok(value)
    }
}

impl_try_unpack_for_owned! {
    impl TryOwnedUnpack for SpinelUint {
        type Unpacked = u32;
        fn try_owned_unpack(iter: &mut std::slice::Iter<'_, u8>) -> anyhow::Result<Self::Unpacked> {
            TryUnpackAs::<SpinelUint>::try_unpack_as(iter)
        }
    }
}

impl<T> TryOwnedUnpack for [T]
where
    T: TryOwnedUnpack,
{
    type Unpacked = Vec<T::Unpacked>;
    fn try_owned_unpack(iter: &mut std::slice::Iter<'_, u8>) -> anyhow::Result<Self::Unpacked> {
        let mut ret: Self::Unpacked = Vec::with_capacity(iter.size_hint().0);

        while iter.len() != 0 {
            ret.push(T::try_array_owned_unpack(iter)?);
        }

        Ok(ret)
    }
    impl_try_array_owned_unpack_sized!(Self);
}

impl<T> TryOwnedUnpack for Vec<T>
where
    T: TryOwnedUnpack,
{
    type Unpacked = Vec<T::Unpacked>;
    fn try_owned_unpack(iter: &mut std::slice::Iter<'_, u8>) -> anyhow::Result<Self::Unpacked> {
        let mut ret: Self::Unpacked = Vec::with_capacity(iter.size_hint().0);

        while iter.len() != 0 {
            ret.push(T::try_array_owned_unpack(iter)?);
        }

        Ok(ret)
    }
    impl_try_array_owned_unpack_sized!(Self);
}

impl<'a, T> TryUnpack<'a> for Vec<T>
where
    T: TryUnpack<'a>,
{
    type Unpacked = Vec<T::Unpacked>;
    fn try_unpack(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self::Unpacked> {
        let mut ret: Self::Unpacked = Vec::with_capacity(iter.size_hint().0);

        while iter.len() != 0 {
            ret.push(T::try_array_unpack(iter)?);
        }

        Ok(ret)
    }
    impl_try_array_unpack_sized!(Self, 'a);
}

impl<T> TryOwnedUnpack for HashSet<T>
where
    T: TryOwnedUnpack,
    T::Unpacked: Eq + Hash,
{
    type Unpacked = HashSet<T::Unpacked>;
    fn try_owned_unpack(iter: &mut std::slice::Iter<'_, u8>) -> anyhow::Result<Self::Unpacked> {
        let mut ret: Self::Unpacked = Default::default();

        while iter.len() != 0 {
            ret.insert(T::try_array_owned_unpack(iter)?);
        }

        Ok(ret)
    }
    impl_try_array_owned_unpack_sized!(Self);
}

impl<'a, T> TryUnpack<'a> for HashSet<T>
where
    T: TryUnpack<'a>,
    T::Unpacked: Eq + Hash,
{
    type Unpacked = HashSet<T::Unpacked>;
    fn try_unpack(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self::Unpacked> {
        let mut ret: Self::Unpacked = Default::default();

        while iter.len() != 0 {
            ret.insert(T::try_array_unpack(iter)?);
        }

        Ok(ret)
    }
    impl_try_array_unpack_sized!(Self, 'a);
}

impl<'a, T> TryUnpackAs<'a, [u8]> for Vec<T>
where
    Self: TryUnpack<'a, Unpacked = Vec<T>>,
{
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        Self::try_unpack(iter)
    }
}

impl<'a, T> TryUnpackAs<'a, SpinelDataWlen> for Vec<T>
where
    Self: TryUnpackAs<'a, [u8]>,
{
    fn try_unpack_as(iter: &mut std::slice::Iter<'a, u8>) -> anyhow::Result<Self> {
        // Get a reference to the buffer.
        let buffer: &'a [u8] = TryUnpackAs::<SpinelDataWlen>::try_unpack_as(iter)?;

        // Unpack using that buffer.
        TryUnpackAs::<[u8]>::try_unpack_as(&mut buffer.iter())
    }
}

impl<T> TryPackAs<[u8]> for Vec<T>
where
    for<'a> &'a [T]: TryPackAs<[u8]>,
{
    fn pack_as_len(&self) -> io::Result<usize> {
        TryPackAs::<[u8]>::pack_as_len(&self.as_slice())
    }

    fn try_pack_as<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        TryPackAs::<[u8]>::try_pack_as(&self.as_slice(), buffer)
    }
}

impl<T> TryPackAs<[u8]> for &[T]
where
    Self: TryPack,
{
    fn pack_as_len(&self) -> io::Result<usize> {
        self.pack_len()
    }

    fn try_pack_as<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        self.try_pack(buffer)
    }
}

impl<T> TryPackAs<SpinelDataWlen> for Vec<T>
where
    for<'a> &'a [T]: TryPackAs<SpinelDataWlen>,
{
    fn pack_as_len(&self) -> io::Result<usize> {
        TryPackAs::<SpinelDataWlen>::pack_as_len(&self.as_slice())
    }

    fn try_pack_as<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        TryPackAs::<SpinelDataWlen>::try_pack_as(&self.as_slice(), buffer)
    }
}

impl<T> TryPackAs<SpinelDataWlen> for &[T]
where
    Self: TryPackAs<[u8]>,
{
    fn pack_as_len(&self) -> io::Result<usize> {
        TryPackAs::<[u8]>::pack_as_len(self).map(|x| x + 2)
    }

    fn try_pack_as<B: std::io::Write + ?Sized>(&self, buffer: &mut B) -> io::Result<usize> {
        // Start with the length of the encoding
        let mut len = TryPackAs::<[u8]>::pack_as_len(self)?;

        // Encode the length of the buffer and add the
        // length of that to our total length.
        len += TryPackAs::<u16>::try_pack_as(&(len as u16), buffer)?;

        // Encode the rest of the object into the buffer.
        TryPackAs::<[u8]>::try_pack_as(self, buffer)?;

        Ok(len)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;

    /// A verification that Spinel structs with
    /// both `d` and `D` fields compile correctly.
    #[allow(unused)]
    #[spinel_packed("dD")]
    #[derive(Debug, Hash, Clone, Eq, PartialEq)]
    pub struct NetworkPacket<'a> {
        pub packet: &'a [u8],
        pub metadata: &'a [u8],
    }

    #[test]
    fn test_uint_pack() {
        let mut buffer = [0u8; 500];

        let mut tmp_buffer = &mut buffer[..];

        assert_eq!(TryPackAs::<SpinelUint>::try_pack_as(&0u32, &mut tmp_buffer).unwrap(), 1,);
        assert_eq!(&tmp_buffer[0..1], &[0x00]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut tmp_buffer[0..1].iter()),
            Ok(0u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(TryPackAs::<SpinelUint>::try_pack_as(&127u32, &mut tmp_buffer).unwrap(), 1,);
        assert_eq!(&buffer[0..1], &[0x7F]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..1].iter()),
            Ok(127u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(TryPackAs::<SpinelUint>::try_pack_as(&128u32, &mut tmp_buffer).unwrap(), 2,);
        assert_eq!(&buffer[0..2], &[0x80, 0x01]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..2].iter()),
            Ok(128u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(TryPackAs::<SpinelUint>::try_pack_as(&16383u32, &mut tmp_buffer).unwrap(), 2,);
        assert_eq!(&buffer[0..2], &[0xFF, 0x7F]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..2].iter()),
            Ok(16383u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(TryPackAs::<SpinelUint>::try_pack_as(&16384u32, &mut tmp_buffer).unwrap(), 3,);
        assert_eq!(&buffer[0..3], &[0x80, 0x80, 0x01]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..3].iter()),
            Ok(16384u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(TryPackAs::<SpinelUint>::try_pack_as(&2097151u32, &mut tmp_buffer).unwrap(), 3,);
        assert_eq!(&buffer[0..3], &[0xFF, 0xFF, 0x7F]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..3].iter()),
            Ok(2097151u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(TryPackAs::<SpinelUint>::try_pack_as(&2097152u32, &mut tmp_buffer).unwrap(), 4,);
        assert_eq!(&buffer[0..4], &[0x80, 0x80, 0x80, 0x01]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..4].iter()),
            Ok(2097152u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(
            TryPackAs::<SpinelUint>::try_pack_as(&268435455u32, &mut tmp_buffer).unwrap(),
            4,
        );
        assert_eq!(&buffer[0..4], &[0xFF, 0xFF, 0xFF, 0x7F]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..4].iter()),
            Ok(268435455u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(
            TryPackAs::<SpinelUint>::try_pack_as(&268435456u32, &mut tmp_buffer).unwrap(),
            5,
        );
        assert_eq!(&buffer[0..5], &[0x80, 0x80, 0x80, 0x80, 0x01]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..5].iter()),
            Ok(268435456u32)
        );

        let mut tmp_buffer = &mut buffer[..];
        assert_eq!(
            TryPackAs::<SpinelUint>::try_pack_as(&4294967295u32, &mut tmp_buffer).unwrap(),
            5,
        );
        assert_eq!(&buffer[0..5], &[0xFF, 0xFF, 0xFF, 0xFF, 0x0F]);
        assert_matches!(
            TryUnpackAs::<SpinelUint>::try_unpack_as(&mut buffer[0..5].iter()),
            Ok(4294967295u32)
        );
    }

    #[test]
    fn test_vec_owned_unpack() {
        let buffer: &[u8] = &[0x34, 0x12, 0xcd, 0xab];

        let out = Vec::<u16>::try_owned_unpack_from_slice(buffer).unwrap();

        assert_eq!(out.as_slice(), &[0x1234, 0xabcd]);
    }

    #[test]
    fn test_vec_unpack() {
        let buffer: &[u8] = &[0x31, 0x32, 0x33, 0x00, 0x34, 0x35, 0x36, 0x00];

        let out = Vec::<&str>::try_unpack_from_slice(buffer).unwrap();

        assert_eq!(out.as_slice(), &["123", "456"]);
    }

    #[test]
    fn test_string_as_vec() {
        let buffer: &[u8] = &[0x31, 0x32, 0x33, 0x00];

        let out = <&[u8] as TryUnpackAs<str>>::try_unpack_as_from_slice(buffer).unwrap();

        assert_eq!(out, &[0x31, 0x32, 0x33]);
    }

    #[test]
    fn test_hashset_owned_unpack() {
        let buffer: &[u8] = &[0x34, 0x12, 0xcd, 0xab];

        let out = HashSet::<u16>::try_owned_unpack_from_slice(buffer).unwrap();

        assert!(out.contains(&0x1234));
        assert!(out.contains(&0xabcd));
    }

    #[test]
    fn test_hashset_unpack() {
        let buffer: &[u8] = &[0x31, 0x32, 0x33, 0x00, 0x34, 0x35, 0x36, 0x00];

        let out = HashSet::<&str>::try_unpack_from_slice(buffer).unwrap();

        assert!(out.contains("123"));
        assert!(out.contains("456"));
    }

    #[test]
    fn test_u32_as_data_unpack() {
        let buffer: &[u8] = &[0x67, 0x45, 0x23, 0x01, 0xfe, 0x0f, 0xdc, 0xba];

        let out = Vec::<u32>::try_unpack_from_slice(buffer).unwrap();

        assert_eq!(out, &[0x01234567, 0xbadc0ffe]);
    }

    #[test]
    fn test_u32_as_data_unpack_wlen() {
        let buffer: &[u8] = &[0x08, 0x00, 0x67, 0x45, 0x23, 0x01, 0xfe, 0x0f, 0xdc, 0xba];

        let out = Vec::<u32>::try_array_unpack(&mut buffer.iter()).unwrap();

        assert_eq!(out, &[0x01234567, 0xbadc0ffe]);
    }

    #[test]
    fn test_u32_as_data_pack() {
        let v: Vec<u32> = vec![0x01234567, 0xbadc0ffe];
        let buffer: &[u8] = &[0x67, 0x45, 0x23, 0x01, 0xfe, 0x0f, 0xdc, 0xba];

        let mut packed = Vec::with_capacity(v.pack_len().unwrap());
        v.try_pack(&mut packed).unwrap();

        assert_eq!(buffer, packed);
    }

    #[test]
    fn test_u32_as_data_pack_wlen() {
        let v: Vec<u32> = vec![0x01234567, 0xbadc0ffe];
        let buffer: &[u8] = &[0x08, 0x00, 0x67, 0x45, 0x23, 0x01, 0xfe, 0x0f, 0xdc, 0xba];

        let mut packed = Vec::with_capacity(v.array_pack_len().unwrap());
        v.try_array_pack(&mut packed).unwrap();

        assert_eq!(buffer, packed);
    }

    #[test]
    fn test_u32_as_data_pack_struct() {
        #[spinel_packed("d")]
        #[derive(Debug, Hash, Clone, Eq, PartialEq)]
        struct Blah {
            v: Vec<u32>,
        }
        let v = Blah { v: vec![0x01234567, 0xbadc0ffe] };
        let buffer: &[u8] = &[0x08, 0x00, 0x67, 0x45, 0x23, 0x01, 0xfe, 0x0f, 0xdc, 0xba];

        let mut packed = Vec::with_capacity(v.pack_len().unwrap());
        v.try_pack(&mut packed).unwrap();

        assert_eq!(buffer, packed);
    }

    #[test]
    fn test_container_pack_unpack() {
        #[spinel_packed("6CLL")]
        #[derive(Debug, Hash, Clone, Eq, PartialEq)]
        pub struct AddressTableEntry {
            pub addr: std::net::Ipv6Addr,
            pub prefix_len: u8,
            pub lifetime_a: u32,
            pub lifetime_b: u32,
        }

        let buffer: &[u8] = &[
            0x19, 0x0, 0xfd, 0xde, 0xad, 0x0, 0xbe, 0xef, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xfe, 0x0,
            0xfc, 0x0, 0x40, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x19, 0x0, 0xfd, 0xde,
            0xad, 0x0, 0xbe, 0xef, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xfe, 0x0, 0x44, 0x0, 0x40, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x19, 0x0, 0xfd, 0xde, 0xad, 0x0, 0xbe, 0xef,
            0x0, 0x0, 0xec, 0x2c, 0xb9, 0xec, 0xa5, 0x14, 0x96, 0xc, 0x40, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0x19, 0x0, 0xfe, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x28,
            0x3f, 0xf, 0x66, 0x3e, 0xa7, 0x4d, 0x33, 0x40, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff,
        ];

        let out = Vec::<AddressTableEntry>::try_unpack_from_slice(buffer).unwrap();

        assert!(out.contains(&AddressTableEntry {
            addr: "fdde:ad00:beef::ff:fe00:4400".parse().unwrap(),
            prefix_len: 64,
            lifetime_a: 0xFFFFFFFF,
            lifetime_b: 0xFFFFFFFF,
        }));
        assert!(out.contains(&AddressTableEntry {
            addr: "fdde:ad00:beef::ff:fe00:fc00".parse().unwrap(),
            prefix_len: 64,
            lifetime_a: 0xFFFFFFFF,
            lifetime_b: 0xFFFFFFFF,
        }));
        assert!(out.contains(&AddressTableEntry {
            addr: "fdde:ad00:beef::ec2c:b9ec:a514:960c".parse().unwrap(),
            prefix_len: 64,
            lifetime_a: 0xFFFFFFFF,
            lifetime_b: 0xFFFFFFFF,
        }));
        assert!(out.contains(&AddressTableEntry {
            addr: "fe80::283f:f66:3ea7:4d33".parse().unwrap(),
            prefix_len: 64,
            lifetime_a: 0xFFFFFFFF,
            lifetime_b: 0xFFFFFFFF,
        }));

        let buffer_pack = out.try_packed().expect("packing failed");

        assert_eq!(buffer_pack.as_slice(), buffer);
    }
}
