// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Fuchsia netdevice session.

mod buffer;

use std::fmt::Debug;
use std::num::{NonZeroU16, NonZeroU32, NonZeroU64, TryFromIntError};
use std::pin::Pin;
use std::sync::{Arc, Weak};
use std::{convert::TryFrom, mem::MaybeUninit, ops::Range, task::Waker};

use explicit::ResultExt as _;
use fidl_fuchsia_hardware_network as netdev;
use fidl_table_validation::ValidFidlTable;
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::{
    future::{poll_fn, Future},
    ready,
    task::{Context, Poll},
};
use parking_lot::Mutex;

use crate::error::{Error, Result};
pub use buffer::Buffer;
use buffer::{
    pool::Pool, AllocKind, DescId, Rx, Tx, NETWORK_DEVICE_DESCRIPTOR_LENGTH,
    NETWORK_DEVICE_DESCRIPTOR_VERSION,
};

/// A session between network device client and driver.
#[derive(Clone)]
pub struct Session {
    inner: Weak<Inner>,
}

impl Debug for Session {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self.inner.upgrade() {
            Some(inner) => {
                let Inner {
                    name,
                    pool: _,
                    proxy: _,
                    rx: _,
                    tx: _,
                    tx_pending: _,
                    rx_ready: _,
                    tx_ready: _,
                } = &*inner;
                f.debug_struct("Session").field("name", &name).finish_non_exhaustive()
            }
            None => f.write_str("Session(closed)"),
        }
    }
}

impl Session {
    /// Creates a new session with the given `name` and `config`.
    pub async fn new(
        device: &netdev::DeviceProxy,
        name: &str,
        config: Config,
    ) -> Result<(Self, Task)> {
        let inner = Inner::new(device, name, config).await?;
        Ok((Session { inner: Arc::downgrade(&inner) }, Task { inner }))
    }

    /// Sends a [`Buffer`] to the network device in this session.
    pub fn send(&self, buffer: Buffer<Tx>) -> Result<()> {
        self.inner()?.send(buffer)
    }

    /// Receives a [`Buffer`] from the network device in this session.
    pub async fn recv(&self) -> Result<Buffer<Rx>> {
        self.inner()?.recv().await
    }

    /// Allocates a [`Buffer`] that may later be queued to the network device.
    ///
    /// The returned buffer will have at least `num_bytes` as size.
    pub async fn alloc_tx_buffer(&self, num_bytes: usize) -> Result<Buffer<Tx>> {
        self.inner()?.pool.alloc_tx_buffer(num_bytes).await
    }

    /// Attaches [`Session`] to a port.
    pub async fn attach<IntoIter>(&self, port: Port, rx_frames: IntoIter) -> Result<()>
    where
        IntoIter: IntoIterator<Item = netdev::FrameType>,
        IntoIter::IntoIter: ExactSizeIterator,
    {
        // NB: Need to bind the future returned by `proxy.attach` to a variable
        // otherwise this function's (`Session::attach`) returned future becomes
        // not `Send` and we get unexpected compiler errors at a distance.
        //
        // The dyn borrow in the signature of `proxy.attach` seems to be the
        // cause of the compiler's confusion.
        let fut = self.inner()?.proxy.attach(&mut port.into(), &mut rx_frames.into_iter());
        let () = fut.await?.map_err(|raw| Error::Attach(port, zx::Status::from_raw(raw)))?;
        Ok(())
    }

    /// Detaches a port from the [`Session`].
    pub async fn detach(&self, port: Port) -> Result<()> {
        let () = self
            .inner()?
            .proxy
            .detach(&mut port.into())
            .await?
            .map_err(|raw| Error::Detach(port, zx::Status::from_raw(raw)))?;
        Ok(())
    }

    /// Retrieves [`Inner`] if the task is still alive.
    fn inner(&self) -> Result<Arc<Inner>> {
        self.inner.upgrade().ok_or(Error::NoProgress)
    }
}

