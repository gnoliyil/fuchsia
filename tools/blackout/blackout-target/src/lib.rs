// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! library for target side of filesystem integrity host-target interaction tests

#![deny(missing_docs)]

use {
    anyhow::{anyhow, Context as _, Result},
    async_trait::async_trait,
    device_watcher::{recursive_wait_and_open, recursive_wait_and_open_node},
    fidl_fuchsia_blackout_test::{ControllerRequest, ControllerRequestStream},
    fidl_fuchsia_device::{ControllerMarker, ControllerProxy},
    fidl_fuchsia_hardware_block::BlockMarker,
    fidl_fuchsia_io as fio, fuchsia_async as fasync,
    fuchsia_component::client::{
        connect_to_named_protocol_at_dir_root, connect_to_protocol_at_path,
    },
    fuchsia_component::server::{ServiceFs, ServiceObj},
    fuchsia_fs::directory::readdir,
    fuchsia_zircon as zx,
    futures::{future, FutureExt, StreamExt, TryFutureExt, TryStreamExt},
    rand::{distributions, rngs::StdRng, Rng, SeedableRng},
    std::sync::Arc,
    storage_isolated_driver_manager::fvm,
    uuid::Uuid,
};

pub mod static_tree;

/// The three steps the target-side of a blackout test needs to implement.
#[async_trait]
pub trait Test {
    /// Setup the test run on the given block_device.
    async fn setup(
        self: Arc<Self>,
        device_label: String,
        device_path: Option<String>,
        seed: u64,
    ) -> Result<()>;
    /// Run the test body on the given device_path.
    async fn test(
        self: Arc<Self>,
        device_label: String,
        device_path: Option<String>,
        seed: u64,
    ) -> Result<()>;
    /// Verify the consistency of the filesystem on the device_path.
    async fn verify(
        self: Arc<Self>,
        device_label: String,
        device_path: Option<String>,
        seed: u64,
    ) -> Result<()>;
}

struct BlackoutController(ControllerRequestStream);

/// A test server, which serves the fuchsia.blackout.test.Controller protocol.
pub struct TestServer<'a, T> {
    fs: ServiceFs<ServiceObj<'a, BlackoutController>>,
    test: Arc<T>,
}

impl<'a, T> TestServer<'a, T>
where
    T: Test + Copy + 'static,
{
    /// Create a new test server for this test.
    pub fn new(test: T) -> Result<TestServer<'a, T>> {
        let mut fs = ServiceFs::new();
        fs.dir("svc").add_fidl_service(BlackoutController);
        fs.take_and_serve_directory_handle()?;

        Ok(TestServer { fs, test: Arc::new(test) })
    }

    /// Start serving the outgoing directory. Blocks until all connections are closed.
    pub async fn serve(self) {
        const MAX_CONCURRENT: usize = 10_000;
        let test = self.test;
        self.fs
            .for_each_concurrent(MAX_CONCURRENT, move |stream| {
                handle_request(test.clone(), stream).unwrap_or_else(|e| tracing::error!("{}", e))
            })
            .await;
    }
}

async fn handle_request<T: Test + 'static>(
    test: Arc<T>,
    BlackoutController(mut stream): BlackoutController,
) -> Result<()> {
    while let Some(request) = stream.try_next().await? {
        handle_controller(test.clone(), request).await?;
    }

    Ok(())
}

async fn handle_controller<T: Test + 'static>(
    test: Arc<T>,
    request: ControllerRequest,
) -> Result<()> {
    match request {
        ControllerRequest::Setup { responder, device_label, device_path, seed } => {
            let mut res = test.setup(device_label, device_path, seed).await.map_err(|e| {
                tracing::error!("{}", e);
                zx::Status::INTERNAL.into_raw()
            });
            responder.send(&mut res)?;
        }
        ControllerRequest::Test { responder, device_label, device_path, seed, duration } => {
            let test_fut = test.test(device_label, device_path, seed).map_err(|e| {
                tracing::error!("{}", e);
                zx::Status::INTERNAL.into_raw()
            });
            if duration != 0 {
                // If a non-zero duration is provided, spawn the test and then return after that
                // duration.
                tracing::info!("starting test and replying in {} seconds...", duration);
                let timer = fasync::Timer::new(std::time::Duration::from_secs(duration));
                let mut res = match future::select(test_fut, timer).await {
                    future::Either::Left((res, _)) => res,
                    future::Either::Right((_, test_fut)) => {
                        fasync::Task::spawn(test_fut.map(|_| ())).detach();
                        Ok(())
                    }
                };
                responder.send(&mut res)?;
            } else {
                // If a zero duration is provided, return once the test step is complete.
                tracing::info!("starting test...");
                responder.send(&mut test_fut.await)?;
            }
        }
        ControllerRequest::Verify { responder, device_label, device_path, seed } => {
            let mut res = test.verify(device_label, device_path, seed).await.map_err(|e| {
                // The test tries failing on purpose, so only print errors as warnings.
                tracing::warn!("{}", e);
                zx::Status::BAD_STATE.into_raw()
            });
            responder.send(&mut res)?;
        }
    }

    Ok(())
}

