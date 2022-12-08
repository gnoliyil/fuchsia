/* automatically generated by rust-bindgen 0.60.1 */

// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(non_camel_case_types)]
#![allow(dead_code)]

pub const EPERM: u32 = 1;
pub const ENOENT: u32 = 2;
pub const ESRCH: u32 = 3;
pub const EINTR: u32 = 4;
pub const EIO: u32 = 5;
pub const ENXIO: u32 = 6;
pub const ENOEXEC: u32 = 8;
pub const EBADF: u32 = 9;
pub const ECHILD: u32 = 10;
pub const EAGAIN: u32 = 11;
pub const ENOMEM: u32 = 12;
pub const EACCES: u32 = 13;
pub const EFAULT: u32 = 14;
pub const ENOTBLK: u32 = 15;
pub const EBUSY: u32 = 16;
pub const EEXIST: u32 = 17;
pub const EXDEV: u32 = 18;
pub const ENODEV: u32 = 19;
pub const ENOTDIR: u32 = 20;
pub const EISDIR: u32 = 21;
pub const EINVAL: u32 = 22;
pub const ENFILE: u32 = 23;
pub const EMFILE: u32 = 24;
pub const ENOTTY: u32 = 25;
pub const ETXTBSY: u32 = 26;
pub const EFBIG: u32 = 27;
pub const ENOSPC: u32 = 28;
pub const ESPIPE: u32 = 29;
pub const EROFS: u32 = 30;
pub const EMLINK: u32 = 31;
pub const EPIPE: u32 = 32;
pub const EDOM: u32 = 33;
pub const ERANGE: u32 = 34;
pub const EDEADLK: u32 = 35;
pub const ENAMETOOLONG: u32 = 36;
pub const ENOLCK: u32 = 37;
pub const ENOSYS: u32 = 38;
pub const ENOTEMPTY: u32 = 39;
pub const ELOOP: u32 = 40;
pub const EWOULDBLOCK: u32 = 11;
pub const ENOMSG: u32 = 42;
pub const EIDRM: u32 = 43;
pub const ECHRNG: u32 = 44;
pub const ELNRNG: u32 = 48;
pub const EUNATCH: u32 = 49;
pub const ENOCSI: u32 = 50;
pub const EBADE: u32 = 52;
pub const EBADR: u32 = 53;
pub const EXFULL: u32 = 54;
pub const ENOANO: u32 = 55;
pub const EBADRQC: u32 = 56;
pub const EBADSLT: u32 = 57;
pub const EDEADLOCK: u32 = 35;
pub const EBFONT: u32 = 59;
pub const ENOSTR: u32 = 60;
pub const ENODATA: u32 = 61;
pub const ETIME: u32 = 62;
pub const ENOSR: u32 = 63;
pub const ENONET: u32 = 64;
pub const ENOPKG: u32 = 65;
pub const EREMOTE: u32 = 66;
pub const ENOLINK: u32 = 67;
pub const EADV: u32 = 68;
pub const ESRMNT: u32 = 69;
pub const ECOMM: u32 = 70;
pub const EPROTO: u32 = 71;
pub const EMULTIHOP: u32 = 72;
pub const EDOTDOT: u32 = 73;
pub const EBADMSG: u32 = 74;
pub const EOVERFLOW: u32 = 75;
pub const ENOTUNIQ: u32 = 76;
pub const EBADFD: u32 = 77;
pub const EREMCHG: u32 = 78;
pub const ELIBACC: u32 = 79;
pub const ELIBBAD: u32 = 80;
pub const ELIBSCN: u32 = 81;
pub const ELIBMAX: u32 = 82;
pub const ELIBEXEC: u32 = 83;
pub const EILSEQ: u32 = 84;
pub const ERESTART: u32 = 85;
pub const ESTRPIPE: u32 = 86;
pub const EUSERS: u32 = 87;
pub const ENOTSOCK: u32 = 88;
pub const EDESTADDRREQ: u32 = 89;
pub const EMSGSIZE: u32 = 90;
pub const EPROTOTYPE: u32 = 91;
pub const ENOPROTOOPT: u32 = 92;
pub const EPROTONOSUPPORT: u32 = 93;
pub const ESOCKTNOSUPPORT: u32 = 94;
pub const EOPNOTSUPP: u32 = 95;
pub const EPFNOSUPPORT: u32 = 96;
pub const EAFNOSUPPORT: u32 = 97;
pub const EADDRINUSE: u32 = 98;
pub const EADDRNOTAVAIL: u32 = 99;
pub const ENETDOWN: u32 = 100;
pub const ENETUNREACH: u32 = 101;
pub const ENETRESET: u32 = 102;
pub const ECONNABORTED: u32 = 103;
pub const ECONNRESET: u32 = 104;
pub const ENOBUFS: u32 = 105;
pub const EISCONN: u32 = 106;
pub const ENOTCONN: u32 = 107;
pub const ESHUTDOWN: u32 = 108;
pub const ETOOMANYREFS: u32 = 109;
pub const ETIMEDOUT: u32 = 110;
pub const ECONNREFUSED: u32 = 111;
pub const EHOSTDOWN: u32 = 112;
pub const EHOSTUNREACH: u32 = 113;
pub const EALREADY: u32 = 114;
pub const EINPROGRESS: u32 = 115;
pub const ESTALE: u32 = 116;
pub const EUCLEAN: u32 = 117;
pub const ENOTNAM: u32 = 118;
pub const ENAVAIL: u32 = 119;
pub const EISNAM: u32 = 120;
pub const EREMOTEIO: u32 = 121;
pub const EDQUOT: u32 = 122;
pub const ENOMEDIUM: u32 = 123;
pub const EMEDIUMTYPE: u32 = 124;
pub const ECANCELED: u32 = 125;
pub const ENOKEY: u32 = 126;
pub const EKEYEXPIRED: u32 = 127;
pub const EKEYREVOKED: u32 = 128;
pub const EKEYREJECTED: u32 = 129;
pub const EOWNERDEAD: u32 = 130;
pub const ENOTRECOVERABLE: u32 = 131;
pub const ERFKILL: u32 = 132;
pub const EHWPOISON: u32 = 133;
pub const ENOTSUP: u32 = 95;
pub type __uint8_t = ::std::os::raw::c_uchar;
pub type __int16_t = ::std::os::raw::c_short;
pub type __int32_t = ::std::os::raw::c_int;
pub type __uint32_t = ::std::os::raw::c_uint;
pub type __int64_t = ::std::os::raw::c_long;
pub type __uint64_t = ::std::os::raw::c_ulong;
pub type __socklen_t = ::std::os::raw::c_uint;
pub type zx_rights_t = u32;
pub type zx_time_t = i64;
pub type zx_handle_t = u32;
pub type zx_status_t = i32;
pub type zx_signals_t = u32;
pub type zx_koid_t = u64;
pub type zx_off_t = u64;
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zx_iovec {
    pub buffer: *mut ::std::os::raw::c_void,
    pub capacity: usize,
}
impl Default for zx_iovec {
    fn default() -> Self {
        let mut s = ::std::mem::MaybeUninit::<Self>::uninit();
        unsafe {
            ::std::ptr::write_bytes(s.as_mut_ptr(), 0, 1);
            s.assume_init()
        }
    }
}
pub type zx_iovec_t = zx_iovec;
pub type zx_obj_type_t = u32;
pub type zxio_flags_t = u32;
pub type zxio_vmo_flags_t = u32;
pub type zxio_signals_t = u32;
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct zxio_tag {
    pub reserved: [u64; 4usize],
}
pub type zxio_t = zxio_tag;
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct zxio_private {
    pub reserved: [u64; 29usize],
}
pub type zxio_private_t = zxio_private;
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct zxio_storage {
    pub io: zxio_t,
    pub reserved: zxio_private_t,
}
pub type zxio_storage_t = zxio_storage;
pub type zxio_object_type_t = u32;
pub type zxio_storage_alloc = ::std::option::Option<
    unsafe extern "C" fn(
        type_: zxio_object_type_t,
        out_storage: *mut *mut zxio_storage_t,
        out_context: *mut *mut ::std::os::raw::c_void,
    ) -> zx_status_t,
