// Copyright 2023 The Fuchsia Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{mem::MaybeUninit, ops::Range};

use fdio;
use fidl_fuchsia_io as fio;
use fuchsia_runtime;
use fuchsia_zircon as zx;
use zerocopy::FromBytes;
use zx::{AsHandleRef, HandleBased, Task};

#[cfg(target_arch = "x86_64")]
extern "C" {
    // This function generates a "return" from the usercopy routine with an error.
    fn hermetic_copy_error();
}

/// Converts a slice to an equivalent MaybeUninit slice.
pub fn slice_to_maybe_uninit_mut<T>(slice: &mut [T]) -> &mut [MaybeUninit<T>] {
    let ptr = slice.as_mut_ptr();
    let ptr = ptr as *mut MaybeUninit<T>;
    // SAFETY: This is effectively reinterpreting the `slice` reference as a
    // slice of uninitialized T's. `MaybeUninit<T>` has the same layout[1] as
    // `T` and we know the original slice is initialized and its okay to from
    // initialized to maybe initialized.
    //
    // [1]: https://doc.rust-lang.org/std/mem/union.MaybeUninit.html#layout-1
    unsafe { std::slice::from_raw_parts_mut(ptr, slice.len()) }
}

type HermeticCopyFn =
    unsafe extern "C" fn(dest: *mut u8, source: *const u8, len: usize, ret_dest: bool) -> usize;

pub struct Usercopy {
    // Pointer to the hermetic_copy routine loaded into memory.
    hermetic_copy_fn: HermeticCopyFn,

    // Pointer to the hermetic_copy_until_null_byte routine loaded into memory.
    hermetic_copy_until_null_byte_fn: HermeticCopyFn,

    // This is an event used to signal the exception handling thread to shut down.
    shutdown_event: zx::Event,

    // Handle to the exception handling thread.
    join_handle: Option<std::thread::JoinHandle<()>>,

    // The range of the restricted address space.
    restricted_address_range: Range<usize>,
}

fn get_hermetic_copy_bin(path: &str) -> Result<Range<usize>, zx::Status> {
    let blob =
        fdio::open_fd(path, fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE)?;
    let exec_vmo = fdio::get_vmo_exec_from_file(&blob)?;
    let copy_size = exec_vmo.get_size()?;
    let mapped_addr = fuchsia_runtime::vmar_root_self().map(
        0,
        &exec_vmo,
        0,
        copy_size as usize,
        zx::VmarFlags::PERM_READ_IF_XOM_UNSUPPORTED | zx::VmarFlags::PERM_EXECUTE,
    )?;
    Ok(mapped_addr..mapped_addr + copy_size as usize)
}

/// Assumes the buffer's first `initialized_until` bytes are initialized and
/// returns the initialized and uninitialized portions.
///
/// # Safety
///
/// The caller must guarantee that `buf`'s first `initialized_until` bytes are
/// initialized.
unsafe fn assume_initialized_until(
    buf: &mut [MaybeUninit<u8>],
    initialized_until: usize,
) -> (&mut [u8], &mut [MaybeUninit<u8>]) {
    let (init_bytes, uninit_bytes) = buf.split_at_mut(initialized_until);
    debug_assert_eq!(init_bytes.len(), initialized_until);

    let init_bytes =
        std::slice::from_raw_parts_mut(init_bytes.as_mut_ptr() as *mut u8, init_bytes.len());

    (init_bytes, uninit_bytes)
}

/// Copies bytes from the source address to the destination address using the
/// provided copy function.
///
/// # Safety
///
/// Only one of `source`/`dest` may be an address to a buffer owned by user/restricted-mode.
/// The other must be a valid Starnix/normal-mode buffer that will never cause a fault
/// when the first `count` bytes are read/written.
unsafe fn do_hermetic_copy(
    f: HermeticCopyFn,
    source: usize,
    dest: usize,
    count: usize,
    ret_dest: bool,
) -> usize {
    let unread_address = unsafe { f(dest as *mut u8, source as *const u8, count, ret_dest) };

    let ret_base = if ret_dest { dest } else { source };

    debug_assert!(
        unread_address >= ret_base,
        "unread_address={:#x}, ret_base={:#x}",
        unread_address,
        ret_base,
    );
    let copied = unread_address - ret_base;
    debug_assert!(
        copied <= count,
        "copied={}, count={}; unread_address={:#x}, ret_base={:#x}",
        copied,
        count,
        unread_address,
        ret_base,
    );
    copied
}