struct Inner {
    pool: Arc<Pool>,
    proxy: netdev::SessionProxy,
    name: String,
    rx: fasync::Fifo<DescId<Rx>>,
    tx: fasync::Fifo<DescId<Tx>>,
    // Pending tx descriptors to be sent.
    tx_pending: Pending<Tx>,
    rx_ready: Mutex<ReadyBuffer<DescId<Rx>>>,
    tx_ready: Mutex<ReadyBuffer<DescId<Tx>>>,
}

impl Inner {
    /// Creates a new session.
    async fn new(device: &netdev::DeviceProxy, name: &str, config: Config) -> Result<Arc<Self>> {
        let (pool, descriptors, data) = Pool::new(config)?;

        let session_info = {
            // The following two constants are not provided by user, panic
            // instead of returning an error.
            let descriptor_length =
                u8::try_from(NETWORK_DEVICE_DESCRIPTOR_LENGTH / std::mem::size_of::<u64>())
                    .expect("descriptor length in 64-bit words not representable by u8");
            netdev::SessionInfo {
                descriptors: Some(descriptors),
                data: Some(data),
                descriptor_version: Some(NETWORK_DEVICE_DESCRIPTOR_VERSION),
                descriptor_length: Some(descriptor_length),
                descriptor_count: Some(config.num_tx_buffers.get() + config.num_rx_buffers.get()),
                options: Some(config.options),
                ..netdev::SessionInfo::EMPTY
            }
        };

        let (client, netdev::Fifos { rx, tx }) = device
            .open_session(name, session_info)
            .await?
            .map_err(|raw| Error::Open(name.to_owned(), zx::Status::from_raw(raw)))?;
        let proxy = client.into_proxy()?;
        let rx =
            fasync::Fifo::from_fifo(rx).map_err(|status| Error::Fifo("create", "rx", status))?;
        let tx =
            fasync::Fifo::from_fifo(tx).map_err(|status| Error::Fifo("create", "tx", status))?;

        Ok(Arc::new(Self {
            pool,
            proxy,
            name: name.to_owned(),
            rx,
            tx,
            tx_pending: Pending::new(Vec::new()),
            rx_ready: Mutex::new(ReadyBuffer::new(config.num_rx_buffers.get().into())),
            tx_ready: Mutex::new(ReadyBuffer::new(config.num_tx_buffers.get().into())),
        }))
    }

    /// Polls to submit available rx descriptors from pool to driver.
    ///
    /// Returns the number of rx descriptors that are submitted.
    fn poll_submit_rx(&self, cx: &mut Context<'_>) -> Poll<Result<usize>> {
        self.pool.rx_pending.poll_submit(&self.rx, cx)
    }

    /// Polls completed rx descriptors from the driver.
    ///
    /// Returns the the head of a completed rx descriptor chain.
    fn poll_complete_rx(&self, cx: &mut Context<'_>) -> Poll<Result<DescId<Rx>>> {
        let mut rx_ready = self.rx_ready.lock();
        rx_ready.poll_with_fifo(cx, &self.rx).map_err(|status| Error::Fifo("read", "rx", status))
    }

    /// Polls to submit tx descriptors that are pending to the driver.
    ///
    /// Returns the number of tx descriptors that are successfully submitted.
    fn poll_submit_tx(&self, cx: &mut Context<'_>) -> Poll<Result<usize>> {
        self.tx_pending.poll_submit(&self.tx, cx)
    }

    /// Polls completed tx descriptors from the driver then puts them in pool.
    fn poll_complete_tx(&self, cx: &mut Context<'_>) -> Poll<Result<()>> {
        let mut tx_ready = self.tx_ready.lock();
        // TODO(https://github.com/rust-lang/rust/issues/63569): Provide entire
        // chain of completed descriptors to the pool at once when slice of
        // MaybeUninit is stabilized.
        tx_ready.poll_with_fifo(cx, &self.tx).map(|r| match r {
            Ok(desc) => self.pool.tx_completed(desc),
            Err(status) => Err(Error::Fifo("read", "tx", status)),
        })
    }

