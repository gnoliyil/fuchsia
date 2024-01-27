// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Emulation of Zircon handles for non-Zircon based operating systems.

pub mod channel;
pub mod socket;

use crate::invoke_for_handle_types;
use bitflags::bitflags;
use fuchsia_zircon_status as zx_status;
use fuchsia_zircon_types as zx_types;
use futures::ready;
use futures::task::noop_waker_ref;
#[cfg(debug_assertions)]
use std::cell::Cell;
use std::collections::{HashMap, VecDeque};
use std::future::Future;
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::pin::Pin;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;
use std::sync::Mutex;
use std::task::{Context, Poll, Waker};
use zx_status::Status;

/// Invalid handle value
const INVALID_HANDLE: u32 = 0;

/// The type of an object.
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash, Copy, Clone)]
pub struct ObjectType(u32);

macro_rules! define_object_type_constant {
    ($x:tt, $docname:expr, $name:ident, $value:expr, $availability:ident) => {
        #[doc = $docname]
        pub const $name: ObjectType = ObjectType($value);
    };
}

impl ObjectType {
    /// No object.
    pub const NONE: ObjectType = ObjectType(0);
    invoke_for_handle_types!(define_object_type_constant);

    /// Converts ObjectType into the underlying zircon type.
    pub fn into_raw(self) -> u32 {
        self.0
    }
}

/// A borrowed reference to an underlying handle
#[derive(Debug, Clone, Copy, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub struct HandleRef<'a>(u32, std::marker::PhantomData<&'a u32>);

impl<'a> HandleRef<'a> {
    /// Returns an invalid handle ref
    pub fn invalid() -> Self {
        Self(INVALID_HANDLE, std::marker::PhantomData)
    }

    /// Duplicate this handle. The new handle will have the given `rights`, which must be a subset
    /// of the rights the existing handle has.
    pub fn duplicate(&self, mut rights: Rights) -> Result<Handle, Status> {
        let mut new_entry = HANDLE_TABLE
            .lock()
            .unwrap()
            .get(&self.raw_handle())
            .cloned()
            .ok_or(Status::BAD_HANDLE)?;
        if rights == Rights::SAME_RIGHTS {
            rights = new_entry.rights;
        }

        if !new_entry.rights.contains(rights) {
            return Err(Status::INVALID_ARGS);
        }
        if !new_entry.rights.contains(Rights::DUPLICATE) {
            return Err(Status::ACCESS_DENIED);
        }
        new_entry.rights = rights;
        let new_handle = alloc_handle();
        let side = new_entry.side;
        new_entry.object.lock().unwrap().increment_open_count(side);
        let _ = HANDLE_TABLE.lock().unwrap().insert(new_handle, new_entry);
        Ok(Handle(new_handle))
    }

    /// Signal an object
    pub fn signal(&self, clear_mask: Signals, set_mask: Signals) -> Result<(), Status> {
        if self.is_invalid() {
            return Err(zx_status::Status::BAD_HANDLE);
        }

        clear_mask.validate_user_signals()?;
        set_mask.validate_user_signals()?;

        let rights = get_hdl_rights(self.raw_handle()).ok_or(zx_status::Status::BAD_HANDLE)?;

        if !rights.contains(Rights::SIGNAL) {
            Err(Status::ACCESS_DENIED)
        } else {
            with_handle(self.0, |mut h, side| {
                // We just checked for an invalid handle above, so this should never fail.
                h.as_hdl_data()
                    .signal(side, clear_mask, set_mask)
                    .status_for_peer()
                    .expect("Handle became invalid while processing signal call");
            });

            Ok(())
        }
    }
}

#[derive(Debug, Eq, PartialEq)]
#[repr(transparent)]
pub struct Koid(u64);

const INVALID_KOID: Koid = Koid(0);

impl Koid {
    pub fn raw_koid(&self) -> u64 {
        self.0
    }
}

#[derive(Debug, Eq, PartialEq)]
pub struct HandleBasicInfo {
    pub koid: Koid,
    pub rights: Rights,
    pub object_type: ObjectType,
    pub related_koid: Koid,
    pub reserved: u32,
}

/// A trait to get a reference to the underlying handle of an object.
pub trait AsHandleRef {
    /// Get a reference to the handle.
    fn as_handle_ref<'a>(&'a self) -> HandleRef<'a>;

    /// Return true if this handle is invalid
    fn is_invalid(&self) -> bool {
        self.as_handle_ref().0 == INVALID_HANDLE
    }

    /// Interpret the reference as a raw handle (an integer type). Two distinct
    /// handles will have different raw values (so it can perhaps be used as a
    /// key in a data structure).
    fn raw_handle(&self) -> u32 {
        self.as_handle_ref().0
    }

    /// Set and clear userspace-accessible signal bits on an object.
    fn signal_handle(&self, clear_mask: Signals, set_mask: Signals) -> Result<(), Status> {
        self.as_handle_ref().signal(clear_mask, set_mask)
    }

    /// Wraps the
    /// [zx_object_get_info](https://fuchsia.dev/fuchsia-src/reference/syscalls/object_get_info.md)
    /// syscall for the ZX_INFO_HANDLE_BASIC topic.
    fn basic_info(&self) -> Result<HandleBasicInfo, zx_status::Status> {
        if self.is_invalid() {
            return Err(zx_status::Status::BAD_HANDLE);
        }
        let koids = with_handle(self.raw_handle(), |mut h, side| {
            let h = h.as_hdl_data();
            h.koids(side)
        });
        let ty = get_hdl_type(self.raw_handle()).ok_or(zx_status::Status::BAD_HANDLE)?;
        let rights = get_hdl_rights(self.raw_handle()).ok_or(zx_status::Status::BAD_HANDLE)?;
        Ok(HandleBasicInfo {
            koid: Koid(koids.0),
            rights,
            object_type: ty.object_type(),
            related_koid: Koid(koids.1),
            reserved: 0,
        })
    }
}

/// A trait implemented by all handles for objects which have a peer.
pub trait Peered: HandleBased {
    /// Set and clear userspace-accessible signal bits on the object's peer. Wraps the
    /// [zx_object_signal_peer](https://fuchsia.dev/fuchsia-src/reference/syscalls/object_signal.md)
    /// syscall.
    fn signal_peer(&self, clear_mask: Signals, set_mask: Signals) -> Result<(), Status> {
        if self.is_invalid() {
            return Err(zx_status::Status::BAD_HANDLE);
        }

        clear_mask.validate_user_signals()?;
        set_mask.validate_user_signals()?;

        let rights = get_hdl_rights(self.raw_handle()).ok_or(zx_status::Status::BAD_HANDLE)?;

        if !rights.contains(Rights::SIGNAL_PEER) {
            Err(Status::ACCESS_DENIED)
        } else {
            with_handle(self.raw_handle(), |mut h, side| {
                h.as_hdl_data().signal(side.opposite(), clear_mask, set_mask).status_for_peer()
            })
        }
    }
}

/// An extension of `AsHandleRef` that adds non-Fuchsia-only operations.
pub trait EmulatedHandleRef: AsHandleRef {
    /// Return the type of a handle.
    fn object_type(&self) -> ObjectType {
        if self.is_invalid() {
            ObjectType::NONE
        } else {
            let ty = get_hdl_type(self.raw_handle()).expect("Bad handle");
            ty.object_type()
        }
    }

    /// Return a reference to the other end of a handle.
    fn related<'a>(&'a self) -> HandleRef<'a> {
        if self.is_invalid() {
            HandleRef(INVALID_HANDLE, std::marker::PhantomData)
        } else {
            let table = HANDLE_TABLE.lock().unwrap();
            if let Some((target, side)) =
                table.get(&self.raw_handle()).map(|x| (Arc::clone(&x.object), x.side))
            {
                for (&handle, entry) in table.iter() {
                    if side == entry.side.opposite() && Arc::ptr_eq(&target, &entry.object) {
                        return HandleRef(handle, std::marker::PhantomData);
                    }
                }
            }
            HandleRef(INVALID_HANDLE, std::marker::PhantomData)
        }
    }

    /// Return a "koid" like value.
    fn koid_pair(&self) -> (u64, u64) {
        if self.is_invalid() {
            (INVALID_KOID.0, INVALID_KOID.0)
        } else {
            with_handle(self.as_handle_ref().0, |mut h, side| h.as_hdl_data().koids(side))
        }
    }

    /// Return true if the handle appears valid (i.e. not `Handle::invalid()`),
    /// but does not exist in the table. This should not normally return true;
    /// when it does, dropping the handle will cause a panic.
    fn is_dangling(&self) -> bool {
        if self.is_invalid() {
            false
        } else {
            !HANDLE_TABLE.lock().unwrap().contains_key(&self.raw_handle())
        }
    }
}

impl<T: AsHandleRef> EmulatedHandleRef for T {}

impl AsHandleRef for HandleRef<'_> {
    fn as_handle_ref<'a>(&'a self) -> HandleRef<'a> {
        HandleRef(self.0, std::marker::PhantomData)
    }
}

/// A trait implemented by all handle-based types.
pub trait HandleBased: AsHandleRef + From<Handle> + Into<Handle> {
    /// Duplicate a handle, possibly reducing the rights available.
    fn duplicate_handle(&self, rights: Rights) -> Result<Self, Status> {
        self.as_handle_ref().duplicate(rights).map(|handle| Self::from(handle))
    }

    /// Creates an instance of this type from a handle.
    ///
    /// This is a convenience function which simply forwards to the `From` trait.
    fn from_handle(handle: Handle) -> Self {
        Self::from(handle)
    }

    /// Converts the value into its inner handle.
    ///
    /// This is a convenience function which simply forwards to the `Into` trait.
    fn into_handle(self) -> Handle {
        self.into()
    }

    /// Converts the handle into it's raw representation.
    ///
    /// The caller takes ownership over the raw handle, and must close it.
    fn into_raw(self) -> u32 {
        self.into_handle().raw_take()
    }
}

/// Representation of a handle-like object
#[derive(PartialEq, Eq, Debug, Ord, PartialOrd, Hash)]
pub struct Handle(u32);

impl Drop for Handle {
    fn drop(&mut self) {
        hdl_close(self.0);
    }
}

impl AsHandleRef for Handle {
    fn as_handle_ref<'a>(&'a self) -> HandleRef<'a> {
        HandleRef(self.0, std::marker::PhantomData)
    }
}

impl HandleBased for Handle {}

impl Handle {
    /// Return an invalid handle
    #[inline(always)]
    pub const fn invalid() -> Handle {
        Handle(INVALID_HANDLE)
    }

    /// Return true if this handle is invalid
    pub fn is_invalid(&self) -> bool {
        self.0 == INVALID_HANDLE
    }

    /// If a raw handle is obtained from some other source, this method converts
    /// it into a type-safe owned handle.
    ///
    /// # Safety
    ///
    /// `hdl` must be a valid raw handle. The returned `Handle` takes ownership
    /// of the raw handle, so it must not be modified or closed by other means
    /// after calling `from_raw`.
    pub const unsafe fn from_raw(hdl: u32) -> Handle {
        Handle(hdl)
    }

    /// Take this handle and return a new handle (leaves this handle invalid)
    pub fn take(&mut self) -> Handle {
        let h = Handle(self.0);
        self.0 = INVALID_HANDLE;
        h
    }

    /// Invalidates this handle and returns its raw handle.
    ///
    /// To avoid leaking resources, the returned raw handle should eventually be
    /// closed. This can be done by converting it back to a [`Handle`] with
    /// [`from_raw`] and dropping it.
    ///
    /// [`from_raw`]: Handle::from_raw
    pub fn raw_take(&mut self) -> u32 {
        let h = self.0;
        self.0 = INVALID_HANDLE;
        h
    }

