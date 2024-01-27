// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::convert::From;

use crate::zxio::{
    zxio_dirent_iterator_next, zxio_dirent_iterator_t, ZXIO_NODE_PROTOCOL_DIRECTORY,
    ZXIO_NODE_PROTOCOL_FILE,
};
use bitflags::bitflags;
use fidl::{encoding::const_assert_eq, endpoints::ServerEnd};
use fidl_fuchsia_io as fio;
use fuchsia_zircon::{
    self as zx,
    sys::{ZX_ERR_INVALID_ARGS, ZX_ERR_NO_MEMORY, ZX_OK},
    zx_status_t, HandleBased,
};
use linux_uapi::x86_64::{__kernel_sockaddr_storage as sockaddr_storage, c_int, c_void};
use std::{ffi::CStr, pin::Pin};
use zxio::{
    msghdr, sockaddr, socklen_t, zx_handle_t, zxio_object_type_t, zxio_storage_t,
    ZXIO_SHUTDOWN_OPTIONS_READ, ZXIO_SHUTDOWN_OPTIONS_WRITE,
};

pub mod zxio;

pub use zxio::zxio_dirent_t;
pub use zxio::zxio_node_attributes_t;
pub use zxio::zxio_signals_t;

bitflags! {
    // These values should match the values in sdk/lib/zxio/include/lib/zxio/types.h
    pub struct ZxioSignals : zxio_signals_t {
        const NONE            =      0;
        const READABLE        = 1 << 0;
        const WRITABLE        = 1 << 1;
        const READ_DISABLED   = 1 << 2;
        const WRITE_DISABLED  = 1 << 3;
        const READ_THRESHOLD  = 1 << 4;
        const WRITE_THRESHOLD = 1 << 5;
        const OUT_OF_BAND     = 1 << 6;
        const ERROR           = 1 << 7;
        const PEER_CLOSED     = 1 << 8;
    }
}

bitflags! {
    /// The flags for shutting down sockets.
    pub struct ZxioShutdownFlags: u32 {
        /// Further transmissions will be disallowed.
        const WRITE = 1 << 0;

        /// Further receptions will be disallowed.
        const READ = 1 << 1;
    }
}

const_assert_eq!(ZxioShutdownFlags::WRITE.bits(), ZXIO_SHUTDOWN_OPTIONS_WRITE);
const_assert_eq!(ZxioShutdownFlags::READ.bits(), ZXIO_SHUTDOWN_OPTIONS_READ);

// TODO: We need a more comprehensive error strategy.
// Our dependencies create elaborate error objects, but Starnix would prefer
// this library produce zx::Status errors for easier conversion to Errno.

#[derive(Default, Debug)]
pub struct ZxioDirent {
    pub protocols: Option<zxio::zxio_node_protocols_t>,
    pub abilities: Option<zxio::zxio_abilities_t>,
    pub id: Option<zxio::zxio_id_t>,
    pub name: Vec<u8>,
}

pub struct DirentIterator {
    iterator: Box<zxio_dirent_iterator_t>,

    /// Whether the iterator has reached the end of dir entries.
    /// This is necessary because the zxio API returns only once the error code
    /// indicating the iterator has reached the end, where subsequent calls may
    /// return other error codes.
    finished: bool,
}

/// It is important that all methods here are &mut self, to require the client
/// to obtain exclusive access to the object, externally locking it.
impl Iterator for DirentIterator {
    type Item = Result<ZxioDirent, zx::Status>;

    /// Returns the next dir entry for this iterator.
    fn next(&mut self) -> Option<Result<ZxioDirent, zx::Status>> {
        if self.finished {
            return None;
        }
        let mut entry = zxio_dirent_t::default();
        let mut name_buffer = Vec::with_capacity(fio::MAX_FILENAME as usize);
        // The FFI interface expects a pointer to std::os::raw:c_char which is i8 on Fuchsia.
        // The Rust str and OsStr types expect raw character data to be stored in a buffer u8 values.
        // The types are equivalent for all practical purposes and Rust permits casting between the types,
        // so we insert a type cast here in the FFI bindings.
        entry.name = name_buffer.as_mut_ptr() as *mut std::os::raw::c_char;
        let status = unsafe { zxio_dirent_iterator_next(&mut *self.iterator.as_mut(), &mut entry) };
        let result = match zx::ok(status) {
            Ok(()) => {
                let result = ZxioDirent::from(entry, name_buffer);
                Ok(result)
            }
            Err(zx::Status::NOT_FOUND) => {
                self.finished = true;
                return None;
            }
            Err(e) => Err(e),
        };
        return Some(result);
    }
}

