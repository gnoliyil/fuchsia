// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_hardware_audio::*;
use fuchsia_async as fasync;
use fuchsia_zircon::{self as zx, DurationNum, HandleBased};
use futures::task::{Context, Poll, Waker};
use futures::FutureExt;
use tracing::debug;

use crate::driver::frames_from_duration;
use crate::types::{AudioSampleFormat, Error, Result};

/// A FrameVmo wraps a VMO with time tracking.  When a FrameVmo is started, it
/// assumes that audio frame data is being written to the VMO at the rate specific
/// in the format it is set to.  Frames that represent a time range can be
/// retrieved from the buffer.
pub(crate) struct FrameVmo {
    /// Ring Buffer VMO. Size zero until the ringbuffer is established.  Shared with
    /// the AudioFrameStream given back to the client.
    vmo: zx::Vmo,

    /// Cached size of the ringbuffer, in bytes.  Used to avoid zx_get_size() syscalls.
    size: usize,

    /// The time that streaming was started.
    /// Used to calculate the currently available frames.
    /// None if the stream is not started.
    start_time: Option<fasync::Time>,

    /// A waker to wake if we have been polled before enough frames are available, or
    /// before we have been started.
    waker: Option<Waker>,

    /// A timer which will fire when there are enough frames to return min_duration frames.
    timer: Option<fasync::Timer>,

    /// The number of frames per second.
    frames_per_second: u32,

    /// The audio format of the frames.
    format: Option<AudioSampleFormat>,

    /// Number of bytes per frame, 0 if format is not set.
    bytes_per_frame: usize,

    /// Frames between notifications. Zero if position notifications aren't enabled
    frames_between_notifications: usize,

    /// A position responder that will be used to notify when the ringbuffer has been read.
    /// This will be notified the correct number of times based on the last read position using
    /// `get_frames`
    position_responder: Option<RingBufferWatchClockRecoveryPositionInfoResponder>,

    /// The next frame index we are due to notify the position.
    /// usize::MAX if position notifications are not enabled.
    next_notify_frame: usize,
}

impl FrameVmo {
    pub(crate) fn new() -> Result<FrameVmo> {
        Ok(FrameVmo {
            vmo: zx::Vmo::create(0).map_err(|e| Error::IOError(e))?,
            size: 0,
            start_time: None,
            waker: None,
            frames_per_second: 0,
            format: None,
            timer: None,
            bytes_per_frame: 0,
            frames_between_notifications: 0,
            position_responder: None,
            next_notify_frame: usize::MAX,
        })
    }

    /// Set the format of this buffer.   Returns a handle representing the VMO.
    /// `frames` is the number of frames the VMO should be able to hold.
    pub(crate) fn set_format(
        &mut self,
        frames_per_second: u32,
        format: AudioSampleFormat,
        channels: u16,
        frames: usize,
        notifications_per_ring: u32,
    ) -> Result<zx::Vmo> {
        if self.start_time.is_some() {
            return Err(Error::InvalidState);
        }
        let bytes_per_frame = format.compute_frame_size(channels as usize)?;
        let new_size = bytes_per_frame * frames;
        self.vmo = zx::Vmo::create(new_size as u64).map_err(|e| Error::IOError(e))?;
        self.bytes_per_frame = bytes_per_frame;
        self.size = new_size;
        self.format = Some(format);
        self.frames_per_second = frames_per_second;
        if notifications_per_ring > 0 {
            // TODO(fxbug.dev/92947) : consider rounding this up to avoid delivering an extra
            // notification sometimes.
            // (and always align the frames notified to the beginning of the buffer)
            self.frames_between_notifications = frames / notifications_per_ring as usize;
        }
        Ok(self.vmo.duplicate_handle(zx::Rights::SAME_RIGHTS).map_err(|e| Error::IOError(e))?)
    }