    /// Sends the [`Buffer`] to the driver.
    fn send(&self, mut buffer: Buffer<Tx>) -> Result<()> {
        buffer.pad()?;
        buffer.commit();
        self.tx_pending.extend(std::iter::once(buffer.leak()));
        Ok(())
    }

    /// Receives a [`Buffer`] from the driver.
    ///
    /// Waits until there is completed rx buffers from the driver.
    async fn recv(&self) -> Result<Buffer<Rx>> {
        poll_fn(|cx| -> Poll<Result<Buffer<Rx>>> {
            let head = ready!(self.poll_complete_rx(cx))?;
            Poll::Ready(self.pool.rx_completed(head))
        })
        .await
    }
}

/// The backing task that drives the session.
///
/// A session can make no progress without this task. All ports will be detached
/// if the task is dropped.
#[must_use = "futures do nothing unless you `.await` or poll them"]
pub struct Task {
    inner: Arc<Inner>,
}

impl Future for Task {
    type Output = Result<()>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let inner = &Pin::into_inner(self).inner;
        loop {
            let mut all_pending = true;
            // TODO(https://fxbug.dev/78342): poll once for all completed
            // descriptors if this becomes a performance bottleneck.
            while inner.poll_complete_tx(cx)?.is_ready() {
                all_pending = false;
            }
            if inner.poll_submit_rx(cx)?.is_ready() {
                all_pending = false;
            }
            if inner.poll_submit_tx(cx).is_ready() {
                all_pending = false;
            }
            if all_pending {
                return Poll::Pending;
            }
        }
    }
}

/// Session configuration.
#[derive(Debug, Clone, Copy)]
pub struct Config {
    /// Buffer stride on VMO, in bytes.
    buffer_stride: NonZeroU64,
    /// Number of rx descriptors to allocate.
    num_rx_buffers: NonZeroU16,
    /// Number of tx descriptors to allocate.
    num_tx_buffers: NonZeroU16,
    /// Session flags.
    options: netdev::SessionFlags,
    /// Buffer layout.
    buffer_layout: BufferLayout,
}

/// Describes the buffer layout that [`Pool`] needs to know.
#[derive(Debug, Clone, Copy)]
struct BufferLayout {
    /// Minimum tx buffer data length.
    min_tx_data: usize,
    /// Minimum tx buffer head length.
    min_tx_head: u16,
    /// Minimum tx buffer tail length.
    min_tx_tail: u16,
    /// The length of a buffer.
    length: usize,
}

/// Network device information with all required fields.
#[derive(Debug, Clone, ValidFidlTable)]
#[fidl_table_src(netdev::DeviceInfo)]
pub struct DeviceInfo {
    /// Minimum descriptor length, in 64-bit words.
    pub min_descriptor_length: u8,
    /// Accepted descriptor version.
    pub descriptor_version: u8,
    /// Maximum number of items in rx FIFO (per session).
    pub rx_depth: u16,
    /// Maximum number of items in tx FIFO (per session).
    pub tx_depth: u16,
    /// Alignment requirement for buffers in the data VMO.
    pub buffer_alignment: u32,
    /// Maximum supported length of buffers in the data VMO, in bytes.
    #[fidl_field_type(optional)]
    pub max_buffer_length: Option<NonZeroU32>,
    /// The minimum rx buffer length required for device.
    pub min_rx_buffer_length: u32,
    /// The minimum tx buffer length required for the device.
    pub min_tx_buffer_length: u32,
    /// The number of bytes the device requests be free as `head` space in a tx buffer.
    pub min_tx_buffer_head: u16,
    /// The amount of bytes the device requests be free as `tail` space in a tx buffer.
    pub min_tx_buffer_tail: u16,
    /// Available rx acceleration flags for this device.
    #[fidl_field_type(default)]
    pub rx_accel: Vec<netdev::RxAcceleration>,
    /// Available tx acceleration flags for this device.
    #[fidl_field_type(default)]
    pub tx_accel: Vec<netdev::TxAcceleration>,
}

