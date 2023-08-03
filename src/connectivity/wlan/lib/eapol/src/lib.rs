// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg_attr(feature = "benchmarks", feature(test))]

use {
    core::mem,
    thiserror::{self, Error},
    tracing::warn,
    wlan_bitfield::bitfield,
    wlan_common::{
        appendable::{Appendable, BufferTooSmall},
        big_endian::{BigEndianU16, BigEndianU64},
        buffer_reader::BufferReader,
    },
    zerocopy::{AsBytes, ByteSlice, FromBytes, FromZeroes, Ref, Unaligned},
};

#[derive(Debug, Error)]
pub enum Error {
    #[error("unexpected end of buffer while parsing frame")]
    FrameTruncated,
    #[error("buffer too short to write frame")]
    BufferTooShort,
    #[error("attempted to parse the wrong frame type")]
    WrongEapolFrame,
    #[error("packet body length is {} but {} bytes are available", _0, _1)]
    WrongPacketBodyLength(u16, u16),
    #[error("failed to calculate mic: {}", _0)]
    MicFunctionFailed(anyhow::Error),
    #[error("expected mic length of {} but got a mic of length {}", _0, _1)]
    WrongMicLen(usize, usize),
    #[error("called finalize with a mic, but key_mic is false")]
    UnexpectedMic,
    #[error("called finalize without a mic, but key_mic is true")]
    ExpectedMic,
}

impl From<BufferTooSmall> for Error {
    fn from(_src: BufferTooSmall) -> Error {
        Error::BufferTooShort
    }
}

pub enum Frame<B: ByteSlice> {
    Key(KeyFrameRx<B>),
    Unsupported(Ref<B, EapolFields>),
}

impl<B: ByteSlice> Frame<B> {
    pub fn parse_fixed_fields(bytes: B) -> Result<Ref<B, EapolFields>, Error> {
        let mut reader = BufferReader::new(bytes);
        reader.read().ok_or(Error::FrameTruncated)
    }
}

// IEEE Std 802.11-2016, 12.7.2, Figure 12-32
#[derive(Debug)]
pub struct KeyFrameRx<B: ByteSlice> {
    pub eapol_fields: Ref<B, EapolFields>,
    pub key_frame_fields: Ref<B, KeyFrameFields>,
    pub key_mic: B, /* AKM dependent size */
    // 2 octets omitted - key data length is calculated from key_data.len()
    pub key_data: B,
}

impl<B: ByteSlice> KeyFrameRx<B> {
    pub fn parse(mic_len: usize, eapol_pdu_buf: B) -> Result<Self, Error> {
        let mut reader = BufferReader::new(eapol_pdu_buf);
        let eapol_fields = reader.read::<EapolFields>().ok_or(Error::FrameTruncated)?;
        if eapol_fields.packet_body_len.to_native() > reader.bytes_remaining() as u16 {
            return Err(Error::WrongPacketBodyLength(
                eapol_fields.packet_body_len.to_native(),
                reader.bytes_remaining() as u16,
            ));
        }
        match eapol_fields.packet_type {
            PacketType::KEY => {
                let key_frame_fields = reader.read().ok_or(Error::FrameTruncated)?;
                let key_mic = reader.read_bytes(mic_len).ok_or(Error::FrameTruncated)?;
                let key_data_len =
                    reader.read_unaligned::<BigEndianU16>().ok_or(Error::FrameTruncated)?;
                let key_data = reader
                    .read_bytes(key_data_len.get().to_native().into())
                    .ok_or(Error::FrameTruncated)?;
                // Some APs add additional bytes to the 802.1X body. This is odd, but doesn't break anything.
                match reader.peek_remaining().len() {
                    0 => (),
                    extra => warn!(bytes = extra, "Ignoring extra bytes in eapol frame body"),
                }
                Ok(KeyFrameRx { eapol_fields, key_frame_fields, key_mic, key_data })
            }
            _ => Err(Error::WrongEapolFrame),
        }
    }