impl Drop for DirentIterator {
    fn drop(&mut self) {
        unsafe {
            zxio::zxio_dirent_iterator_destroy(&mut *self.iterator.as_mut());
        };
    }
}

unsafe impl Send for DirentIterator {}
unsafe impl Sync for DirentIterator {}

impl ZxioDirent {
    fn from(dirent: zxio_dirent_t, name_buffer: Vec<u8>) -> ZxioDirent {
        let protocols = if dirent.has.protocols { Some(dirent.protocols) } else { None };
        let abilities = if dirent.has.abilities { Some(dirent.abilities) } else { None };
        let id = if dirent.has.id { Some(dirent.id) } else { None };
        let mut name = name_buffer;
        unsafe { name.set_len(dirent.name_length as usize) };
        ZxioDirent { protocols, abilities, id, name }
    }

    pub fn is_dir(&self) -> bool {
        self.protocols.map(|p| p & ZXIO_NODE_PROTOCOL_DIRECTORY > 0).unwrap_or(false)
    }

    pub fn is_file(&self) -> bool {
        self.protocols.map(|p| p & ZXIO_NODE_PROTOCOL_FILE > 0).unwrap_or(false)
    }
}

pub struct ZxioErrorCode(i16);
impl ZxioErrorCode {
    pub fn raw(&self) -> i16 {
        self.0
    }
}

pub struct RecvMessageInfo {
    pub address: Vec<u8>,
    pub message: Vec<u8>,
    pub message_length: usize,
    pub flags: i32,
}

// `ZxioStorage` is marked as `PhantomPinned` in order to prevent unsafe moves
// of the `zxio_storage_t`, because it may store self-referential types defined
// in zxio.
#[derive(Default)]
struct ZxioStorage {
    storage: zxio::zxio_storage_t,
    _pin: std::marker::PhantomPinned,
}

/// A handle to a zxio object.
///
/// Note: the underlying storage backing the object is pinned on the heap
/// because it can contain self referential data.
pub struct Zxio {
    inner: Pin<Box<ZxioStorage>>,
}

impl Default for Zxio {
    fn default() -> Self {
        Self { inner: Box::pin(ZxioStorage::default()) }
    }
}

/// A trait that provides functionality to connect a channel to a FIDL service.
//
// TODO(https://github.com/rust-lang/rust/issues/44291): allow clients to pass
// in a more general function pointer (`fn(&str, zx::Channel) -> zx::Status`)
// rather than having to implement this trait.
pub trait ServiceConnector {
    /// Connect `server_end` to the protocol available at `service`.
    fn connect(service: &str, server_end: zx::Channel) -> zx::Status;
}

/// Connect a handle to a specified service.
///
/// SAFETY: Dereferences the raw pointers `service_name` and `provider_handle`.
unsafe extern "C" fn service_connector<S: ServiceConnector>(
    service_name: *const std::os::raw::c_char,
    provider_handle: *mut zx_handle_t,
) -> zx_status_t {
    let (client_end, server_end) = zx::Channel::create();

    let service = match CStr::from_ptr(service_name).to_str() {
        Ok(s) => s,
        Err(_) => return ZX_ERR_INVALID_ARGS,
    };

    let status = S::connect(service, server_end).into_raw();
    if status != ZX_OK {
        return status;
    }

    *provider_handle = client_end.into_handle().into_raw();
    ZX_OK
}

/// Sets `out_storage` as the zxio_storage of `out_context`.
///
/// This function is intended to be passed to zxio_socket().
///
/// SAFETY: Dereferences the raw pointer `out_storage`.
unsafe extern "C" fn storage_allocator(
    _type: zxio_object_type_t,
    out_storage: *mut *mut zxio_storage_t,
    out_context: *mut *mut ::std::os::raw::c_void,
) -> zx_status_t {
    let zxio_ptr_ptr = out_context as *mut *mut zxio_storage_t;
    if let Some(zxio_ptr) = zxio_ptr_ptr.as_mut() {
        if let Some(zxio) = zxio_ptr.as_mut() {
            *out_storage = zxio;
            return ZX_OK;
        }
    }
    ZX_ERR_NO_MEMORY
}