impl DeviceInfo {
    /// Create a new session config from the device information.
    ///
    /// This method also does the boundary checks so that data_length/offset fields read
    /// from descriptors are safe to convert to [`usize`].
    pub fn make_config(
        &self,
        default_buffer_length: usize,
        options: netdev::SessionFlags,
    ) -> Result<Config> {
        let DeviceInfo {
            min_descriptor_length,
            descriptor_version,
            rx_depth,
            tx_depth,
            buffer_alignment,
            max_buffer_length,
            min_rx_buffer_length,
            min_tx_buffer_length,
            min_tx_buffer_head,
            min_tx_buffer_tail,
            rx_accel: _,
            tx_accel: _,
        } = self;
        if NETWORK_DEVICE_DESCRIPTOR_VERSION != *descriptor_version {
            return Err(Error::Config(format!(
                "descriptor version mismatch: {} != {}",
                NETWORK_DEVICE_DESCRIPTOR_VERSION, descriptor_version
            )));
        }
        if NETWORK_DEVICE_DESCRIPTOR_LENGTH < usize::from(*min_descriptor_length) {
            return Err(Error::Config(format!(
                "descriptor length too small: {} < {}",
                NETWORK_DEVICE_DESCRIPTOR_LENGTH, min_descriptor_length
            )));
        }

        let num_rx_buffers =
            NonZeroU16::new(*rx_depth).ok_or_else(|| Error::Config("no RX buffers".to_owned()))?;
        let num_tx_buffers =
            NonZeroU16::new(*tx_depth).ok_or_else(|| Error::Config("no TX buffers".to_owned()))?;

        let max_buffer_length = max_buffer_length
            .and_then(|max| {
                // The error case is the case where max_buffer_length can't fix in a
                // usize, but we use it to compare it to usizes, so that's
                // equivalent to no limit.
                usize::try_from(max.get()).ok_checked::<TryFromIntError>()
            })
            .unwrap_or(usize::MAX);
        let min_buffer_length = usize::try_from(*min_rx_buffer_length)
            .ok_checked::<TryFromIntError>()
            .unwrap_or(usize::MAX);

        let buffer_length =
            usize::min(max_buffer_length, usize::max(min_buffer_length, default_buffer_length));

        let buffer_alignment = usize::try_from(*buffer_alignment).map_err(
            |std::num::TryFromIntError { .. }| {
                Error::Config(format!(
                    "buffer_alignment not representable within usize: {}",
                    buffer_alignment,
                ))
            },
        )?;

        let buffer_stride = buffer_length
            .checked_add(buffer_alignment - 1)
            .map(|x| x / buffer_alignment * buffer_alignment)
            .ok_or_else(|| {
                Error::Config(format!(
                    "not possible to align {} to {} under usize::MAX",
                    buffer_length, buffer_alignment,
                ))
            })?;

        if buffer_stride < buffer_length {
            return Err(Error::Config(format!(
                "buffer stride too small {} < {}",
                buffer_stride, buffer_length
            )));
        }

        if buffer_length < usize::from(*min_tx_buffer_head) + usize::from(*min_tx_buffer_tail) {
            return Err(Error::Config(format!(
                "buffer length {} does not meet minimum tx buffer head/tail requirement {}/{}",
                buffer_length, min_tx_buffer_head, min_tx_buffer_tail,
            )));
        }

        let num_buffers =
            rx_depth.checked_add(*tx_depth).filter(|num| *num != u16::MAX).ok_or_else(|| {
                Error::Config(format!(
                    "too many buffers requested: {} + {} > u16::MAX",
                    rx_depth, tx_depth
                ))
            })?;

        let buffer_stride =
            u64::try_from(buffer_stride).map_err(|std::num::TryFromIntError { .. }| {
                Error::Config(format!("buffer_stride too big: {} > u64::MAX", buffer_stride))
            })?;

        // This is following the practice of rust stdlib to ensure allocation
        // size never reaches isize::MAX.
        // https://doc.rust-lang.org/std/primitive.pointer.html#method.add-1.
        match buffer_stride.checked_mul(num_buffers.into()).map(isize::try_from) {
            None | Some(Err(std::num::TryFromIntError { .. })) => {
                return Err(Error::Config(format!(
                    "too much memory required for the buffers: {} * {} > isize::MAX",
                    buffer_stride, num_buffers
                )))
            }
            Some(Ok(_total)) => (),
        };

        let buffer_stride = NonZeroU64::new(buffer_stride)
            .ok_or_else(|| Error::Config("buffer_stride is zero".to_owned()))?;

        let min_tx_data = match usize::try_from(*min_tx_buffer_length)
            .map(|min_tx| (min_tx <= buffer_length).then(|| min_tx))
        {
            Ok(Some(min_tx_buffer_length)) => min_tx_buffer_length,
            // Either the conversion or the comparison failed.
            Ok(None) | Err(std::num::TryFromIntError { .. }) => {
                return Err(Error::Config(format!(
                    "buffer_length smaller than minimum TX requirement: {} < {}",
                    buffer_length, *min_tx_buffer_length
                )));
            }
        };

        Ok(Config {
            buffer_stride,
            num_rx_buffers,
            num_tx_buffers,
            options,
            buffer_layout: BufferLayout {
                length: buffer_length,
                min_tx_head: *min_tx_buffer_head,
                min_tx_tail: *min_tx_buffer_tail,
                min_tx_data,
            },
        })
    }