impl Usercopy {
    /// Returns a new instance of `Usercopy` if unified address spaces is
    /// supported on the target architecture.
    pub fn new(restricted_address_range: Range<usize>) -> Result<Option<Self>, zx::Status> {
        if cfg!(not(target_arch = "x86_64")) {
            return Ok(None);
        }

        let hermetic_copy_addr_range = get_hermetic_copy_bin("/pkg/hermetic_copy.bin")?;
        let hermetic_copy_fn: HermeticCopyFn =
            unsafe { std::mem::transmute(hermetic_copy_addr_range.start) };

        let hermetic_copy_until_null_byte_addr_range =
            get_hermetic_copy_bin("/pkg/hermetic_copy_until_null_byte.bin")?;
        let hermetic_copy_until_null_byte_fn: HermeticCopyFn =
            unsafe { std::mem::transmute(hermetic_copy_until_null_byte_addr_range.start) };

        let (tx, rx) = std::sync::mpsc::channel::<zx::Status>();

        let shutdown_event = zx::Event::create();
        let shutdown_event_clone =
            shutdown_event.duplicate_handle(zx::Rights::SAME_RIGHTS).unwrap();

        let faultable_addresses = restricted_address_range.clone();
        let join_handle = std::thread::spawn(move || {
            let exception_channel_result =
                fuchsia_runtime::job_default().create_exception_channel();

            let exception_channel = match exception_channel_result {
                Ok(c) => c,
                Err(e) => {
                    let _ = tx.send(e);
                    return;
                }
            };

            // register exception handler
            let _ = tx.send(zx::Status::OK);

            // loop on exceptions
            loop {
                let mut wait_items = [
                    zx::WaitItem {
                        handle: exception_channel.as_handle_ref(),
                        waitfor: zx::Signals::CHANNEL_READABLE,
                        pending: zx::Signals::empty(),
                    },
                    zx::WaitItem {
                        handle: shutdown_event_clone.as_handle_ref(),
                        waitfor: zx::Signals::USER_0,
                        pending: zx::Signals::empty(),
                    },
                ];
                let _ = zx::object_wait_many(&mut wait_items, zx::Time::INFINITE);
                if wait_items[1].pending == zx::Signals::USER_0 {
                    break;
                }
                let mut buf = zx::MessageBuf::new();
                exception_channel.read(&mut buf).unwrap();

                let excp_info = zx::sys::zx_exception_info_t::read_from(buf.bytes()).unwrap();

                if excp_info.type_ != zx::sys::ZX_EXCP_FATAL_PAGE_FAULT {
                    // Only process page faults.
                    continue;
                }

                let excp = zx::Exception::from_handle(buf.take_handle(0).unwrap());

                #[cfg(target_arch = "x86_64")]
                {
                    let thread = excp.get_thread().unwrap();
                    let mut regs = thread.read_state_general_regs().unwrap();

                    let pc = regs.rip as usize;
                    if !hermetic_copy_addr_range.contains(&pc)
                        && !hermetic_copy_until_null_byte_addr_range.contains(&pc)
                    {
                        continue;
                    }

                    let report = thread.get_exception_report().unwrap();

                    let fault_address = unsafe { report.context.arch.x86_64.cr2 };
                    if !faultable_addresses.contains(&(fault_address as usize)) {
                        continue;
                    }

                    regs.rip = hermetic_copy_error as u64;
                    regs.rax = fault_address;
                    thread.write_state_general_regs(regs).unwrap();
                }

                // To avoid unused errors.
                #[cfg(not(target_arch = "x86_64"))]
                let _ = faultable_addresses;

                excp.set_exception_state(&zx::sys::ZX_EXCEPTION_STATE_HANDLED).unwrap();
            }
        });

        match rx.recv().unwrap() {
            zx::Status::OK => {}
            s => {
                return Err(s);
            }
        };

        Ok(Some(Self {
            hermetic_copy_fn,
            hermetic_copy_until_null_byte_fn,
            shutdown_event,
            join_handle: Some(join_handle),
            restricted_address_range,
        }))
    }

