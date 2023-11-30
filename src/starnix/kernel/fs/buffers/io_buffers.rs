// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::mm::{
    read_to_array, read_to_object_as_bytes, read_to_vec, MemoryAccessor, MemoryAccessorExt,
    MemoryManager,
};
use starnix_uapi::{
    errno, error, errors::Errno, user_address::UserAddress, user_buffer::UserBuffer,
};
use std::mem::MaybeUninit;
use zerocopy::FromBytes;

/// The callback for `OutputBuffer::write_each`. The callback is passed the buffers to write to in
/// order, and must return for each, how many bytes has been written.
pub type OutputBufferCallback<'a> = dyn FnMut(&mut [MaybeUninit<u8>]) -> Result<usize, Errno> + 'a;

fn slice_to_maybe_uninit(buffer: &[u8]) -> &[MaybeUninit<u8>] {
    // SAFETY: &[u8] and &[MaybeUninit<u8>] have the same layout.
    unsafe { std::slice::from_raw_parts(buffer.as_ptr() as *const MaybeUninit<u8>, buffer.len()) }
}

/// The OutputBuffer allows for writing bytes to a buffer.
/// A single OutputBuffer will only write up to MAX_RW_COUNT bytes which is the maximum size of a
/// single operation.
pub trait OutputBuffer: std::fmt::Debug {
    /// Calls `callback` for each segment to write data for. `callback` must returns the number of
    /// bytes actually written. When it returns less than the size of the input buffer, the write
    /// is stopped.
    ///
    /// Returns the total number of bytes written.
    fn write_each(&mut self, callback: &mut OutputBufferCallback<'_>) -> Result<usize, Errno>;

    /// Returns the number of bytes available to be written into the buffer.
    fn available(&self) -> usize;

    /// Returns the number of bytes already written into the buffer.
    fn bytes_written(&self) -> usize;

    /// Fills this buffer with zeros.
    fn zero(&mut self) -> Result<usize, Errno>;

    /// Write the content of `buffer` into this buffer. If this buffer is too small, the write will
    /// be partial.
    ///
    /// Returns the number of bytes written in this buffer.
    fn write(&mut self, buffer: &[u8]) -> Result<usize, Errno> {
        let mut buffer = slice_to_maybe_uninit(buffer);

        self.write_each(&mut move |data| {
            let size = std::cmp::min(buffer.len(), data.len());
            let (to_clone, remaining) = buffer.split_at(size);
            data[0..size].clone_from_slice(to_clone);
            buffer = remaining;
            Ok(size)
        })
    }

    /// Write the content of `buffer` into this buffer. It is an error to pass a buffer larger than
    /// the number of bytes available in this buffer. In that case, the content of the buffer after
    /// the operation is unspecified.
    ///
    /// In case of success, always returns `buffer.len()`.
    fn write_all(&mut self, buffer: &[u8]) -> Result<usize, Errno> {
        let size = self.write(buffer)?;
        if size != buffer.len() {
            error!(EINVAL)
        } else {
            Ok(size)
        }
    }

    /// Write the content of the given `InputBuffer` into this buffer. The number of bytes written
    /// will be the smallest between the number of bytes available in this buffer and in the
    /// `InputBuffer`.
    ///
    /// Returns the number of bytes read and written.
    fn write_buffer(&mut self, input: &mut dyn InputBuffer) -> Result<usize, Errno> {
        self.write_each(&mut move |data| {
            let size = std::cmp::min(data.len(), input.available());
            input.read_exact(&mut data[0..size])
        })
    }
}

/// The callback for `InputBuffer::peek_each` and `InputBuffer::read_each`. The callback is passed
/// the buffers to write to in order, and must return for each, how many bytes has been read.

pub type InputBufferCallback<'a> = dyn FnMut(&[u8]) -> Result<usize, Errno> + 'a;

/// The InputBuffer allows for reading bytes from a buffer.
/// A single InputBuffer will only read up to MAX_RW_COUNT bytes which is the maximum size of a
/// single operation.
pub trait InputBuffer: std::fmt::Debug {
    /// Calls `callback` for each segment to peek data from. `callback` must returns the number of
    /// bytes actually peeked. When it returns less than the size of the output buffer, the read
    /// is stopped.
    ///
    /// Returns the total number of bytes peeked.
    fn peek_each(&mut self, callback: &mut InputBufferCallback<'_>) -> Result<usize, Errno>;

    /// Returns the number of bytes available to be read from the buffer.
    fn available(&self) -> usize;

    /// Returns the number of bytes already read from the buffer.
    fn bytes_read(&self) -> usize;

    /// Clear the remaining content in the buffer. Returns the number of bytes swallowed. After this
    /// method returns, `available()` will returns 0. This does not touch the data in the buffer.
    fn drain(&mut self) -> usize;

    /// Consumes `length` bytes of data from this buffer.
    fn advance(&mut self, length: usize) -> Result<(), Errno>;

    /// Calls `callback` for each segment to read data from. `callback` must returns the number of
    /// bytes actually read. When it returns less than the size of the output buffer, the read
    /// is stopped.
    ///
    /// Returns the total number of bytes read.
    fn read_each(&mut self, callback: &mut InputBufferCallback<'_>) -> Result<usize, Errno> {
        let length = self.peek_each(callback)?;
        self.advance(length)?;
        Ok(length)
    }

    /// Read all the remaining content in this buffer and returns it as a `Vec`.
    fn read_all(&mut self) -> Result<Vec<u8>, Errno> {
        let result = self.peek_all()?;
        let drain_result = self.drain();
        assert!(result.len() == drain_result);
        Ok(result)
    }

    /// Peek all the remaining content in this buffer and returns it as a `Vec`.
    fn peek_all(&mut self) -> Result<Vec<u8>, Errno> {
        let mut result = vec![];
        result.reserve(self.available());
        self.peek_each(&mut |data| {
            result.extend_from_slice(data);
            Ok(data.len())
        })?;
        Ok(result)
    }

    /// Write the content of this buffer into `buffer`.
    /// If `buffer` is too small, the read will be partial.
    /// If `buffer` is too large, the remaining bytes will be left untouched.
    ///
    /// Returns the number of bytes read from this buffer.
    fn read(&mut self, buffer: &mut [MaybeUninit<u8>]) -> Result<usize, Errno> {
        let mut index = 0;
        self.read_each(&mut move |data: &[u8]| {
            let data = slice_to_maybe_uninit(data);

            let size = std::cmp::min(buffer.len() - index, data.len());
            buffer[index..index + size].clone_from_slice(&data[0..size]);
            index += size;
            Ok(size)
        })
    }

    /// Read the exact number of bytes required to fill buf.
    ///
    /// If `buffer` is larger than the number of available bytes, an error will be returned.
    ///
    /// In case of success, always returns `buffer.len()`.
    fn read_exact(&mut self, buffer: &mut [MaybeUninit<u8>]) -> Result<usize, Errno> {
        let size = self.read(buffer)?;
        if size != buffer.len() {
            error!(EINVAL)
        } else {
            Ok(size)
        }
    }
}

pub trait InputBufferExt: InputBuffer {
    /// Reads exactly `len` bytes into a returned `Vec`.
    ///
    /// Returns an error if `len` is larger than the number of available bytes.
    fn read_to_vec_exact(&mut self, len: usize) -> Result<Vec<u8>, Errno> {
        // SAFETY: `data.read_exact` returns `len` bytes on success.
        unsafe { read_to_vec(len, |buf| self.read_exact(buf)) }
    }