impl Zxio {
    pub fn new_socket<S: ServiceConnector>(
        domain: c_int,
        socket_type: c_int,
        protocol: c_int,
    ) -> Result<Result<Self, ZxioErrorCode>, zx::Status> {
        let zxio = Zxio::default();
        let mut out_context = zxio.as_storage_ptr() as *mut c_void;
        let mut out_code = 0;

        let status = unsafe {
            zxio::zxio_socket(
                Some(service_connector::<S>),
                domain,
                socket_type,
                protocol,
                Some(storage_allocator),
                &mut out_context as *mut *mut c_void,
                &mut out_code,
            )
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(zxio)),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    fn as_ptr(&self) -> *mut zxio::zxio_t {
        &self.inner.storage.io as *const zxio::zxio_t as *mut zxio::zxio_t
    }

    fn as_storage_ptr(&self) -> *mut zxio::zxio_storage_t {
        &self.inner.storage as *const zxio::zxio_storage_t as *mut zxio::zxio_storage_t
    }

    pub fn create(handle: zx::Handle) -> Result<Zxio, zx::Status> {
        let zxio = Zxio::default();
        let status = unsafe { zxio::zxio_create(handle.into_raw(), zxio.as_storage_ptr()) };
        zx::ok(status)?;
        Ok(zxio)
    }

    pub fn open(&self, flags: fio::OpenFlags, mode: u32, path: &str) -> Result<Self, zx::Status> {
        let zxio = Zxio::default();
        let status = unsafe {
            zxio::zxio_open(
                self.as_ptr(),
                flags.bits(),
                mode,
                path.as_ptr() as *const ::std::os::raw::c_char,
                path.len(),
                zxio.as_storage_ptr(),
            )
        };
        zx::ok(status)?;
        Ok(zxio)
    }

    pub fn read(&self, data: &mut [u8]) -> Result<usize, zx::Status> {
        let flags = zxio::zxio_flags_t::default();
        let mut actual = 0usize;
        let status = unsafe {
            zxio::zxio_read(
                self.as_ptr(),
                data.as_ptr() as *mut ::std::os::raw::c_void,
                data.len(),
                flags,
                &mut actual,
            )
        };
        zx::ok(status)?;
        Ok(actual)
    }

    pub fn clone(&self) -> Result<Zxio, zx::Status> {
        let mut handle = 0;
        let status = unsafe { zxio::zxio_clone(self.as_ptr(), &mut handle) };
        zx::ok(status)?;
        unsafe { Zxio::create(zx::Handle::from_raw(handle)) }
    }

    pub fn read_at(&self, offset: u64, data: &mut [u8]) -> Result<usize, zx::Status> {
        let flags = zxio::zxio_flags_t::default();
        let mut actual = 0usize;
        let status = unsafe {
            zxio::zxio_read_at(
                self.as_ptr(),
                offset,
                data.as_ptr() as *mut ::std::os::raw::c_void,
                data.len(),
                flags,
                &mut actual,
            )
        };
        zx::ok(status)?;
        Ok(actual)
    }

    pub fn write(&self, data: &[u8]) -> Result<usize, zx::Status> {
        let flags = zxio::zxio_flags_t::default();
        let mut actual = 0;
        let status = unsafe {
            zxio::zxio_write(
                self.as_ptr(),
                data.as_ptr() as *const ::std::os::raw::c_void,
                data.len(),
                flags,
                &mut actual,
            )
        };
        zx::ok(status)?;
        Ok(actual)
    }

    pub fn write_at(&self, offset: u64, data: &[u8]) -> Result<usize, zx::Status> {
        let flags = zxio::zxio_flags_t::default();
        let mut actual = 0;
        let status = unsafe {
            zxio::zxio_write_at(
                self.as_ptr(),
                offset,
                data.as_ptr() as *const ::std::os::raw::c_void,
                data.len(),
                flags,
                &mut actual,
            )
        };
        zx::ok(status)?;
        Ok(actual)
    }

    pub fn truncate(&self, length: u64) -> Result<(), zx::Status> {
        let status = unsafe { zxio::zxio_truncate(self.as_ptr(), length) };
        zx::ok(status)?;
        Ok(())
    }

    pub fn vmo_get(&self, flags: zx::VmarFlags) -> Result<zx::Vmo, zx::Status> {
        let mut vmo = 0;
        let status = unsafe { zxio::zxio_vmo_get(self.as_ptr(), flags.bits(), &mut vmo) };
        zx::ok(status)?;
        let handle = unsafe { zx::Handle::from_raw(vmo) };
        Ok(zx::Vmo::from(handle))
    }

    pub fn attr_get(&self) -> Result<zxio_node_attributes_t, zx::Status> {
        let mut attributes = zxio_node_attributes_t::default();
        let status = unsafe { zxio::zxio_attr_get(self.as_ptr(), &mut attributes) };
        zx::ok(status)?;
        Ok(attributes)
    }

    pub fn wait_begin(
        &self,
        zxio_signals: zxio_signals_t,
    ) -> (zx::Unowned<'_, zx::Handle>, zx::Signals) {
        let mut handle = zx::sys::ZX_HANDLE_INVALID;
        let mut zx_signals = zx::sys::ZX_SIGNAL_NONE;
        unsafe { zxio::zxio_wait_begin(self.as_ptr(), zxio_signals, &mut handle, &mut zx_signals) };
        let handle = unsafe { zx::Unowned::<zx::Handle>::from_raw_handle(handle) };
        let signals = zx::Signals::from_bits_truncate(zx_signals);
        (handle, signals)
    }

    pub fn wait_end(&self, signals: zx::Signals) -> zxio_signals_t {
        let mut zxio_signals = ZxioSignals::NONE.bits();
        unsafe {
            zxio::zxio_wait_end(self.as_ptr(), signals.bits(), &mut zxio_signals);
        }
        zxio_signals
    }

    pub fn create_dirent_iterator(&self) -> Result<DirentIterator, zx::Status> {
        let mut zxio_iterator = Box::default();
        let status = unsafe { zxio::zxio_dirent_iterator_init(&mut *zxio_iterator, self.as_ptr()) };
        let iterator = DirentIterator { iterator: zxio_iterator, finished: false };
        zx::ok(status)?;
        Ok(iterator)
    }

    pub fn connect(&self, addr: &[u8]) -> Result<Result<(), ZxioErrorCode>, zx::Status> {
        let mut out_code = 0;
        let status = unsafe {
            zxio::zxio_connect(
                self.as_ptr(),
                addr.as_ptr() as *const sockaddr,
                addr.len() as socklen_t,
                &mut out_code,
            )
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(())),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn bind(&self, addr: &[u8]) -> Result<Result<(), ZxioErrorCode>, zx::Status> {
        let mut out_code = 0;
        let status = unsafe {
            zxio::zxio_bind(
                self.as_ptr(),
                addr.as_ptr() as *const sockaddr,
                addr.len() as socklen_t,
                &mut out_code,
            )
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(())),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn listen(&self, backlog: i32) -> Result<Result<(), ZxioErrorCode>, zx::Status> {
        let mut out_code = 0;
        let status = unsafe {
            zxio::zxio_listen(self.as_ptr(), backlog as std::os::raw::c_int, &mut out_code)
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(())),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn accept(&self) -> Result<Result<Zxio, ZxioErrorCode>, zx::Status> {
        let mut addrlen = std::mem::size_of::<sockaddr_storage>() as socklen_t;
        let mut addr = vec![0u8; addrlen as usize];
        let zxio = Zxio::default();
        let mut out_code = 0;
        let status = unsafe {
            zxio::zxio_accept(
                self.as_ptr(),
                addr.as_mut_ptr() as *mut sockaddr,
                &mut addrlen,
                zxio.as_storage_ptr(),
                &mut out_code,
            )
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(zxio)),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn getsockname(&self) -> Result<Result<Vec<u8>, ZxioErrorCode>, zx::Status> {
        let mut addrlen = std::mem::size_of::<sockaddr_storage>() as socklen_t;
        let mut addr = vec![0u8; addrlen as usize];
        let mut out_code = 0;
        let status = unsafe {
            zxio::zxio_getsockname(
                self.as_ptr(),
                addr.as_mut_ptr() as *mut sockaddr,
                &mut addrlen,
                &mut out_code,
            )
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(addr[..addrlen as usize].to_vec())),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn getpeername(&self) -> Result<Result<Vec<u8>, ZxioErrorCode>, zx::Status> {
        let mut addrlen = std::mem::size_of::<sockaddr_storage>() as socklen_t;
        let mut addr = vec![0u8; addrlen as usize];
        let mut out_code = 0;
        let status = unsafe {
            zxio::zxio_getpeername(
                self.as_ptr(),
                addr.as_mut_ptr() as *mut sockaddr,
                &mut addrlen,
                &mut out_code,
            )
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(addr[..addrlen as usize].to_vec())),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn getsockopt(
        &self,
        level: u32,
        optname: u32,
        mut optlen: socklen_t,
    ) -> Result<Result<Vec<u8>, ZxioErrorCode>, zx::Status> {
        let mut optval = vec![0u8; optlen as usize];
        let mut out_code = 0;
        let status = unsafe {
            zxio::zxio_getsockopt(
                self.as_ptr(),
                level as std::os::raw::c_int,
                optname as std::os::raw::c_int,
                optval.as_mut_ptr() as *mut std::os::raw::c_void,
                &mut optlen,
                &mut out_code,
            )
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(optval[..optlen as usize].to_vec())),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn setsockopt(
        &self,
        level: i32,
        optname: i32,
        optval: &[u8],
    ) -> Result<Result<(), ZxioErrorCode>, zx::Status> {
        let mut out_code = 0;
        let status = unsafe {
            zxio::zxio_setsockopt(
                self.as_ptr(),
                level,
                optname,
                optval.as_ptr() as *const std::os::raw::c_void,
                optval.len() as socklen_t,
                &mut out_code,
            )
        };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(())),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn shutdown(
        &self,
        flags: ZxioShutdownFlags,
    ) -> Result<Result<(), ZxioErrorCode>, zx::Status> {
        let mut out_code = 0;
        let status = unsafe { zxio::zxio_shutdown(self.as_ptr(), flags.bits(), &mut out_code) };
        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(())),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn sendmsg(
        &self,
        mut addr: Vec<u8>,
        mut buffer: Vec<u8>,
        mut cmsg: Vec<u8>,
        flags: u32,
    ) -> Result<Result<usize, ZxioErrorCode>, zx::Status> {
        let mut msg = zxio::msghdr::default();
        msg.msg_name = match addr.len() {
            0 => std::ptr::null_mut() as *mut std::os::raw::c_void,
            _ => addr.as_mut_ptr() as *mut std::os::raw::c_void,
        };
        msg.msg_namelen = addr.len() as u32;

        let mut iov = zxio::iovec {
            iov_base: buffer.as_mut_ptr() as *mut std::os::raw::c_void,
            iov_len: buffer.len(),
        };
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;

        msg.msg_control = cmsg.as_mut_ptr() as *mut std::os::raw::c_void;
        msg.msg_controllen = cmsg.len();

        let mut out_code = 0;
        let mut out_actual = 0;

        let status = unsafe {
            zxio::zxio_sendmsg(
                self.as_ptr(),
                &msg,
                flags as std::os::raw::c_int,
                &mut out_actual,
                &mut out_code,
            )
        };

        zx::ok(status)?;
        match out_code {
            0 => Ok(Ok(out_actual)),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }

    pub fn recvmsg(
        &self,
        iovec_length: usize,
        flags: u32,
    ) -> Result<Result<RecvMessageInfo, ZxioErrorCode>, zx::Status> {
        let mut msg = msghdr::default();
        let mut addr = vec![0u8; std::mem::size_of::<sockaddr_storage>()];
        msg.msg_name = addr.as_mut_ptr() as *mut std::os::raw::c_void;
        msg.msg_namelen = addr.len() as u32;

        let mut iov_buf = vec![0u8; iovec_length];
        let mut iov = zxio::iovec {
            iov_base: iov_buf.as_mut_ptr() as *mut std::os::raw::c_void,
            iov_len: iovec_length,
        };
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;

        let mut out_code = 0;
        let mut out_actual = 0;
        let status = unsafe {
            zxio::zxio_recvmsg(
                self.as_ptr(),
                &mut msg,
                flags as std::os::raw::c_int,
                &mut out_actual,
                &mut out_code,
            )
        };
        zx::ok(status)?;

        let min_buf_len = std::cmp::min(iov_buf.len(), out_actual);
        match out_code {
            0 => Ok(Ok(RecvMessageInfo {
                address: addr[..msg.msg_namelen as usize].to_vec(),
                message: iov_buf[..min_buf_len].to_vec(),
                message_length: out_actual,
                flags: msg.msg_flags,
            })),
            _ => Ok(Err(ZxioErrorCode(out_code))),
        }
    }
}

impl Drop for Zxio {
    fn drop(&mut self) {
        unsafe {
            zxio::zxio_close_new_transitional(self.as_ptr(), true);
        };
    }
}

enum NodeKind {
    File,
    Directory,
    Unknown,
}

impl NodeKind {
    fn from(info: &fio::NodeInfoDeprecated) -> NodeKind {
        match info {
            fio::NodeInfoDeprecated::File(_) => NodeKind::File,
            fio::NodeInfoDeprecated::Directory(_) => NodeKind::Directory,
            _ => NodeKind::Unknown,
        }
    }

    fn from2(representation: &fio::Representation) -> NodeKind {
        match representation {
            fio::Representation::File(_) => NodeKind::File,
            fio::Representation::Directory(_) => NodeKind::Directory,
            _ => NodeKind::Unknown,
        }
    }
}

/// A fuchsia.io.Node along with its NodeInfoDeprecated.
///
/// The NodeInfoDeprecated provides information about the concrete protocol spoken by the
/// node.
struct DescribedNode {
    node: fio::NodeSynchronousProxy,
    kind: NodeKind,
}

/// Open the given path in the given directory.
///
/// The semantics for the flags and mode arguments are defined by the
/// fuchsia.io/Directory.Open message.
///
/// This function adds OPEN_FLAG_DESCRIBE to the given flags and then blocks
/// until the directory describes the newly opened node.
///
/// Returns the opened Node, along with its NodeInfoDeprecated, or an error.
fn directory_open(
    directory: &fio::DirectorySynchronousProxy,
    path: &str,
    flags: fio::OpenFlags,
    mode: u32,
    deadline: zx::Time,
) -> Result<DescribedNode, zx::Status> {
    let flags = flags | fio::OpenFlags::DESCRIBE;

    let (client_end, server_end) = zx::Channel::create();
    directory.open(flags, mode, path, ServerEnd::new(server_end)).map_err(|_| zx::Status::IO)?;
    let node = fio::NodeSynchronousProxy::new(client_end);

    match node.wait_for_event(deadline).map_err(|_| zx::Status::IO)? {
        fio::NodeEvent::OnOpen_ { s: status, info } => {
            zx::Status::ok(status)?;
            Ok(DescribedNode { node, kind: NodeKind::from(&*info.ok_or(zx::Status::IO)?) })
        }
        fio::NodeEvent::OnRepresentation { payload } => {
            Ok(DescribedNode { node, kind: NodeKind::from2(&payload) })
        }
    }
}

/// Open a VMO at the given path in the given directory.
///
/// The semantics for the vmo_flags argument are defined by the
/// fuchsia.io/File.GetBackingMemory message (i.e., VmoFlags::*).
///
/// If the node at the given path is not a VMO, then this function returns
/// a zx::Status::IO error.
pub fn directory_open_vmo(
    directory: &fio::DirectorySynchronousProxy,
    path: &str,
    vmo_flags: fio::VmoFlags,
    deadline: zx::Time,
) -> Result<zx::Vmo, zx::Status> {
    let mut open_flags = fio::OpenFlags::empty();
    if vmo_flags.contains(fio::VmoFlags::WRITE) {
        open_flags |= fio::OpenFlags::RIGHT_WRITABLE;
    }
    if vmo_flags.contains(fio::VmoFlags::READ) {
        open_flags |= fio::OpenFlags::RIGHT_READABLE;
    }
    if vmo_flags.contains(fio::VmoFlags::EXECUTE) {
        open_flags |= fio::OpenFlags::RIGHT_EXECUTABLE;
    }

    let description = directory_open(directory, path, open_flags, 0, deadline)?;
    let file = match description.kind {
        NodeKind::File => fio::FileSynchronousProxy::new(description.node.into_channel()),
        _ => return Err(zx::Status::IO),
    };

    let vmo = file
        .get_backing_memory(vmo_flags, deadline)
        .map_err(|_: fidl::Error| zx::Status::IO)?
        .map_err(zx::Status::from_raw)?;
    Ok(vmo)
}

/// Read the content of the file at the given path in the given directory.
///
/// If the node at the given path is not a file, then this function returns
/// a zx::Status::IO error.
pub fn directory_read_file(
    directory: &fio::DirectorySynchronousProxy,
    path: &str,
    deadline: zx::Time,
) -> Result<Vec<u8>, zx::Status> {
    let description = directory_open(directory, path, fio::OpenFlags::RIGHT_READABLE, 0, deadline)?;
    let file = match description.kind {
        NodeKind::File => fio::FileSynchronousProxy::new(description.node.into_channel()),
        _ => return Err(zx::Status::IO),
    };

    let mut result = Vec::new();
    loop {
        let mut data = file
            .read(fio::MAX_TRANSFER_SIZE, deadline)
            .map_err(|_: fidl::Error| zx::Status::IO)?
            .map_err(zx::Status::from_raw)?;
        let finished = (data.len() as u64) < fio::MAX_TRANSFER_SIZE;
        result.append(&mut data);
        if finished {
            return Ok(result);
        }
    }
}

/// Open the given path in the given directory without blocking.
///
/// A zx::Channel to the opened node is returned (or an error).
///
/// It is an error to supply the OPEN_FLAG_DESCRIBE flag in flags.
///
/// This function will "succeed" even if the given path does not exist in the
/// given directory because this function does not wait for the directory to
/// confirm that the path exists.
pub fn directory_open_async(
    directory: &fio::DirectorySynchronousProxy,
    path: &str,
    flags: fio::OpenFlags,
    mode: u32,
) -> Result<zx::Channel, zx::Status> {
    if flags.intersects(fio::OpenFlags::DESCRIBE) {
        return Err(zx::Status::INVALID_ARGS);
    }

    let (client_end, server_end) = zx::Channel::create();
    directory.open(flags, mode, path, ServerEnd::new(server_end)).map_err(|_| zx::Status::IO)?;
    Ok(client_end)
}

/// Open a directory at the given path in the given directory without blocking.
///
/// This function adds the OPEN_FLAG_DIRECTORY flag and uses the
/// MODE_TYPE_DIRECTORY mode to ensure that the open operation completes only
/// if the given path is actually a directory, which means clients can start
/// using the returned DirectorySynchronousProxy immediately without waiting
/// for the server to complete the operation.
///
/// This function will "succeed" even if the given path does not exist in the
/// given directory or if the path is not a directory because this function
/// does not wait for the directory to confirm that the path exists and is a
/// directory.
pub fn directory_open_directory_async(
    directory: &fio::DirectorySynchronousProxy,
    path: &str,
    flags: fio::OpenFlags,
) -> Result<fio::DirectorySynchronousProxy, zx::Status> {
    let flags = flags | fio::OpenFlags::DIRECTORY;
    let mode = fio::MODE_TYPE_DIRECTORY;
    let client = directory_open_async(directory, path, flags, mode)?;
    Ok(fio::DirectorySynchronousProxy::new(client))
}

pub fn directory_clone(
    directory: &fio::DirectorySynchronousProxy,
    flags: fio::OpenFlags,
) -> Result<fio::DirectorySynchronousProxy, zx::Status> {
    let (client_end, server_end) = zx::Channel::create();
    directory.clone(flags, ServerEnd::new(server_end)).map_err(|_| zx::Status::IO)?;
    Ok(fio::DirectorySynchronousProxy::new(client_end))
}

pub fn file_clone(
    file: &fio::FileSynchronousProxy,
    flags: fio::OpenFlags,
) -> Result<fio::FileSynchronousProxy, zx::Status> {
    let (client_end, server_end) = zx::Channel::create();
    file.clone(flags, ServerEnd::new(server_end)).map_err(|_| zx::Status::IO)?;
    Ok(fio::FileSynchronousProxy::new(client_end))
}

#[cfg(test)]
mod test {
    use super::*;

    use anyhow::Error;
    use fidl::endpoints::Proxy;
    use fidl_fuchsia_io as fio;
    use fuchsia_async as fasync;
    use fuchsia_fs::directory;
    use fuchsia_zircon::{AsHandleRef, HandleBased};

    fn open_pkg() -> fio::DirectorySynchronousProxy {
        let pkg_proxy = directory::open_in_namespace(
            "/pkg",
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE,
        )
        .expect("failed to open /pkg");
        fio::DirectorySynchronousProxy::new(
            pkg_proxy
                .into_channel()
                .expect("failed to convert proxy into channel")
                .into_zx_channel(),
        )
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_directory_open() -> Result<(), Error> {
        let pkg = open_pkg();
        let description = directory_open(
            &pkg,
            "bin/syncio_lib_test",
            fio::OpenFlags::RIGHT_READABLE,
            0,
            zx::Time::INFINITE,
        )?;
        assert!(match description.kind {
            NodeKind::File => true,
            _ => false,
        });
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_directory_open_vmo() -> Result<(), Error> {
        let pkg = open_pkg();
        let vmo = directory_open_vmo(
            &pkg,
            "bin/syncio_lib_test",
            fio::VmoFlags::READ | fio::VmoFlags::EXECUTE,
            zx::Time::INFINITE,
        )?;
        assert!(!vmo.is_invalid_handle());

        let info = vmo.basic_info()?;
        assert_eq!(zx::Rights::READ, info.rights & zx::Rights::READ);
        assert_eq!(zx::Rights::EXECUTE, info.rights & zx::Rights::EXECUTE);
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_directory_read_file() -> Result<(), Error> {
        let pkg = open_pkg();
        let data = directory_read_file(&pkg, "bin/syncio_lib_test", zx::Time::INFINITE)?;

        assert!(!data.is_empty());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_directory_open_directory_async() -> Result<(), Error> {
        let pkg = open_pkg();
        let bin = directory_open_directory_async(
            &pkg,
            "bin",
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE,
        )?;
        let vmo = directory_open_vmo(
            &bin,
            "syncio_lib_test",
            fio::VmoFlags::READ | fio::VmoFlags::EXECUTE,
            zx::Time::INFINITE,
        )?;
        assert!(!vmo.is_invalid_handle());

        let info = vmo.basic_info()?;
        assert_eq!(zx::Rights::READ, info.rights & zx::Rights::READ);
        assert_eq!(zx::Rights::EXECUTE, info.rights & zx::Rights::EXECUTE);
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_directory_open_zxio_async() -> Result<(), Error> {
        let pkg_proxy = directory::open_in_namespace(
            "/pkg",
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE,
        )
        .expect("failed to open /pkg");
        let zx_channel = pkg_proxy
            .into_channel()
            .expect("failed to convert proxy into channel")
            .into_zx_channel();
        let storage = zxio::zxio_storage_t::default();
        let status = unsafe {
            zxio::zxio_create(
                zx_channel.into_raw(),
                &storage as *const zxio::zxio_storage_t as *mut zxio::zxio_storage_t,
            )
        };
        assert_eq!(status, zx::sys::ZX_OK);
        let io = &storage.io as *const zxio::zxio_t as *mut zxio::zxio_t;
        let close_status = unsafe { zxio::zxio_close_new_transitional(io, true) };
        assert_eq!(close_status, zx::sys::ZX_OK);
        Ok(())
    }

    #[fuchsia::test]
    async fn test_directory_enumerate() -> Result<(), Error> {
        let pkg_dir_handle = directory::open_in_namespace(
            "/pkg",
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE,
        )
        .expect("failed to open /pkg")
        .into_channel()
        .expect("could not unwrap channel")
        .into_zx_channel()
        .into();

        let io: Zxio = Zxio::create(pkg_dir_handle)?;
        let iter = io.create_dirent_iterator().expect("failed to create iterator");
        let expected_dir_names = vec![".", "bin", "lib", "meta"];
        let mut found_dir_names = iter
            .map(|e| {
                let dirent = e.expect("dirent");
                assert!(dirent.is_dir());
                std::str::from_utf8(&dirent.name).expect("name was not valid utf8").to_string()
            })
            .collect::<Vec<_>>();
        found_dir_names.sort();
        assert_eq!(expected_dir_names, found_dir_names);

        // Check all entry inside bin are either "." or a file
        let bin_io = io
            .open(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE, 0, "bin")
            .expect("open");
        for entry in bin_io.create_dirent_iterator().expect("failed to create iterator") {
            let dirent = entry.expect("dirent");
            if dirent.name == b"." {
                assert!(dirent.is_dir());
            } else {
                assert!(dirent.is_file());
            }
        }

        Ok(())
    }

    #[fuchsia::test]
    fn test_storage_allocator() {
        let mut out_storage = zxio_storage_t::default();
        let mut out_storage_ptr = &mut out_storage as *mut zxio_storage_t;

        let mut out_context = Zxio::default();
        let mut out_context_ptr = &mut out_context as *mut Zxio;

        let out = unsafe {
            storage_allocator(
                0 as zxio_object_type_t,
                &mut out_storage_ptr as *mut *mut zxio_storage_t,
                &mut out_context_ptr as *mut *mut Zxio as *mut *mut c_void,
            )
        };
        assert_eq!(out, ZX_OK);
    }

    #[fuchsia::test]
    fn test_storage_allocator_bad_context() {
        let mut out_storage = zxio_storage_t::default();
        let mut out_storage_ptr = &mut out_storage as *mut zxio_storage_t;

        let out_context = std::ptr::null_mut();

        let out = unsafe {
            storage_allocator(
                0 as zxio_object_type_t,
                &mut out_storage_ptr as *mut *mut zxio_storage_t,
                out_context,
            )
        };
        assert_eq!(out, ZX_ERR_NO_MEMORY);
    }
}
