// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Defines the buffer traits needed by the TCP implementation. The traits
//! in this module provide a common interface for platform-specific buffers
//! used by TCP.

use alloc::{vec, vec::Vec};
use core::{cmp, convert::TryFrom, num::NonZeroUsize, ops::Range};

use crate::transport::tcp::{
    segment::Payload,
    seqnum::{SeqNum, WindowSize},
};

/// Common super trait for both sending and receiving buffer.
pub trait Buffer: Default {
    /// Returns the number of bytes in the buffer that can be read.
    fn len(&self) -> usize;

    /// Returns the maximum number of bytes that can reside in the buffer.
    fn cap(&self) -> usize;
}

/// Trait for receiving end of an TCP connection.
pub trait ReceiveBuffer: Buffer {
    /// Writes `data` into the buffer at `offset`.
    ///
    /// Returns the number of bytes written.
    fn write_at<P: Payload>(&mut self, offset: usize, data: &P) -> usize;

    /// Marks `count` bytes available for the application to read.
    ///
    /// # Panics
    ///
    /// Panics if the caller attempts to make more bytes readable than the
    /// buffer has capacity for. That is, this method panics if
    /// `self.len() + count > self.cap()`
    fn make_readable(&mut self, count: usize);

    /// Calls `f` with contiguous sequence of readable bytes in the buffer and
    /// discards the amount of bytes returned by `f`.
    ///
    /// # Panics
    ///
    /// Panics if the closure wants to discard more bytes than possible, i.e.,
    /// the value returned by `f` is greater than `self.len()`.
    fn read_with<'a, F>(&'a mut self, f: F) -> usize
    where
        F: for<'b> FnOnce(&'b [&'a [u8]]) -> usize;
}

/// A circular buffer implementation.
#[derive(Debug, Clone)]
pub(super) struct RingBuffer {
    // It must not be empty because we can not divide a number by 0.
    storage: Vec<u8>,
    // The index where the reader starts to read.
    head: usize,
    // Anything between [head, head+len) is readable.
    len: usize,
}

impl RingBuffer {
    /// Creates a new `RingBuffer`.
    pub(super) fn new(capacity: NonZeroUsize) -> Self {
        Self { storage: vec![0; capacity.get()], head: 0, len: 0 }
    }
}

impl Default for RingBuffer {
    fn default() -> Self {
        Self::new(NonZeroUsize::new(WindowSize::DEFAULT.into()).unwrap())
    }
}

impl Buffer for RingBuffer {
    fn len(&self) -> usize {
        self.len
    }

    fn cap(&self) -> usize {
        self.storage.len()
    }
}

impl ReceiveBuffer for RingBuffer {
    fn write_at<P: Payload>(&mut self, offset: usize, data: &P) -> usize {
        let Self { storage, head, len } = self;
        let available = storage.len() - *len;
        if offset > available {
            return 0;
        }
        let start_at = (*head + *len + offset) % storage.len();
        let to_write = cmp::min(data.len(), available);
        // Write the first part of the payload.
        let first_len = cmp::min(to_write, storage.len() - start_at);
        data.partial_copy(0, &mut storage[start_at..start_at + first_len]);
        // If we have more to write, wrap around and start from the beginning
        // of the storage.
        if to_write > first_len {
            data.partial_copy(first_len, &mut storage[0..to_write - first_len]);
        }
        to_write
    }

    fn make_readable(&mut self, count: usize) {
        assert!(count <= self.cap() - self.len());
        self.len += count;
    }

    fn read_with<'a, F>(&'a mut self, f: F) -> usize
    where
        F: for<'b> FnOnce(&'b [&'a [u8]]) -> usize,
    {
        let Self { storage, head, len } = self;
        // Don't read past the end of storage.
        let end = *head + *len;
        let nread = if end > storage.len() {
            let first_part = &storage[*head..storage.len()];
            let second_part = &storage[0..*len - first_part.len()];
            f(&[first_part, second_part][..])
        } else {
            let all_bytes = &storage[*head..end];
            f(&[all_bytes][..])
        };
        assert!(nread <= *len);
        *len -= nread;
        *head = (*head + nread) % storage.len();
        nread
    }
}