    /// Reads up to `limit` bytes into a returned `Vec`.
    fn read_to_vec_limited(&mut self, limit: usize) -> Result<Vec<u8>, Errno> {
        // SAFETY: `data.read` returns the number of bytes read.
        unsafe { read_to_vec(limit, |buf| self.read(buf)) }
    }

    /// Reads bytes into the array.
    ///
    /// Returns an error if `N` is larger than the number of available bytes.
    fn read_to_array<const N: usize>(&mut self) -> Result<[u8; N], Errno> {
        // SAFETY: `data.read_exact` returns `N` bytes on success.
        unsafe {
            read_to_array::<_, N>(|buf| {
                self.read_exact(buf).map(|bytes_read| debug_assert_eq!(bytes_read, buf.len()))
            })
        }
    }

    /// Interprets the buffer as an object.
    ///
    /// Returns an error if the buffer does not have enough bytes to represent the
    /// object.
    fn read_to_object<T: FromBytes>(&mut self) -> Result<T, Errno> {
        // SAFETY: the callback returns successfully only if the required number of
        // bytes were read.
        unsafe {
            read_to_object_as_bytes(|buf| {
                if self.read(buf)? != buf.len() {
                    error!(EINVAL)
                } else {
                    Ok(())
                }
            })
        }
    }
}

impl InputBufferExt for dyn InputBuffer + '_ {}
impl<T: InputBuffer> InputBufferExt for T {}

/// An OutputBuffer that write data to user space memory through a `MemoryManager`.
///
/// `USE_VMO` indicates whether the data should be written through a VMO. If `false`,
/// the user space memory is assumed to be mapped in the current address space and
/// the memory will be written to directly instead of going through the VMO.
pub struct UserBuffersOutputBuffer<'a, const USE_VMO: bool = false> {
    mm: &'a MemoryManager,
    buffers: Vec<UserBuffer>,
    available: usize,
    bytes_written: usize,
}

impl<'a, const USE_VMO: bool> std::fmt::Debug for UserBuffersOutputBuffer<'a, USE_VMO> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("UserBuffersOutputBuffer")
            .field("buffers", &self.buffers)
            .field("available", &self.available)
            .field("bytes_written", &self.bytes_written)
            .finish()
    }
}

