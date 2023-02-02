// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::fs::buffers::OutputBuffer;
use crate::task::CurrentTask;
use crate::types::*;

/// State for a file whose contents is generated by an iterator returning one chunk at a time.
///
/// This struct is only the state and does not implement FileOps (because doing so would require
/// GATs). To use, store this inside your FileOps and call SeqFile::read_at from your read_at impl.
/// Pass the task/offset/data through from read_at and also pass your iterator. A new iterator is
/// created for each read call and its scope limited to that read call, which allows it to be a
/// closure that captures local variables of the read_at implementation.
///
/// The iterator takes the cursor, outputs the next chunk of data in the sequences, and returns the
/// the advanced cursor value. At the start of iteration, the cursor is Default::default().
///
/// Simple example:
/// ```
/// [#derive(Default)]
/// struct IntegersFile {
///     seq: Mutex<SeqFileState<i32>>,
/// }
/// impl FileOps for IntegersFile {
///     fn read_at(
///         &self,
///         _file: &FileObject,
///         current_task: &CurrentTask,
///         offset: usize,
///         data: &mut dyn InputBuffer,
///     ) -> Result<usize, Errno> {
///         // The cursor starts at i32::default(), which is 0.
///         self.seq.lock().read_at(current_task, |cursor: i32, sink: &mut SeqFileBuf| {
///             write!(sink, "{}", cursor)?;
///             Ok(Some(cursor + 1))
///         }, offset, data)
///     }
/// }
/// ```
pub struct SeqFileState<C: Default> {
    /// The current position in the sequence. This is an opaque object. Stepping the iterator
    /// replaces it with the next value in the sequence.
    cursor: Option<C>,

    /// Buffer for upcoming data in the sequence. Read calls will expand this buffer until it is
    /// big enough and then copy out data from it.
    buf: SeqFileBuf,

    /// The current seek offset in the file. The first byte in the buffer is at this offset in the
    /// file.
    ///
    /// If a read has an offset greater than this, bytes will be generated from the iterator
    /// and skipped. If a read has an offset less than this, all state is reset and iteration
    /// starts from the beginning until it reaches the requested offset.
    byte_offset: usize,
}

impl<C: Default> Default for SeqFileState<C> {
    fn default() -> Self {
        Self { cursor: Some(C::default()), buf: SeqFileBuf::default(), byte_offset: 0 }
    }
}

impl<C: Default> SeqFileState<C> {
    fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn read_at<'a>(
        &mut self,
        _current_task: &CurrentTask,
        mut iter: impl SeqIterator<'a, C>,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        if offset < self.byte_offset {
            self.reset();
        }
        let read_size = data.available();

        // 1. Grow the buffer until either EOF or it's at least as big as the read request
        while self.byte_offset + self.buf.0.len() < offset + read_size {
            let cursor = if let Some(cursor) = std::mem::take(&mut self.cursor) {
                cursor
            } else {
                break;
            };
            let mut buf = std::mem::take(&mut self.buf);
            self.cursor = iter.next(cursor, &mut buf).map_err(|e| {
                // Reset everything on failure
                self.reset();
                e
            })?;
            self.buf = buf;

            // If the seek pointer is ahead of our current byte offset, we will generate data that
            // needs to be thrown away. Calculation for that is here.
            let to_drain = std::cmp::min(offset - self.byte_offset, self.buf.0.len());
            self.buf.0.drain(..to_drain);
            self.byte_offset += to_drain;
        }
        std::mem::drop(iter);

        // 2. Write out as much of the buffer as possible and shift the rest down
        let written = data.write(&self.buf.0)?;
        self.buf.0.drain(..written);
        self.byte_offset += written;
        Ok(written)
    }
}

#[derive(Default)]
pub struct SeqFileBuf(Vec<u8>);
impl SeqFileBuf {
    pub fn write(&mut self, data: &[u8]) {
        self.0.extend_from_slice(data);
    }
    pub fn write_fmt(&mut self, args: std::fmt::Arguments<'_>) -> Result<usize, Errno> {
        let start_size = self.0.len();
        std::io::Write::write_fmt(&mut self.0, args).map_err(|_| errno!(EINVAL))?;
        let end_size = self.0.len();
        Ok(end_size - start_size)
    }
}

pub trait SeqIterator<'a, C> {
    /// Appends the next chunk of the file to the buffer and advances the cursor. A return of None
    /// means end-of-file.
    fn next(&mut self, cursor: C, sink: &mut SeqFileBuf) -> Result<Option<C>, Errno>;
}
impl<'a, F, C> SeqIterator<'a, C> for F
where
    F: FnMut(C, &mut SeqFileBuf) -> Result<Option<C>, Errno> + 'a,
{
    fn next(&mut self, offset: C, sink: &mut SeqFileBuf) -> Result<Option<C>, Errno> {
        self(offset, sink)
    }
}

#[cfg(test)]
mod test {
    use super::*;

    use crate::fs::buffers::{InputBuffer, VecOutputBuffer};
    use crate::fs::*;
    use crate::task::*;
    use crate::testing::*;

    use crate::lock::Mutex;

    /// A test FileOps implementation that returns 256 bytes. Each byte is equal to its offset in
    /// the file.
    #[cfg(test)]
    #[derive(Default)]
    struct TestSeqFile {
        seq: Mutex<SeqFileState<u8>>,
    }

    impl FileOps for TestSeqFile {
        fileops_impl_seekable!();
        fileops_impl_nonblocking!();

        fn read_at(
            &self,
            _file: &FileObject,
            current_task: &CurrentTask,
            offset: usize,
            data: &mut dyn OutputBuffer,
        ) -> Result<usize, Errno> {
            self.seq.lock().read_at(
                current_task,
                |i: u8, sink: &mut SeqFileBuf| {
                    sink.write(&[i]);
                    Ok(if i == u8::MAX { None } else { Some(i + 1) })
                },
                offset,
                data,
            )
        }

        fn write_at(
            &self,
            _file: &FileObject,
            _current_task: &CurrentTask,
            _offset: usize,
            _data: &mut dyn InputBuffer,
        ) -> Result<usize, Errno> {
            error!(ENOSYS)
        }
    }

    #[::fuchsia::test]
    fn test_stuff() -> Result<(), Errno> {
        let (_kern, current_task) = create_kernel_and_task();
        let file = Anon::new_file(&current_task, Box::<TestSeqFile>::default(), OpenFlags::RDONLY);

        let read_test = |offset: usize, length: usize| -> Result<Vec<u8>, Errno> {
            let mut buffer = VecOutputBuffer::new(length);
            file.read_at(&current_task, offset, &mut buffer)?;
            Ok(buffer.data().to_vec())
        };

        assert_eq!(read_test(0, 2)?, &[0, 1]);
        assert_eq!(read_test(2, 2)?, &[2, 3]);
        assert_eq!(read_test(4, 4)?, &[4, 5, 6, 7]);
        assert_eq!(read_test(0, 2)?, &[0, 1]);
        assert_eq!(read_test(4, 2)?, &[4, 5]);
        Ok(())
    }
}