    /// Copies data from `source` to the restricted address `dest_addr`.
    ///
    /// Returns the number of bytes copied.
    pub fn copyout(&self, source: &[u8], dest_addr: usize) -> usize {
        // Assumption: The address 0 is invalid and cannot be mapped.  The error encoding scheme has
        // a collision on the value 0 - it could mean that there was a fault at the address 0 or
        // that there was no fault. We want to treat an attempt to copy to 0 as a fault always.
        if dest_addr == 0 || !self.restricted_address_range.contains(&dest_addr) {
            return 0;
        }

        // SAFETY: `source` is a valid Starnix-owned buffer and `dest_addr` is the user-mode
        // buffer.
        unsafe {
            do_hermetic_copy(
                self.hermetic_copy_fn,
                source.as_ptr() as usize,
                dest_addr,
                source.len(),
                true,
            )
        }
    }

    /// Copies data from the restricted address `source_addr` to `dest`.
    ///
    /// Returns the read and unread bytes.
    pub fn copyin<'a>(
        &self,
        source_addr: usize,
        dest: &'a mut [MaybeUninit<u8>],
    ) -> (&'a mut [u8], &'a mut [MaybeUninit<u8>]) {
        // Assumption: The address 0 is invalid and cannot be mapped.  The error encoding scheme has
        // a collision on the value 0 - it could mean that there was a fault at the address 0 or
        // that there was no fault. We want to treat an attempt to copy from 0 as a fault always.
        if source_addr == 0 || !self.restricted_address_range.contains(&source_addr) {
            return (&mut [], dest);
        }

        // SAFETY: `dest` is a valid Starnix-owned buffer and `source_addr` is the user-mode
        // buffer.
        let read_count = unsafe {
            do_hermetic_copy(
                self.hermetic_copy_fn,
                source_addr,
                dest.as_ptr() as usize,
                dest.len(),
                false,
            )
        };

        // SAFETY: `dest`'s first `read_count` bytes are initialized
        unsafe { assume_initialized_until(dest, read_count) }
    }

    /// Copies data from the restricted address `source_addr` to `dest` until the
    /// first null byte.
    ///
    /// Returns the read and unread bytes. The read bytes includes the null byte
    /// if present.
    pub fn copyin_until_null_byte<'a>(
        &self,
        source_addr: usize,
        dest: &'a mut [MaybeUninit<u8>],
    ) -> (&'a mut [u8], &'a mut [MaybeUninit<u8>]) {
        // Assumption: The address 0 is invalid and cannot be mapped.  The error encoding scheme has
        // a collision on the value 0 - it could mean that there was a fault at the address 0 or
        // that there was no fault. We want to treat an attempt to copy from 0 as a fault always.
        if source_addr == 0 || !self.restricted_address_range.contains(&source_addr) {
            return (&mut [], dest);
        }

        // SAFETY: `dest` is a valid Starnix-owned buffer and `source_addr` is the user-mode
        // buffer.
        let read_count = unsafe {
            do_hermetic_copy(
                self.hermetic_copy_until_null_byte_fn,
                source_addr,
                dest.as_ptr() as usize,
                dest.len(),
                false,
            )
        };

        // SAFETY: `dest`'s first `read_count` bytes are initialized
        unsafe { assume_initialized_until(dest, read_count) }
    }
}

impl Drop for Usercopy {
    fn drop(&mut self) {
        self.shutdown_event.signal_handle(zx::Signals::empty(), zx::Signals::USER_0).unwrap();
        self.join_handle.take().unwrap().join().unwrap();
    }
}

#[cfg(test)]
mod test {
    use super::*;

    use test_case::test_case;

    macro_rules! new_usercopy_for_test {
        ($stmt:expr) => {{
            let usercopy = $stmt.unwrap();
            if cfg!(target_arch = "x86_64") {
                usercopy.unwrap()
            } else {
                // Skip test if not a supported architecture
                assert!(usercopy.is_none());
                return;
            }
        }};
    }

    #[test_case(0)]
    #[test_case(1)]
    #[test_case(7)]
    #[test_case(8)]
    #[test_case(9)]
    #[test_case(128)]
    #[test_case(zx::system_get_page_size() as usize - 1)]
    #[test_case(zx::system_get_page_size() as usize)]
    #[::fuchsia::test]
    fn copyout_no_fault(buf_len: usize) {
        let page_size = zx::system_get_page_size() as usize;

        let source = vec!['a' as u8; buf_len];

        let dest_vmo = zx::Vmo::create(page_size as u64).unwrap();

        let root_vmar = fuchsia_runtime::vmar_root_self();

        let mapped_addr = root_vmar
            .map(0, &dest_vmo, 0, page_size, zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE)
            .unwrap();

        let usercopy = new_usercopy_for_test!(Usercopy::new(mapped_addr..mapped_addr + page_size));

        let result = usercopy.copyout(&source, mapped_addr);
        assert_eq!(result, buf_len);

        assert_eq!(
            unsafe { std::slice::from_raw_parts(mapped_addr as *const u8, buf_len) },
            &vec!['a' as u8; buf_len]
        );
    }