    /// Create a config for a primary session.
    pub fn primary_config(&self, default_buffer_length: usize) -> Result<Config> {
        self.make_config(default_buffer_length, netdev::SessionFlags::PRIMARY)
    }
}

/// A port of the device.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Port {
    pub(crate) base: u8,
    pub(crate) salt: u8,
}

impl TryFrom<netdev::PortId> for Port {
    type Error = Error;
    fn try_from(netdev::PortId { base, salt }: netdev::PortId) -> Result<Self> {
        if base <= netdev::MAX_PORTS {
            Ok(Self { base, salt })
        } else {
            Err(Error::InvalidPortId(base))
        }
    }
}

impl From<Port> for netdev::PortId {
    fn from(Port { base, salt }: Port) -> Self {
        Self { base, salt }
    }
}

/// Pending descriptors to be sent to driver.
struct Pending<K: AllocKind> {
    inner: Mutex<(Vec<DescId<K>>, Option<Waker>)>,
}

impl<K: AllocKind> Pending<K> {
    fn new(descs: Vec<DescId<K>>) -> Self {
        Self { inner: Mutex::new((descs, None)) }
    }

    /// Extends the pending descriptors buffer.
    fn extend(&self, descs: impl IntoIterator<Item = DescId<K>>) {
        let mut guard = self.inner.lock();
        let (storage, waker) = &mut *guard;
        storage.extend(descs);
        if let Some(waker) = waker.take() {
            waker.wake();
        }
    }

    /// Submits the pending buffer to the driver through [`zx::Fifo`].
    ///
    /// It will return [`Poll::Pending`] if any of the following happens:
    ///   - There are no descriptors pending.
    ///   - The fifo is not ready for write.
    fn poll_submit(
        &self,
        fifo: &fasync::Fifo<DescId<K>>,
        cx: &mut Context<'_>,
    ) -> Poll<Result<usize>> {
        let mut guard = self.inner.lock();
        let (storage, waker) = &mut *guard;
        if storage.is_empty() {
            *waker = Some(cx.waker().clone());
            return Poll::Pending;
        }

        // TODO(https://fxbug.dev/32098): We're assuming that writing to the
        // FIFO here is a sufficient memory barrier for the other end to access
        // the data. That is currently true but not really guaranteed by the
        // API.
        let submitted = ready!(fifo.try_write(cx, &storage[..]))
            .map_err(|status| Error::Fifo("write", K::REFL.as_str(), status))?;
        let _drained = storage.drain(0..submitted);
        Poll::Ready(Ok(submitted))
    }
}