    /// Populates buf with the underlying bytes of this keyframe.
    ///
    /// If clear_mic is true, the MIC field will be populated with zeroes. This should be used when
    /// recalculating the frame MIC during a key exchange.
    pub fn write_into<A: Appendable>(&self, clear_mic: bool, buf: &mut A) -> Result<(), Error> {
        let required_size =
            self.eapol_fields.packet_body_len.to_native() as usize + mem::size_of::<EapolFields>();
        if !buf.can_append(required_size) {
            return Err(Error::BufferTooShort);
        }
        buf.append_value(self.eapol_fields.as_bytes())?;
        buf.append_value(self.key_frame_fields.as_bytes())?;
        if clear_mic {
            buf.append_bytes_zeroed(self.key_mic.len())?;
        } else {
            buf.append_bytes(self.key_mic.as_bytes())?;
        }
        buf.append_value(&BigEndianU16::from_native(self.key_data.len() as u16))?;
        buf.append_bytes(&self.key_data)?;
        Ok(())
    }
}

#[derive(Debug, Clone)]
pub struct KeyFrameTx {
    pub protocol_version: ProtocolVersion,
    pub key_frame_fields: KeyFrameFields,
    mic_len: usize,
    key_data: Vec<u8>,
}

const KEY_DATA_LEN_BYTES: usize = 2;

impl KeyFrameTx {
    pub fn new(
        protocol_version: ProtocolVersion,
        key_frame_fields: KeyFrameFields,
        key_data: Vec<u8>,
        mic_len: usize,
    ) -> Self {
        KeyFrameTx { protocol_version, key_frame_fields, mic_len, key_data }
    }

    pub fn serialize(self) -> KeyFrameTxFinalizer {
        KeyFrameTxFinalizer::new(
            self.protocol_version,
            self.key_frame_fields,
            self.key_data,
            self.mic_len,
        )
    }
}

/// KeyFrameTxFinalizer stores a key frame that has been serialized into a buffer, but which may
/// still require the addition of a MIC before it can be transmitted.
pub struct KeyFrameTxFinalizer {
    buf: Vec<u8>,
    mic_offset: Option<usize>,
    mic_len: usize,
}

impl KeyFrameTxFinalizer {
    fn new(
        version: ProtocolVersion,
        key_frame_fields: KeyFrameFields,
        key_data: Vec<u8>,
        mic_len: usize,
    ) -> Self {
        let packet_body_len =
            mem::size_of::<KeyFrameFields>() + mic_len + KEY_DATA_LEN_BYTES + key_data.len();
        let size = mem::size_of::<EapolFields>() + packet_body_len;
        let packet_body_len = BigEndianU16::from_native(packet_body_len as u16);
        // The Vec implementation of Appendable will never fail to append, so none of the expects
        // here should ever trigger.
        let mut buf = Vec::with_capacity(size);
        buf.append_value(&EapolFields { version, packet_type: PacketType::KEY, packet_body_len })
            .expect("bad eapol allocation");
        buf.append_value(&key_frame_fields).expect("bad eapol allocation");
        let mic_offset = if KeyInformation(key_frame_fields.key_info.to_native()).key_mic() {
            Some(mem::size_of::<EapolFields>() + mem::size_of::<KeyFrameFields>())
        } else {
            None
        };
        buf.append_bytes_zeroed(mic_len as usize).expect("bad eapol allocation");
        buf.append_value(&BigEndianU16::from_native(key_data.len() as u16))
            .expect("bad eapol allocation");
        buf.append_bytes(&key_data[..]).expect("bad eapol allocation");
        KeyFrameTxFinalizer { buf, mic_offset, mic_len }
    }

    /// Access a key frame buffer that may still need to have its MIC field populated. This should
    /// generally only be used when calculating the MIC to pass to finalize_with_mic.
    pub fn unfinalized_buf(&self) -> &[u8] {
        &self.buf[..]
    }

    /// Generate a final key frame buffer. Fails if the given MIC is the wrong size, or if the
    /// key_mic bit is not set for this frame.
    pub fn finalize_with_mic(mut self, mic: &[u8]) -> Result<KeyFrameBuf, Error> {
        match self.mic_offset {
            Some(offset) => {
                if self.mic_len != mic.len() {
                    Err(Error::WrongMicLen(self.mic_len, mic.len()))
                } else {
                    self.buf[offset..offset + self.mic_len].copy_from_slice(mic);
                    Ok(KeyFrameBuf { buf: self.buf, mic_len: self.mic_len })
                }
            }
            None => Err(Error::UnexpectedMic),
        }
    }