    #[test_case(1, 2)]
    #[test_case(1, 4)]
    #[test_case(1, 8)]
    #[test_case(1, 16)]
    #[test_case(1, 32)]
    #[test_case(1, 64)]
    #[test_case(1, 128)]
    #[test_case(1, 256)]
    #[test_case(1, 512)]
    #[test_case(1, 1024)]
    #[test_case(32, 64)]
    #[test_case(32, 128)]
    #[test_case(32, 256)]
    #[test_case(32, 512)]
    #[test_case(32, 1024)]
    #[::fuchsia::test]
    fn copyout_fault(offset: usize, buf_len: usize) {
        let page_size = zx::system_get_page_size() as usize;

        let source = vec!['a' as u8; buf_len];

        let dest_vmo = zx::Vmo::create(page_size as u64).unwrap();

        let root_vmar = fuchsia_runtime::vmar_root_self();

        let mapped_addr = root_vmar
            .map(
                0,
                &dest_vmo,
                0,
                page_size * 2,
                zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE,
            )
            .unwrap();

        let usercopy =
            new_usercopy_for_test!(Usercopy::new(mapped_addr..mapped_addr + page_size * 2));

        let dest_addr = mapped_addr + page_size - offset;

        let result = usercopy.copyout(&source, dest_addr);

        assert_eq!(result, offset);

        assert_eq!(
            unsafe { std::slice::from_raw_parts(dest_addr as *const u8, offset) },
            &vec!['a' as u8; offset][..],
        );
    }

    #[test_case(0)]
    #[test_case(1)]
    #[test_case(7)]
    #[test_case(8)]
    #[test_case(9)]
    #[test_case(128)]
    #[test_case(zx::system_get_page_size() as usize - 1)]
    #[test_case(zx::system_get_page_size() as usize)]
    #[::fuchsia::test]
    fn copyin_no_fault(buf_len: usize) {
        let page_size = zx::system_get_page_size() as usize;

        let mut dest = Vec::with_capacity(buf_len);

        let source_vmo = zx::Vmo::create(page_size as u64).unwrap();

        let root_vmar = fuchsia_runtime::vmar_root_self();

        let mapped_addr = root_vmar
            .map(0, &source_vmo, 0, page_size, zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE)
            .unwrap();

        unsafe { std::slice::from_raw_parts_mut(mapped_addr as *mut u8, buf_len) }.fill('a' as u8);

        let usercopy = new_usercopy_for_test!(Usercopy::new(mapped_addr..mapped_addr + page_size));
        let (read_bytes, unread_bytes) = usercopy.copyin(mapped_addr, dest.spare_capacity_mut());
        let expected = vec!['a' as u8; buf_len];
        assert_eq!(read_bytes, &expected);
        assert_eq!(unread_bytes.len(), 0);

        // SAFETY: OK because the copyin was successful.
        unsafe { dest.set_len(buf_len) }
        assert_eq!(dest, expected);
    }