/// Assembler for out-of-order segment data.
#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq, Eq))]
pub(super) struct Assembler {
    // `nxt` is the next sequence number to be expected. It should be before
    // any sequnce number of the out-of-order sequence numbers we keep track
    // of below.
    nxt: SeqNum,
    // Holds all the sequence number ranges which we have already received.
    // These ranges are sorted and should have a gap of at least 1 byte
    // between any consecutive two. These ranges should only be after `nxt`.
    outstanding: Vec<Range<SeqNum>>,
}

impl Assembler {
    /// Creates a new assembler.
    pub(super) fn new(nxt: SeqNum) -> Self {
        Self { outstanding: Vec::new(), nxt }
    }

    /// Returns the next sequence number expected to be received.
    pub(super) fn nxt(&self) -> SeqNum {
        self.nxt
    }

    /// Inserts a received segment.
    ///
    /// The newly added segment will be merged with as many existing ones as
    /// possible and `nxt` will be advanced to the highest ACK number possible.
    ///
    /// Returns number of bytes that should be available for the application
    /// to consume.
    ///
    /// # Panics
    ///
    /// Panics if `start` is after `end` or if `start` is before `self.nxt`.
    pub(super) fn insert(&mut self, Range { start, end }: Range<SeqNum>) -> usize {
        assert!(!start.after(end));
        assert!(!start.before(self.nxt));
        self.insert_inner(start..end);

        let Self { outstanding, nxt } = self;
        if outstanding[0].start == *nxt {
            let advanced = outstanding.remove(0);
            *nxt = advanced.end;
            // The following unwrap is safe because it is invalid to have
            // have a range where `end` is before `start`.
            usize::try_from(advanced.end - advanced.start).unwrap()
        } else {
            0
        }
    }

    fn insert_inner(&mut self, Range { mut start, mut end }: Range<SeqNum>) {
        let Self { outstanding, nxt: _ } = self;

        if start == end {
            return;
        }

        if outstanding.is_empty() {
            outstanding.push(Range { start, end });
            return;
        }

        // Search for the first segment whose `start` is greater.
        let first_after = {
            let mut cur = 0;
            while cur < outstanding.len() {
                if start.before(outstanding[cur].start) {
                    break;
                }
                cur += 1;
            }
            cur
        };

        let mut merge_right = 0;
        for range in &outstanding[first_after..outstanding.len()] {
            if end.before(range.start) {
                break;
            }
            merge_right += 1;
            if end.before(range.end) {
                end = range.end;
                break;
            }
        }

        let mut merge_left = 0;
        for range in (&outstanding[0..first_after]).iter().rev() {
            if start.after(range.end) {
                break;
            }
            // There is no guarantee that `end.after(range.end)`, not doing
            // the following may shrink existing coverage. For example:
            // range.start = 0, range.end = 10, start = 0, end = 1, will result
            // in only 0..1 being tracked in the resulting assembler. We didn't
            // do the symmetrical thing above when merging to the right because
            // the search guarantees that `start.before(range.start)`, thus the
            // problem doesn't exist there. The asymmetry rose from the fact
            // that we used `start` to perform the search.
            if end.before(range.end) {
                end = range.end;
            }
            merge_left += 1;
            if start.after(range.start) {
                start = range.start;
                break;
            }
        }

        if merge_left == 0 && merge_right == 0 {
            // If the new segment cannot merge with any of its neighbors, we
            // add a new entry for it.
            outstanding.insert(first_after, Range { start, end });
        } else {
            // Otherwise, we put the new segment at the left edge of the merge
            // window and remove all other existing segments.
            let left_edge = first_after - merge_left;
            let right_edge = first_after + merge_right;
            outstanding[left_edge] = Range { start, end };
            for i in right_edge..outstanding.len() {
                outstanding[i - merge_left - merge_right + 1] = outstanding[i].clone();
            }
            outstanding.truncate(outstanding.len() - merge_left - merge_right + 1);
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::transport::tcp::seqnum::WindowSize;
    use proptest::{
        proptest,
        strategy::{Just, Strategy},
        test_runner::Config,
    };
    use proptest_support::failed_seeds;
    use test_case::test_case;

    proptest! {
        #![proptest_config(Config {
            // Add all failed seeds here.
            failure_persistence: failed_seeds!(),
            ..Config::default()
        })]