    pub(crate) fn set_position_responder(
        &mut self,
        position_responder: RingBufferWatchClockRecoveryPositionInfoResponder,
    ) {
        self.position_responder = Some(position_responder);
    }

    /// Start the audio clock for the buffer at `time`
    /// Can't start if the format has not been set.
    pub(crate) fn start(&mut self, time: fasync::Time) -> Result<()> {
        if self.start_time.is_some() || self.format.is_none() {
            return Err(Error::InvalidState);
        }
        self.start_time = Some(time);
        if self.frames_between_notifications > 0 {
            self.next_notify_frame = 0;
        }
        if let Some(w) = self.waker.take() {
            debug!("ringing the waker to start");
            w.wake();
        }
        Ok(())
    }

    /// Stop the audio clock in the buffer.
    /// returns true if the streaming was stopped.
    pub(crate) fn stop(&mut self) -> Result<bool> {
        if self.format.is_none() {
            return Err(Error::InvalidState);
        }
        let start_time = self.start_time.take();
        Ok(start_time.is_some())
    }

    /// Set the next-available-frame timer to fire `count` frames in the future from now.
    /// Replaces the currently active timer.
    /// Returns Poll::Pending if it was set to a time in the future, or Poll::Ready(()) if it would
    /// be ready now.
    fn reset_frame_timer(&mut self, deadline: fasync::Time, cx: &mut Context<'_>) -> Poll<()> {
        let mut timer = fasync::Timer::new(deadline);
        let Poll::Pending = timer.poll_unpin(cx) else {
            return Poll::Ready(());
        };
        self.timer = Some(timer);
        Poll::Pending
    }

    /// Calculate the number of bytes that `num` frames will occupy given the current set format.
    /// Returns None if no format has been set.
    pub(crate) fn bytecount_frames(&self, num: usize) -> Option<usize> {
        (self.bytes_per_frame != 0).then_some(self.bytes_per_frame * num)
    }