>;
pub type zxio_node_protocols_t = u64;
pub type zxio_id_t = u64;
pub type zxio_operations_t = u64;
pub type zxio_abilities_t = zxio_operations_t;
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct zxio_node_attr {
    pub protocols: zxio_node_protocols_t,
    pub abilities: zxio_abilities_t,
    pub id: zxio_id_t,
    pub content_size: u64,
    pub storage_size: u64,
    pub link_count: u64,
    pub creation_time: u64,
    pub modification_time: u64,
    pub has: zxio_node_attr_zxio_node_attr_has_t,
}
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct zxio_node_attr_zxio_node_attr_has_t {
    pub protocols: bool,
    pub abilities: bool,
    pub id: bool,
    pub content_size: bool,
    pub storage_size: bool,
    pub link_count: bool,
    pub creation_time: bool,
    pub modification_time: bool,
}
pub type zxio_node_attributes_t = zxio_node_attr;
pub type zxio_seek_origin_t = u32;
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zxio_dirent_iterator {
    pub io: *mut zxio_t,
    pub opaque: [u8; 65584usize],
}
impl Default for zxio_dirent_iterator {
    fn default() -> Self {
        let mut s = ::std::mem::MaybeUninit::<Self>::uninit();
        unsafe {
            ::std::ptr::write_bytes(s.as_mut_ptr(), 0, 1);
            s.assume_init()
        }
    }
}
pub type zxio_dirent_iterator_t = zxio_dirent_iterator;
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct zxio_dirent {
    pub protocols: zxio_node_protocols_t,
    pub abilities: zxio_abilities_t,
    pub id: zxio_id_t,
    pub has: zxio_dirent_zxio_dirent_has_t,
    pub name_length: u8,
    pub name: *mut ::std::os::raw::c_char,
}
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct zxio_dirent_zxio_dirent_has_t {
    pub protocols: bool,
    pub abilities: bool,
    pub id: bool,
}
impl Default for zxio_dirent {
    fn default() -> Self {
        let mut s = ::std::mem::MaybeUninit::<Self>::uninit();
        unsafe {
            ::std::ptr::write_bytes(s.as_mut_ptr(), 0, 1);
            s.assume_init()
        }
    }
}
pub type zxio_dirent_t = zxio_dirent;
pub type zxio_shutdown_options_t = u32;
pub type va_list = __builtin_va_list;
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct zx_info_handle_basic {
    pub koid: zx_koid_t,
    pub rights: zx_rights_t,
    pub type_: zx_obj_type_t,
    pub related_koid: zx_koid_t,
    pub reserved: u32,
    pub padding1: [u8; 4usize],
}
pub type zx_info_handle_basic_t = zx_info_handle_basic;
extern "C" {
    pub fn zxio_create(handle: zx_handle_t, storage: *mut zxio_storage_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_create_with_on_open(
        handle: zx_handle_t,
        storage: *mut zxio_storage_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_create_with_info(
        handle: zx_handle_t,
        handle_info: *const zx_info_handle_basic_t,
        storage: *mut zxio_storage_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_create_with_type(
        storage: *mut zxio_storage_t,
        type_: zxio_object_type_t,
        ...
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_close(io: *mut zxio_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_release(io: *mut zxio_t, out_handle: *mut zx_handle_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_borrow(io: *mut zxio_t, out_handle: *mut zx_handle_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_clone(io: *mut zxio_t, out_handle: *mut zx_handle_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_wait_one(
        io: *mut zxio_t,
        signals: zxio_signals_t,
        deadline: zx_time_t,
        out_observed: *mut zxio_signals_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_wait_begin(
        io: *mut zxio_t,
        zxio_signals: zxio_signals_t,
        out_handle: *mut zx_handle_t,
        out_zx_signals: *mut zx_signals_t,
    );
}
extern "C" {
    pub fn zxio_wait_end(
        io: *mut zxio_t,
        zx_signals: zx_signals_t,
        out_zxio_signals: *mut zxio_signals_t,
    );
}
extern "C" {
    pub fn zxio_sync(io: *mut zxio_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_attr_get(io: *mut zxio_t, out_attr: *mut zxio_node_attributes_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_attr_set(io: *mut zxio_t, attr: *const zxio_node_attributes_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_read(
        io: *mut zxio_t,
        buffer: *mut ::std::os::raw::c_void,
        capacity: usize,
        flags: zxio_flags_t,
        out_actual: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_read_at(
        io: *mut zxio_t,
        offset: zx_off_t,
        buffer: *mut ::std::os::raw::c_void,
        capacity: usize,
        flags: zxio_flags_t,
        out_actual: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_write(
        io: *mut zxio_t,
        buffer: *const ::std::os::raw::c_void,
        capacity: usize,
        flags: zxio_flags_t,
        out_actual: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_write_at(
        io: *mut zxio_t,
        offset: zx_off_t,
        buffer: *const ::std::os::raw::c_void,
        capacity: usize,
        flags: zxio_flags_t,
        out_actual: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_readv(
        io: *mut zxio_t,
        vector: *const zx_iovec_t,
        vector_count: usize,
        flags: zxio_flags_t,
        out_actual: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_readv_at(
        io: *mut zxio_t,
        offset: zx_off_t,
        vector: *const zx_iovec_t,
        vector_count: usize,
        flags: zxio_flags_t,
        out_actual: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_writev(
        io: *mut zxio_t,
        vector: *const zx_iovec_t,
        vector_count: usize,
        flags: zxio_flags_t,
        out_actual: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_writev_at(
        io: *mut zxio_t,
        offset: zx_off_t,
        vector: *const zx_iovec_t,
        vector_count: usize,
        flags: zxio_flags_t,
        out_actual: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_seek(
        io: *mut zxio_t,
        start: zxio_seek_origin_t,
        offset: i64,
        out_offset: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_truncate(io: *mut zxio_t, length: u64) -> zx_status_t;
}
extern "C" {
    pub fn zxio_flags_get(io: *mut zxio_t, out_flags: *mut u32) -> zx_status_t;
}
extern "C" {
    pub fn zxio_flags_set(io: *mut zxio_t, flags: u32) -> zx_status_t;
}
extern "C" {
    pub fn zxio_token_get(io: *mut zxio_t, out_token: *mut zx_handle_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_vmo_get(
        io: *mut zxio_t,
        flags: zxio_vmo_flags_t,
        out_vmo: *mut zx_handle_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_on_mapped(io: *mut zxio_t, ptr: *mut ::std::os::raw::c_void) -> zx_status_t;
}
extern "C" {
    pub fn zxio_vmo_get_copy(io: *mut zxio_t, out_vmo: *mut zx_handle_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_vmo_get_clone(io: *mut zxio_t, out_vmo: *mut zx_handle_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_vmo_get_exact(io: *mut zxio_t, out_vmo: *mut zx_handle_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_vmo_get_exec(io: *mut zxio_t, out_vmo: *mut zx_handle_t) -> zx_status_t;
}
extern "C" {
    pub fn zxio_get_read_buffer_available(
        io: *mut zxio_t,
        out_available: *mut usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_shutdown(
        io: *mut zxio_t,
        options: zxio_shutdown_options_t,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_open(
        directory: *mut zxio_t,
        flags: u32,
        mode: u32,
        path: *const ::std::os::raw::c_char,
        path_len: usize,
        storage: *mut zxio_storage_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_open_async(
        directory: *mut zxio_t,
        flags: u32,
        mode: u32,
        path: *const ::std::os::raw::c_char,
        path_len: usize,
        request: zx_handle_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_add_inotify_filter(
        io: *mut zxio_t,
        path: *const ::std::os::raw::c_char,
        path_len: usize,
        mask: u32,
        watch_descriptor: u32,
        socket: zx_handle_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_unlink(
        directory: *mut zxio_t,
        name: *const ::std::os::raw::c_char,
        name_len: usize,
        flags: ::std::os::raw::c_int,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_rename(
        old_directory: *mut zxio_t,
        old_path: *const ::std::os::raw::c_char,
        old_path_len: usize,
        new_directory_token: zx_handle_t,
        new_path: *const ::std::os::raw::c_char,
        new_path_len: usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_link(
        src_directory: *mut zxio_t,
        src_path: *const ::std::os::raw::c_char,
        src_path_len: usize,
        dst_directory_token: zx_handle_t,
        dst_path: *const ::std::os::raw::c_char,
        dst_path_len: usize,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_dirent_iterator_init(
        iterator: *mut zxio_dirent_iterator_t,
        directory: *mut zxio_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_dirent_iterator_next(
        iterator: *mut zxio_dirent_iterator_t,
        inout_entry: *mut zxio_dirent_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_dirent_iterator_destroy(iterator: *mut zxio_dirent_iterator_t);
}
extern "C" {
    pub fn zxio_isatty(io: *mut zxio_t, tty: *mut bool) -> zx_status_t;
}
extern "C" {
    pub fn zxio_get_window_size(io: *mut zxio_t, width: *mut u32, height: *mut u32) -> zx_status_t;
}
extern "C" {
    pub fn zxio_set_window_size(io: *mut zxio_t, width: u32, height: u32) -> zx_status_t;
}
extern "C" {
    pub fn zxio_ioctl(
        io: *mut zxio_t,
        request: ::std::os::raw::c_int,
        out_code: *mut i16,
        va: *mut __va_list_tag,
    ) -> zx_status_t;
}
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct iovec {
    pub iov_base: *mut ::std::os::raw::c_void,
    pub iov_len: usize,
}
impl Default for iovec {
    fn default() -> Self {
        let mut s = ::std::mem::MaybeUninit::<Self>::uninit();
        unsafe {
            ::std::ptr::write_bytes(s.as_mut_ptr(), 0, 1);
            s.assume_init()
        }
    }
}
pub type socklen_t = __socklen_t;
pub type sa_family_t = ::std::os::raw::c_ushort;
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct sockaddr {
    pub sa_family: sa_family_t,
    pub sa_data: [::std::os::raw::c_char; 14usize],
}
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct msghdr {
    pub msg_name: *mut ::std::os::raw::c_void,
    pub msg_namelen: socklen_t,
    pub msg_iov: *mut iovec,
    pub msg_iovlen: usize,
    pub msg_control: *mut ::std::os::raw::c_void,
    pub msg_controllen: usize,
    pub msg_flags: ::std::os::raw::c_int,
}
impl Default for msghdr {
    fn default() -> Self {
        let mut s = ::std::mem::MaybeUninit::<Self>::uninit();
        unsafe {
            ::std::ptr::write_bytes(s.as_mut_ptr(), 0, 1);
            s.assume_init()
        }
    }
}
pub type zxio_service_connector = ::std::option::Option<
    unsafe extern "C" fn(
        service_name: *const ::std::os::raw::c_char,
        provider_handle: *mut zx_handle_t,
    ) -> zx_status_t,
>;
extern "C" {
    pub fn zxio_socket(
        service_connector: zxio_service_connector,
        domain: ::std::os::raw::c_int,
        type_: ::std::os::raw::c_int,
        protocol: ::std::os::raw::c_int,
        allocator: zxio_storage_alloc,
        out_context: *mut *mut ::std::os::raw::c_void,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_bind(
        io: *mut zxio_t,
        addr: *const sockaddr,
        addrlen: socklen_t,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_connect(
        io: *mut zxio_t,
        addr: *const sockaddr,
        addrlen: socklen_t,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_listen(
        io: *mut zxio_t,
        backlog: ::std::os::raw::c_int,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_accept(
        io: *mut zxio_t,
        addr: *mut sockaddr,
        addrlen: *mut socklen_t,
        out_storage: *mut zxio_storage_t,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_getsockname(
        io: *mut zxio_t,
        addr: *mut sockaddr,
        addrlen: *mut socklen_t,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_getpeername(
        io: *mut zxio_t,
        addr: *mut sockaddr,
        addrlen: *mut socklen_t,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_getsockopt(
        io: *mut zxio_t,
        level: ::std::os::raw::c_int,
        optname: ::std::os::raw::c_int,
        optval: *mut ::std::os::raw::c_void,
        optlen: *mut socklen_t,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_setsockopt(
        io: *mut zxio_t,
        level: ::std::os::raw::c_int,
        optname: ::std::os::raw::c_int,
        optval: *const ::std::os::raw::c_void,
        optlen: socklen_t,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_recvmsg(
        io: *mut zxio_t,
        msg: *mut msghdr,
        flags: ::std::os::raw::c_int,
        out_actual: *mut usize,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_sendmsg(
        io: *mut zxio_t,
        msg: *const msghdr,
        flags: ::std::os::raw::c_int,
        out_actual: *mut usize,
        out_code: *mut i16,
    ) -> zx_status_t;
}
extern "C" {
    pub fn zxio_get_posix_mode(
        protocols: zxio_node_protocols_t,
        abilities: zxio_abilities_t,
    ) -> u32;
}
pub const ZXIO_SHUTDOWN_OPTIONS_READ: zxio_shutdown_options_t = 2;
pub const ZXIO_SHUTDOWN_OPTIONS_WRITE: zxio_shutdown_options_t = 1;
pub const ZXIO_NODE_PROTOCOL_NONE: zxio_node_protocols_t = 0;
pub const ZXIO_NODE_PROTOCOL_CONNECTOR: zxio_node_protocols_t = 1;
pub const ZXIO_NODE_PROTOCOL_DIRECTORY: zxio_node_protocols_t = 2;
pub const ZXIO_NODE_PROTOCOL_FILE: zxio_node_protocols_t = 4;
pub type __builtin_va_list = [__va_list_tag; 1usize];
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct __va_list_tag {
    pub gp_offset: ::std::os::raw::c_uint,
    pub fp_offset: ::std::os::raw::c_uint,
    pub overflow_arg_area: *mut ::std::os::raw::c_void,
    pub reg_save_area: *mut ::std::os::raw::c_void,
}
impl Default for __va_list_tag {
    fn default() -> Self {
        let mut s = ::std::mem::MaybeUninit::<Self>::uninit();
        unsafe {
            ::std::ptr::write_bytes(s.as_mut_ptr(), 0, 1);
            s.assume_init()
        }
    }
}