        #[test]
        fn assembler_insertion(insertions in proptest::collection::vec(assembler::insertions(), 200)) {
            let mut assembler = Assembler::new(SeqNum::new(0));
            let mut num_insertions_performed = 0;
            let mut min_seq = SeqNum::new(WindowSize::MAX.into());
            let mut max_seq = SeqNum::new(0);
            for Range { start, end } in insertions {
                if min_seq.after(start) {
                    min_seq = start;
                }
                if max_seq.before(end) {
                    max_seq = end;
                }
                // assert that it's impossible to have more entries than the
                // number of insertions performed.
                assert!(assembler.outstanding.len() <= num_insertions_performed);
                assembler.insert_inner(start..end);
                num_insertions_performed += 1;

                // assert that the ranges are sorted and don't overlap with
                // each other.
                for i in 1..assembler.outstanding.len() {
                    assert!(assembler.outstanding[i-1].end.before(assembler.outstanding[i].start));
                }
            }
            assert_eq!(assembler.outstanding.first().unwrap().start, min_seq);
            assert_eq!(assembler.outstanding.last().unwrap().end, max_seq);
        }

        #[test]
        fn ring_buffer_make_readable((mut rb, avail) in ring_buffer::with_available()) {
            let old_storage = rb.storage.clone();
            let old_head = rb.head;
            let old_len = rb.len();
            rb.make_readable(avail);
            // Assert that length is updated but everything else is unchanged.
            let RingBuffer { storage, head, len } = rb;
            assert_eq!(len, old_len + avail);
            assert_eq!(head, old_head);
            assert_eq!(storage, old_storage);
        }

        #[test]
        fn ring_buffer_write_at((mut rb, offset, data) in ring_buffer::with_offset_data()) {
            let old_head = rb.head;
            let old_len = rb.len();
            assert_eq!(rb.write_at(offset, &&data[..]), data.len());
            assert_eq!(rb.head, old_head);
            assert_eq!(rb.len(), old_len);
            for i in 0..data.len() {
                let masked = (rb.head + rb.len + offset + i) % rb.storage.len();
                // Make sure that data are written.
                assert_eq!(rb.storage[masked], data[i]);
                rb.storage[masked] = 0;
            }
            // And the other parts of the storage are untouched.
            assert_eq!(rb.storage, vec![0; rb.storage.len()])
        }