    /// Create a replacement for a handle, possibly reducing the rights available. This invalidates
    /// the original handle. Wraps the
    /// [zx_handle_replace](https://fuchsia.dev/fuchsia-src/reference/syscalls/handle_replace.md)
    /// syscall.
    pub fn replace(self, target_rights: Rights) -> Result<Handle, zx_status::Status> {
        if target_rights == Rights::SAME_RIGHTS {
            return Ok(self);
        }
        if self.is_invalid() {
            return Err(zx_status::Status::BAD_HANDLE);
        }

        let mut table = HANDLE_TABLE.lock().unwrap();
        let std::collections::hash_map::Entry::Occupied(entry) =
            table.entry(self.raw_handle())
        else {
            return Err(zx_status::Status::BAD_HANDLE);
        };

        if !entry.get().rights.contains(target_rights) {
            return Err(zx_status::Status::INVALID_ARGS);
        }

        let new_handle = alloc_handle();
        let mut entry = entry.remove_entry().1;
        entry.rights = target_rights;
        let _ = table.insert(new_handle, entry);
        Ok(Handle(new_handle))
    }
}

/// An emulated Zircon Channel
#[derive(PartialEq, Eq, Debug, PartialOrd, Ord, Hash)]
pub struct Channel(u32);

impl From<Handle> for Channel {
    fn from(mut hdl: Handle) -> Channel {
        let out = Channel(hdl.0);
        hdl.0 = INVALID_HANDLE;
        out
    }
}
impl From<Channel> for Handle {
    fn from(mut hdl: Channel) -> Handle {
        let out = unsafe { Handle::from_raw(hdl.0) };
        hdl.0 = INVALID_HANDLE;
        out
    }
}
impl HandleBased for Channel {}
impl AsHandleRef for Channel {
    fn as_handle_ref<'a>(&'a self) -> HandleRef<'a> {
        HandleRef(self.0, std::marker::PhantomData)
    }
}

impl Peered for Channel {}

impl Drop for Channel {
    fn drop(&mut self) {
        hdl_close(self.0);
    }
}

impl Channel {
    /// Create a channel, resulting in a pair of `Channel` objects representing both
    /// sides of the channel. Messages written into one maybe read from the opposite.
    pub fn create() -> (Channel, Channel) {
        let rights = Rights::CHANNEL_DEFAULT;
        let (left, right, obj) = new_handle_pair(HdlType::Channel, rights);

        let mut obj = obj.lock().unwrap();
        let KObjectEntry::Channel(obj) = &mut *obj else {
            unreachable!("Channel we just allocated wasn't present or wasn't a channel");
        };
        let mut hdl_ref = HdlRef::Channel(obj);
        hdl_ref
            .as_hdl_data()
            .signal(Side::Left, Signals::NONE, Signals::OBJECT_WRITABLE)
            .expect("Handle wasn't open immediately after creation");
        hdl_ref
            .as_hdl_data()
            .signal(Side::Right, Signals::NONE, Signals::OBJECT_WRITABLE)
            .expect("Handle wasn't open immediately after creation");

        (Channel(left), Channel(right))
    }

    /// Returns true if the channel is closed (i.e. other side was dropped).
    pub fn is_closed(&self) -> bool {
        assert!(!self.is_invalid());
        let Some(object) = HANDLE_TABLE.lock().unwrap().get(&self.0).map(|x| Arc::clone(&x.object)) else {
            return true;
        };

        let object = object.lock().unwrap();
        !object.is_open()
    }

    /// Read a message from a channel.
    pub fn read(&self, buf: &mut MessageBuf) -> Result<(), zx_status::Status> {
        let (bytes, handles) = buf.split_mut();
        self.read_split(bytes, handles)
    }

    /// Read a message from a channel into a separate byte vector and handle vector.
    pub fn read_split(
        &self,
        bytes: &mut Vec<u8>,
        handles: &mut Vec<Handle>,
    ) -> Result<(), zx_status::Status> {
        match self.poll_read(&mut Context::from_waker(noop_waker_ref()), bytes, handles) {
            Poll::Ready(r) => r,
            Poll::Pending => Err(zx_status::Status::SHOULD_WAIT),
        }
    }

    fn poll_read(
        &self,
        cx: &mut Context<'_>,
        bytes: &mut Vec<u8>,
        handles: &mut Vec<Handle>,
    ) -> Poll<Result<(), zx_status::Status>> {
        with_handle(self.0, |h, side| {
            if let HdlRef::Channel(obj) = h {
                if let Some(mut msg) = obj.q.side_mut(side.opposite()).pop_front() {
                    std::mem::swap(bytes, &mut msg.bytes);
                    std::mem::swap(handles, &mut msg.handles);
                    if obj.q.side_mut(side.opposite()).is_empty() {
                        obj.signal(side, Signals::OBJECT_READABLE, Signals::NONE)
                            .status_for_self()?;
                    }
                    Poll::Ready(Ok(()))
                } else if obj.is_open() {
                    obj.wakers.side_mut(side).pending_readable(cx)
                } else {
                    Poll::Ready(Err(zx_status::Status::PEER_CLOSED))
                }
            } else {
                unreachable!();
            }
        })
    }

    /// Reads a message from a channel into a fixed size buffer. If either `buf` or `handles` is
    /// not large enough to hold the message, then `Err((buf_len, handles_len))` is returned. The
    /// caller is then expected to invoke the read function again, albeit with a resized buffer
    /// large enough to fit the output values.
    ///
    /// If there are any general errors that happen during read, then `Ok(Err(_), (0, 0))` is
    /// returned.
    ///
    /// On success, `Ok(Ok(()), (buf_len, handles_len))` is returned. As with zx_channel_read, of
    /// which this function is an analogue, there are no partial reads.
    ///
    /// It is important to remember to check the `Ok(_)` result for potential errors, like
    /// `PEER_CLOSED`, for example.
    pub fn read_raw(
        &self,
        buf: &mut [u8],
        handles: &mut [MaybeUninit<Handle>],
    ) -> Result<(Result<(), zx_status::Status>, usize, usize), (usize, usize)> {
        match self.poll_read_raw(&mut Context::from_waker(noop_waker_ref()), buf, handles) {
            Poll::Ready(r) => r,
            Poll::Pending => Ok((Err(zx_status::Status::SHOULD_WAIT), 0, 0)),
        }
    }

    fn poll_read_raw(
        &self,
        cx: &mut Context<'_>,
        buf: &mut [u8],
        handles: &mut [MaybeUninit<Handle>],
    ) -> Poll<Result<(Result<(), zx_status::Status>, usize, usize), (usize, usize)>> {
        with_handle(self.0, |h, side| {
            let HdlRef::Channel(obj) = h else { unreachable!() };
            let read_side = obj.q.side_mut(side.opposite());
            if let Some(msg) = read_side.front() {
                let msg_bytes_len = msg.bytes.len();
                let msg_handles_len = msg.handles.len();
                if msg_bytes_len > buf.len() || msg_handles_len > handles.len() {
                    Poll::Ready(Err((msg_bytes_len, msg_handles_len)))
                } else {
                    let msg = read_side.pop_front().unwrap();
                    buf[..msg_bytes_len].clone_from_slice(msg.bytes.as_slice());
                    msg.handles
                        .into_iter()
                        .enumerate()
                        .for_each(|(i, hdl)| handles[i] = MaybeUninit::new(hdl));
                    if read_side.is_empty() {
                        match obj
                            .signal(side, Signals::OBJECT_READABLE, Signals::NONE)
                            .status_for_self()
                        {
                            Err(e) => {
                                return Poll::Ready(Ok((Err(e), msg_bytes_len, msg_handles_len)))
                            }
                            Ok(_) => {}
                        }
                    }
                    Poll::Ready(Ok((Ok(()), msg_bytes_len, msg_handles_len)))
                }
            } else if obj.is_open() {
                obj.wakers.side_mut(side).pending_readable(cx)
            } else {
                Poll::Ready(Ok((Err(zx_status::Status::PEER_CLOSED), 0, 0)))
            }
        })
    }

    /// Read a message from a channel.
    pub fn read_etc(&self, buf: &mut MessageBufEtc) -> Result<(), zx_status::Status> {
        let (bytes, handles) = buf.split_mut();
        self.read_etc_split(bytes, handles)
    }

    /// Read a message from a channel into a separate byte vector and handle vector.
    pub fn read_etc_split(
        &self,
        bytes: &mut Vec<u8>,
        handles: &mut Vec<HandleInfo>,
    ) -> Result<(), zx_status::Status> {
        match self.poll_read_etc(&mut Context::from_waker(noop_waker_ref()), bytes, handles) {
            Poll::Ready(r) => r,
            Poll::Pending => Err(zx_status::Status::SHOULD_WAIT),
        }
    }

    fn poll_read_etc(
        &self,
        cx: &mut Context<'_>,
        bytes: &mut Vec<u8>,
        handle_infos: &mut Vec<HandleInfo>,
    ) -> Poll<Result<(), zx_status::Status>> {
        let mut handles = Vec::new();
        ready!(self.poll_read(cx, bytes, &mut handles))?;
        handle_infos.clear();
        handle_infos.extend(handles.into_iter().map(|handle| {
            let h_raw = handle.raw_handle();
            let ty = get_hdl_type(h_raw).expect("Bad handle");
            let rights = get_hdl_rights(h_raw).expect("Bad handle");
            HandleInfo { handle, object_type: ty.object_type(), rights }
        }));
        Poll::Ready(Ok(()))
    }

    /// Write a message to a channel.
    pub fn write(&self, bytes: &[u8], handles: &mut [Handle]) -> Result<(), zx_status::Status> {
        let bytes_vec = bytes.to_vec();
        let mut handles_vec = Vec::with_capacity(handles.len());
        for i in 0..handles.len() {
            handles_vec.push(std::mem::replace(&mut handles[i], Handle::invalid()));
        }
        with_handle(self.0, |h, side| {
            if let HdlRef::Channel(obj) = h {
                if !obj.is_open() {
                    return Err(zx_status::Status::PEER_CLOSED);
                }
                obj.q
                    .side_mut(side)
                    .push_back(ChannelMessage { bytes: bytes_vec, handles: handles_vec });
                obj.signal(side.opposite(), Signals::NONE, Signals::OBJECT_READABLE)
                    .status_for_peer()?;
                Ok(())
            } else {
                unreachable!();
            }
        })
    }

    /// Write a message to a channel.
    pub fn write_etc<'a>(
        &self,
        bytes: &[u8],
        handles: &mut [HandleDisposition<'a>],
    ) -> Result<(), zx_status::Status> {
        let bytes_vec = bytes.to_vec();
        let mut handles_vec = Vec::with_capacity(handles.len());
        for hd in handles {
            let op: HandleOp<'a> =
                std::mem::replace(&mut hd.handle_op, HandleOp::Move(Handle::invalid()));
            if let HandleOp::Move(handle) = op {
                let ty = get_hdl_type(handle.raw_handle()).ok_or(zx_status::Status::BAD_HANDLE)?;
                if ty.object_type() != handle.object_type()
                    && handle.object_type() != ObjectType::NONE
                {
                    return Err(zx_status::Status::INVALID_ARGS);
                }
                handles_vec.push(handle.replace(hd.rights)?);
            } else {
                panic!("unimplemented HandleOp");
            }
        }
        with_handle(self.0, |h, side| {
            if let HdlRef::Channel(obj) = h {
                if !obj.is_open() {
                    // Move the handles outside this closure before dropping them.  If any are
                    // channels in the same shard as this channel, dropping them will attempt to
                    // re-acquire the lock held by with_handle.
                    return Err(handles_vec);
                }
                obj.q
                    .side_mut(side)
                    .push_back(ChannelMessage { bytes: bytes_vec, handles: handles_vec });
                obj.signal(side.opposite(), Signals::NONE, Signals::OBJECT_READABLE)
                    .expect("Signalling readable should never fail here");
                Ok(())
            } else {
                unreachable!();
            }
        })
        .map_err(|_handles_to_drop| zx_status::Status::PEER_CLOSED)
    }
}