    /// Generate a final key frame buffer. Fails if the key_mic bit is set for this frame.
    pub fn finalize_without_mic(self) -> Result<KeyFrameBuf, Error> {
        match self.mic_offset {
            None => Ok(KeyFrameBuf { buf: self.buf, mic_len: self.mic_len }),
            _ => Err(Error::ExpectedMic),
        }
    }
}

/// A KeyFrameBuf is a wrapper for a Vec<u8> that is guaranteed to contain a parseable
/// eapol key frame.
#[derive(Debug, PartialEq, Eq, Clone)]
pub struct KeyFrameBuf {
    buf: Vec<u8>,
    mic_len: usize,
}

impl KeyFrameBuf {
    /// A FinalizedKeyFrameBuf guarantees that the buffer can be parsed as a KeyFrame, so this
    /// should always succeed. Panics if the parse somehow fails.
    pub fn keyframe(&self) -> KeyFrameRx<&[u8]> {
        KeyFrameRx::parse(self.mic_len, &self.buf[..])
            .expect("finalized eapol keyframe buffer failed to parse")
    }

    /// A mutable keyframe may be rendered invalid, violating the KeyFrameBuf contract, so this
    /// instead populates the passed buffer with the keyframe contents. Panics if the parse somehow
    /// fails.
    pub fn copy_keyframe_mut<'a>(&self, buf: &'a mut Vec<u8>) -> KeyFrameRx<&'a mut [u8]> {
        buf.append_bytes(&self.buf[..]).expect("failed to append to vector! um!");
        KeyFrameRx::parse(self.mic_len, &mut buf[..])
            .expect("finalized eapol keyframe buffer failed to parse")
    }
}

impl std::ops::Deref for KeyFrameBuf {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        &self.buf[..]
    }
}

impl From<KeyFrameBuf> for Vec<u8> {
    fn from(src: KeyFrameBuf) -> Vec<u8> {
        src.buf
    }
}

// IEEE Std 802.1X-2010, 11.9, Table 11-5
#[derive(AsBytes, FromZeroes, FromBytes, Debug, Clone, Copy, PartialEq, Eq, Unaligned, Default)]
#[repr(C)]
pub struct KeyDescriptor(u8);

impl KeyDescriptor {
    pub const RESERVED: Self = Self(0);
    pub const RC4: Self = Self(1);
    pub const IEEE802DOT11: Self = Self(2);

    // This descriptor is specified by the WiFi Alliance WPA standard rather than IEEE.
    pub const LEGACY_WPA1: Self = Self(254);
}

// IEEE Std 802.11-2016, 12.7.2 b.2)
#[derive(Debug, Clone, PartialEq, Eq, Default)]
#[repr(C)]
pub struct KeyType(bool);

impl KeyType {
    pub const GROUP_SMK: Self = Self(false);
    pub const PAIRWISE: Self = Self(true);
}

// IEEE Std 802.1X-2010, 11.3.1
#[derive(
    AsBytes, FromZeroes, FromBytes, Debug, Clone, Copy, Unaligned, PartialEq, Eq, PartialOrd, Ord,
)]
#[repr(C)]
pub struct ProtocolVersion(u8);

impl ProtocolVersion {
    pub const IEEE802DOT1X2001: Self = Self(1);
    pub const IEEE802DOT1X2004: Self = Self(2);
    pub const IEEE802DOT1X2010: Self = Self(3);
}

// IEEE Std 802.1X-2010, 11.3.2, Table 11-3
#[derive(AsBytes, FromZeroes, FromBytes, Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct PacketType(u8);

impl PacketType {
    pub const EAP: Self = Self(0);
    pub const START: Self = Self(1);
    pub const LOGOFF: Self = Self(2);
    pub const KEY: Self = Self(3);
    pub const ASF_ALERT: Self = Self(4);
    pub const MKA: Self = Self(5);
    pub const ANNOUNCEMENT_GENERIC: Self = Self(6);
    pub const ANNOUNCEMENT_SPECIFIC: Self = Self(7);
    pub const ANNOUNCEMENT_REQ: Self = Self(8);
}