impl<'a, const USE_VMO: bool> UserBuffersOutputBuffer<'a, USE_VMO> {
    fn new_inner(mm: &'a MemoryManager, buffers: Vec<UserBuffer>) -> Result<Self, Errno> {
        let (mut buffers, available) =
            UserBuffer::cap_buffers_to_max_rw_count(mm.maximum_valid_user_address, buffers)?;
        // Reverse the buffers as the element will be removed as they are handled.
        buffers.reverse();
        Ok(Self { mm, buffers, available, bytes_written: 0 })
    }
}

impl<'a> UserBuffersOutputBuffer<'a> {
    pub fn new(mm: &'a MemoryManager, buffers: Vec<UserBuffer>) -> Result<Self, Errno> {
        Self::new_inner(mm, buffers)
    }

    pub fn new_at(
        mm: &'a MemoryManager,
        address: UserAddress,
        length: usize,
    ) -> Result<Self, Errno> {
        Self::new(mm, vec![UserBuffer { address, length }])
    }
}

impl<'a> UserBuffersOutputBuffer<'a, true> {
    pub fn vmo_new(mm: &'a MemoryManager, buffers: Vec<UserBuffer>) -> Result<Self, Errno> {
        Self::new_inner(mm, buffers)
    }
}

impl<'a, const USE_VMO: bool> OutputBuffer for UserBuffersOutputBuffer<'a, USE_VMO> {
    fn write_each(&mut self, callback: &mut OutputBufferCallback<'_>) -> Result<usize, Errno> {
        let mut bytes_written = 0;
        while let Some(mut buffer) = self.buffers.pop() {
            if buffer.is_null() {
                continue;
            }
            // SAFETY: `callback` returns the number of bytes read on success.
            let bytes = unsafe {
                read_to_vec(buffer.length, |buf| {
                    let result = callback(buf)?;
                    if result > buffer.length {
                        return error!(EINVAL);
                    }
                    Ok(result)
                })
            }?;
            bytes_written += if USE_VMO {
                self.mm.vmo_write_memory(buffer.address, &bytes)
            } else {
                self.mm.write_memory(buffer.address, &bytes)
            }?;
            let bytes_len = bytes.len();
            buffer.advance(bytes_len)?;
            self.available -= bytes_len;
            self.bytes_written += bytes_len;
            if !buffer.is_empty() {
                self.buffers.push(buffer);
                break;
            }
        }
        Ok(bytes_written)
    }

    fn available(&self) -> usize {
        self.available
    }

    fn bytes_written(&self) -> usize {
        self.bytes_written
    }

    fn zero(&mut self) -> Result<usize, Errno> {
        let mut bytes_written = 0;
        while let Some(mut buffer) = self.buffers.pop() {
            if buffer.is_null() {
                continue;
            }

            let count = self.mm.zero(buffer.address, buffer.length)?;
            buffer.advance(count)?;
            bytes_written += count;

            self.available -= count;
            self.bytes_written += count;

            if !buffer.is_empty() {
                self.buffers.push(buffer);
                break;
            }
        }

        Ok(bytes_written)
    }
}