/// Socket options available portable
#[derive(Clone, Copy)]
pub enum SocketOpts {
    /// A bytestream style socket
    STREAM,
    /// A datagram style socket
    DATAGRAM,
}

/// Emulation of a Zircon Socket.
#[derive(PartialEq, Eq, Debug, PartialOrd, Ord, Hash)]
pub struct Socket(u32);

impl From<Handle> for Socket {
    fn from(mut hdl: Handle) -> Socket {
        let out = Socket(hdl.0);
        hdl.0 = INVALID_HANDLE;
        out
    }
}
impl From<Socket> for Handle {
    fn from(mut hdl: Socket) -> Handle {
        let out = unsafe { Handle::from_raw(hdl.0) };
        hdl.0 = INVALID_HANDLE;
        out
    }
}
impl HandleBased for Socket {}
impl AsHandleRef for Socket {
    fn as_handle_ref<'a>(&'a self) -> HandleRef<'a> {
        HandleRef(self.0, std::marker::PhantomData)
    }
}

impl Peered for Socket {}

impl Drop for Socket {
    fn drop(&mut self) {
        hdl_close(self.0);
    }
}

impl Socket {
    /// Create a streaming socket.
    pub fn create_stream() -> (Socket, Socket) {
        let rights = Rights::SOCKET_DEFAULT;
        let (left, right, obj) = new_handle_pair(HdlType::StreamSocket, rights);

        let mut obj = obj.lock().unwrap();
        let KObjectEntry::StreamSocket(obj) = &mut *obj else {
                    unreachable!("Channel we just allocated wasn't present or wasn't a channel");
                };
        let mut hdl_ref = HdlRef::StreamSocket(obj);
        hdl_ref
            .as_hdl_data()
            .signal(Side::Left, Signals::NONE, Signals::OBJECT_WRITABLE)
            .expect("Handle wasn't open immediately after creation");
        hdl_ref
            .as_hdl_data()
            .signal(Side::Right, Signals::NONE, Signals::OBJECT_WRITABLE)
            .expect("Handle wasn't open immediately after creation");
        (Socket(left), Socket(right))
    }

    /// Create a datagram socket.
    pub fn create_datagram() -> (Socket, Socket) {
        let rights = Rights::SOCKET_DEFAULT;
        let (left, right, obj) = new_handle_pair(HdlType::DatagramSocket, rights);

        let mut obj = obj.lock().unwrap();
        let KObjectEntry::DatagramSocket(obj) = &mut *obj else {
                    unreachable!("Channel we just allocated wasn't present or wasn't a channel");
                };
        let mut hdl_ref = HdlRef::DatagramSocket(obj);
        hdl_ref
            .as_hdl_data()
            .signal(Side::Left, Signals::NONE, Signals::OBJECT_WRITABLE)
            .expect("Handle wasn't open immediately after creation");
        hdl_ref
            .as_hdl_data()
            .signal(Side::Right, Signals::NONE, Signals::OBJECT_WRITABLE)
            .expect("Handle wasn't open immediately after creation");
        (Socket(left), Socket(right))
    }

    /// Write the given bytes into the socket.
    /// Return value (on success) is number of bytes actually written.
    pub fn write(&self, bytes: &[u8]) -> Result<usize, zx_status::Status> {
        with_handle(self.0, |h, side| {
            match h {
                HdlRef::StreamSocket(obj) => {
                    if !obj.is_open() {
                        return Err(zx_status::Status::PEER_CLOSED);
                    }
                    obj.q.side_mut(side).extend(bytes);
                    obj.signal(side.opposite(), Signals::NONE, Signals::OBJECT_READABLE)
                        .status_for_peer()?;
                }
                HdlRef::DatagramSocket(obj) => {
                    if !obj.is_open() {
                        return Err(zx_status::Status::PEER_CLOSED);
                    }
                    obj.q.side_mut(side).push_back(bytes.to_vec());
                    obj.signal(side.opposite(), Signals::NONE, Signals::OBJECT_READABLE)
                        .status_for_peer()?;
                }
                _ => panic!("Non socket passed to Socket::write"),
            }
            Ok(bytes.len())
        })
    }

    /// Return how many bytes are buffered in the socket
    pub fn outstanding_read_bytes(&self) -> Result<usize, zx_status::Status> {
        let (len, open) = with_handle(self.0, |h, side| match h {
            HdlRef::StreamSocket(obj) => (obj.q.side(side.opposite()).len(), obj.is_open()),
            HdlRef::DatagramSocket(obj) => (
                obj.q.side(side.opposite()).front().map(|frame| frame.len()).unwrap_or(0),
                obj.is_open(),
            ),
            _ => panic!("Non socket passed to Socket::outstanding_read_bytes"),
        });
        if len > 0 {
            return Ok(len);
        }
        if !open {
            return Err(zx_status::Status::PEER_CLOSED);
        }
        Ok(0)
    }

    fn poll_read(
        &self,
        bytes: &mut [u8],
        ctx: &mut Context<'_>,
    ) -> Poll<Result<usize, zx_status::Status>> {
        with_handle(self.0, |h, side| match h {
            HdlRef::StreamSocket(obj) => {
                if bytes.is_empty() {
                    if obj.is_open() {
                        return Poll::Ready(Ok(0));
                    } else {
                        return Poll::Ready(Err(zx_status::Status::PEER_CLOSED));
                    }
                }
                let read = obj.q.side_mut(side.opposite());
                let copy_bytes = std::cmp::min(bytes.len(), read.len());
                if copy_bytes == 0 {
                    if obj.is_open() {
                        return obj.wakers.side_mut(side).pending_readable(ctx);
                    } else {
                        return Poll::Ready(Err(zx_status::Status::PEER_CLOSED));
                    }
                }
                for (i, b) in read.drain(..copy_bytes).enumerate() {
                    bytes[i] = b;
                }
                if read.is_empty() {
                    obj.signal(side, Signals::OBJECT_READABLE, Signals::NONE).status_for_self()?;
                }
                Poll::Ready(Ok(copy_bytes))
            }
            HdlRef::DatagramSocket(obj) => {
                if let Some(frame) = obj.q.side_mut(side.opposite()).pop_front() {
                    let n = std::cmp::min(bytes.len(), frame.len());
                    bytes[..n].clone_from_slice(&frame[..n]);
                    if obj.q.side_mut(side.opposite()).is_empty() {
                        obj.signal(side, Signals::OBJECT_READABLE, Signals::NONE)
                            .status_for_self()?;
                    }
                    Poll::Ready(Ok(n))
                } else if !obj.is_open() {
                    Poll::Ready(Err(zx_status::Status::PEER_CLOSED))
                } else {
                    obj.wakers.side_mut(side).pending_readable(ctx)
                }
            }
            _ => panic!("Non socket passed to Socket::read"),
        })
    }

    /// Read bytes from the socket.
    /// Return value (on success) is number of bytes actually read.
    pub fn read(&self, bytes: &mut [u8]) -> Result<usize, zx_status::Status> {
        match self.poll_read(bytes, &mut Context::from_waker(noop_waker_ref())) {
            Poll::Ready(r) => r,
            Poll::Pending => Err(zx_status::Status::SHOULD_WAIT),
        }
    }
}

/// Emulation of Zircon EventPair.
#[derive(PartialEq, Eq, Debug, PartialOrd, Ord, Hash)]
pub struct EventPair(u32);

impl From<Handle> for EventPair {
    fn from(mut hdl: Handle) -> EventPair {
        let out = EventPair(hdl.0);
        hdl.0 = INVALID_HANDLE;
        out
    }
}
impl From<EventPair> for Handle {
    fn from(mut hdl: EventPair) -> Handle {
        let out = unsafe { Handle::from_raw(hdl.0) };
        hdl.0 = INVALID_HANDLE;
        out
    }
}
impl HandleBased for EventPair {}
impl AsHandleRef for EventPair {
    fn as_handle_ref<'a>(&'a self) -> HandleRef<'a> {
        HandleRef(self.0, std::marker::PhantomData)
    }
}

impl Peered for EventPair {}

impl Drop for EventPair {
    fn drop(&mut self) {
        hdl_close(self.0);
    }
}

impl EventPair {
    /// Create an event pair.
    pub fn create() -> (Self, Self) {
        Self::try_create().unwrap()
    }

    /// Create an event pair.
    pub fn try_create() -> Result<(EventPair, EventPair), Status> {
        let rights = Rights::EVENTPAIR_DEFAULT;
        let (left, right, _) = new_handle_pair(HdlType::EventPair, rights);
        Ok((EventPair(left), EventPair(right)))
    }
}

/// Emulation of Zircon Event.
#[derive(PartialEq, Eq, Debug, PartialOrd, Ord, Hash)]
pub struct Event(u32);

impl From<Handle> for Event {
    fn from(mut hdl: Handle) -> Event {
        let out = Event(hdl.0);
        hdl.0 = INVALID_HANDLE;
        out
    }
}
impl From<Event> for Handle {
    fn from(mut hdl: Event) -> Handle {
        let out = unsafe { Handle::from_raw(hdl.0) };
        hdl.0 = INVALID_HANDLE;
        out
    }
}
impl HandleBased for Event {}
impl AsHandleRef for Event {
    fn as_handle_ref<'a>(&'a self) -> HandleRef<'a> {
        HandleRef(self.0, std::marker::PhantomData)
    }
}

impl Drop for Event {
    fn drop(&mut self) {
        hdl_close(self.0);
    }
}

impl Event {
    /// Create an event .
    pub fn create() -> Event {
        Event(new_handle(HdlType::Event, Rights::EVENT_DEFAULT).0)
    }
}

/// A buffer for _receiving_ messages from a channel.
///
/// A `MessageBuf` is essentially a byte buffer and a vector of
/// handles, but move semantics for "taking" handles requires special handling.
///
/// Note that for sending messages to a channel, the caller manages the buffers,
/// using a plain byte slice and `Vec<Handle>`.
#[derive(Debug, Default)]
pub struct MessageBuf {
    bytes: Vec<u8>,
    handles: Vec<Handle>,
}

impl MessageBuf {
    /// Create a new, empty, message buffer.
    pub fn new() -> Self {
        Default::default()
    }

    /// Create a new non-empty message buffer.
    pub fn new_with(v: Vec<u8>, h: Vec<Handle>) -> Self {
        Self { bytes: v, handles: h }
    }

    /// Splits apart the message buf into a vector of bytes and a vector of handles.
    pub fn split_mut(&mut self) -> (&mut Vec<u8>, &mut Vec<Handle>) {
        (&mut self.bytes, &mut self.handles)
    }

    /// Splits apart the message buf into a vector of bytes and a vector of handles.
    pub fn split(self) -> (Vec<u8>, Vec<Handle>) {
        (self.bytes, self.handles)
    }

    /// Ensure that the buffer has the capacity to hold at least `n_bytes` bytes.
    pub fn ensure_capacity_bytes(&mut self, n_bytes: usize) {
        ensure_capacity(&mut self.bytes, n_bytes);
    }

