// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::installer::BootloaderType,
    anyhow::{Context as _, Error},
    fidl::endpoints::Proxy,
    fidl_fuchsia_fshost::{BlockWatcherMarker, BlockWatcherProxy},
    fidl_fuchsia_hardware_block::BlockMarker,
    fidl_fuchsia_hardware_block_partition::PartitionProxy,
    fidl_fuchsia_mem::Buffer,
    fidl_fuchsia_paver::{Asset, Configuration, DynamicDataSinkProxy},
    fuchsia_zircon as zx,
    futures::future::try_join,
    futures::TryFutureExt,
    payload_streamer::{BlockDevicePayloadStreamer, PayloadStreamer},
    recovery_util::block::BlockDevice,
    regex,
    remote_block_device::{BlockClient, MutableBufferSlice, RemoteBlockClient},
    std::{cmp::min, fmt, sync::Mutex},
};

/// Number of nanoseconds in a second.
const NS_PER_S: i64 = 1_000_000_000;

struct BlockWatcherPauser {
    proxy: Option<BlockWatcherProxy>,
}

impl BlockWatcherPauser {
    pub async fn new() -> Result<Self, Error> {
        let connection = fuchsia_component::client::connect_to_protocol::<BlockWatcherMarker>()
            .context("Connecting to block watcher")?;
        zx::Status::ok(connection.pause().await.context("Sending pause")?)
            .context("Pausing block watcher")?;
        Ok(BlockWatcherPauser { proxy: Some(connection) })
    }

    pub async fn resume(mut self) -> Result<(), Error> {
        zx::Status::ok(self.proxy.take().unwrap().resume().await.context("Sending resume")?)
            .context("Resuming block watcher")
    }
}

impl Drop for BlockWatcherPauser {
    fn drop(&mut self) {
        assert!(self.proxy.is_none())
    }
}

#[derive(Debug, PartialEq)]
pub enum PartitionPaveType {
    Asset { r#type: Asset, config: Configuration },
    Volume,
    Bootloader,
}

/// Represents a partition that will be paved to the disk.
pub struct Partition {
    pave_type: PartitionPaveType,
    src: String,
    size: u64,
    block_size: u64,
}

/// This GUID is used by the installer to identify partitions that contain
/// data that will be installed to disk. The `fx mkinstaller` tool generates
/// images containing partitions with this GUID.
static WORKSTATION_INSTALLER_GPT: [u8; 16] = [
    0xce, 0x98, 0xce, 0x4d, 0x7e, 0xe7, 0xc1, 0x45, 0xa8, 0x63, 0xca, 0xf9, 0x2f, 0x13, 0x30, 0xc1,
];

/// These GUIDs are used by the installer to identify partitions that contain
/// data that will be installed to disk from a usb disk. The `fx make-fuchsia-vol`
/// tool generates images containing partitions with these GUIDs.
static WORKSTATION_PARTITION_GPTS: [[u8; 16]; 5] = [
    [
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9,
        0x3B,
    ], // efi
    [
        0x5D, 0x39, 0x75, 0x1D, 0xC6, 0xF2, 0x6B, 0x47, 0xA8, 0xB7, 0x45, 0xCC, 0x1C, 0x97, 0xB4,
        0x76,
    ], // misc
    [
        0x86, 0xCC, 0x30, 0xDE, 0x4A, 0x1F, 0x31, 0x4A, 0x93, 0xC4, 0x66, 0xF1, 0x47, 0xD3, 0x3E,
        0x05,
    ], // zircon-a
    [
        0xDF, 0x04, 0xCC, 0x23, 0x78, 0xC2, 0xE7, 0x4C, 0x84, 0x71, 0x89, 0x7D, 0x1A, 0x4B, 0xCD,
        0xF7,
    ], // zircon-b
    [
        0x57, 0xCF, 0xE5, 0xA0, 0xEF, 0x2D, 0xBE, 0x46, 0xA8, 0x0C, 0xA2, 0x06, 0x7C, 0x37, 0xCD,
        0x49,
    ], // zircon-r
];

impl Partition {
    /// Creates a new partition. Returns `None` if the partition is not
    /// a partition that should be paved to the disk.
    ///
    /// # Arguments
    /// * `src` - path to a block device that represents this partition.
    /// * `part` - a |PartitionProxy| that is connected to this partition.
    /// * `bootloader` - the |BootloaderType| of this device.
    ///
    async fn new(
        src: String,
        part: PartitionProxy,
        bootloader: BootloaderType,
    ) -> Result<Option<Self>, Error> {
        let (status, guid) = part.get_type_guid().await.context("Get type guid failed")?;
        if let None = guid {
            return Err(Error::new(zx::Status::from_raw(status)));
        }

        let (_status, name) = part.get_name().await.context("Get name failed")?;
        let pave_type;
        if let Some(string) = name {
            let guid = guid.unwrap();
            if guid.value != WORKSTATION_INSTALLER_GPT
                && !(src.contains("usb-bus") && WORKSTATION_PARTITION_GPTS.contains(&guid.value))
            {
                return Ok(None);
            }
            // TODO(fxbug.dev/44595) support any other partitions that might be needed
            if string == "storage-sparse" {
                pave_type = Some(PartitionPaveType::Volume);
            } else if bootloader == BootloaderType::Efi {
                pave_type = Partition::get_efi_pave_type(&string.to_lowercase());
            } else if bootloader == BootloaderType::Coreboot {
                pave_type = Partition::get_coreboot_pave_type(&string);
            } else {
                pave_type = None;
            }
        } else {
            return Ok(None);
        }

        if let Some(pave_type) = pave_type {
            let info =
                part.get_info().await.context("Get info failed")?.map_err(zx::Status::from_raw)?;
            let block_size = info.block_size.into();
            let size = info.block_count * block_size;

            Ok(Some(Partition { pave_type, src, size, block_size }))
        } else {
            Ok(None)
        }
    }