/// Generate a Vec<u8> of random bytes from a seed using a standard distribution.
pub fn generate_content(seed: u64) -> Vec<u8> {
    let mut rng = StdRng::seed_from_u64(seed);

    let size = rng.gen_range(1..1 << 16);
    rng.sample_iter(&distributions::Standard).take(size).collect()
}

/// Find the device in /dev/class/block that represents a given topological path. Returns the full
/// path of the device in /dev/class/block.
pub async fn find_dev(dev: &str) -> Result<String> {
    let dev_class_block = fuchsia_fs::directory::open_in_namespace(
        "/dev/class/block",
        fuchsia_fs::OpenFlags::RIGHT_READABLE,
    )?;
    for entry in readdir(&dev_class_block).await? {
        let path = format!("/dev/class/block/{}", entry.name);
        let proxy = connect_to_protocol_at_path::<ControllerMarker>(&path)?;
        let topo_path = proxy.get_topological_path().await?.map_err(|s| zx::Status::from_raw(s))?;
        tracing::info!("{} => {}", path, topo_path);
        if dev == topo_path {
            return Ok(path);
        }
    }
    Err(anyhow::anyhow!("Couldn't find {} in /dev/class/block", dev))
}

/// Returns a directory proxy connected to /dev.
pub fn dev() -> fio::DirectoryProxy {
    fuchsia_fs::directory::open_in_namespace("/dev", fuchsia_fs::OpenFlags::RIGHT_READABLE)
        .expect("failed to open /dev")
}

fn dev_class_block() -> fio::DirectoryProxy {
    fuchsia_fs::directory::open_in_namespace(
        "/dev/class/block",
        fuchsia_fs::OpenFlags::RIGHT_READABLE,
    )
    .expect("failed to open /dev/class/block")
}

const RAMDISK_PREFIX: &'static str = "/dev/sys/platform/00:00:2d/ramctl";