    /// Retrieve audio frames available starting at `next_frame`.
    /// `next_frame` is a frame index, starting at zero when the buffer timer was started.
    /// Fills `buf` with as many frames as will fill the slice.
    /// Returns the index of the last frame returned, and a count of frames that were missed
    /// due to buffer overwrite (slow polling).
    /// If the buffer cannot be filled, Poll::Pending is returned and the waker
    /// associated with `cx` will be woken at a time when enough data is expected to be
    /// available.
    /// If polled before start, the waker associate with `cx` will be woken when started.
    /// Returns Err(Error::InvalidArgs) if the buffer size is not a multiple of a positive number
    /// of frames, or the buffer is larger than the possible available frames in the buffer.
    /// See `bytecount_frames` to calculate buffer sizes for a count of frames.
    ///
    /// Calling poll_frames with buffers of varying size is not expected, but should be supported.
    pub(crate) fn poll_frames(
        &mut self,
        mut next_frame: usize,
        buf: &mut [u8],
        cx: &mut Context<'_>,
    ) -> Poll<Result<(usize, usize)>> {
        let start_time = match self.start_time.as_ref() {
            None => {
                self.waker = Some(cx.waker().clone());
                return Poll::Pending;
            }
            Some(time) => *time,
        };
        assert!(self.bytes_per_frame != 0, "bytes per frame not set before start");
        if buf.len() == 0 || (buf.len() % self.bytes_per_frame) != 0 {
            return Poll::Ready(Err(Error::InvalidArgs));
        }
        let count = buf.len() / self.bytes_per_frame;
        let vmo_frames = self.size / self.bytes_per_frame;
        if count > vmo_frames || count == 0 {
            return Poll::Ready(Err(Error::InvalidArgs));
        }

        let now = fasync::Time::now();
        // Start time is the zero frame.
        let current_frame_count = self.frames_before(now);
        let oldest_frame_count = current_frame_count.saturating_sub(vmo_frames);

        let missing_frames = if oldest_frame_count <= next_frame {
            0
        } else {
            oldest_frame_count - next_frame as usize
        };

        if missing_frames > 0 {
            next_frame = oldest_frame_count;
        }

        let frames_available = current_frame_count.saturating_sub(next_frame);
        if frames_available < count {
            // Set the timer to wake when enough frames will be available
            let deadline = now + self.duration_from_frames(count - frames_available);
            let () = futures::ready!(self.reset_frame_timer(deadline, cx));
            // Somehow raced to a time where they are available, try again.
            return self.poll_frames(next_frame, buf, cx);
        }

        let mut frame_from_idx = next_frame % vmo_frames;
        let frame_until = next_frame + count;
        let mut frame_until_idx = frame_until % vmo_frames;

        // If the until idx is behind the from idx, we need to wrap-around.
        if frame_from_idx >= frame_until_idx {
            frame_until_idx += vmo_frames;
        }

        let mut ndx = 0;

        // If we wrap around, read to the end into the buffer, then set up to read the rest.
        if frame_until_idx > vmo_frames {
            let frames_to_read = vmo_frames - frame_from_idx;
            let bytes_to_read = frames_to_read * self.bytes_per_frame;
            let byte_start = frame_from_idx * self.bytes_per_frame;
            self.vmo
                .read(&mut buf[0..bytes_to_read], byte_start as u64)
                .map_err(|e| Error::IOError(e))?;
            frame_from_idx = 0;
            frame_until_idx -= vmo_frames;
            ndx = bytes_to_read;
        }

        let frames_to_read = frame_until_idx - frame_from_idx;
        let bytes_to_read = frames_to_read * self.bytes_per_frame;
        let byte_start = frame_from_idx * self.bytes_per_frame;

        self.vmo
            .read(&mut buf[ndx..ndx + bytes_to_read], byte_start as u64)
            .map_err(|e| Error::IOError(e))?;

        // We're returning frames from just after the `next_frame` up to `frame_until`
        // Notify if we have a position responder and we read past the clock notification time.
        if frame_until > self.next_notify_frame && self.position_responder.is_some() {
            let end_frame_time = start_time + self.duration_from_frames(frame_until);
            let notify_frame_bytes = frame_until_idx * self.bytes_per_frame;
            let next_notify_frame = self.next_notify_frame + self.frames_between_notifications;
            let mut info = RingBufferPositionInfo {
                timestamp: end_frame_time.into_nanos(),
                position: notify_frame_bytes as u32,
            };
            if let Some(Ok(())) = self.position_responder.take().map(|t| t.send(&mut info)) {
                self.next_notify_frame = next_notify_frame;
            }
        }
        // Index of the last frame returned is -1 because we index from zero.
        Poll::Ready(Ok((frame_until - 1, missing_frames)))
    }

    /// Count of the number of frames that have ended before `time`.
    fn frames_before(&self, time: fasync::Time) -> usize {
        match self.start_time.as_ref() {
            Some(start) if time > *start => self.frames_from_duration(time - *start) as usize,
            _ => 0,
        }
    }

    /// Frames from duration, based on the current frames per second of the ringbuffer.
    fn frames_from_duration(&self, duration: fasync::Duration) -> usize {
        frames_from_duration(self.frames_per_second as usize, duration)
    }