/// An intermediary buffer used to reduce syscall overhead by acting as a proxy
/// to read entries from a FIFO.
///
/// `ReadyBuffer` caches read entries from a FIFO in pre-allocated memory,
/// allowing different batch sizes between what is acquired from the FIFO and
/// what's processed by the caller.
struct ReadyBuffer<T> {
    // NB: A vector of `MaybeUninit` here allows us to give a transparent memory
    // layout to the FIFO object but still move objects out of our buffer
    // without needing a `T: Default` implementation. There's a small added
    // benefit of not paying for memory initialization on creation as well, but
    // that's mostly negligible given all allocation is performed upfront.
    data: Vec<MaybeUninit<T>>,
    available: Range<usize>,
}

impl<T> Drop for ReadyBuffer<T> {
    fn drop(&mut self) {
        let Self { data, available } = self;
        for initialized in &mut data[available.clone()] {
            // SAFETY: the available range keeps track of initialized buffers,
            // we must drop them on drop to uphold `MaybeUninit` expectations.
            unsafe { initialized.assume_init_drop() }
        }
        *available = 0..0;
    }
}

impl<T> ReadyBuffer<T> {
    fn new(capacity: usize) -> Self {
        let data = std::iter::from_fn(|| Some(MaybeUninit::uninit())).take(capacity).collect();
        Self { data, available: 0..0 }
    }

    fn poll_with_fifo(
        &mut self,
        cx: &mut Context<'_>,
        fifo: &fuchsia_async::Fifo<T>,
    ) -> Poll<std::result::Result<T, zx::Status>>
    where
        T: fasync::FifoEntry,
    {
        let Self { data, available: Range { start, end } } = self;

        loop {
            // Always pop from available data first.
            if *start != *end {
                let desc = std::mem::replace(&mut data[*start], MaybeUninit::uninit());
                *start += 1;
                // SAFETY: Descriptor was in the initialized section, it was
                // initialized.
                let desc = unsafe { desc.assume_init() };
                return Poll::Ready(Ok(desc));
            }
            // Fetch more from the FIFO.
            let count = ready!(fifo.try_read(cx, &mut data[..]))?;
            *start = 0;
            *end = count;
        }
    }
}

#[cfg(test)]
mod tests {
    use std::{num::NonZeroU32, ops::Deref};

    use assert_matches::assert_matches;
    use test_case::test_case;

    use super::{
        buffer::NETWORK_DEVICE_DESCRIPTOR_LENGTH, buffer::NETWORK_DEVICE_DESCRIPTOR_VERSION,
        BufferLayout, Config, DeviceInfo, Error,
    };

    const BASE_DEVICE_INFO: DeviceInfo = DeviceInfo {
        min_descriptor_length: 0,
        descriptor_version: 1,
        rx_depth: 1,
        tx_depth: 1,
        buffer_alignment: 1,
        max_buffer_length: None,
        min_rx_buffer_length: 0,
        min_tx_buffer_head: 0,
        min_tx_buffer_length: 0,
        min_tx_buffer_tail: 0,
        rx_accel: Vec::new(),
        tx_accel: Vec::new(),
    };

    const DEFAULT_BUFFER_LENGTH: usize = 2048;