/// During the setup step, formats a device with fvm, creating a single partition named
/// [`partition_label`]. If [`device_path`] is `None`, finds a device which already had fvm, erase
/// it, and then set it up in this way. Returns the path to the device with the created partition,
/// only once the device is enumerated, so it can be used immediately.
pub async fn set_up_partition(
    partition_label: &str,
    device_dir: Option<&fio::DirectoryProxy>,
    skip_ramdisk: bool,
) -> Result<ControllerProxy> {
    let mut device_controller = None;
    let mut owned_device_dir = None;
    let device_dir = match device_dir {
        Some(device_dir) => {
            device_controller = Some(
                connect_to_named_protocol_at_dir_root::<ControllerMarker>(device_dir, ".")
                    .context("new class path connect failed")?,
            );
            device_dir
        }
        None => {
            let dev_class_block_dir = dev_class_block();
            for entry in readdir(&dev_class_block_dir).await.context("readdir failed")? {
                let entry_controller = connect_to_named_protocol_at_dir_root::<ControllerMarker>(
                    &dev_class_block_dir,
                    &entry.name,
                )
                .context("get_topo controller connect failed")?;
                let topo_path = entry_controller
                    .get_topological_path()
                    .await
                    .context("transport error on get_topological_path")?
                    .map_err(zx::Status::from_raw)
                    .context("get_topo failed")?;
                if skip_ramdisk && topo_path.starts_with(RAMDISK_PREFIX) {
                    continue;
                }
                if let Some(fvm_index) = topo_path.find("/block/fvm") {
                    let fvm_path = format!("{}/block", &topo_path[..fvm_index]);
                    let fvm_controller = connect_to_protocol_at_path::<ControllerMarker>(&fvm_path)
                        .context("new class path connect failed")?;
                    fvm_controller
                        .unbind_children()
                        .await
                        .context("unbind children call failed")?
                        .map_err(zx::Status::from_raw)
                        .context("unbind children returned error")?;
                    device_controller = Some(fvm_controller);
                    owned_device_dir = Some(fuchsia_fs::directory::open_in_namespace(
                        &fvm_path,
                        fuchsia_fs::OpenFlags::empty(),
                    )?);
                    break;
                }
            }
            owned_device_dir
                .as_ref()
                .ok_or_else(|| anyhow!("failed to find a device with fvm on it"))?
        }
    };

    let fvm_slice_size = 8192_usize * 4;

    // Get the size of the underlying device so we can use a bunch of it for our fancy new fvm
    // partition without dealing with expansion. We do this because some tests use the size of the
    // device to figure out what they can do, but getting the total used bytes from a filesystem
    // doesn't take into account possible expansion.
    let device_size_bytes = {
        let fvm_block = fuchsia_component::client::connect_to_named_protocol_at_dir_root::<
            BlockMarker,
        >(device_dir, ".")?;
        let info = fvm_block
            .get_info()
            .await
            .context("fvm path get info call failed")?
            .map_err(zx::Status::from_raw)
            .context("fvm path get info returned error")?;
        u64::from(info.block_size) * info.block_count
    };

    // It's tricky to really allocate the maximum number of slices, so we use a whole lot less than
    // that. It should still be enough to count.
    let num_slices = device_size_bytes / fvm_slice_size as u64 / 2;
    let fvm_volume_size = num_slices * fvm_slice_size as u64;

    let device_controller =
        device_controller.ok_or_else(|| anyhow!("invalid device controller"))?;
    let volume_manager = fvm::set_up_fvm(&device_controller, device_dir, fvm_slice_size)
        .await
        .context("set_up_fvm failed")?;
    fvm::create_fvm_volume(
        &volume_manager,
        partition_label,
        Uuid::new_v4().as_bytes(),
        Uuid::new_v4().as_bytes(),
        Some(fvm_volume_size),
        0,
    )
    .await
    .context("create_fvm_volume failed")?;
    recursive_wait_and_open::<ControllerMarker>(
        device_dir,
        &format!("/fvm/{}-p-1/block", partition_label),
    )
    .await
    .context("recursive_wait for new fvm path failed")
}

/// During the test or verify steps, finds a block device which represents an fvm partition named
/// [`partition_label`]. If [`device_path`] is provided, this assumes the partition is on that
/// device. Returns the topological path to the device, only returning once the device is
/// enumerated, so it can be used immediately.
pub async fn find_partition(
    partition_label: &str,
    device_dir: Option<&fio::DirectoryProxy>,
) -> Result<ControllerProxy> {
    if let Some(device_dir) = device_dir {
        match fuchsia_fs::directory::open_no_describe::<ControllerMarker>(
            device_dir,
            &format!("/fvm/{}-p-1/block", partition_label),
            fuchsia_fs::OpenFlags::empty(),
        ) {
            Ok(partition_controller) => {
                return Ok(partition_controller);
            }
            Err(fuchsia_fs::node::OpenError::OpenError(zx::Status::NOT_FOUND)) => {
                // If we failed to open that path, it might be because the fvm driver isn't bound yet.
                let device_controller =
                    fuchsia_component::client::connect_to_named_protocol_at_dir_root::<
                        ControllerMarker,
                    >(device_dir, ".")?;
                fvm::bind_fvm_driver(&device_controller).await?;
                recursive_wait_and_open_node(
                    device_dir,
                    &format!("/fvm/{}-p-1/block", partition_label),
                )
                .await
                .context("recursive_wait on expected fvm path failed")?;
            }
            Err(err) => return Err(err).context("failed to open fvm path"),
        }
    }

    let dev_class_block_dir = dev_class_block();
    for entry in readdir(&dev_class_block_dir).await? {
        let class_path = format!("/dev/class/block/{}", entry.name);
        let partition = connect_to_protocol_at_path::<
            fidl_fuchsia_hardware_block_partition::PartitionMarker,
        >(&class_path)
        .context("class path partition connect failed")?;
        // The device might not support the partition protocol, in which case we skip it. Also skip
        // it if an error is returned, or if no name is returned.
        let entry_name = if let Ok((0, Some(entry_name))) = partition.get_name().await {
            entry_name
        } else {
            continue;
        };
        if &entry_name == partition_label {
            let controller_proxy = connect_to_named_protocol_at_dir_root::<ControllerMarker>(
                &dev_class_block_dir,
                &entry.name,
            )?;
            return Ok(controller_proxy);
        }
    }

    return Err(anyhow::anyhow!("couldn't find device with name \"{}\"", partition_label));
}