// IEEE Std 802.11-2016, 12.7.2, Figure 12-33
#[bitfield(
    0..=2   key_descriptor_version,
    3       key_type as KeyType(bool),
    // WFA, WPA1 Spec. 3.1, Chapter 2.2.4, Key Information.
    // These bits are reserved for non-WPA1 protection.
    4..=5   legacy_wpa1_key_id,
    6       install,
    7       key_ack,
    8       key_mic,
    9       secure,
    10      error,
    11      request,
    12      encrypted_key_data,
    13      smk_message,
    14..=15 _, // reserved
)]
#[derive(AsBytes, FromZeroes, FromBytes, PartialEq, Clone, Default)]
#[repr(C)]
pub struct KeyInformation(pub u16);

#[derive(AsBytes, FromZeroes, FromBytes, Debug, Clone, Unaligned)]
#[repr(C, packed)]
pub struct EapolFields {
    pub version: ProtocolVersion,
    pub packet_type: PacketType,
    pub packet_body_len: BigEndianU16,
}

// IEEE Std 802.11-2016, 12.7.2, Figure 12-32
#[derive(AsBytes, FromZeroes, FromBytes, Default, Debug, Clone, Unaligned)]
#[repr(C, packed)]
pub struct KeyFrameFields {
    pub descriptor_type: KeyDescriptor,
    key_info: BigEndianU16,
    pub key_len: BigEndianU16,
    pub key_replay_counter: BigEndianU64,
    pub key_nonce: [u8; 32],
    pub key_iv: [u8; 16],
    pub key_rsc: BigEndianU64,
    _reserved: [u8; 8],
}

impl KeyFrameFields {
    pub fn new(
        descriptor_type: KeyDescriptor,
        key_info: KeyInformation,
        key_len: u16,
        key_replay_counter: u64,
        key_nonce: [u8; 32],
        key_iv: [u8; 16],
        key_rsc: u64,
    ) -> Self {
        let KeyInformation(key_info) = key_info;
        Self {
            descriptor_type,
            key_info: BigEndianU16::from_native(key_info),
            key_len: BigEndianU16::from_native(key_len),
            key_replay_counter: BigEndianU64::from_native(key_replay_counter),
            key_nonce,
            key_iv,
            key_rsc: BigEndianU64::from_native(key_rsc),
            _reserved: [0u8; 8],
        }
    }

    pub fn key_info(&self) -> KeyInformation {
        KeyInformation(self.key_info.to_native())
    }
    pub fn set_key_info(&mut self, key_info: KeyInformation) {
        let KeyInformation(key_info) = key_info;
        self.key_info = BigEndianU16::from_native(key_info);
    }
}

pub fn to_array<A>(slice: &[u8]) -> A
where
    A: Sized + Default + AsMut<[u8]>,
{
    let mut array = Default::default();
    <A as AsMut<[u8]>>::as_mut(&mut array).clone_from_slice(slice);
    array
}

#[cfg(test)]
mod tests {
    use super::*;
    use wlan_common::{assert_variant, buffer_writer::BufferWriter};

    #[cfg(feature = "benchmarks")]
    mod benches {
        use super::*;
        use test::{black_box, Bencher};

        #[bench]
        fn bench_key_frame_from_bytes(b: &mut Bencher) {
            let frame: Vec<u8> = vec![
                0x01, 0x03, 0x00, 0xb3, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
                0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
                0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x54, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01,
                0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03,
                0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02,
                0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01,
                0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03,
                0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02,
                0x03,
            ];
            b.iter(|| KeyFrameRx::parse(black_box(16), &frame[..]));
        }
    }

    #[test]
    fn test_key_info() {
        let value = 0b1010_0000_0000_0000u16;
        let key_info = KeyInformation(value);
        assert_eq!(key_info.key_descriptor_version(), 0);
        assert!(key_info.smk_message());
        let cloned = key_info.clone();
        assert_eq!(key_info, cloned);
    }

    #[test]
    fn test_not_key_frame() {
        let frame: Vec<u8> = vec![
            0x01, 0x01, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00,
        ];
        let result = KeyFrameRx::parse(16, &frame[..]);
        assert_variant!(result, Err(Error::WrongEapolFrame));
    }