    #[test_case(1, 2)]
    #[test_case(1, 4)]
    #[test_case(1, 8)]
    #[test_case(1, 16)]
    #[test_case(1, 32)]
    #[test_case(1, 64)]
    #[test_case(1, 128)]
    #[test_case(1, 256)]
    #[test_case(1, 512)]
    #[test_case(1, 1024)]
    #[test_case(32, 64)]
    #[test_case(32, 128)]
    #[test_case(32, 256)]
    #[test_case(32, 512)]
    #[test_case(32, 1024)]
    #[::fuchsia::test]
    fn copyin_fault(offset: usize, buf_len: usize) {
        let page_size = zx::system_get_page_size() as usize;

        let mut dest = vec![0u8; buf_len];

        let source_vmo = zx::Vmo::create(page_size as u64).unwrap();

        let root_vmar = fuchsia_runtime::vmar_root_self();

        let mapped_addr = root_vmar
            .map(
                0,
                &source_vmo,
                0,
                page_size * 2,
                zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE,
            )
            .unwrap();

        let source_addr = mapped_addr + page_size - offset;

        unsafe { std::slice::from_raw_parts_mut(source_addr as *mut u8, offset) }.fill('a' as u8);

        let usercopy =
            new_usercopy_for_test!(Usercopy::new(mapped_addr..mapped_addr + page_size * 2));

        let (read_bytes, unread_bytes) =
            usercopy.copyin(source_addr, slice_to_maybe_uninit_mut(&mut dest));
        let expected_copied = vec!['a' as u8; offset];
        let expected_uncopied = vec![0 as u8; buf_len - offset];
        assert_eq!(read_bytes, &expected_copied);
        assert_eq!(unread_bytes.len(), expected_uncopied.len());

        assert_eq!(&dest[0..offset], &expected_copied);
        assert_eq!(&dest[offset..], &expected_uncopied);
    }

    #[test_case(0)]
    #[test_case(1)]
    #[test_case(7)]
    #[test_case(8)]
    #[test_case(9)]
    #[test_case(128)]
    #[test_case(zx::system_get_page_size() as usize - 1)]
    #[test_case(zx::system_get_page_size() as usize)]
    #[::fuchsia::test]
    fn copyin_until_null_byte_no_fault(buf_len: usize) {
        let page_size = zx::system_get_page_size() as usize;

        let mut dest = Vec::with_capacity(buf_len);

        let source_vmo = zx::Vmo::create(page_size as u64).unwrap();

        let root_vmar = fuchsia_runtime::vmar_root_self();

        let mapped_addr = root_vmar
            .map(0, &source_vmo, 0, page_size, zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE)
            .unwrap();

        unsafe { std::slice::from_raw_parts_mut(mapped_addr as *mut u8, buf_len) }.fill('a' as u8);

        let usercopy = new_usercopy_for_test!(Usercopy::new(mapped_addr..mapped_addr + page_size));

        let (read_bytes, unread_bytes) =
            usercopy.copyin_until_null_byte(mapped_addr, dest.spare_capacity_mut());
        let expected = vec!['a' as u8; buf_len];
        assert_eq!(read_bytes, &expected);
        assert_eq!(unread_bytes.len(), 0);

        // SAFETY: OK because the copyin_until_null_byte was successful.
        unsafe { dest.set_len(dest.capacity()) }
        assert_eq!(dest, expected);
    }

    #[test_case(1, 2)]
    #[test_case(1, 4)]
    #[test_case(1, 8)]
    #[test_case(1, 16)]
    #[test_case(1, 32)]
    #[test_case(1, 64)]
    #[test_case(1, 128)]
    #[test_case(1, 256)]
    #[test_case(1, 512)]
    #[test_case(1, 1024)]
    #[test_case(32, 64)]
    #[test_case(32, 128)]
    #[test_case(32, 256)]
    #[test_case(32, 512)]
    #[test_case(32, 1024)]
    #[::fuchsia::test]
    fn copyin_until_null_byte_fault(offset: usize, buf_len: usize) {
        let page_size = zx::system_get_page_size() as usize;

        let mut dest = vec![0u8; buf_len];

        let source_vmo = zx::Vmo::create(page_size as u64).unwrap();

        let root_vmar = fuchsia_runtime::vmar_root_self();

        let mapped_addr = root_vmar
            .map(
                0,
                &source_vmo,
                0,
                page_size * 2,
                zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE,
            )
            .unwrap();

        let source_addr = mapped_addr + page_size - offset;

        unsafe { std::slice::from_raw_parts_mut(source_addr as *mut u8, offset) }.fill('a' as u8);

        let usercopy =
            new_usercopy_for_test!(Usercopy::new(mapped_addr..mapped_addr + page_size * 2));

        let (read_bytes, unread_bytes) =
            usercopy.copyin_until_null_byte(source_addr, slice_to_maybe_uninit_mut(&mut dest));
        let expected_copied = vec!['a' as u8; offset];
        let expected_uncopied = vec![0 as u8; buf_len - offset];
        assert_eq!(read_bytes, &expected_copied);
        assert_eq!(unread_bytes.len(), expected_uncopied.len());

        assert_eq!(&dest[0..offset], &expected_copied);
        assert_eq!(&dest[offset..], &expected_uncopied);
    }