/// An InputBuffer that read data from user space memory through a `MemoryManager`.
///
/// `USE_VMO` indicates whether the data should be read through a VMO. If `false`,
/// the user space memory is assumed to be mapped in the current address space and
/// the memory will be read directly instead of going through the VMO.
pub struct UserBuffersInputBuffer<'a, const USE_VMO: bool = false> {
    mm: &'a MemoryManager,
    buffers: Vec<UserBuffer>,
    available: usize,
    bytes_read: usize,
}

impl<'a, const USE_VMO: bool> std::fmt::Debug for UserBuffersInputBuffer<'a, USE_VMO> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("UserBuffersOutputBuffer")
            .field("buffers", &self.buffers)
            .field("available", &self.available)
            .field("bytes_read", &self.bytes_read)
            .finish()
    }
}

impl<'a, const USE_VMO: bool> UserBuffersInputBuffer<'a, USE_VMO> {
    fn new_inner(mm: &'a MemoryManager, buffers: Vec<UserBuffer>) -> Result<Self, Errno> {
        let (mut buffers, available) =
            UserBuffer::cap_buffers_to_max_rw_count(mm.maximum_valid_user_address, buffers)?;
        // Reverse the buffers as the element will be removed as they are handled.
        buffers.reverse();
        Ok(Self { mm, buffers, available, bytes_read: 0 })
    }
}

impl<'a> UserBuffersInputBuffer<'a> {
    pub fn new(mm: &'a MemoryManager, buffers: Vec<UserBuffer>) -> Result<Self, Errno> {
        Self::new_inner(mm, buffers)
    }

    pub fn new_at(
        mm: &'a MemoryManager,
        address: UserAddress,
        length: usize,
    ) -> Result<Self, Errno> {
        Self::new(mm, vec![UserBuffer { address, length }])
    }
}

impl<'a> UserBuffersInputBuffer<'a, true> {
    pub fn vmo_new(mm: &'a MemoryManager, buffers: Vec<UserBuffer>) -> Result<Self, Errno> {
        Self::new_inner(mm, buffers)
    }
}

impl<'a, const USE_VMO: bool> InputBuffer for UserBuffersInputBuffer<'a, USE_VMO> {
    fn peek_each(&mut self, callback: &mut InputBufferCallback<'_>) -> Result<usize, Errno> {
        let mut read = 0;
        for buffer in self.buffers.iter().rev() {
            if buffer.is_null() {
                continue;
            }
            let bytes = if USE_VMO {
                self.mm.vmo_read_memory_to_vec(buffer.address, buffer.length)
            } else {
                self.mm.read_memory_to_vec(buffer.address, buffer.length)
            }?;
            let result = callback(&bytes)?;
            if result > buffer.length {
                return error!(EINVAL);
            }
            read += result;
            if result != buffer.length {
                break;
            }
        }
        Ok(read)
    }

    fn drain(&mut self) -> usize {
        let result = self.available;
        self.bytes_read += self.available;
        self.available = 0;
        self.buffers.clear();
        result
    }

    fn advance(&mut self, mut length: usize) -> Result<(), Errno> {
        if length > self.available {
            return error!(EINVAL);
        }
        self.available -= length;
        self.bytes_read += length;
        while let Some(mut buffer) = self.buffers.pop() {
            if length < buffer.length {
                buffer.advance(length)?;
                self.buffers.push(buffer);
                return Ok(());
            }
            length -= buffer.length;
            if length == 0 {
                return Ok(());
            }
        }
        if length != 0 {
            error!(EINVAL)
        } else {
            Ok(())
        }
    }

    fn available(&self) -> usize {
        self.available
    }
    fn bytes_read(&self) -> usize {
        self.bytes_read
    }
}

/// An OutputBuffer that write data to an internal buffer.
#[derive(Debug)]
pub struct VecOutputBuffer {
    buffer: Vec<u8>,
    // Used to keep track of the requested capacity. `Vec::with_capacity` may
    // allocate more than the requested capacity so we can't rely on
    // `Vec::capacity` to return the expected capacity.
    capacity: usize,
}

impl VecOutputBuffer {
    pub fn new(capacity: usize) -> Self {
        Self { buffer: Vec::with_capacity(capacity), capacity }
    }