    /// Return an amount of time that guarantees that `frames` frames has passed.
    /// This means that partial nanoseconds will be rounded up, so that
    /// [time, time + duration_from_frames(n)] is guaranteed to include n audio frames.
    /// Only defined for positive numbers of frames.
    fn duration_from_frames(&self, frames: usize) -> fasync::Duration {
        let fps = self.frames_per_second as i64;
        let secs = frames as i64 / fps;
        let leftover_frames = frames as i64 % fps;
        let nanos = (leftover_frames * 1_000_000_000i64) / fps;
        let roundup_nano = if 0 != ((leftover_frames * 1_000_000_000i64) % fps) { 1 } else { 0 };
        secs.seconds() + (nanos + roundup_nano).nanos()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use async_utils::PollExt;
    use fixture::fixture;
    use fuchsia_zircon as zx;

    // Convenience choice because one byte = one frame.
    const TEST_FORMAT: AudioSampleFormat = AudioSampleFormat::Eight { unsigned: false };
    const TEST_CHANNELS: u16 = 1;
    const TEST_FPS: u32 = 48000;
    const TEST_FRAMES: usize = TEST_FPS as usize / 2;
    // Duration of the whole VMO.
    const TEST_VMO_DURATION: fasync::Duration = fasync::Duration::from_millis(500);

    // At 48kHz, each frame is 20833 and 1/3 nanoseconds. We add one nanosecond
    // so that the one and two frames are completely within the time.
    const ONE_FRAME_NANOS: i64 = 20833 + 1;
    const TWO_FRAME_NANOS: i64 = 20833 * 2 + 1;
    const THREE_FRAME_NANOS: i64 = 20833 * 3 + 1;

    fn get_test_vmo(frames: usize) -> FrameVmo {
        let mut vmo = FrameVmo::new().expect("can't make a framevmo");
        let _handle = vmo.set_format(TEST_FPS, TEST_FORMAT, TEST_CHANNELS, frames, 0).unwrap();
        vmo
    }

    fn with_test_vmo<F>(_name: &str, test: F)
    where
        F: FnOnce(FrameVmo) -> (),
    {
        test(get_test_vmo(TEST_FRAMES))
    }

    #[fixture(with_test_vmo)]
    #[fuchsia::test]
    fn duration_from_frames(vmo: FrameVmo) {
        assert_eq!(ONE_FRAME_NANOS.nanos(), vmo.duration_from_frames(1));
        assert_eq!(TWO_FRAME_NANOS.nanos(), vmo.duration_from_frames(2));
        assert_eq!(THREE_FRAME_NANOS.nanos(), vmo.duration_from_frames(3));

        assert_eq!(1.second(), vmo.duration_from_frames(TEST_FPS as usize));

        assert_eq!(1500.millis(), vmo.duration_from_frames(72000));
    }

    #[fixture(with_test_vmo)]
    #[fuchsia::test]
    fn frames_from_duration(vmo: FrameVmo) {
        assert_eq!(0, vmo.frames_from_duration(0.nanos()));

        assert_eq!(0, vmo.frames_from_duration((ONE_FRAME_NANOS - 1).nanos()));
        assert_eq!(1, vmo.frames_from_duration(ONE_FRAME_NANOS.nanos()));

        // Three frames is an exact number of nanoseconds, testing the edge.
        assert_eq!(2, vmo.frames_from_duration((THREE_FRAME_NANOS - 1).nanos()));
        assert_eq!(3, vmo.frames_from_duration(THREE_FRAME_NANOS.nanos()));
        assert_eq!(3, vmo.frames_from_duration((THREE_FRAME_NANOS + 1).nanos()));

        assert_eq!(TEST_FPS as usize, vmo.frames_from_duration(1.second()));
        assert_eq!(72000, vmo.frames_from_duration(1500.millis()));

        assert_eq!(10660, vmo.frames_from_duration(zx::Duration::from_nanos(222084000)));
    }

    #[fuchsia::test]
    fn start_stop() {
        let _exec = fasync::TestExecutor::new();

        let mut vmo = FrameVmo::new().expect("can't make a framevmo");

        // Starting before set_format is an error.
        assert!(vmo.start(fasync::Time::now()).is_err());
        // Stopping before set_format is an error.
        assert!(vmo.stop().is_err());

        let _handle = vmo.set_format(TEST_FPS, TEST_FORMAT, TEST_CHANNELS, TEST_FRAMES, 0).unwrap();

        let start_time = fasync::Time::now();
        vmo.start(start_time).unwrap();
        match vmo.start(start_time) {
            Err(Error::InvalidState) => {}
            x => panic!("Expected Err(InvalidState) from double start but got {:?}", x),
        };

        assert_eq!(true, vmo.stop().unwrap());
        // stop is idempotent, but will return false if it was already stopped
        assert_eq!(false, vmo.stop().unwrap());

        vmo.start(start_time).unwrap();
    }

    fn frames_before_exact(
        vmo: &mut FrameVmo,
        time_nanos: i64,
        duration: zx::Duration,
        frames: usize,
    ) {
        let _ = vmo.stop();
        let start_time = fasync::Time::from_nanos(time_nanos);
        vmo.start(start_time).unwrap();
        assert_eq!(frames, vmo.frames_before(start_time + duration));
    }

    #[fixture(with_test_vmo)]
    #[fuchsia::test]
    fn frames_before(mut vmo: FrameVmo) {
        let _exec = fasync::TestExecutor::new();

        let start_time = fasync::Time::now();
        vmo.start(start_time).unwrap();

        assert_eq!(0, vmo.frames_before(start_time));

        assert_eq!(1, vmo.frames_before(start_time + ONE_FRAME_NANOS.nanos()));
        assert_eq!(2, vmo.frames_before(start_time + (THREE_FRAME_NANOS - 1).nanos()));
        assert_eq!(3, vmo.frames_before(start_time + THREE_FRAME_NANOS.nanos()));

        assert_eq!(TEST_FPS as usize / 4, vmo.frames_before(start_time + 250.millis()));

        let three_quarters_dur = 375.millis();
        assert_eq!(3 * TEST_FPS as usize / 8, vmo.frames_before(start_time + three_quarters_dur));

        assert_eq!(10521, vmo.frames_before(start_time + 219188000.nanos()));

        frames_before_exact(&mut vmo, 273533747037, 219188000.nanos(), 10521);
        frames_before_exact(&mut vmo, 714329925362, 219292000.nanos(), 10526);
    }

    #[fixture(with_test_vmo)]
    #[fuchsia::test]
    fn poll_frames(mut vmo: FrameVmo) {
        let exec = fasync::TestExecutor::new_with_fake_time();
        exec.set_fake_time(fasync::Time::from_nanos(1_000_000_000));

        let start_time = fasync::Time::now();
        vmo.start(start_time).unwrap();

        let half_dur = TEST_VMO_DURATION / 2;
        const QUART_FRAMES: usize = TEST_FRAMES / 4;
        let mut quart_frames_buf = [0; QUART_FRAMES];
        exec.set_fake_time(fasync::Time::after(half_dur));

        let mut no_wake_cx = Context::from_waker(futures_test::task::panic_waker_ref());

        let res = vmo.poll_frames(0, &mut quart_frames_buf, &mut no_wake_cx);
        let (frame_idx, missed) = res.expect("frames should be ready").expect("no error");

        assert_eq!(0, missed);
        // index returned should be equal to the number of frames returned minus 1 - zero is the
        // first frame
        assert_eq!(frame_idx, QUART_FRAMES - 1);

        // After another half_dur, we should be able to read the whole buffer from start to finish.
        exec.set_fake_time(fasync::Time::after(half_dur));

        let mut full_buf = [0; TEST_FRAMES];
        let res = vmo.poll_frames(0, &mut full_buf, &mut no_wake_cx);
        let (frame_idx, missed) = res.expect("frames should be ready").expect("no error");

        assert_eq!(0, missed);
        // index returned should be equal to the number of frames returned minus 1 (zero indexing)
        assert_eq!(frame_idx, TEST_FRAMES - 1);

        // Each `half_dur` period should pseudo-fill half the vmo.
        // After three halves, we should have the oldest frame half-way through the buffer (at
        // index 12000)
        exec.set_fake_time(fasync::Time::after(half_dur));

        // Should be able to get frames that span from the middle of the buffer to the middle of the
        // buffer (from index 12000 to index 11999).  This should be 24000 frames.
        // TODO(fxbug.dev/90313): should mark the buffer somehow to confirm that the data is correct
        let res = vmo.poll_frames(TEST_FRAMES / 2, &mut full_buf, &mut no_wake_cx);
        let (frame_idx, missed) = res.expect("frames should be ready").expect("no error");

        assert_eq!(0, missed);
        assert_eq!(frame_idx, TEST_FRAMES + TEST_FRAMES / 2 - 1);

        // Should be able to get a set of frames that is all located before the oldest point in the
        // VMO, which is currently in the middle of the VMO.
        // This should be from about a quarter in to halfway in (now)
        // Should be able to get exactly the min_duration amount of frames.
        let res =
            vmo.poll_frames(frame_idx + 1 - QUART_FRAMES, &mut quart_frames_buf, &mut no_wake_cx);
        let (frame_idx, missed) = res.expect("frames should be ready").expect("no error");

        assert_eq!(0, missed);
        assert_eq!(frame_idx, TEST_FRAMES + TEST_FRAMES / 2 - 1);
    }

    #[fixture(with_test_vmo)]
    #[fuchsia::test]
    fn poll_frames_waking(mut vmo: FrameVmo) {
        let mut exec = fasync::TestExecutor::new_with_fake_time();
        exec.set_fake_time(fasync::Time::from_nanos(1_000_000_000));

        let half_dur = TEST_VMO_DURATION / 2;
        const QUART_FRAMES: usize = TEST_FRAMES / 4;
        let mut quart_frames_buf = [0; QUART_FRAMES];
        exec.set_fake_time(fasync::Time::after(half_dur));

        let (waker, count) = futures_test::task::new_count_waker();
        let mut counting_wake_cx = Context::from_waker(&waker);

        // Polled before start, should wake the waker when started.
        let res = vmo.poll_frames(0, &mut quart_frames_buf[..], &mut counting_wake_cx);
        res.expect_pending("should be pending before start");

        let start_time = fasync::Time::now();
        vmo.start(start_time).unwrap();

        // Woken when we start.
        assert_eq!(count, 1);

        // Polling before frames are ready should return pending, and set a timer.
        let res = vmo.poll_frames(0, &mut quart_frames_buf[..], &mut counting_wake_cx);
        res.expect_pending("should be pending before start");

        // Each `half_dur` period should pseudo-fill half the vmo.
        // After a half_dur, we should have been woken.
        let new_time = fasync::Time::after(half_dur);
        exec.set_fake_time(new_time);
        assert!(exec.wake_expired_timers());

        assert_eq!(count, 2);

        // Should be ready now, with half the duration in frames available.
        let res = vmo.poll_frames(0, &mut quart_frames_buf[..], &mut counting_wake_cx);
        let (frame_idx, missed) = res.expect("frames should be ready").expect("no error");
        assert_eq!(0, missed);
        assert_eq!(frame_idx, QUART_FRAMES - 1);

        // Polling again with more frames than we have should return pending because there
        // isn't enough data available.
        let mut half_frames_buf = [0; TEST_FRAMES / 2];
        let res = vmo.poll_frames(frame_idx + 1, &mut half_frames_buf, &mut counting_wake_cx);
        res.expect_pending("should be pending because not enough data");

        assert_eq!(count, 2);
    }

    #[fuchsia::test]
    fn multibyte_poll_frames() {
        let exec = fasync::TestExecutor::new_with_fake_time();
        exec.set_fake_time(fasync::Time::from_nanos(1_000_000_000));
        let mut vmo = FrameVmo::new().expect("can't make a framevmo");
        let format = AudioSampleFormat::Sixteen { unsigned: false, invert_endian: false };
        let frames = TEST_FPS as usize / 2;

        // No byte count can be determined before the format is set.
        assert!(vmo.bytecount_frames(10).is_none());

        let _handle = vmo.set_format(TEST_FPS, format, 2, frames, 0).unwrap();

        let half_dur = 250.millis();

        // Start in the past so we can be sure the frames are in the past.
        let start_time = fasync::Time::now() - half_dur;
        vmo.start(start_time).unwrap();

        let bytecount =
            vmo.bytecount_frames(frames / 2).expect("a bytecount should exist after format");
        let mut half_frames_buf = vec![0; bytecount];

        let mut no_wake_cx = Context::from_waker(futures_test::task::panic_waker_ref());
        let res = vmo.poll_frames(0, half_frames_buf.as_mut_slice(), &mut no_wake_cx);
        let (idx, missed) = res.expect("frames should be ready").expect("no error");

        assert_eq!(0, missed);
        // Still frames are in frame counts, not byte counts.
        assert_eq!(idx, frames / 2 - 1);
    }

    #[fixture(with_test_vmo)]
    #[fuchsia::test]
    fn poll_frames_boundaries(mut vmo: FrameVmo) {
        let exec = fasync::TestExecutor::new_with_fake_time();
        exec.set_fake_time(fasync::Time::from_nanos(1_000_000_000));
        let mut no_wake_cx = Context::from_waker(futures_test::task::panic_waker_ref());

        let start_time = fasync::Time::now();

        vmo.start(start_time).unwrap();

        // Just before the frame finishes, we shouldn't be able to get it.
        exec.set_fake_time(start_time + THREE_FRAME_NANOS.nanos() - 1.nanos());
        let mut one_frame_buf = [0; 1];
        vmo.poll_frames(2, &mut one_frame_buf, &mut no_wake_cx)
            .expect_pending("third frame shouldn't be ready");
        // Exactly when the frame finishes, should be able to get the frame.
        exec.set_fake_time(start_time + THREE_FRAME_NANOS.nanos());
        let res = vmo.poll_frames(2, &mut one_frame_buf, &mut no_wake_cx);
        let (idx, missed) = res.expect("third frame should be ready").expect("no error");

        assert_eq!(0, missed);
        // index is the index of the last frame returned, which should just be the one frame
        // index we requested.
        assert_eq!(2, idx);

        // a bunch of time has passed, let's get one frame again.
        let much_later_ns = 3999 * THREE_FRAME_NANOS;
        let next_frame_idx = 3998 * 3 + 2;
        exec.set_fake_time(start_time + much_later_ns.nanos());
        let res = vmo.poll_frames(next_frame_idx, &mut one_frame_buf, &mut no_wake_cx);
        let (idx, missed) = res.expect("frame should be ready").expect("no error");
        assert_eq!(0, missed);
        // index is the index of the last frame returned.
        assert_eq!(next_frame_idx, idx);

        let mut all_frames_len = 0;
        let mut total_duration = 0.nanos();

        let moment_length = 10_000.nanos();

        let mut moment_start = start_time;
        let mut moment_end = moment_start;
        let mut next_index = 0;

        let mut ten_frames_buf = [0; 10];

        while total_duration < 250.millis() {
            moment_end += moment_length;
            exec.set_fake_time(moment_end);
            total_duration += moment_length;
            // the cx here will never be woken since we never wake timers
            let Poll::Ready(res) = vmo.poll_frames(next_index, &mut ten_frames_buf, &mut no_wake_cx) else {
                continue;
            };
            let (last_idx, missed) = res.expect("no error");
            assert_eq!(0, missed);
            all_frames_len += ten_frames_buf.len();
            assert_eq!(
                all_frames_len,
                vmo.frames_before(moment_end + 1.nanos()),
                "frame miscount after {:?} - {:?} moment",
                moment_start,
                moment_end
            );
            moment_start = moment_end;
            next_index = last_idx + 1;
        }

        assert_eq!(TEST_FRAMES / 2, all_frames_len, "should be a quarter second worth of frames");
    }
}