    /// Ensure that the buffer has the capacity to hold at least `n_handles` handles.
    pub fn ensure_capacity_handles(&mut self, n_handles: usize) {
        ensure_capacity(&mut self.handles, n_handles);
    }

    /// Ensure that at least n_bytes bytes are initialized (0 fill).
    pub fn ensure_initialized_bytes(&mut self, n_bytes: usize) {
        if n_bytes <= self.bytes.len() {
            return;
        }
        self.bytes.resize(n_bytes, 0);
    }

    /// Get a reference to the bytes of the message buffer, as a `&[u8]` slice.
    pub fn bytes(&self) -> &[u8] {
        self.bytes.as_slice()
    }

    /// The number of handles in the message buffer. Note this counts the number
    /// available when the message was received; `take_handle` does not affect
    /// the count.
    pub fn n_handles(&self) -> usize {
        self.handles.len()
    }

    /// Take the handle at the specified index from the message buffer. If the
    /// method is called again with the same index, it will return `None`, as
    /// will happen if the index exceeds the number of handles available.
    pub fn take_handle(&mut self, index: usize) -> Option<Handle> {
        self.handles.get_mut(index).and_then(|handle| {
            if handle.is_invalid() {
                None
            } else {
                Some(std::mem::replace(handle, Handle::invalid()))
            }
        })
    }

    /// Clear the bytes and handles contained in the buf. This will drop any
    /// contained handles, resulting in their resources being freed.
    pub fn clear(&mut self) {
        self.bytes.clear();
        self.handles.clear();
    }
}

/// A buffer for _receiving_ messages from a channel.
///
/// This differs from `MessageBuf` in that it holds `HandleInfo` with
/// extended handle information.
///
/// A `MessageBufEtc` is essentially a byte buffer and a vector of handle
/// infos, but move semantics for "taking" handles requires special handling.
///
/// Note that for sending messages to a channel, the caller manages the buffers,
/// using a plain byte slice and `Vec<HandleDisposition>`.
#[derive(Debug, Default)]
pub struct MessageBufEtc {
    bytes: Vec<u8>,
    handle_infos: Vec<HandleInfo>,
}

impl MessageBufEtc {
    /// Create a new, empty, message buffer.
    pub fn new() -> Self {
        Default::default()
    }

    /// Create a new non-empty message buffer.
    pub fn new_with(v: Vec<u8>, h: Vec<HandleInfo>) -> Self {
        Self { bytes: v, handle_infos: h }
    }

    /// Splits apart the message buf into a vector of bytes and a vector of handle infos.
    pub fn split_mut(&mut self) -> (&mut Vec<u8>, &mut Vec<HandleInfo>) {
        (&mut self.bytes, &mut self.handle_infos)
    }

    /// Splits apart the message buf into a vector of bytes and a vector of handle infos.
    pub fn split(self) -> (Vec<u8>, Vec<HandleInfo>) {
        (self.bytes, self.handle_infos)
    }

    /// Ensure that the buffer has the capacity to hold at least `n_bytes` bytes.
    pub fn ensure_capacity_bytes(&mut self, n_bytes: usize) {
        ensure_capacity(&mut self.bytes, n_bytes);
    }

    /// Ensure that the buffer has the capacity to hold at least `n_handles` handle infos.
    pub fn ensure_capacity_handle_infos(&mut self, n_handle_infos: usize) {
        ensure_capacity(&mut self.handle_infos, n_handle_infos);
    }

    /// Ensure that at least n_bytes bytes are initialized (0 fill).
    pub fn ensure_initialized_bytes(&mut self, n_bytes: usize) {
        if n_bytes <= self.bytes.len() {
            return;
        }
        self.bytes.resize(n_bytes, 0);
    }

    /// Get a reference to the bytes of the message buffer, as a `&[u8]` slice.
    pub fn bytes(&self) -> &[u8] {
        self.bytes.as_slice()
    }

    /// The number of handles in the message buffer. Note this counts the number
    /// available when the message was received; `take_handle` does not affect
    /// the count.
    pub fn n_handle_infos(&self) -> usize {
        self.handle_infos.len()
    }

    /// Take the handle at the specified index from the message buffer. If the
    /// method is called again with the same index, it will return `None`, as
    /// will happen if the index exceeds the number of handles available.
    pub fn take_handle_info(&mut self, index: usize) -> Option<HandleInfo> {
        self.handle_infos.get_mut(index).and_then(|handle_info| {
            if handle_info.handle.is_invalid() {
                None
            } else {
                Some(std::mem::replace(
                    handle_info,
                    HandleInfo {
                        handle: Handle::invalid(),
                        object_type: ObjectType::NONE,
                        rights: Rights::NONE,
                    },
                ))
            }
        })
    }

    /// Clear the bytes and handles contained in the buf. This will drop any
    /// contained handles, resulting in their resources being freed.
    pub fn clear(&mut self) {
        self.bytes.clear();
        self.handle_infos.clear();
    }
}

fn ensure_capacity<T>(vec: &mut Vec<T>, size: usize) {
    let len = vec.len();
    if size > len {
        vec.reserve(size - len);
    }
}

pub mod on_signals {
    use super::*;

    /// Wait for some signals to be raised.
    #[must_use = "futures do nothing unless polled"]
    pub struct OnSignals<'a> {
        h: u32,
        koid: u64,
        signals: Signals,
        _lifetime: PhantomData<&'a ()>,
    }

    impl Unpin for OnSignals<'_> {}

    impl<'a> OnSignals<'a> {
        /// Construct a new OnSignals
        pub fn new<T: AsHandleRef>(handle: &'a T, signals: Signals) -> Self {
            Self::from_ref(handle.as_handle_ref(), signals)
        }

        /// Creates a new `OnSignals` using a HandleRef instead of an AsHandleRef.
        ///
        /// Passing a HandleRef to OnSignals::new is likely to lead to borrow check errors, since
        /// the resulting OnSignals is tied to the lifetime of the HandleRef itself and not the
        /// handle it refers to. Use this instead when you need to pass a HandleRef.
        pub fn from_ref(handle: HandleRef<'a>, signals: Signals) -> Self {
            let h = handle.0;
            with_handle(h, |mut hdl, side| Self {
                h,
                koid: hdl.as_hdl_data().koids(side).0,
                signals,
                _lifetime: PhantomData,
            })
        }

        /// This function allows the `OnSignals` object to live for the `'static` lifetime.
        ///
        /// It is functionally a no-op, but callers of this method should note that
        /// `OnSignals` will not fire if the handle that was used to create it is dropped or
        /// transferred to another process.
        pub fn extend_lifetime(self) -> OnSignals<'static> {
            OnSignals { h: self.h, koid: self.koid, signals: self.signals, _lifetime: PhantomData }
        }
    }

    impl Future for OnSignals<'_> {
        type Output = Result<Signals, Status>;
        fn poll(self: Pin<&mut Self>, ctx: &mut Context<'_>) -> Poll<Self::Output> {
            with_handle(self.h, |mut h, side| {
                let h = h.as_hdl_data();
                if self.koid != h.koids(side).0 || !h.is_side_open(side) {
                    if self.signals.contains(Signals::HANDLE_CLOSED) {
                        Poll::Ready(Signals::HANDLE_CLOSED)
                    } else {
                        Poll::Pending
                    }
                } else {
                    h.poll_signals(ctx, side, self.signals)
                }
            })
            .map(Ok)
        }
    }
}

bitflags! {
    /// Signals that can be waited upon.
    ///
    /// See [signals](https://fuchsia.dev/fuchsia-src/concepts/kernel/signals) for more information.
    #[repr(transparent)]
    pub struct Signals : u32 {
        /// No signals
        const NONE = 0x00000000;
        /// All object signals
        const OBJECT_ALL = 0x00ffffff;
        /// All user signals
        const USER_ALL = 0xff000000;

        /// Object signal 0
        const OBJECT_0 = 1 << 0;
        /// Object signal 1
        const OBJECT_1 = 1 << 1;
        /// Object signal 2
        const OBJECT_2 = 1 << 2;
        /// Object signal 3
        const OBJECT_3 = 1 << 3;
        /// Object signal 4
        const OBJECT_4 = 1 << 4;
        /// Object signal 5
        const OBJECT_5 = 1 << 5;
        /// Object signal 6
        const OBJECT_6 = 1 << 6;
        /// Object signal 7
        const OBJECT_7 = 1 << 7;
        /// Object signal 8
        const OBJECT_8 = 1 << 8;
        /// Object signal 9
        const OBJECT_9 = 1 << 9;
        /// Object signal 10
        const OBJECT_10 = 1 << 10;
        /// Object signal 11
        const OBJECT_11 = 1 << 11;
        /// Object signal 12
        const OBJECT_12 = 1 << 12;
        /// Object signal 13
        const OBJECT_13 = 1 << 13;
        /// Object signal 14
        const OBJECT_14 = 1 << 14;
        /// Object signal 15
        const OBJECT_15 = 1 << 15;
        /// Object signal 16
        const OBJECT_16 = 1 << 16;
        /// Object signal 17
        const OBJECT_17 = 1 << 17;
        /// Object signal 18
        const OBJECT_18 = 1 << 18;
        /// Object signal 19
        const OBJECT_19 = 1 << 19;
        /// Object signal 20
        const OBJECT_20 = 1 << 20;
        /// Object signal 21
        const OBJECT_21 = 1 << 21;
        /// Object signal 22
        const OBJECT_22 = 1 << 22;
        /// Handle closed
        const HANDLE_CLOSED = 1 << 23;
        /// User signal 0
        const USER_0 = 1 << 24;
        /// User signal 1
        const USER_1 = 1 << 25;
        /// User signal 2
        const USER_2 = 1 << 26;
        /// User signal 3
        const USER_3 = 1 << 27;
        /// User signal 4
        const USER_4 = 1 << 28;
        /// User signal 5
        const USER_5 = 1 << 29;
        /// User signal 6
        const USER_6 = 1 << 30;
        /// User signal 7
        const USER_7 = 1 << 31;

        /// All user signals
        const USER_SIGNALS = Self::USER_0.bits() |
        Self::USER_1.bits() |
        Self::USER_2.bits() |
        Self::USER_3.bits() |
        Self::USER_4.bits() |
        Self::USER_5.bits() |
        Self::USER_6.bits() |
        Self::USER_7.bits();

        /// Object is readable
        const OBJECT_READABLE = Self::OBJECT_0.bits();
        /// Object is writable
        const OBJECT_WRITABLE = Self::OBJECT_1.bits();
        /// Object peer closed
        const OBJECT_PEER_CLOSED = Self::OBJECT_2.bits();

        /// Channel peer closed
        const CHANNEL_PEER_CLOSED = Self::OBJECT_PEER_CLOSED.bits();
    }
}

impl Signals {
    /// Returns `Status::INVALID_ARGS` if this signal set contains non-user signals.
    fn validate_user_signals(&self) -> Result<(), Status> {
        if Signals::USER_SIGNALS.contains(*self) {
            Ok(())
        } else {
            Err(Status::INVALID_ARGS)
        }
    }
}

