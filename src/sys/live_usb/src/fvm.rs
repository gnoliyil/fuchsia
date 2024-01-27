// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::gpt,
    anyhow::{Context, Error},
    fidl_fuchsia_device::ControllerMarker,
    fidl_fuchsia_fshost::{BlockWatcherMarker, BlockWatcherProxy},
    fidl_fuchsia_hardware_block::BlockMarker,
    fidl_fuchsia_hardware_block_partition::PartitionMarker,
    fuchsia_zircon as zx,
    futures::future::try_join,
    futures::TryFutureExt,
    payload_streamer::{BlockDevicePayloadStreamer, PayloadStreamer},
    ramdevice_client::{RamdiskClient, RamdiskClientBuilder},
    tracing::{error, info},
};

/// Block size of the ramdisk.
const BLOCK_SIZE: u64 = 512;

/// Returns the size in bytes of the partition at `path`.
pub async fn get_partition_size(path: &str) -> Result<u64, Error> {
    let info = fuchsia_component::client::connect_to_protocol_at_path::<PartitionMarker>(path)
        .context("connecting to partition")?
        .get_info()
        .await
        .context("sending get info request")?
        .map_err(zx::Status::from_raw)
        .context("getting partition info")?;
    Ok(u64::from(info.block_size) * u64::from(info.block_count))
}

struct BlockWatcherPauser {
    connection: Option<BlockWatcherProxy>,
}

impl BlockWatcherPauser {
    async fn new() -> Result<Self, Error> {
        let connection = fuchsia_component::client::connect_to_protocol::<BlockWatcherMarker>()
            .context("connecting to block watcher")?;
        connection.pause().await.context("pausing the block watcher")?;

        Ok(Self { connection: Some(connection) })
    }

    async fn resume(&mut self) -> Result<(), Error> {
        if let Some(conn) = self.connection.take() {
            conn.resume().await.context("resuming the block watcher")?;
        }
        Ok(())
    }
}

impl Drop for BlockWatcherPauser {
    fn drop(&mut self) {
        if self.connection.is_some() {
            let mut executor = fuchsia_async::LocalExecutor::new();
            executor
                .run_singlethreaded(self.resume())
                .map_err(|err| error!(?err, "failed to resume block watcher"))
                .ok();
        }
    }
}

pub struct FvmRamdisk {
    ramdisk: RamdiskClient,
    sparse_fvm_path: String,
    pauser: BlockWatcherPauser,
}

impl FvmRamdisk {
    pub async fn new(ramdisk_size: u64, sparse_fvm_path: String) -> Result<Self, Error> {
        Self::new_with_physmemfn(ramdisk_size, sparse_fvm_path, zx::system_get_physmem).await
    }

    async fn new_with_physmemfn<PhysmemFn>(
        ramdisk_size: u64,
        sparse_fvm_path: String,
        get_physmem_size: PhysmemFn,
    ) -> Result<Self, Error>
    where
        PhysmemFn: Fn() -> u64,
    {
        let physmem_size = get_physmem_size();
        if ramdisk_size >= physmem_size {
            return Err(anyhow::anyhow!(
                "Not enough available physical memory to create the ramdisk!"
            ));
        }
        let pauser = BlockWatcherPauser::new().await.context("pausing block watcher")?;
        let ramdisk = Self::create_ramdisk_with_fvm(ramdisk_size)
            .await
            .context("Creating ramdisk with partition table")?;

        let ret = FvmRamdisk { ramdisk, sparse_fvm_path, pauser };

        // Manually bind the GPT driver now that the disk contains a valid GPT, so that
        // the paver can pave to the disk.
        ret.rebind_gpt_driver().await.context("Rebinding GPT driver")?;
        Ok(ret)
    }

    /// Pave the FVM into this ramdisk, consuming this object
    /// and leaving the ramdisk to run once the application has quit.
    pub async fn pave_fvm(mut self) -> Result<(), Error> {
        info!("connecting to the paver");
        let client_end = self.ramdisk.open().await.context("Opening ramdisk")?;
        let paver =
            fuchsia_component::client::connect_to_protocol::<fidl_fuchsia_paver::PaverMarker>()?;
        let (data_sink, remote) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_paver::DynamicDataSinkMarker>()?;
        let () = paver.use_block_device(
            client_end,
            self.ramdisk.open_controller().await.context("Opening ramdisk controller")?,
            remote,
        )?;

        // Set up a PayloadStream to serve the data sink.
        let fvm_block = fuchsia_component::client::connect_to_protocol_at_path::<BlockMarker>(
            &self.sparse_fvm_path,
        )?;
        let streamer: Box<dyn PayloadStreamer> =
            Box::new(BlockDevicePayloadStreamer::new(fvm_block).await?);
        let (client, server) =
            fidl::endpoints::create_request_stream::<fidl_fuchsia_paver::PayloadStreamMarker>()?;

        let server = streamer.service_payload_stream_requests(server);

        // Run the server and client ends of the PayloadStream concurrently.
        try_join(server, data_sink.write_volumes(client).map_err(|e| e.into())).await?;

        self.pauser.resume().await.context("resuming block watcher")?;
        // Now that the ramdisk has successfully been paved, and the block watcher resumed,
        // we need to force a rebind of everything so that the block watcher sees the final
        // state of the FVM partition. Without this, the block watcher ignores the FVM because
        // it was paused when it appeared.
        self.rebind_gpt_driver().await.context("rebinding GPT driver")?;
        // Forget the ramdisk, so that we don't run its destructor (which closes it).
        // This means that we don't have to keep running to keep the ramdisk around.
        std::mem::forget(self.ramdisk);
        Ok(())
    }

    /// Creates a ramdisk and writes a suitable partition table to it,
    /// and automatically rebinds the appropriate drivers so the GPT is parsed.
    async fn create_ramdisk_with_fvm(size: u64) -> Result<RamdiskClient, Error> {
        let blocks = size / BLOCK_SIZE;
        let ramdisk = RamdiskClientBuilder::new(BLOCK_SIZE, blocks)
            .build()
            .await
            .context("building ramdisk")?;
        let channel = ramdisk.open().await.context("Opening ramdisk")?;
        let block_client = remote_block_device::RemoteBlockClientSync::new(channel)
            .context("creating remote block client")?;
        let cache = remote_block_device::cache::Cache::new(block_client)
            .context("creating remote block client cache")?;
        gpt::write_ramdisk(Box::new(cache)).context("writing GPT")?;

        Ok(ramdisk)
    }

    /// Explicitly binds the GPT driver to the given ramdisk.
    async fn rebind_gpt_driver(&self) -> Result<(), Error> {
        let client_end = self.ramdisk.open().await?;
        // TODO(https://fxbug.dev/112484): this relies on multiplexing.
        let client_end =
            fidl::endpoints::ClientEnd::<ControllerMarker>::new(client_end.into_channel());
        let controller = client_end.into_proxy()?;
        controller.rebind("gpt.cm").await?.map_err(zx::Status::from_raw)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[fuchsia::test]
    async fn test_not_enough_physmem() {
        assert!(FvmRamdisk::new_with_physmemfn(512, "".to_owned(), || 512).await.is_err());
        assert!(FvmRamdisk::new_with_physmemfn(512, "".to_owned(), || 6).await.is_err());
    }
}