        #[test]
        fn ring_buffer_read_with((mut rb, expected, consume) in ring_buffer::with_read_data()) {
            assert_eq!(rb.len(), expected.len());
            let nread = rb.read_with(|readable| {
                assert!(readable.len() == 1 || readable.len() == 2);
                let got = readable.concat();
                assert_eq!(got, expected);
                consume
            });
            assert_eq!(nread, consume);
            assert_eq!(rb.len(), expected.len() - consume);
        }
    }

    #[test_case([Range { start: 0, end: 10 }]
        => Assembler { outstanding: vec![], nxt: SeqNum::new(10) })]
    #[test_case([Range{ start: 10, end: 15 }, Range { start: 5, end: 10 }]
        => Assembler { outstanding: vec![Range { start: SeqNum::new(5), end: SeqNum::new(15) }], nxt: SeqNum::new(0)})]
    #[test_case([Range{ start: 10, end: 15 }, Range { start: 0, end: 5 }, Range { start: 5, end: 10 }]
        => Assembler { outstanding: vec![], nxt: SeqNum::new(15) })]
    #[test_case([Range{ start: 10, end: 15 }, Range { start: 5, end: 10 }, Range { start: 0, end: 5 }]
        => Assembler { outstanding: vec![], nxt: SeqNum::new(15) })]
    fn assembler_examples(ops: impl IntoIterator<Item = Range<u32>>) -> Assembler {
        let mut assembler = Assembler::new(SeqNum::new(0));
        for Range { start, end } in ops.into_iter() {
            let _advanced = assembler.insert(SeqNum::new(start)..SeqNum::new(end));
        }
        assembler
    }

    #[test]
    fn ring_buffer_example() {
        let mut rb = RingBuffer::new(NonZeroUsize::new(16).unwrap());
        assert_eq!(rb.write_at(5, &"World".as_bytes()), 5);
        assert_eq!(rb.write_at(0, &"Hello".as_bytes()), 5);
        rb.make_readable(10);
        assert_eq!(
            rb.read_with(|readable| {
                assert_eq!(readable, &["HelloWorld".as_bytes()]);
                5
            }),
            5
        );
        assert_eq!(
            rb.read_with(|readable| {
                assert_eq!(readable, &["World".as_bytes()]);
                readable[0].len()
            }),
            5
        );
        assert_eq!(rb.write_at(0, &"HelloWorld".as_bytes()), 10);
        rb.make_readable(10);
        assert_eq!(
            rb.read_with(|readable| {
                assert_eq!(readable, &["HelloW".as_bytes(), "orld".as_bytes()]);
                6
            }),
            6
        );
        assert_eq!(rb.len(), 4);
        assert_eq!(
            rb.read_with(|readable| {
                assert_eq!(readable, &["orld".as_bytes()]);
                4
            }),
            4
        );
        assert_eq!(rb.len(), 0);
    }

    mod assembler {
        use super::*;
        pub(super) fn insertions() -> impl Strategy<Value = Range<SeqNum>> {
            (0..=u32::from(WindowSize::MAX)).prop_flat_map(|start| {
                (start + 1..=u32::from(WindowSize::MAX)).prop_flat_map(move |end| {
                    Just(Range { start: SeqNum::new(start), end: SeqNum::new(end) })
                })
            })
        }
    }

    mod ring_buffer {
        use super::*;

        fn arb_ring_buffer_args() -> impl Strategy<Value = (usize, usize, usize)> {
            // Use a small capacity so that we have a higher chance to exercise
            // wrapping around logic.
            (1..=32usize).prop_flat_map(|cap| {
                //  cap      head     len
                (Just(cap), 0..cap, 0..=cap)
            })
        }

        /// A strategy for a [`RingBuffer`] and a valid offset to write.
        pub(super) fn with_available() -> impl Strategy<Value = (RingBuffer, usize)> {
            arb_ring_buffer_args().prop_flat_map(|(cap, head, len)| {
                (Just(RingBuffer { storage: vec![0; cap], head, len }), 0..=cap - len)
            })
        }

        /// A strategy for a [`RingBuffer`], a valid offset and data to write.
        pub(super) fn with_offset_data() -> impl Strategy<Value = (RingBuffer, usize, Vec<u8>)> {
            arb_ring_buffer_args().prop_flat_map(|(cap, head, len)| {
                (0..=cap - len).prop_flat_map(move |offset| {
                    (0..=cap - len - offset).prop_flat_map(move |data_len| {
                        (
                            Just(RingBuffer { storage: vec![0; cap], head, len }),
                            Just(offset),
                            proptest::collection::vec(1..=u8::MAX, data_len),
                        )
                    })
                })
            })
        }

        /// A strategy for a [`RingBuffer`], its readable data, and how many
        /// bytes to consume.
        pub(super) fn with_read_data() -> impl Strategy<Value = (RingBuffer, Vec<u8>, usize)> {
            arb_ring_buffer_args().prop_flat_map(|(cap, head, len)| {
                proptest::collection::vec(1..=u8::MAX, len).prop_flat_map(move |data| {
                    // Fill the RingBuffer with the data.
                    let mut rb = RingBuffer { storage: vec![0; cap], head, len: 0 };
                    assert_eq!(rb.write_at(0, &&data[..]), len);
                    rb.make_readable(len);
                    (Just(rb), Just(data), 0..=len)
                })
            })
        }
    }
}