bitflags! {
    /// Rights associated with a handle.
    ///
    /// See [rights](https://fuchsia.dev/fuchsia-src/concepts/kernel/rights) for more information.
    #[repr(C)]
    pub struct Rights: u32 {
        /// No rights.
        const NONE           = 0;
        /// Duplicate right.
        const DUPLICATE      = 1 << 0;
        /// Transfer right.
        const TRANSFER       = 1 << 1;
        /// Read right.
        const READ           = 1 << 2;
        /// Write right.
        const WRITE          = 1 << 3;
        /// Execute right.
        const EXECUTE = 1 << 4;
        /// Map right.
        const MAP = 1 << 5;
        /// Get Property right.
        const GET_PROPERTY = 1 << 6;
        /// Set Property right.
        const SET_PROPERTY = 1 << 7;
        /// Enumerate right.
        const ENUMERATE = 1 << 8;
        /// Destroy right.
        const DESTROY = 1 << 9;
        /// Set Policy right.
        const SET_POLICY = 1 << 10;
        /// Get Policy right.
        const GET_POLICY = 1 << 11;
        /// Signal right.
        const SIGNAL = 1 << 12;
        /// Signal Peer right.
        const SIGNAL_PEER = 1 << 13;
        /// Wait right.
        const WAIT = 1 << 14;
        /// Inspect right.
        const INSPECT = 1 << 15;
        /// Manage Job right.
        const MANAGE_JOB = 1 << 16;
        /// Manage Process right.
        const MANAGE_PROCESS = 1 << 17;
        /// Manage Thread right.
        const MANAGE_THREAD = 1 << 18;
        /// Apply Profile right.
        const APPLY_PROFILE = 1 << 19;
        /// Manage Socket right.
        const MANAGE_SOCKET = 1 << 20;
        /// Same rights.
        const SAME_RIGHTS = 1 << 31;
        /// A basic set of rights for most things.
        const BASIC_RIGHTS = Rights::TRANSFER.bits() |
                             Rights::DUPLICATE.bits() |
                             Rights::WAIT.bits() |
                             Rights::INSPECT.bits();
        /// IO related rights
        const IO = Rights::WRITE.bits() |
                   Rights::READ.bits();
        /// Rights of a new socket.
        const SOCKET_DEFAULT = Rights::BASIC_RIGHTS.bits() |
                                Rights::IO.bits() |
                                Rights::SIGNAL.bits() |
                                Rights::SIGNAL_PEER.bits();
        /// Rights of a new channel.
        const CHANNEL_DEFAULT = (Rights::BASIC_RIGHTS.bits() & !Rights::DUPLICATE.bits()) |
                                Rights::IO.bits() |
                                Rights::SIGNAL.bits() |
                                Rights::SIGNAL_PEER.bits();
        /// Rights of a new event pair.
        const EVENTPAIR_DEFAULT =
                                Rights::TRANSFER.bits() |
                                Rights::DUPLICATE.bits() |
                                Rights::IO.bits() |
                                Rights::SIGNAL.bits() |
                                Rights::SIGNAL_PEER.bits();
        /// Rights of a new event.
        const EVENT_DEFAULT = Rights::BASIC_RIGHTS.bits() |
                              Rights::SIGNAL.bits();
    }
}

impl Rights {
    /// Same as from_bits() but a const fn.
    #[inline]
    pub const fn from_bits_const(bits: u32) -> Option<Rights> {
        if (bits & !Rights::all().bits()) == 0 {
            return Some(Rights { bits });
        } else {
            None
        }
    }
}

/// Handle operation.
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub enum HandleOp<'a> {
    /// Move the handle.
    Move(Handle),
    /// Duplicate the handle.
    Duplicate(HandleRef<'a>),
}

/// Operation to perform on handles during write.
/// Based on zx_handle_disposition_t, but does not match the same layout.
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub struct HandleDisposition<'a> {
    /// Handle operation.
    pub handle_op: HandleOp<'a>,
    /// Object type to check, or NONE to avoid the check.
    pub object_type: ObjectType,
    /// Rights to send, or SAME_RIGHTS.
    /// Rights are checked against existing handle rights to ensure there is no
    /// increase in rights.
    pub rights: Rights,
    /// Result of attempting to write this handle disposition.
    pub result: Status,
}

/// HandleInfo represents a handle with additional metadata.
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub struct HandleInfo {
    /// The handle.
    pub handle: Handle,
    /// The type of object referenced by the handle.
    pub object_type: ObjectType,
    /// The rights of the handle.
    pub rights: Rights,
}

impl HandleInfo {
    /// # Safety
    ///
    /// See [`Handle::from_raw`] for requirements about the validity and closing
    /// of `raw.handle`.
    ///
    /// `raw.rights` must be a bitwise combination of one or more [`Rights`]
    /// with no additional bits set.
    ///
    /// Note that while `raw.ty` _should_ correspond to the type of the handle,
    /// that this is not required for safety.
    pub const unsafe fn from_raw(raw: zx_types::zx_handle_info_t) -> HandleInfo {
        HandleInfo {
            handle: Handle::from_raw(raw.handle),
            object_type: ObjectType(raw.ty),
            rights: Rights::from_bits_unchecked(raw.rights),
        }
    }
}

#[derive(Default)]
struct Sided<T> {
    left: T,
    right: T,
}

impl<T> Sided<T> {
    fn side_mut(&mut self, side: Side) -> &mut T {
        match side {
            Side::Left => &mut self.left,
            Side::Right => &mut self.right,
        }
    }

    fn side(&self, side: Side) -> &T {
        match side {
            Side::Left => &self.left,
            Side::Right => &self.right,
        }
    }
}