    pub fn data(&self) -> &[u8] {
        &self.buffer
    }

    pub fn reset(&mut self) {
        self.buffer.truncate(0)
    }
}

impl From<VecOutputBuffer> for Vec<u8> {
    fn from(data: VecOutputBuffer) -> Self {
        data.buffer
    }
}

impl OutputBuffer for VecOutputBuffer {
    fn write_each(&mut self, callback: &mut OutputBufferCallback<'_>) -> Result<usize, Errno> {
        let current_len = self.buffer.len();
        let written =
            callback(&mut self.buffer.spare_capacity_mut()[..self.capacity - current_len])?;
        if current_len + written > self.capacity {
            return error!(EINVAL);
        }
        // SAFETY: the vector is now initialized for an extra `written` bytes.
        unsafe { self.buffer.set_len(current_len + written) }
        Ok(written)
    }

    fn available(&self) -> usize {
        self.capacity - self.buffer.len()
    }

    fn bytes_written(&self) -> usize {
        self.buffer.len()
    }

    fn zero(&mut self) -> Result<usize, Errno> {
        let zeroed = self.capacity - self.buffer.len();
        self.buffer.resize(self.capacity, 0);
        Ok(zeroed)
    }
}

/// An InputBuffer that read data from an internal buffer.
#[derive(Debug)]
pub struct VecInputBuffer {
    buffer: Vec<u8>,

    // Invariant: `bytes_read <= buffer.len()` at all times.
    bytes_read: usize,
}

impl VecInputBuffer {
    pub fn new(buffer: &[u8]) -> Self {
        Self { buffer: buffer.to_vec(), bytes_read: 0 }
    }
}

impl From<Vec<u8>> for VecInputBuffer {
    fn from(buffer: Vec<u8>) -> Self {
        Self { buffer, bytes_read: 0 }
    }
}

impl InputBuffer for VecInputBuffer {
    fn peek_each(&mut self, callback: &mut InputBufferCallback<'_>) -> Result<usize, Errno> {
        let read = callback(&self.buffer[self.bytes_read..])?;
        if self.bytes_read + read > self.buffer.len() {
            return error!(EINVAL);
        }
        debug_assert!(self.bytes_read <= self.buffer.len());
        Ok(read)
    }
    fn advance(&mut self, length: usize) -> Result<(), Errno> {
        if length > self.buffer.len() {
            return error!(EINVAL);
        }
        self.bytes_read += length;
        debug_assert!(self.bytes_read <= self.buffer.len());
        Ok(())
    }
    fn available(&self) -> usize {
        self.buffer.len() - self.bytes_read
    }
    fn bytes_read(&self) -> usize {
        self.bytes_read
    }
    fn drain(&mut self) -> usize {
        let result = self.available();
        self.bytes_read += result;
        result
    }
}