    #[test]
    fn test_padding_okay() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x63, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03, 0x04,
        ];
        KeyFrameRx::parse(16, &frame[..]).expect("parsing keyframe failed");
    }

    #[test]
    fn test_padding_past_pdu_len_okay() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x62, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03, 0x04,
        ];
        KeyFrameRx::parse(16, &frame[..]).expect("parsing keyframe failed");
    }

    #[test]
    fn test_too_short() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x60, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01,
        ];
        let result = KeyFrameRx::parse(16, &frame[..]);
        assert_variant!(result, Err(Error::FrameTruncated));
    }

    #[test]
    fn test_bad_packet_body_len() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0xff, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03,
        ];
        let result = KeyFrameRx::parse(16, &frame[..]);
        assert_variant!(result, Err(Error::WrongPacketBodyLength(0xff, 0x62)));
    }

    #[test]
    fn test_dynamic_mic_size() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x72, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
            0x07, 0x08, 0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1,
            0x22, 0x79, 0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38,
            0x98, 0x25, 0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        KeyFrameRx::parse(32, &frame[..]).expect("parsing keyframe failed");
    }

    #[test]
    fn test_as_bytes() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x62, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03,
        ];
        let keyframe = KeyFrameRx::parse(16, &frame[..]).expect("parsing keyframe failed");
        verify_as_bytes_result(keyframe, false, &frame[..]);
    }

    #[test]
    fn test_as_bytes_dynamic_mic_size() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x72, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
            0x07, 0x08, 0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1,
            0x22, 0x79, 0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38,
            0x98, 0x25, 0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        let keyframe = KeyFrameRx::parse(32, &frame[..]).expect("parsing keyframe failed");
        verify_as_bytes_result(keyframe, false, &frame[..]);
    }

    #[test]
    fn test_as_bytes_buffer_too_small() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x72, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
            0x07, 0x08, 0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1,
            0x22, 0x79, 0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38,
            0x98, 0x25, 0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        let keyframe = KeyFrameRx::parse(32, &frame[..]).expect("parsing keyframe failed");
        let mut buf = [0u8; 40];
        let mut writer = BufferWriter::new(&mut buf[..]);
        let result = keyframe.write_into(true, &mut writer);
        assert_variant!(result, Err(Error::BufferTooShort));
    }

    #[test]
    fn test_as_bytes_clear_mic() {
        #[rustfmt::skip]
            let frame: Vec<u8> = vec![
                0x01, 0x03, 0x00, 0x62, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
                0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
                0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                // MIC
                0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
                0x0F, 0x10,
                0x00, 0x03, 0x01, 0x02, 0x03,
            ];
        let keyframe = KeyFrameRx::parse(16, &frame[..]).expect("parsing keyframe failed");

        #[rustfmt::skip]
            let expected: Vec<u8> = vec![
                0x01, 0x03, 0x00, 0x62, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
                0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
                0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                // Cleared MIC
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00,
                0x00, 0x03, 0x01, 0x02, 0x03,
            ];
        verify_as_bytes_result(keyframe, true, &expected[..]);
    }

    fn verify_as_bytes_result(keyframe: KeyFrameRx<&[u8]>, clear_mic: bool, expected: &[u8]) {
        let mut buf = Vec::with_capacity(128);
        keyframe.write_into(clear_mic, &mut buf).expect("failed to convert keyframe to bytes");
        let written = buf.len();
        let left_over = buf.split_off(written);
        assert_eq!(&buf[..], expected);
        assert!(left_over.iter().all(|b| *b == 0));
    }

    #[test]
    fn test_correct_packet() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x62, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03,
        ];
        let result = KeyFrameRx::parse(16, &frame[..]);
        let keyframe = result.expect("parsing keyframe failed");
        assert_eq!({ keyframe.eapol_fields.version }, ProtocolVersion::IEEE802DOT1X2001);
        assert_eq!({ keyframe.eapol_fields.packet_type }, PacketType::KEY);
        assert_eq!(keyframe.eapol_fields.packet_body_len.to_native(), 98);
        assert_eq!({ keyframe.key_frame_fields.descriptor_type }, KeyDescriptor::IEEE802DOT11);
        assert_eq!(keyframe.key_frame_fields.key_info(), KeyInformation(0x008a));
        assert_eq!(keyframe.key_frame_fields.key_info().key_descriptor_version(), 2);
        assert!(keyframe.key_frame_fields.key_info().key_ack());
        assert_eq!(keyframe.key_frame_fields.key_len.to_native(), 16);
        assert_eq!(keyframe.key_frame_fields.key_replay_counter.to_native(), 1);
        let nonce: Vec<u8> = vec![
            0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79, 0xfe, 0xc3, 0xb9,
            0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25, 0xf8, 0xc7, 0xca,
            0x55, 0x86, 0xbc, 0xda,
        ];
        assert_eq!(&keyframe.key_frame_fields.key_nonce[..], &nonce[..]);
        assert_eq!(keyframe.key_frame_fields.key_rsc.to_native(), 0);
        let mic = [0; 16];
        assert_eq!(&keyframe.key_mic[..], mic);
        let data: Vec<u8> = vec![0x01, 0x02, 0x03];
        assert_eq!(&keyframe.key_data[..], &data[..]);
    }

    #[test]
    fn test_correct_construct() {
        let expected_frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x62, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03,
        ];
        let nonce: [u8; 32] = [
            0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79, 0xfe, 0xc3, 0xb9,
            0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25, 0xf8, 0xc7, 0xca,
            0x55, 0x86, 0xbc, 0xda,
        ];
        let iv = [0u8; 16];
        let data: Vec<u8> = vec![0x01, 0x02, 0x03];
        let new_frame = KeyFrameTx::new(
            ProtocolVersion::IEEE802DOT1X2001,
            KeyFrameFields::new(
                KeyDescriptor::IEEE802DOT11,
                KeyInformation(0x008a),
                16,
                1,
                nonce,
                iv,
                0,
            ),
            data,
            16,
        )
        .serialize()
        .finalize_without_mic()
        .expect("failed to construct eapol keyframe without mic");
        assert_eq!(&new_frame[..], &expected_frame[..]);
    }

    #[test]
    fn test_construct_wrong_mic() {
        let nonce: [u8; 32] = [
            0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79, 0xfe, 0xc3, 0xb9,
            0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25, 0xf8, 0xc7, 0xca,
            0x55, 0x86, 0xbc, 0xda,
        ];
        let iv = [0u8; 16];
        let data: Vec<u8> = vec![0x01, 0x02, 0x03];
        let mut new_frame = KeyFrameTx::new(
            ProtocolVersion::IEEE802DOT1X2001,
            KeyFrameFields::new(
                KeyDescriptor::IEEE802DOT11,
                KeyInformation(0x018a),
                16,
                1,
                nonce,
                iv,
                0,
            ),
            data,
            16,
        );
        new_frame
            .clone()
            .serialize()
            .finalize_without_mic()
            .expect_err("should fail when finalizing keyframe without expected mic");
        new_frame.key_frame_fields.key_info = BigEndianU16::from_native(0x008a);
        new_frame
            .serialize()
            .finalize_with_mic(&vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16][..])
            .expect_err("should fail when finalizing keyframe with unexpected mic");
    }

    #[test]
    fn test_construct_with_mic() {
        #[rustfmt::skip]
        let expected_frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x62, 0x02, 0x01, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            // MIC
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
            0xff, 0x00,
            0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        #[rustfmt::skip]
        let zeroed_mic_frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x62, 0x02, 0x01, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            // MIC
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00,
            0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        let nonce: [u8; 32] = [
            0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79, 0xfe, 0xc3, 0xb9,
            0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25, 0xf8, 0xc7, 0xca,
            0x55, 0x86, 0xbc, 0xda,
        ];
        let iv = [0u8; 16];
        let data: Vec<u8> = vec![0x01, 0x02, 0x03];
        let mic = vec![
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
            0xff, 0x00,
        ];

        let new_frame = KeyFrameTx::new(
            ProtocolVersion::IEEE802DOT1X2001,
            KeyFrameFields::new(
                KeyDescriptor::IEEE802DOT11,
                KeyInformation(0x018a),
                16,
                1,
                nonce,
                iv,
                0,
            ),
            data,
            16,
        )
        .serialize();
        assert_eq!(new_frame.unfinalized_buf(), &zeroed_mic_frame[..]);
        let new_frame = new_frame
            .finalize_with_mic(&mic[..])
            .expect("failed to finalize eapol keyframe with mic");
        assert_eq!(&new_frame[..], &expected_frame[..]);
    }
}