struct ChannelMessage {
    bytes: Vec<u8>,
    handles: Vec<Handle>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum HdlType {
    Channel,
    StreamSocket,
    DatagramSocket,
    EventPair,
    Event,
}

impl HdlType {
    fn object_type(&self) -> ObjectType {
        match self {
            HdlType::Channel => ObjectType::CHANNEL,
            HdlType::StreamSocket => ObjectType::SOCKET,
            HdlType::DatagramSocket => ObjectType::SOCKET,
            HdlType::EventPair => ObjectType::EVENTPAIR,
            HdlType::Event => ObjectType::EVENT,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Side {
    Left,
    Right,
}

impl Side {
    fn opposite(self) -> Side {
        match self {
            Side::Left => Side::Right,
            Side::Right => Side::Left,
        }
    }
}

#[derive(Default)]
struct WakerSlot(Vec<Waker>);

impl WakerSlot {
    fn wake(&mut self) {
        self.0.drain(..).for_each(|w| w.wake());
    }

    fn arm(&mut self, ctx: &mut Context<'_>) {
        self.0.push(ctx.waker().clone())
    }
}

#[derive(Default)]
struct Wakers {
    signals: [WakerSlot; 32],
}

impl Wakers {
    fn for_signals_in(&mut self, signals: Signals, mut f: impl FnMut(&mut WakerSlot)) {
        for i in 0..32 {
            if signals.bits() & (1 << i) != 0 {
                f(&mut self.signals[i])
            }
        }
    }

    fn wake(&mut self, signals: Signals) {
        self.for_signals_in(signals, |w| w.wake())
    }

    fn arm(&mut self, signals: Signals, ctx: &mut Context<'_>) {
        self.for_signals_in(signals, |w| w.arm(ctx))
    }

    fn pending<R>(&mut self, signals: Signals, ctx: &mut Context<'_>) -> Poll<R> {
        self.arm(signals, ctx);
        Poll::Pending
    }

    fn pending_readable<R>(&mut self, ctx: &mut Context<'_>) -> Poll<R> {
        self.pending(
            Signals::OBJECT_READABLE | Signals::HANDLE_CLOSED | Signals::OBJECT_PEER_CLOSED,
            ctx,
        )
    }
}

#[derive(Debug)]
enum SignalError {
    HandleInvalid,
}

trait SignalErrorToStatus<T> {
    fn status_for_self(self) -> Result<T, Status>;
    fn status_for_peer(self) -> Result<T, Status>;
}

impl<T> SignalErrorToStatus<T> for Result<T, SignalError> {
    fn status_for_self(self) -> Result<T, Status> {
        self.map_err(|e| {
            let SignalError::HandleInvalid = e;
            Status::BAD_HANDLE
        })
    }

    fn status_for_peer(self) -> Result<T, Status> {
        self.map_err(|e| {
            let SignalError::HandleInvalid = e;
            Status::PEER_CLOSED
        })
    }
}

trait HdlData {
    fn signal(
        &mut self,
        side: Side,
        clear_mask: Signals,
        set_mask: Signals,
    ) -> Result<(), SignalError>;
    fn poll_signals(
        &mut self,
        ctx: &mut Context<'_>,
        side: Side,
        signals: Signals,
    ) -> Poll<Signals>;
    fn koids(&self, side: Side) -> (u64, u64);
    fn is_side_open(&self, side: Side) -> bool;
}

struct KObject<Q> {
    two_sided: bool,
    q: Sided<Q>,
    wakers: Sided<Wakers>,
    open_count: Sided<usize>,
    koid_left: u64,
    signals: Sided<Signals>,
}

impl<Q> KObject<Q> {
    fn is_open(&self) -> bool {
        *self.open_count.side(Side::Left) != 0
            && (!self.two_sided || *self.open_count.side(Side::Right) != 0)
    }

    fn do_close(&mut self, side: Side) {
        let open_count = self.open_count.side_mut(side);
        *open_count = open_count.saturating_sub(1);

        if *open_count == 0 {
            self.wakers.side_mut(side).wake(Signals::HANDLE_CLOSED);

            if *self.open_count.side(side.opposite()) != 0 {
                self.wakers.side_mut(side).wake(Signals::HANDLE_CLOSED);
                self.signal(side.opposite(), Signals::empty(), Signals::OBJECT_PEER_CLOSED)
                    .expect("Close reported other side was open but we could not signal it.");
            }
        }
    }
}

impl<Q> HdlData for KObject<Q> {
    fn signal(
        &mut self,
        side: Side,
        clear_mask: Signals,
        set_mask: Signals,
    ) -> Result<(), SignalError> {
        if *self.open_count.side(side) == 0 {
            return Err(SignalError::HandleInvalid);
        }
        let signals = self.signals.side_mut(side);
        signals.remove(clear_mask);
        signals.insert(set_mask);
        self.wakers.side_mut(side).wake(*signals);
        Ok(())
    }

    fn poll_signals(
        &mut self,
        ctx: &mut Context<'_>,
        side: Side,
        signals: Signals,
    ) -> Poll<Signals> {
        let intersection = *self.signals.side(side) & signals;
        if !intersection.is_empty() {
            Poll::Ready(intersection)
        } else {
            self.wakers.side_mut(side).pending(signals, ctx)
        }
    }

    fn koids(&self, side: Side) -> (u64, u64) {
        if !self.two_sided {
            assert_eq!(side, Side::Left);
            return (self.koid_left, INVALID_KOID.0);
        }
        match side {
            Side::Left => (self.koid_left, self.koid_left + 1),
            Side::Right => (self.koid_left + 1, self.koid_left),
        }
    }

    fn is_side_open(&self, side: Side) -> bool {
        *self.open_count.side(side) != 0
    }
}

enum KObjectEntry {
    Channel(KObject<VecDeque<ChannelMessage>>),
    StreamSocket(KObject<VecDeque<u8>>),
    DatagramSocket(KObject<VecDeque<Vec<u8>>>),
    EventPair(KObject<()>),
    Event(KObject<()>),
}

impl KObjectEntry {
    fn is_open(&self) -> bool {
        match self {
            KObjectEntry::Channel(k) => k.is_open(),
            KObjectEntry::StreamSocket(k) => k.is_open(),
            KObjectEntry::DatagramSocket(k) => k.is_open(),
            KObjectEntry::EventPair(k) => k.is_open(),
            KObjectEntry::Event(k) => k.is_open(),
        }
    }

    fn increment_open_count(&mut self, side: Side) {
        match self {
            KObjectEntry::Channel(k) => *k.open_count.side_mut(side) += 1,
            KObjectEntry::StreamSocket(k) => *k.open_count.side_mut(side) += 1,
            KObjectEntry::DatagramSocket(k) => *k.open_count.side_mut(side) += 1,
            KObjectEntry::EventPair(k) => *k.open_count.side_mut(side) += 1,
            KObjectEntry::Event(k) => *k.open_count.side_mut(side) += 1,
        }
    }

    fn do_close(&mut self, side: Side) {
        match self {
            KObjectEntry::Channel(k) => k.do_close(side),
            KObjectEntry::StreamSocket(k) => k.do_close(side),
            KObjectEntry::DatagramSocket(k) => k.do_close(side),
            KObjectEntry::EventPair(k) => k.do_close(side),
            KObjectEntry::Event(k) => k.do_close(side),
        }
    }
}

#[derive(Clone)]
struct HandleTableEntry {
    object: Arc<Mutex<KObjectEntry>>,
    rights: Rights,
    side: Side,
}

/// Allocate two new handles which point to the peered sides of a single object. The `hdl_type` must
/// be a handle type that refers to a paired object, e.g. it should not be `HdlType::Event`.
fn new_handle_pair(hdl_type: HdlType, rights: Rights) -> (u32, u32, Arc<Mutex<KObjectEntry>>) {
    fn new_kobject<T: Default>() -> KObject<T> {
        KObject {
            two_sided: true,
            q: Default::default(),
            wakers: Default::default(),
            open_count: Sided { left: 1, right: 1 },
            koid_left: NEXT_KOID.fetch_add(2, Ordering::Relaxed),
            signals: Sided { left: Signals::empty(), right: Signals::empty() },
        }
    }

    let kobject_entry = Arc::new(Mutex::new(match hdl_type {
        HdlType::Channel => KObjectEntry::Channel(new_kobject()),
        HdlType::StreamSocket => KObjectEntry::StreamSocket(new_kobject()),
        HdlType::DatagramSocket => KObjectEntry::DatagramSocket(new_kobject()),
        HdlType::EventPair => KObjectEntry::EventPair(new_kobject()),
        HdlType::Event => panic!("Can't create a paired handle for the unpaired Event handle type"),
    }));

    let left = HandleTableEntry { object: Arc::clone(&kobject_entry), rights, side: Side::Left };

    let right = HandleTableEntry { object: Arc::clone(&kobject_entry), rights, side: Side::Right };

    let left_handle = alloc_handle();
    let right_handle = alloc_handle();
    let mut handle_table = HANDLE_TABLE.lock().unwrap();
    let _ = handle_table.insert(left_handle, left);
    let _ = handle_table.insert(right_handle, right);

    (left_handle, right_handle, kobject_entry)
}

/// Allocate a new handle which points to a single object. The `hdl_type` must be a handle type that
/// does not refer to a paired object, i.e. it must be `HdlType::Event`.
fn new_handle(hdl_type: HdlType, rights: Rights) -> (u32, Arc<Mutex<KObjectEntry>>) {
    fn new_kobject<T: Default>() -> KObject<T> {
        KObject {
            two_sided: false,
            q: Default::default(),
            wakers: Default::default(),
            open_count: Sided { left: 1, right: 0 },
            koid_left: NEXT_KOID.fetch_add(2, Ordering::Relaxed),
            signals: Sided { left: Signals::empty(), right: Signals::empty() },
        }
    }

    let kobject_entry = Arc::new(Mutex::new(match hdl_type {
        HdlType::Event => KObjectEntry::Event(new_kobject()),
        _ => panic!("Cannot create single handle for paired object"),
    }));

    let left = HandleTableEntry { object: Arc::clone(&kobject_entry), rights, side: Side::Left };

    let left_handle = alloc_handle();
    let mut handle_table = HANDLE_TABLE.lock().unwrap();
    let _ = handle_table.insert(left_handle, left);

    (left_handle, kobject_entry)
}

fn alloc_handle() -> u32 {
    FREE_HANDLES
        .lock()
        .unwrap()
        .pop_front()
        .unwrap_or_else(|| NEXT_HANDLE.fetch_add(1, Ordering::Relaxed))
}

enum HdlRef<'a> {
    Channel(&'a mut KObject<VecDeque<ChannelMessage>>),
    StreamSocket(&'a mut KObject<VecDeque<u8>>),
    DatagramSocket(&'a mut KObject<VecDeque<Vec<u8>>>),
    EventPair(&'a mut KObject<()>),
    Event(&'a mut KObject<()>),
}

impl<'a> HdlRef<'a> {
    fn as_hdl_data<'b>(&'b mut self) -> &'b mut dyn HdlData {
        match self {
            HdlRef::Channel(hdl) => *hdl,
            HdlRef::StreamSocket(hdl) => *hdl,
            HdlRef::DatagramSocket(hdl) => *hdl,
            HdlRef::EventPair(hdl) => *hdl,
            HdlRef::Event(hdl) => *hdl,
        }
    }
}

#[cfg(debug_assertions)]
std::thread_local! {
    static IN_WITH_HANDLE: Cell<bool> = Cell::new(false);
}

fn with_handle<R>(handle: u32, f: impl FnOnce(HdlRef<'_>, Side) -> R) -> R {
    #[cfg(debug_assertions)]
    IN_WITH_HANDLE.with(|iwh| assert_eq!(iwh.replace(true), false));
    let (side, object) = {
        let handle_table = HANDLE_TABLE.lock().unwrap();
        let entry = handle_table.get(&handle).expect("Tried to use dangling handle");
        (entry.side, Arc::clone(&entry.object))
    };

    let mut object = object.lock().unwrap();
    let r = match &mut *object {
        KObjectEntry::Channel(o) => f(HdlRef::Channel(&mut *o), side),
        KObjectEntry::StreamSocket(o) => f(HdlRef::StreamSocket(&mut *o), side),
        KObjectEntry::DatagramSocket(o) => f(HdlRef::DatagramSocket(&mut *o), side),
        KObjectEntry::EventPair(o) => f(HdlRef::EventPair(&mut *o), side),
        KObjectEntry::Event(o) => f(HdlRef::Event(&mut *o), side),
    };
    #[cfg(debug_assertions)]
    IN_WITH_HANDLE.with(|iwh| assert_eq!(iwh.replace(false), true));
    r
}

lazy_static::lazy_static! {
    static ref HANDLE_TABLE: Mutex<HashMap<u32, HandleTableEntry>> = Default::default();
    static ref FREE_HANDLES: Mutex<VecDeque<u32>> = Default::default();
}

static NEXT_KOID: AtomicU64 = AtomicU64::new(1);
static NEXT_HANDLE: AtomicU32 = AtomicU32::new(1);

fn get_hdl_type(handle: u32) -> Option<HdlType> {
    let object = {
        let table = HANDLE_TABLE.lock().unwrap();
        Arc::clone(&table.get(&handle)?.object)
    };

    let object = object.lock().unwrap();
    match &*object {
        KObjectEntry::Channel(_) => Some(HdlType::Channel),
        KObjectEntry::StreamSocket(_) => Some(HdlType::StreamSocket),
        KObjectEntry::DatagramSocket(_) => Some(HdlType::DatagramSocket),
        KObjectEntry::EventPair(_) => Some(HdlType::EventPair),
        KObjectEntry::Event(_) => Some(HdlType::Event),
    }
}

fn get_hdl_rights(handle: u32) -> Option<Rights> {
    let table = HANDLE_TABLE.lock().unwrap();
    table.get(&handle).map(|x| x.rights)
}

/// Close the handle: no action if hdl==INVALID_HANDLE
fn hdl_close(hdl: u32) {
    if hdl == INVALID_HANDLE {
        return;
    }
    let Some(entry) = HANDLE_TABLE.lock().unwrap().remove(&hdl) else {
        return;
    };

    entry.object.lock().unwrap().do_close(entry.side);
    FREE_HANDLES.lock().unwrap().push_back(hdl);
}

#[cfg(test)]
mod test {
    use super::*;
    use fuchsia_zircon_status as zx_status;
    use futures::FutureExt;
    use std::mem::ManuallyDrop;
    use zx_status::Status;

    /// Returns a "handle" which mimics a closed handle.
    ///
    /// This handle is not a real handle, and will cause panics most places it is used. It should
    /// be impossible to get this kind of handle safely, but some tests want to assert what happens
    /// if you try to use a closed handle.
    ///
    /// The result is wrapped in a ManuallyDrop because you should avoid dropping the resulting
    /// handle because that too will trigger a panic.
    #[deny(unsafe_op_in_unsafe_fn)]
    unsafe fn mimic_closed_handle() -> std::mem::ManuallyDrop<Handle> {
        let raw_unallocated_handle = alloc_handle();
        let unallocated_handle = unsafe { Handle::from_raw(raw_unallocated_handle) };
        std::mem::ManuallyDrop::new(unallocated_handle)
    }

    #[test]
    fn channel_create_not_closed() {
        let (a, b) = Channel::create();
        assert_eq!(a.is_closed(), false);
        assert_eq!(b.is_closed(), false);
    }

    #[test]
    fn channel_drop_left_closes_right() {
        let (a, b) = Channel::create();
        drop(a);
        assert_eq!(b.is_closed(), true);
    }

    #[test]
    fn channel_drop_right_closes_left() {
        let (a, b) = Channel::create();
        drop(b);
        assert_eq!(a.is_closed(), true);
    }

    #[test]
    fn channel_read_raw() {
        const UNINIT: MaybeUninit<Handle> = MaybeUninit::<Handle>::uninit();
        let (a, b) = Channel::create();
        let (c, d) = Channel::create();
        let mut buf: [u8; 2] = [0, 0];
        let mut handles = [UNINIT; 2];
        assert_eq!(
            b.read_raw(&mut buf, &mut handles).ok().unwrap(),
            (Err(Status::SHOULD_WAIT), 0, 0)
        );
        d.write(&[4, 5, 6], &mut vec![]).unwrap();
        a.write(&[1, 2, 3], &mut vec![c.into(), d.into()]).unwrap();

        // Should err even though handle length is the same.
        let (b_len, h_len) = b.read_raw(&mut buf[..], &mut handles[..]).err().unwrap();
        assert_eq!(b_len, 3);
        assert_eq!(h_len, 2);

        let mut buf = [0, 0, 0];
        assert_eq!(b.read_raw(&mut buf, &mut handles).ok().unwrap(), (Ok(()), 3, 2));
        assert_eq!(buf, [1, 2, 3]);
        assert_eq!(handles.len(), 2);
        let mut handles_iter = handles.into_iter().take(2);
        let c: Channel = unsafe { handles_iter.next().unwrap().assume_init() }.into();
        let d: Channel = unsafe { handles_iter.next().unwrap().assume_init() }.into();
        let mut handles = [UNINIT; 0];
        assert_eq!(c.read_raw(&mut buf, &mut handles).ok().unwrap(), (Ok(()), 3, 0));
        assert_eq!(buf, [4, 5, 6]);
        b.write(&[1, 2], &mut vec![c.into(), d.into()]).unwrap();

        // Checks that having an incorrect handle buffer size also fails.
        assert_eq!(a.read_raw(&mut buf, &mut handles).err().unwrap(), (2, 2));

        let mut handles = [UNINIT; 2];

        // Verifies that copying into an "oversized" buffer does not fail (copy_to_slice panics
        // if the slices are not the same size).
        let _ = a.read_raw(&mut buf, &mut handles).unwrap();

        let mut handles_iter = handles.into_iter().take(2);
        let c: Channel = unsafe { handles_iter.next().unwrap().assume_init() }.into();
        let d: Channel = unsafe { handles_iter.next().unwrap().assume_init() }.into();

        // Verifies that the passed channels weren't closed after being moved.
        c.write(&[6, 7, 8], &mut vec![]).unwrap();
        let mut handles = [UNINIT; 0];
        assert_eq!(d.read_raw(&mut buf, &mut handles).unwrap(), (Ok(()), 3, 0));
        assert_eq!(buf, [6, 7, 8]);
    }

    #[test]
    fn channel_write_read() {
        let (a, b) = Channel::create();
        let (c, d) = Channel::create();
        let mut incoming = MessageBuf::new();

        assert_eq!(b.read(&mut incoming).err().unwrap(), Status::SHOULD_WAIT);
        d.write(&[4, 5, 6], &mut vec![]).unwrap();
        a.write(&[1, 2, 3], &mut vec![c.into(), d.into()]).unwrap();

        b.read(&mut incoming).unwrap();
        assert_eq!(incoming.bytes(), &[1, 2, 3]);
        assert_eq!(incoming.n_handles(), 2);
        let c: Channel = incoming.take_handle(0).unwrap().into();
        let d: Channel = incoming.take_handle(1).unwrap().into();
        c.read(&mut incoming).unwrap();
        drop(d);
        assert_eq!(incoming.bytes(), &[4, 5, 6]);
        assert_eq!(incoming.n_handles(), 0);
    }

    #[test]
    fn channel_write_etc_read_etc() {
        let (a, b) = Channel::create();
        let (c, d) = Channel::create();
        let mut incoming = MessageBufEtc::new();

        assert_eq!(b.read_etc(&mut incoming).err().unwrap(), Status::SHOULD_WAIT);
        d.write(&[4, 5, 6], &mut vec![]).unwrap();
        let mut hds = vec![
            HandleDisposition {
                handle_op: HandleOp::Move(c.into()),
                object_type: ObjectType::CHANNEL,
                rights: Rights::SAME_RIGHTS,
                result: Status::OK,
            },
            HandleDisposition {
                handle_op: HandleOp::Move(d.into()),
                object_type: ObjectType::CHANNEL,
                rights: Rights::TRANSFER | Rights::READ,
                result: Status::OK,
            },
        ];
        a.write_etc(&[1, 2, 3], &mut hds).unwrap();

        b.read_etc(&mut incoming).unwrap();
        assert_eq!(incoming.bytes(), &[1, 2, 3]);
        assert_eq!(incoming.n_handle_infos(), 2);

        let mut c_handle_info = incoming.take_handle_info(0).unwrap();
        assert_eq!(c_handle_info.object_type, ObjectType::CHANNEL);
        assert_eq!(c_handle_info.rights, Rights::CHANNEL_DEFAULT);
        let mut d_handle_info = incoming.take_handle_info(1).unwrap();
        assert_eq!(d_handle_info.object_type, ObjectType::CHANNEL);
        assert_eq!(d_handle_info.rights, Rights::TRANSFER | Rights::READ);
        let c: Channel = std::mem::replace(&mut c_handle_info.handle, Handle::invalid()).into();
        let d: Channel = std::mem::replace(&mut d_handle_info.handle, Handle::invalid()).into();
        c.read_etc(&mut incoming).unwrap();
        drop(d);
        assert_eq!(incoming.bytes(), &[4, 5, 6]);
        assert_eq!(incoming.n_handle_infos(), 0);
    }

    #[test]
    fn mixed_channel_write_read_etc() {
        let (a, b) = Channel::create();
        let (c, _) = Channel::create();
        a.write(&[1, 2, 3], &mut [c.into()]).unwrap();
        let mut buf = MessageBufEtc::new();
        b.read_etc(&mut buf).unwrap();
        assert_eq!(buf.bytes(), &[1, 2, 3]);
        assert_eq!(buf.n_handle_infos(), 1);
        let hi = &buf.handle_infos[0];
        assert_eq!(hi.object_type, ObjectType::CHANNEL);
        assert_eq!(hi.rights, Rights::CHANNEL_DEFAULT);
        assert_ne!(hi.handle, Handle::invalid());
    }

    #[test]
    fn mixed_channel_write_etc_read() {
        let (a, b) = Channel::create();
        let (c, _) = Channel::create();
        let hd = HandleDisposition {
            handle_op: HandleOp::Move(c.into()),
            object_type: ObjectType::NONE,
            rights: Rights::SAME_RIGHTS,
            result: Status::OK,
        };
        a.write_etc(&[1, 2, 3], &mut [hd]).unwrap();
        let mut buf = MessageBuf::new();
        b.read(&mut buf).unwrap();
        assert_eq!(buf.bytes(), &[1, 2, 3]);
        assert_eq!(buf.n_handles(), 1);
        assert_ne!(buf.handles[0], Handle::invalid());
    }

    #[test]
    fn socket_write_read() {
        let (a, b) = Socket::create_stream();
        a.write(&[1, 2, 3]).unwrap();
        let mut buf = [0u8; 128];
        assert_eq!(b.read(&mut buf).unwrap(), 3);
        assert_eq!(&buf[0..3], &[1, 2, 3]);
    }

    #[test]
    fn socket_dup_write_read() {
        let (a, b) = Socket::create_stream();
        let c = a.duplicate_handle(Rights::SAME_RIGHTS).unwrap();
        a.write(&[1, 2, 3]).unwrap();
        c.write(&[4, 5, 6]).unwrap();
        drop(c);
        a.write(&[7, 8, 9]).unwrap();
        let mut buf = [0u8; 128];
        assert_eq!(b.read(&mut buf).unwrap(), 9);
        assert_eq!(&buf[0..9], &[1, 2, 3, 4, 5, 6, 7, 8, 9]);
    }

    #[test]
    fn socket_write_dup_read() {
        let (a, b) = Socket::create_stream();
        let c = b.duplicate_handle(Rights::SAME_RIGHTS).unwrap();
        a.write(&[1, 2, 3, 4, 5, 6]).unwrap();
        let mut buf = [0u8; 3];
        assert_eq!(b.read(&mut buf).unwrap(), 3);
        assert_eq!(&buf[0..3], &[1, 2, 3]);
        drop(b);
        assert_eq!(c.read(&mut buf).unwrap(), 3);
        assert_eq!(&buf[0..3], &[4, 5, 6]);
    }

    #[test]
    fn socket_dup_requires_right() {
        let (_a, b) = Socket::create_stream();
        let c = b.duplicate_handle(Rights::SOCKET_DEFAULT & !Rights::DUPLICATE).unwrap();
        assert!(matches!(c.duplicate_handle(Rights::SAME_RIGHTS), Err(Status::ACCESS_DENIED)));
    }

    #[test]
    fn channel_basic() {
        let (p1, p2) = Channel::create();

        let mut empty = vec![];
        assert!(p1.write(b"hello", &mut empty).is_ok());

        let mut buf = MessageBuf::new();
        assert!(p2.read(&mut buf).is_ok());
        assert_eq!(buf.bytes(), b"hello");
    }

    #[test]
    fn channel_send_handle() {
        let hello_length: usize = 5;

        // Create a pair of channels and a pair of sockets.
        let (p1, p2) = Channel::create();
        let (s1, s2) = Socket::create_stream();

        // Send one socket down the channel
        let mut handles_to_send: Vec<Handle> = vec![s1.into_handle()];
        assert!(p1.write(b"", &mut handles_to_send).is_ok());
        // The handle vector should only contain invalid handles.
        for handle in handles_to_send {
            assert!(handle.is_invalid());
        }

        // Read the handle from the receiving channel.
        let mut buf = MessageBuf::new();
        assert!(p2.read(&mut buf).is_ok());
        assert_eq!(buf.n_handles(), 1);
        // Take the handle from the buffer.
        let received_handle = buf.take_handle(0).unwrap();
        // Should not affect number of handles.
        assert_eq!(buf.n_handles(), 1);
        // Trying to take it again should fail.
        assert!(buf.take_handle(0).is_none());

        // Now to test that we got the right handle, try writing something to it...
        let received_socket = Socket::from(received_handle);
        assert!(received_socket.write(b"hello").is_ok());

        // ... and reading it back from the original VMO.
        let mut read_vec = vec![0; hello_length];
        assert!(s2.read(&mut read_vec).is_ok());
        assert_eq!(read_vec, b"hello");
    }

    #[test]
    fn socket_basic() {
        let (s1, s2) = Socket::create_stream();

        // Write two packets and read from other end
        assert_eq!(s1.write(b"hello").unwrap(), 5);
        assert_eq!(s1.write(b"world").unwrap(), 5);

        let mut read_vec = vec![0; 11];
        assert_eq!(s2.read(&mut read_vec).unwrap(), 10);
        assert_eq!(&read_vec[0..10], b"helloworld");

        // Try reading when there is nothing to read.
        assert_eq!(s2.read(&mut read_vec), Err(Status::SHOULD_WAIT));
    }

    #[cfg(not(target_os = "fuchsia"))]
    #[test]
    fn object_type_is_correct() {
        let (c1, c2) = Channel::create();
        let (s1, s2) = Socket::create_stream();
        assert_eq!(c1.into_handle().object_type(), ObjectType::CHANNEL);
        assert_eq!(c2.into_handle().object_type(), ObjectType::CHANNEL);
        assert_eq!(s1.into_handle().object_type(), ObjectType::SOCKET);
        assert_eq!(s2.into_handle().object_type(), ObjectType::SOCKET);
    }

    #[cfg(not(target_os = "fuchsia"))]
    #[test]
    fn invalid_handle_is_not_dangling() {
        let h = Handle::invalid();
        assert!(!h.is_dangling());
    }

    #[cfg(not(target_os = "fuchsia"))]
    #[test]
    fn live_valid_handle_is_not_dangling() {
        let (c1, c2) = Channel::create();
        assert!(!c1.is_dangling());
        assert!(!c2.is_dangling());
    }

    #[cfg(not(target_os = "fuchsia"))]
    #[test]
    fn closed_handle_is_dangling() {
        let closed_handle = unsafe { mimic_closed_handle() };

        assert!(closed_handle.is_dangling());
    }

    #[test]
    fn handle_basic_info_success() {
        let (c1, c2) = Channel::create();
        let c1_info = c1.basic_info().unwrap();
        let c2_info = c2.basic_info().unwrap();

        assert_ne!(c1_info.koid, INVALID_KOID);
        assert_ne!(c1_info.related_koid, INVALID_KOID);
        assert_ne!(c1_info.koid, c1_info.related_koid);
        assert_eq!(c1_info.related_koid, c2_info.koid);
        assert_eq!(c1_info.koid, c2_info.related_koid);

        assert_eq!(c1_info.rights, Rights::CHANNEL_DEFAULT);
        assert_eq!(c1_info.object_type, ObjectType::CHANNEL);
        assert_eq!(c1_info.reserved, 0);
    }

    #[test]
    fn handle_basic_info_invalid() {
        assert_eq!(Handle::invalid().basic_info().unwrap_err(), zx_status::Status::BAD_HANDLE);

        // Note non-zero but invalid handles can't be tested because of a
        // panic when the handle isn't found in with_handle().
    }

    #[test]
    fn handle_replace_success() {
        let (c1, c2) = Channel::create();
        let c1_basic_info = c1.basic_info().unwrap();
        let c2_basic_info = c2.basic_info().unwrap();
        assert_eq!(c1_basic_info.rights, Rights::CHANNEL_DEFAULT);

        let new_handle = c1.into_handle().replace(Rights::TRANSFER | Rights::WRITE).unwrap();

        let new_c1_basic_info = new_handle.basic_info().unwrap();
        assert_eq!(new_c1_basic_info.koid, c1_basic_info.koid);
        assert_eq!(new_c1_basic_info.related_koid, c1_basic_info.related_koid);
        assert_eq!(new_c1_basic_info.object_type, ObjectType::CHANNEL);
        assert_eq!(new_c1_basic_info.rights, Rights::TRANSFER | Rights::WRITE);

        let new_c2_basic_info = c2.basic_info().unwrap();
        assert_eq!(new_c2_basic_info.koid, c2_basic_info.koid);
        assert_eq!(new_c2_basic_info.related_koid, c2_basic_info.related_koid);
        assert_eq!(new_c2_basic_info.object_type, c2_basic_info.object_type);
        assert_eq!(new_c2_basic_info.rights, c2_basic_info.rights);
    }

    #[test]
    fn handle_replace_invalid() {
        assert_eq!(
            Handle::invalid().replace(Rights::TRANSFER).unwrap_err(),
            zx_status::Status::BAD_HANDLE
        );

        let closed_handle = unsafe { mimic_closed_handle() };
        assert_eq!(
            ManuallyDrop::into_inner(closed_handle).replace(Rights::TRANSFER).unwrap_err(),
            zx_status::Status::BAD_HANDLE
        );
    }

    #[test]
    fn handle_replace_increasing_rights() {
        let (c1, _) = Channel::create();
        let orig_basic_info = c1.basic_info().unwrap();
        assert_eq!(orig_basic_info.rights, Rights::CHANNEL_DEFAULT);
        assert_eq!(
            c1.into_handle().replace(Rights::DUPLICATE).unwrap_err(),
            zx_status::Status::INVALID_ARGS
        );
    }

    #[test]
    fn handle_replace_same_rights() {
        let (c1, _) = Channel::create();
        let orig_basic_info = c1.basic_info().unwrap();
        assert_eq!(orig_basic_info.rights, Rights::CHANNEL_DEFAULT);
        let orig_raw = c1.raw_handle();

        let new_handle = c1.into_handle().replace(Rights::SAME_RIGHTS).unwrap();
        assert_eq!(new_handle.raw_handle(), orig_raw);

        let new_basic_info = new_handle.basic_info().unwrap();
        assert_eq!(new_basic_info.rights, Rights::CHANNEL_DEFAULT);
    }

    #[test]
    fn await_user_signal() {
        let (c1, _) = EventPair::create();
        let mut on_sig = on_signals::OnSignals::new(&c1, Signals::USER_0);
        let (waker, count) = futures_test::task::new_count_waker();
        let mut ctx = Context::from_waker(&waker);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Pending);
        assert_eq!(count, 0);
        c1.signal_handle(Signals::empty(), Signals::USER_1).unwrap();
        assert_eq!(count, 0);
        c1.signal_handle(Signals::empty(), Signals::USER_0).unwrap();
        assert_eq!(count, 1);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::USER_0)));
        c1.signal_handle(Signals::USER_0, Signals::empty()).unwrap();
        let mut on_sig = on_signals::OnSignals::new(&c1, Signals::USER_0);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Pending);
        assert_eq!(count, 1);
        c1.signal_handle(Signals::empty(), Signals::USER_0).unwrap();
        assert_eq!(count, 2);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::USER_0)));
    }

    #[test]
    fn await_user_signal_peer() {
        let (c1, c2) = EventPair::create();
        let mut on_sig = on_signals::OnSignals::new(&c1, Signals::USER_0);
        let (waker, count) = futures_test::task::new_count_waker();
        let mut ctx = Context::from_waker(&waker);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Pending);
        assert_eq!(count, 0);
        c2.signal_peer(Signals::empty(), Signals::USER_1).unwrap();
        assert_eq!(count, 0);
        c2.signal_peer(Signals::empty(), Signals::USER_0).unwrap();
        assert_eq!(count, 1);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::USER_0)));
        c2.signal_peer(Signals::USER_0, Signals::empty()).unwrap();
        let mut on_sig = on_signals::OnSignals::new(&c1, Signals::USER_0);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Pending);
        assert_eq!(count, 1);
        c2.signal_peer(Signals::empty(), Signals::USER_0).unwrap();
        assert_eq!(count, 2);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::USER_0)));
    }

    #[test]
    fn await_close_signal() {
        let (c1, c2) = EventPair::create();
        let mut on_sig = on_signals::OnSignals::new(&c1, Signals::OBJECT_PEER_CLOSED);
        let (waker, count) = futures_test::task::new_count_waker();
        let mut ctx = Context::from_waker(&waker);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Pending);
        assert_eq!(count, 0);
        c1.signal_handle(Signals::empty(), Signals::USER_1).unwrap();
        assert_eq!(count, 0);
        c1.signal_handle(Signals::empty(), Signals::USER_0).unwrap();
        assert_eq!(count, 0);
        std::mem::drop(c2);
        assert_eq!(count, 1);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::OBJECT_PEER_CLOSED)));
    }

    #[test]
    fn user_signal_no_rights() {
        let (c1, _) = EventPair::create();
        let c1 = c1.into_handle().replace(Rights::EVENTPAIR_DEFAULT & !Rights::SIGNAL).unwrap();
        assert_eq!(c1.signal_handle(Signals::empty(), Signals::USER_1), Err(Status::ACCESS_DENIED));
    }

    #[test]
    fn user_signal_peer_no_rights() {
        let (c1, _) = EventPair::create();
        let c1 =
            c1.into_handle().replace(Rights::EVENTPAIR_DEFAULT & !Rights::SIGNAL_PEER).unwrap();
        let c1 = EventPair::from(c1);
        assert_eq!(c1.signal_peer(Signals::empty(), Signals::USER_1), Err(Status::ACCESS_DENIED));
    }

    #[test]
    fn kernel_signal_denied() {
        let (c1, _) = EventPair::create();
        assert_eq!(
            c1.signal_handle(Signals::empty(), Signals::OBJECT_WRITABLE),
            Err(Status::INVALID_ARGS)
        );
    }

    #[test]
    fn kernel_signal_peer_denied() {
        let (c1, _) = EventPair::create();
        assert_eq!(
            c1.signal_peer(Signals::empty(), Signals::OBJECT_WRITABLE),
            Err(Status::INVALID_ARGS)
        );
    }

    #[test]
    fn handles_always_writable() {
        let mut ctx = futures_test::task::noop_context();
        let (s1, s2) = Socket::create_stream();
        let mut on_sig = on_signals::OnSignals::new(&s1, Signals::OBJECT_WRITABLE);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::OBJECT_WRITABLE)));
        let mut on_sig = on_signals::OnSignals::new(&s2, Signals::OBJECT_WRITABLE);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::OBJECT_WRITABLE)));

        let (s1, s2) = Socket::create_datagram();
        let mut on_sig = on_signals::OnSignals::new(&s1, Signals::OBJECT_WRITABLE);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::OBJECT_WRITABLE)));
        let mut on_sig = on_signals::OnSignals::new(&s2, Signals::OBJECT_WRITABLE);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::OBJECT_WRITABLE)));

        let (c1, c2) = Channel::create();
        let mut on_sig = on_signals::OnSignals::new(&c1, Signals::OBJECT_WRITABLE);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::OBJECT_WRITABLE)));
        let mut on_sig = on_signals::OnSignals::new(&c2, Signals::OBJECT_WRITABLE);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::OBJECT_WRITABLE)));
    }

    #[test]
    fn read_signal() {
        let (c1, c2) = Channel::create();
        let mut on_sig = on_signals::OnSignals::new(&c1, Signals::OBJECT_READABLE);
        let (waker, count) = futures_test::task::new_count_waker();
        let mut ctx = Context::from_waker(&waker);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Pending);
        assert_eq!(count, 0);
        assert_eq!(c2.write(b"abc", &mut []), Ok(()));
        assert_eq!(count, 1);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::OBJECT_READABLE)));
    }

    #[test]
    fn event_is_one_sided() {
        let e = Event::create();
        assert_eq!(e.object_type(), ObjectType::EVENT);
        assert!(e.related().is_invalid());
        assert_eq!(e.koid_pair().1, INVALID_KOID.0);
        assert_eq!(e.basic_info().unwrap().related_koid, INVALID_KOID);
    }

    #[test]
    fn event_replace_rights() {
        let e = Event::create();
        assert_eq!(e.basic_info().unwrap().rights, Rights::EVENT_DEFAULT);
        let e = e.into_handle().replace(Rights::TRANSFER).unwrap();
        assert_eq!(e.basic_info().unwrap().rights, Rights::TRANSFER);
    }

    #[test]
    fn event_user_signal() {
        let e = Event::create();
        let mut on_sig = on_signals::OnSignals::new(&e, Signals::USER_0);
        let (waker, count) = futures_test::task::new_count_waker();
        let mut ctx = Context::from_waker(&waker);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Pending);
        assert_eq!(count, 0);
        assert_eq!(e.signal_handle(Signals::empty(), Signals::USER_0), Ok(()));
        assert_eq!(count, 1);
        assert_eq!(on_sig.poll_unpin(&mut ctx), Poll::Ready(Ok(Signals::USER_0)));
    }

    #[test]
    fn mix_one_sided_and_two_sided_handles() {
        // Test non-sided, left-side, and right-side handles for odd/even koids.
        let e1 = Event::create(); // odd
        let (c1, c2) = Channel::create(); // (even, odd)
        let e2 = Event::create(); // even
        let (p1, p2) = EventPair::create(); // (odd, even)

        let e1_info = e1.basic_info().unwrap();
        let c1_info = c1.basic_info().unwrap();
        let c2_info = c2.basic_info().unwrap();
        let e2_info = e2.basic_info().unwrap();
        let p1_info = p1.basic_info().unwrap();
        let p2_info = p2.basic_info().unwrap();

        assert_eq!(e1_info.object_type, ObjectType::EVENT);
        assert_eq!(c1_info.object_type, ObjectType::CHANNEL);
        assert_eq!(c2_info.object_type, ObjectType::CHANNEL);
        assert_eq!(e2_info.object_type, ObjectType::EVENT);
        assert_eq!(p1_info.object_type, ObjectType::EVENTPAIR);
        assert_eq!(p2_info.object_type, ObjectType::EVENTPAIR);

        assert_eq!(e1_info.related_koid, INVALID_KOID);
        assert_eq!(c1_info.related_koid, c2_info.koid);
        assert_eq!(c2_info.related_koid, c1_info.koid);
        assert_eq!(e2_info.related_koid, INVALID_KOID);
        assert_eq!(p1_info.related_koid, p2_info.koid);
        assert_eq!(p2_info.related_koid, p1_info.koid);

        assert_eq!(e1.koid_pair(), (e1_info.koid.0, INVALID_KOID.0));
        assert_eq!(c1.koid_pair(), (c1_info.koid.0, c2_info.koid.0));
        assert_eq!(c2.koid_pair(), (c2_info.koid.0, c1_info.koid.0));
        assert_eq!(e2.koid_pair(), (e2_info.koid.0, INVALID_KOID.0));
        assert_eq!(p1.koid_pair(), (p1_info.koid.0, p2_info.koid.0));
        assert_eq!(p2.koid_pair(), (p2_info.koid.0, p1_info.koid.0));

        assert!(e1.related().is_invalid());
        assert_eq!(c1.related(), c2.as_handle_ref());
        assert_eq!(c2.related(), c1.as_handle_ref());
        assert!(e2.related().is_invalid());
        assert_eq!(p1.related(), p2.as_handle_ref());
        assert_eq!(p2.related(), p1.as_handle_ref());
    }
}