    #[test_case(DEFAULT_BUFFER_LENGTH, DeviceInfo {
        min_descriptor_length: u8::MAX,
        ..BASE_DEVICE_INFO
    }, format!("descriptor length too small: {} < {}", NETWORK_DEVICE_DESCRIPTOR_LENGTH, u8::MAX))]
    #[test_case(DEFAULT_BUFFER_LENGTH, DeviceInfo {
        descriptor_version: 42,
        ..BASE_DEVICE_INFO
    }, format!("descriptor version mismatch: {} != {}", NETWORK_DEVICE_DESCRIPTOR_VERSION, 42))]
    #[test_case(DEFAULT_BUFFER_LENGTH, DeviceInfo {
        tx_depth: 0,
        ..BASE_DEVICE_INFO
    }, "no TX buffers")]
    #[test_case(DEFAULT_BUFFER_LENGTH, DeviceInfo {
        rx_depth: 0,
        ..BASE_DEVICE_INFO
    }, "no RX buffers")]
    #[test_case(DEFAULT_BUFFER_LENGTH, DeviceInfo {
        tx_depth: u16::MAX,
        rx_depth: u16::MAX,
        ..BASE_DEVICE_INFO
    }, format!("too many buffers requested: {} + {} > u16::MAX", u16::MAX, u16::MAX))]
    #[test_case(DEFAULT_BUFFER_LENGTH, DeviceInfo {
        min_tx_buffer_length: DEFAULT_BUFFER_LENGTH as u32 + 1,
        ..BASE_DEVICE_INFO
    }, format!(
        "buffer_length smaller than minimum TX requirement: {} < {}",
        DEFAULT_BUFFER_LENGTH, DEFAULT_BUFFER_LENGTH + 1))]
    #[test_case(DEFAULT_BUFFER_LENGTH, DeviceInfo {
        min_tx_buffer_head: DEFAULT_BUFFER_LENGTH as u16 + 1,
        ..BASE_DEVICE_INFO
    }, format!(
        "buffer length {} does not meet minimum tx buffer head/tail requirement {}/0",
        DEFAULT_BUFFER_LENGTH, DEFAULT_BUFFER_LENGTH + 1))]
    #[test_case(DEFAULT_BUFFER_LENGTH, DeviceInfo {
        min_tx_buffer_tail: DEFAULT_BUFFER_LENGTH as u16 + 1,
        ..BASE_DEVICE_INFO
    }, format!(
        "buffer length {} does not meet minimum tx buffer head/tail requirement 0/{}",
        DEFAULT_BUFFER_LENGTH, DEFAULT_BUFFER_LENGTH + 1))]
    #[test_case(0, BASE_DEVICE_INFO, "buffer_stride is zero")]
    #[test_case(usize::MAX, BASE_DEVICE_INFO,
    format!(
        "too much memory required for the buffers: {} * {} > isize::MAX",
        usize::MAX, 2))]
    #[test_case(usize::MAX, DeviceInfo {
        buffer_alignment: 2,
        ..BASE_DEVICE_INFO
    }, format!(
        "not possible to align {} to {} under usize::MAX",
        usize::MAX, 2))]
    fn configs_from_device_info_err(
        buffer_length: usize,
        info: DeviceInfo,
        expected: impl Deref<Target = str>,
    ) {
        assert_matches!(
            info.primary_config(buffer_length),
            Err(Error::Config(got)) if got.as_str() == expected.deref()
        );
    }

    #[test_case(DeviceInfo {
        min_rx_buffer_length: DEFAULT_BUFFER_LENGTH as u32 + 1,
        ..BASE_DEVICE_INFO
    }, DEFAULT_BUFFER_LENGTH + 1; "default below min")]
    #[test_case(DeviceInfo {
        max_buffer_length: NonZeroU32::new(DEFAULT_BUFFER_LENGTH as u32 - 1),
        ..BASE_DEVICE_INFO
    }, DEFAULT_BUFFER_LENGTH - 1; "default above max")]
    #[test_case(DeviceInfo {
        min_rx_buffer_length: DEFAULT_BUFFER_LENGTH as u32 - 1,
        max_buffer_length: NonZeroU32::new(DEFAULT_BUFFER_LENGTH as u32 + 1),
        ..BASE_DEVICE_INFO
    }, DEFAULT_BUFFER_LENGTH; "default in bounds")]
    fn configs_from_device_buffer_length(info: DeviceInfo, expected_length: usize) {
        let config = info.primary_config(DEFAULT_BUFFER_LENGTH).expect("is valid");
        let Config {
            buffer_layout: BufferLayout { length, min_tx_data: _, min_tx_head: _, min_tx_tail: _ },
            buffer_stride: _,
            num_rx_buffers: _,
            num_tx_buffers: _,
            options: _,
        } = config;
        assert_eq!(length, expected_length);
    }
}