    fn get_efi_pave_type(label: &str) -> Option<PartitionPaveType> {
        if label.starts_with("zircon-") && label.len() == "zircon-x".len() {
            let configuration = Partition::letter_to_configuration(label.chars().last().unwrap());
            Some(PartitionPaveType::Asset { r#type: Asset::Kernel, config: configuration })
        } else if label.starts_with("vbmeta_") && label.len() == "vbmeta_x".len() {
            let configuration = Partition::letter_to_configuration(label.chars().last().unwrap());
            Some(PartitionPaveType::Asset {
                r#type: Asset::VerifiedBootMetadata,
                config: configuration,
            })
        } else if label.starts_with("efi") || label.starts_with("fuchsia.esp") {
            Some(PartitionPaveType::Bootloader)
        } else {
            None
        }
    }

    fn get_coreboot_pave_type(label: &str) -> Option<PartitionPaveType> {
        if let Ok(re) = regex::Regex::new(r"^zircon-(.)\.signed$") {
            if let Some(captures) = re.captures(label) {
                let config = Partition::letter_to_configuration(
                    captures.get(1).unwrap().as_str().chars().last().unwrap(),
                );
                Some(PartitionPaveType::Asset { r#type: Asset::Kernel, config: config })
            } else {
                None
            }
        } else {
            None
        }
    }

    /// Gather all partitions that are children of the given block device,
    /// and return them.
    ///
    /// # Arguments
    /// * `block_device` - the |BlockDevice| to get partitions from.
    /// * `all_devices` - All known block devices in the system.
    /// * `bootloader` - the |BootloaderType| of this device.
    pub async fn get_partitions(
        block_device: &BlockDevice,
        all_devices: &Vec<BlockDevice>,
        bootloader: BootloaderType,
    ) -> Result<Vec<Self>, Error> {
        let mut partitions = Vec::new();

        for entry in all_devices {
            if !entry.topo_path.starts_with(&block_device.topo_path) || entry == block_device {
                // Skip partitions that are not children of this block device, and skip the block
                // device itself.
                continue;
            }
            let (local, remote) = zx::Channel::create();
            fdio::service_connect(&entry.class_path, remote).context("Connecting to partition")?;
            let local = fidl::AsyncChannel::from_channel(local).context("Creating AsyncChannel")?;

            let proxy = PartitionProxy::from_channel(local);
            if let Some(partition) = Partition::new(entry.class_path.clone(), proxy, bootloader)
                .await
                .context(format!(
                    "Creating partition for block device at {} ({})",
                    entry.topo_path, entry.class_path
                ))?
            {
                partitions.push(partition);
            }
        }
        Ok(partitions)
    }

    /// Pave this partition to disk, using the given |DynamicDataSinkProxy|.
    pub async fn pave(
        &self,
        data_sink: &DynamicDataSinkProxy,
        progress_callback: Box<dyn Send + Sync + Fn(usize, usize) -> ()>,
    ) -> Result<(), Error> {
        match self.pave_type {
            PartitionPaveType::Asset { r#type: asset, config } => {
                let mut fidl_buf = self.read_data().await?;
                data_sink.write_asset(config, asset, &mut fidl_buf).await?;
            }
            PartitionPaveType::Bootloader => {
                let mut fidl_buf = self.read_data().await?;
                // Currently we only store the bootloader in slot A, we don't use an A/B/R scheme.
                data_sink.write_firmware(Configuration::A, "", &mut fidl_buf).await?;
            }
            PartitionPaveType::Volume => {
                let pauser = BlockWatcherPauser::new().await.context("Pausing block watcher")?;
                let result = self.pave_volume_paused(data_sink, progress_callback).await;
                pauser.resume().await.context("Resuming block watcher")?;
                result?;
            }
        };
        Ok(())
    }

    /// Pave a volume while the block watcher is paused.
    async fn pave_volume_paused(
        &self,
        data_sink: &DynamicDataSinkProxy,
        progress_callback: Box<dyn Send + Sync + Fn(usize, usize) -> ()>,
    ) -> Result<(), Error> {
        // Set up a PayloadStream to serve the data sink.
        let partition_block =
            fuchsia_component::client::connect_to_protocol_at_path::<BlockMarker>(&self.src)?;
        let streamer: Box<dyn PayloadStreamer> =
            Box::new(BlockDevicePayloadStreamer::new(partition_block).await?);
        let start_time = zx::Time::get_monotonic();
        let last_percent = Mutex::new(0 as i64);
        let status_callback = move |data_read, data_total| {
            progress_callback(data_read, data_total);
            if data_total == 0 {
                return;
            }
            let percent: i64 =
                unsafe { (((data_read as f64) / (data_total as f64)) * 100.0).to_int_unchecked() };
            let mut prev = last_percent.lock().unwrap();
            if percent != *prev {
                let now = zx::Time::get_monotonic();
                let nanos = now.into_nanos() - start_time.into_nanos();
                let secs = nanos / NS_PER_S;
                let rate = ((data_read as f64) / (secs as f64)) / (1024 as f64);

                tracing::info!("Paving FVM: {}% ({:.02} KiB/s)", percent, rate);
                *prev = percent;
            }
        };
        streamer.set_status_callback(Box::new(status_callback)).await;
        let (client, server) =
            fidl::endpoints::create_request_stream::<fidl_fuchsia_paver::PayloadStreamMarker>()?;

        // Run the server and client ends of the PayloadStream concurrently.
        try_join(
            streamer.service_payload_stream_requests(server),
            data_sink.write_volumes(client).map_err(|e| e.into()),
        )
        .await?;

        Ok(())
    }

    /// Pave this A/B partition to its 'B' slot.
    /// Will return an error if the partition is not an A/B partition.
    pub async fn pave_b(&self, data_sink: &DynamicDataSinkProxy) -> Result<(), Error> {
        if !self.is_ab() {
            return Err(Error::from(zx::Status::NOT_SUPPORTED));
        }

        let mut fidl_buf = self.read_data().await?;
        match self.pave_type {
            PartitionPaveType::Asset { r#type: asset, config: _ } => {
                // pave() will always pave to A, so this always paves to B.
                // The A/B config from the partition is not respected because on a fresh
                // install we want A/B to be identical, so we install the same thing to both.
                data_sink.write_asset(Configuration::B, asset, &mut fidl_buf).await?;
                Ok(())
            }
            _ => Err(Error::from(zx::Status::NOT_SUPPORTED)),
        }
    }

    /// Returns true if this partition has A/B variants when installed.
    pub fn is_ab(&self) -> bool {
        if let PartitionPaveType::Asset { r#type: _, config } = self.pave_type {
            // We only check against the A configuration because |letter_to_configuration|
            // returns A for 'A' and 'B' configurations.
            return config == Configuration::A;
        }
        return false;
    }

    /// Read this partition into a FIDL buffer.
    async fn read_data(&self) -> Result<Buffer, Error> {
        let mut rounded_size = self.size;
        let page_size = u64::from(zx::system_get_page_size());
        if rounded_size % page_size != 0 {
            rounded_size += page_size;
            rounded_size -= rounded_size % page_size;
        }

        let vmo = zx::Vmo::create_with_opts(zx::VmoOptions::RESIZABLE, rounded_size)?;

        let proxy =
            fuchsia_component::client::connect_to_protocol_at_path::<BlockMarker>(&self.src)
                .with_context(|| format!("Connecting to block device {}", &self.src))?;
        let block_device = RemoteBlockClient::new(proxy).await?;
        let vmo_id = block_device.attach_vmo(&vmo).await?;

        // Reading too much at a time causes the UMS driver to return an error.
        let max_read_length: u64 = self.block_size * 100;
        let mut read: u64 = 0;
        while read < self.size {
            let read_size = min(self.size - read, max_read_length);
            if let Err(e) = block_device
                .read_at(MutableBufferSlice::new_with_vmo_id(&vmo_id, read, read_size), read)
                .await
                .context("Reading from partition to VMO")
            {
                // Need to detach before returning.
                block_device.detach_vmo(vmo_id).await?;
                return Err(e);
            }

            read += read_size;
        }

        block_device.detach_vmo(vmo_id).await?;

        return Ok(Buffer { vmo: fidl::Vmo::from(vmo), size: self.size });
    }

    /// Return the |Configuration| that is represented by the given
    /// character. Returns 'Recovery' for the letters 'R' and 'r', and 'A' for
    /// anything else.
    fn letter_to_configuration(letter: char) -> Configuration {
        // Note that we treat 'A' and 'B' the same, as the installer will install
        // the same image to both A and B.
        match letter {
            'A' | 'a' => Configuration::A,
            'B' | 'b' => Configuration::A,
            'R' | 'r' => Configuration::Recovery,
            _ => Configuration::A,
        }
    }
}

impl fmt::Debug for Partition {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.pave_type {
            PartitionPaveType::Asset { r#type, config } => write!(
                f,
                "Partition[src={}, pave_type={:?}, asset={:?}, config={:?}]",
                self.src, self.pave_type, r#type, config
            ),
            _ => write!(f, "Partition[src={}, pave_type={:?}]", self.src, self.pave_type),
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl_fuchsia_hardware_block::{BlockInfo, Flag},
        fidl_fuchsia_hardware_block_partition::{
            Guid, PartitionMarker, PartitionRequest, PartitionRequestStream,
        },
        fuchsia_async as fasync,
        futures::TryStreamExt,
    };

    async fn serve_partition(
        label: &str,
        block_size: u32,
        block_count: u64,
        guid: [u8; 16],
        mut stream: PartitionRequestStream,
    ) -> Result<(), Error> {
        while let Some(req) = stream.try_next().await? {
            match req {
                PartitionRequest::GetName { responder } => responder.send(0, Some(label))?,
                PartitionRequest::GetInfo { responder } => responder.send(&mut Ok(BlockInfo {
                    block_count,
                    block_size,
                    max_transfer_size: 0,
                    flags: Flag::empty(),
                }))?,
                PartitionRequest::GetTypeGuid { responder } => {
                    responder.send(0, Some(&mut Guid { value: guid }))?
                }
                _ => panic!("Expected a GetInfo/GetName request, but did not get one."),
            }
        }
        Ok(())
    }

    fn mock_partition(
        label: &'static str,
        block_size: usize,
        block_count: usize,
        guid: [u8; 16],
    ) -> Result<PartitionProxy, Error> {
        let (proxy, stream) = fidl::endpoints::create_proxy_and_stream::<PartitionMarker>()?;
        fasync::Task::local(
            serve_partition(
                label,
                block_size.try_into().unwrap(),
                block_count.try_into().unwrap(),
                guid,
                stream,
            )
            .unwrap_or_else(|e| panic!("Error while serving fake block device: {}", e)),
        )
        .detach();
        Ok(proxy)
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_bad_guid() -> Result<(), Error> {
        let proxy = mock_partition("zircon-a", 512, 1000, [0xaa; 16])?;
        let part = Partition::new("zircon-a".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_none());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_zircona() -> Result<(), Error> {
        let proxy = mock_partition("zircon-a", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("zircon-a".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(
            part.pave_type,
            PartitionPaveType::Asset { r#type: Asset::Kernel, config: Configuration::A }
        );
        assert_eq!(part.size, 512 * 1000);
        assert_eq!(part.src, "zircon-a");
        assert!(part.is_ab());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_zirconb() -> Result<(), Error> {
        let proxy = mock_partition("zircon-b", 20, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("zircon-b".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(
            part.pave_type,
            PartitionPaveType::Asset { r#type: Asset::Kernel, config: Configuration::A }
        );
        assert_eq!(part.size, 20 * 1000);
        assert_eq!(part.src, "zircon-b");
        assert!(part.is_ab());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_zirconr() -> Result<(), Error> {
        let proxy = mock_partition("zircon-r", 40, 200, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("zircon-r".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(
            part.pave_type,
            PartitionPaveType::Asset { r#type: Asset::Kernel, config: Configuration::Recovery }
        );
        assert_eq!(part.size, 40 * 200);
        assert_eq!(part.src, "zircon-r");
        assert!(!part.is_ab());
        Ok(())
    }

    async fn new_partition_vbmetax_test_helper(
        name: &'static str,
        expected_config: Configuration,
    ) -> Result<(), Error> {
        let proxy = mock_partition(name, 40, 200, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new(name.to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(
            part.pave_type,
            PartitionPaveType::Asset {
                r#type: Asset::VerifiedBootMetadata,
                config: expected_config
            }
        );
        assert_eq!(part.size, 40 * 200);
        assert_eq!(part.src, name);
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_vbmetaa() -> Result<(), Error> {
        new_partition_vbmetax_test_helper("vbmeta_a", Configuration::A).await
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_vbmetab() -> Result<(), Error> {
        // 'A' and 'B' are treated the same, as the installer will install
        // the same image to both A and B.
        new_partition_vbmetax_test_helper("vbmeta_b", Configuration::A).await
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_vbmetar() -> Result<(), Error> {
        new_partition_vbmetax_test_helper("vbmeta_r", Configuration::Recovery).await
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_efi() -> Result<(), Error> {
        let proxy = mock_partition("efi", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("efi".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(part.pave_type, PartitionPaveType::Bootloader);
        assert_eq!(part.size, 512 * 1000);
        assert_eq!(part.src, "efi");
        assert!(!part.is_ab());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_fvm() -> Result<(), Error> {
        let proxy = mock_partition("storage-sparse", 2048, 4097, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("storage-sparse".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(part.pave_type, PartitionPaveType::Volume);
        assert_eq!(part.size, 2048 * 4097);
        assert_eq!(part.src, "storage-sparse");
        assert!(!part.is_ab());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_zircona_unsigned_coreboot() -> Result<(), Error> {
        let proxy = mock_partition("zircon-a", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("zircon-a".to_string(), proxy, BootloaderType::Coreboot).await?;
        assert!(part.is_none());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_zircona_signed_coreboot() -> Result<(), Error> {
        let proxy = mock_partition("zircon-a.signed", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part =
            Partition::new("zircon-a.signed".to_string(), proxy, BootloaderType::Coreboot).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(
            part.pave_type,
            PartitionPaveType::Asset { r#type: Asset::Kernel, config: Configuration::A }
        );
        assert_eq!(part.size, 512 * 1000);
        assert_eq!(part.src, "zircon-a.signed");
        assert!(part.is_ab());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_unknown() -> Result<(), Error> {
        let proxy = mock_partition("unknown-label", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("unknown-label".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_none());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_zedboot_efi() -> Result<(), Error> {
        let proxy = mock_partition("zedboot-efi", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("zedboot-efi".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_none());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_invalid_partitions_coreboot() -> Result<(), Error> {
        let proxy = mock_partition("zircon-.signed", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part =
            Partition::new("zircon-.signed".to_string(), proxy, BootloaderType::Coreboot).await?;
        assert!(part.is_none());

        let proxy = mock_partition("zircon-aa.signed", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part =
            Partition::new("zircon-aa.signed".to_string(), proxy, BootloaderType::Coreboot).await?;
        assert!(part.is_none());

        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_invalid_partitions_efi() -> Result<(), Error> {
        let proxy = mock_partition("zircon-", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("zircon-".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_none());

        let proxy = mock_partition("zircon-aa", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part = Partition::new("zircon-aa".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_none());

        let proxy = mock_partition("zircon-a.signed", 512, 1000, WORKSTATION_INSTALLER_GPT)?;
        let part =
            Partition::new("zircon-a.signed".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_none());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_usb_bad_guid() -> Result<(), Error> {
        let proxy = mock_partition("zircon-a", 512, 1000, [0xaa; 16])?;
        let part = Partition::new("/dev/usb-bus".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_none());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_usb_zircona() -> Result<(), Error> {
        let proxy = mock_partition("zircon-a", 512, 1000, WORKSTATION_PARTITION_GPTS[2])?;
        let part = Partition::new("/dev/usb-bus".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(
            part.pave_type,
            PartitionPaveType::Asset { r#type: Asset::Kernel, config: Configuration::A }
        );
        assert_eq!(part.size, 512 * 1000);
        assert_eq!(part.src, "/dev/usb-bus");
        assert!(part.is_ab());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_usb_zirconb() -> Result<(), Error> {
        let proxy = mock_partition("zircon-b", 20, 1000, WORKSTATION_PARTITION_GPTS[3])?;
        let part = Partition::new("/dev/usb-bus".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(
            part.pave_type,
            PartitionPaveType::Asset { r#type: Asset::Kernel, config: Configuration::A }
        );
        assert_eq!(part.size, 20 * 1000);
        assert_eq!(part.src, "/dev/usb-bus");
        assert!(part.is_ab());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_usb_zirconr() -> Result<(), Error> {
        let proxy = mock_partition("zircon-r", 40, 200, WORKSTATION_PARTITION_GPTS[4])?;
        let part = Partition::new("/dev/usb-bus".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(
            part.pave_type,
            PartitionPaveType::Asset { r#type: Asset::Kernel, config: Configuration::Recovery }
        );
        assert_eq!(part.size, 40 * 200);
        assert_eq!(part.src, "/dev/usb-bus");
        assert!(!part.is_ab());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_new_partition_usb_efi() -> Result<(), Error> {
        let proxy = mock_partition("efi-system", 512, 1000, WORKSTATION_PARTITION_GPTS[0])?;
        let part = Partition::new("/dev/usb-bus".to_string(), proxy, BootloaderType::Efi).await?;
        assert!(part.is_some());
        let part = part.unwrap();
        assert_eq!(part.pave_type, PartitionPaveType::Bootloader);
        assert_eq!(part.size, 512 * 1000);
        assert_eq!(part.src, "/dev/usb-bus");
        assert!(!part.is_ab());
        Ok(())
    }
}