impl VecInputBuffer {
    /// Read an object from userspace memory and increment the read position.
    ///
    /// Returns an error if there is not enough available bytes compared to the size of `T`.
    pub fn read_object<T: FromBytes>(&mut self) -> Result<T, Errno> {
        let size = std::mem::size_of::<T>();
        let end = self.bytes_read + size;
        if end > self.buffer.len() {
            return error!(EINVAL);
        }
        let obj = T::read_from(&self.buffer[self.bytes_read..end]).ok_or_else(|| errno!(EINVAL))?;
        self.bytes_read = end;
        debug_assert!(self.bytes_read <= self.buffer.len());
        Ok(obj)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{mm::PAGE_SIZE, testing::*};
    use usercopy::slice_to_maybe_uninit_mut;

    #[::fuchsia::test]
    async fn test_data_input_buffer() {
        let (_kernel, current_task) = create_kernel_and_task();
        let mm = current_task.mm();

        let page_size = *PAGE_SIZE;
        let addr = map_memory(&current_task, UserAddress::default(), 64 * page_size);

        let data: Vec<u8> = (0..1024).map(|i| (i % 256) as u8).collect();
        mm.write_memory(addr, &data).expect("failed to write test data");

        let input_iovec = vec![
            UserBuffer { address: addr, length: 25 },
            UserBuffer { address: addr + 64usize, length: 12 },
        ];

        // Test incorrect callback.
        {
            let mut input_buffer = UserBuffersInputBuffer::new(mm, input_iovec.clone())
                .expect("UserBuffersInputBuffer");
            assert!(input_buffer.peek_each(&mut |data| Ok(data.len() + 1)).is_err());
        }

        // Test drain
        {
            let mut input_buffer = UserBuffersInputBuffer::new(mm, input_iovec.clone())
                .expect("UserBuffersInputBuffer");
            assert_eq!(input_buffer.available(), 37);
            assert_eq!(input_buffer.bytes_read(), 0);
            assert_eq!(input_buffer.drain(), 37);
            assert_eq!(input_buffer.available(), 0);
            assert_eq!(input_buffer.bytes_read(), 37);
        }

        // Test read_all
        {
            let mut input_buffer = UserBuffersInputBuffer::new(mm, input_iovec.clone())
                .expect("UserBuffersInputBuffer");
            assert_eq!(input_buffer.available(), 37);
            assert_eq!(input_buffer.bytes_read(), 0);
            let buffer = input_buffer.read_all().expect("read_all");
            assert_eq!(input_buffer.available(), 0);
            assert_eq!(input_buffer.bytes_read(), 37);
            assert_eq!(buffer.len(), 37);
            assert_eq!(&data[..25], &buffer[..25]);
            assert_eq!(&data[64..76], &buffer[25..37]);
        }

        // Test read
        {
            let mut input_buffer =
                UserBuffersInputBuffer::new(mm, input_iovec).expect("UserBuffersInputBuffer");
            let mut buffer = [0; 50];
            assert_eq!(input_buffer.available(), 37);
            assert_eq!(input_buffer.bytes_read(), 0);
            assert_eq!(
                input_buffer
                    .read_exact(slice_to_maybe_uninit_mut(&mut buffer[0..20]))
                    .expect("read"),
                20
            );
            assert_eq!(input_buffer.available(), 17);
            assert_eq!(input_buffer.bytes_read(), 20);
            assert_eq!(
                input_buffer
                    .read_exact(slice_to_maybe_uninit_mut(&mut buffer[20..37]))
                    .expect("read"),
                17
            );
            assert!(input_buffer.read_exact(slice_to_maybe_uninit_mut(&mut buffer[37..])).is_err());
            assert_eq!(input_buffer.available(), 0);
            assert_eq!(input_buffer.bytes_read(), 37);
            assert_eq!(&data[..25], &buffer[..25]);
            assert_eq!(&data[64..76], &buffer[25..37]);
        }
    }

    #[::fuchsia::test]
    async fn test_data_output_buffer() {
        let (_kernel, current_task) = create_kernel_and_task();
        let mm = current_task.mm();

        let page_size = *PAGE_SIZE;
        let addr = map_memory(&current_task, UserAddress::default(), 64 * page_size);

        let output_iovec = vec![
            UserBuffer { address: addr, length: 25 },
            UserBuffer { address: addr + 64usize, length: 12 },
        ];

        let data: Vec<u8> = (0..1024).map(|i| (i % 256) as u8).collect();

        // Test incorrect callback.
        {
            let mut output_buffer = UserBuffersOutputBuffer::new(mm, output_iovec.clone())
                .expect("UserBuffersOutputBuffer");
            assert!(output_buffer.write_each(&mut |data| Ok(data.len() + 1)).is_err());
        }

        // Test write
        {
            let mut output_buffer =
                UserBuffersOutputBuffer::new(mm, output_iovec).expect("UserBuffersOutputBuffer");
            assert_eq!(output_buffer.available(), 37);
            assert_eq!(output_buffer.bytes_written(), 0);
            assert_eq!(output_buffer.write_all(&data[0..20]).expect("write"), 20);
            assert_eq!(output_buffer.available(), 17);
            assert_eq!(output_buffer.bytes_written(), 20);
            assert_eq!(output_buffer.write_all(&data[20..37]).expect("write"), 17);
            assert_eq!(output_buffer.available(), 0);
            assert_eq!(output_buffer.bytes_written(), 37);
            assert!(output_buffer.write_all(&data[37..50]).is_err());

            let buffer = mm.read_memory_to_array::<128>(addr).expect("failed to write test data");
            assert_eq!(&data[0..25], &buffer[0..25]);
            assert_eq!(&data[25..37], &buffer[64..76]);
        }
    }

    #[::fuchsia::test]
    fn test_vec_input_buffer() {
        let mut input_buffer = VecInputBuffer::new(b"helloworld");
        assert!(input_buffer.peek_each(&mut |data| Ok(data.len() + 1)).is_err());

        let mut input_buffer = VecInputBuffer::new(b"helloworld");
        assert_eq!(input_buffer.bytes_read(), 0);
        assert_eq!(input_buffer.available(), 10);
        assert_eq!(input_buffer.drain(), 10);
        assert_eq!(input_buffer.bytes_read(), 10);
        assert_eq!(input_buffer.available(), 0);

        let mut input_buffer = VecInputBuffer::new(b"helloworld");
        assert_eq!(input_buffer.bytes_read(), 0);
        assert_eq!(input_buffer.available(), 10);
        assert_eq!(&input_buffer.read_all().expect("read_all"), b"helloworld");
        assert_eq!(input_buffer.bytes_read(), 10);
        assert_eq!(input_buffer.available(), 0);

        let mut input_buffer = VecInputBuffer::new(b"helloworld");
        let mut buffer = [0; 5];
        assert_eq!(
            input_buffer.read_exact(slice_to_maybe_uninit_mut(&mut buffer)).expect("read"),
            5
        );
        assert_eq!(input_buffer.bytes_read(), 5);
        assert_eq!(input_buffer.available(), 5);
        assert_eq!(&buffer, b"hello");
        assert_eq!(
            input_buffer.read_exact(slice_to_maybe_uninit_mut(&mut buffer)).expect("read"),
            5
        );
        assert_eq!(input_buffer.bytes_read(), 10);
        assert_eq!(input_buffer.available(), 0);
        assert_eq!(&buffer, b"world");
        assert!(input_buffer.read_exact(slice_to_maybe_uninit_mut(&mut buffer)).is_err());

        // Test read_object
        let mut input_buffer = VecInputBuffer::new(b"hello");
        assert_eq!(input_buffer.bytes_read(), 0);
        let buffer: [u8; 3] = input_buffer.read_object().expect("read_object");
        assert_eq!(&buffer, b"hel");
        assert_eq!(input_buffer.bytes_read(), 3);
        let buffer: [u8; 2] = input_buffer.read_object().expect("read_object");
        assert_eq!(&buffer, b"lo");
        assert_eq!(input_buffer.bytes_read(), 5);
        assert!(input_buffer.read_object::<[u8; 1]>().is_err());
        assert_eq!(input_buffer.bytes_read(), 5);

        let mut input_buffer = VecInputBuffer::new(b"hello");
        assert_eq!(input_buffer.bytes_read(), 0);
        assert!(input_buffer.read_object::<[u8; 100]>().is_err());
        assert_eq!(input_buffer.bytes_read(), 0);
    }

    #[::fuchsia::test]
    fn test_vec_output_buffer() {
        let mut output_buffer = VecOutputBuffer::new(10);
        assert!(output_buffer.write_each(&mut |data| Ok(data.len() + 1)).is_err());
        assert_eq!(output_buffer.bytes_written(), 0);
        assert_eq!(output_buffer.available(), 10);
        assert_eq!(output_buffer.write_all(b"hello").expect("write"), 5);
        assert_eq!(output_buffer.bytes_written(), 5);
        assert_eq!(output_buffer.available(), 5);
        assert_eq!(output_buffer.data(), b"hello");
        assert_eq!(output_buffer.write_all(b"world").expect("write"), 5);
        assert_eq!(output_buffer.bytes_written(), 10);
        assert_eq!(output_buffer.available(), 0);
        assert_eq!(output_buffer.data(), b"helloworld");
        assert!(output_buffer.write_all(b"foo").is_err());
        let data: Vec<u8> = output_buffer.into();
        assert_eq!(data, b"helloworld".to_vec());
    }

    #[::fuchsia::test]
    fn test_vec_write_buffer() {
        let mut input_buffer = VecInputBuffer::new(b"helloworld");
        let mut output_buffer = VecOutputBuffer::new(20);
        assert_eq!(output_buffer.write_buffer(&mut input_buffer).expect("write_buffer"), 10);
        assert_eq!(output_buffer.data(), b"helloworld");
    }
}