    #[test_case(0)]
    #[test_case(1)]
    #[test_case(2)]
    #[test_case(126)]
    #[test_case(127)]
    #[::fuchsia::test]
    fn copyin_until_null_byte_no_fault_with_zero(zero_idx: usize) {
        const DEST_LEN: usize = 128;

        let page_size = zx::system_get_page_size() as usize;

        let mut dest = vec!['b' as u8; DEST_LEN];

        let source_vmo = zx::Vmo::create(page_size as u64).unwrap();

        let root_vmar = fuchsia_runtime::vmar_root_self();

        let mapped_addr = root_vmar
            .map(0, &source_vmo, 0, page_size, zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE)
            .unwrap();

        {
            let slice =
                unsafe { std::slice::from_raw_parts_mut(mapped_addr as *mut u8, dest.len()) };
            slice.fill('a' as u8);
            slice[zero_idx] = 0;
        };

        let usercopy = new_usercopy_for_test!(Usercopy::new(mapped_addr..mapped_addr + page_size));

        let (read_bytes, unread_bytes) =
            usercopy.copyin_until_null_byte(mapped_addr, slice_to_maybe_uninit_mut(&mut dest));
        let expected_copied_non_zero_bytes = vec!['a' as u8; zero_idx];
        let expected_uncopied = vec!['b' as u8; DEST_LEN - zero_idx - 1];
        assert_eq!(&read_bytes[..zero_idx], &expected_copied_non_zero_bytes);
        assert_eq!(&read_bytes[zero_idx..], &[0]);
        assert_eq!(unread_bytes.len(), expected_uncopied.len());

        assert_eq!(&dest[..zero_idx], &expected_copied_non_zero_bytes);
        assert_eq!(dest[zero_idx], 0);
        assert_eq!(&dest[zero_idx + 1..], &expected_uncopied);
    }

    #[test_case(0..1, 0)]
    #[test_case(0..1, 1)]
    #[test_case(0..1, 2)]
    #[test_case(5..10, 0)]
    #[test_case(5..10, 1)]
    #[test_case(5..10, 2)]
    #[test_case(5..10, 5)]
    #[test_case(5..10, 7)]
    #[test_case(5..10, 10)]
    #[::fuchsia::test]
    fn starting_fault_address_copyin_until_null_byte(range: Range<usize>, addr: usize) {
        let usercopy = new_usercopy_for_test!(Usercopy::new(range));

        let mut dest = vec![0u8];

        let (read_bytes, unread_bytes) =
            usercopy.copyin_until_null_byte(addr, slice_to_maybe_uninit_mut(&mut dest));
        assert_eq!(read_bytes, &[]);
        assert_eq!(unread_bytes.len(), dest.len());
        assert_eq!(dest, [0]);
    }

    #[test_case(0..1, 0)]
    #[test_case(0..1, 1)]
    #[test_case(0..1, 2)]
    #[test_case(5..10, 0)]
    #[test_case(5..10, 1)]
    #[test_case(5..10, 2)]
    #[test_case(5..10, 5)]
    #[test_case(5..10, 7)]
    #[test_case(5..10, 10)]
    #[::fuchsia::test]
    fn starting_fault_address_copyin(range: Range<usize>, addr: usize) {
        let usercopy = new_usercopy_for_test!(Usercopy::new(range));

        let mut dest = vec![0u8];

        let (read_bytes, unread_bytes) =
            usercopy.copyin(addr, slice_to_maybe_uninit_mut(&mut dest));
        assert_eq!(read_bytes, &[]);
        assert_eq!(unread_bytes.len(), dest.len());
        assert_eq!(dest, [0]);
    }

    #[test_case(0..1, 0)]
    #[test_case(0..1, 1)]
    #[test_case(0..1, 2)]
    #[test_case(5..10, 0)]
    #[test_case(5..10, 1)]
    #[test_case(5..10, 2)]
    #[test_case(5..10, 5)]
    #[test_case(5..10, 7)]
    #[test_case(5..10, 10)]
    #[::fuchsia::test]
    fn starting_fault_address_copyout(range: Range<usize>, addr: usize) {
        let usercopy = new_usercopy_for_test!(Usercopy::new(range));

        let source = vec![0u8];

        let result = usercopy.copyout(&source, addr);
        assert_eq!(result, 0);
        assert_eq!(source, [0]);
    }
}
