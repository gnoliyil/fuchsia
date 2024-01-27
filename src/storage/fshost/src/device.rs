// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod constants;

use {
    anyhow::{anyhow, Context, Error},
    async_trait::async_trait,
    fidl::endpoints::{create_endpoints, ClientEnd, Proxy as _},
    fidl_fuchsia_device::ControllerMarker,
    fidl_fuchsia_hardware_block::BlockMarker,
    fidl_fuchsia_hardware_block_volume::VolumeAndNodeProxy,
    fidl_fuchsia_io::OpenFlags,
    fs_management::format::{detect_disk_format, DiskFormat},
    fuchsia_component::client::connect_to_protocol_at_path,
    fuchsia_zircon as zx,
};

#[async_trait]
pub trait Device: Send + Sync {
    /// Returns BlockInfo (the result of calling fuchsia.hardware.block/Block.Query).
    async fn get_block_info(&self) -> Result<fidl_fuchsia_hardware_block::BlockInfo, Error>;

    /// True if this is a NAND device.
    fn is_nand(&self) -> bool;

    /// Returns the format as determined by content sniffing. This should be used sparingly when
    /// other means of determining the format are not possible.
    async fn content_format(&mut self) -> Result<DiskFormat, Error>;

    /// Returns the topological path.
    fn topological_path(&self) -> &str;

    /// Returns the path in /dev/class. This path is absolute and includes the /dev/class prefix.
    fn path(&self) -> &str;

    /// If this device is a partition, this returns the label. Otherwise, an error is returned.
    async fn partition_label(&mut self) -> Result<&str, Error>;

    /// If this device is a partition, this returns the type GUID. Otherwise, an error is returned.
    async fn partition_type(&mut self) -> Result<&[u8; 16], Error>;

    /// If this device is a partition, this returns the instance GUID.
    /// Otherwise, an error is returned.
    async fn partition_instance(&mut self) -> Result<&[u8; 16], Error>;

    /// If this device is a volume, this allows resizing the device.
    /// Returns actual byte size assuming success, or error.
    async fn resize(&mut self, _target_size_bytes: u64) -> Result<u64, Error> {
        Err(anyhow!("Unimplemented"))
    }

    /// Returns a channel connected to the device.
    fn client_end(&self) -> Result<ClientEnd<BlockMarker>, Error>;

    /// Returns a new Device, which is a child of this device with the specified suffix. This
    /// function will return when the device is available. This function assumes the child device
    /// will show up in /dev/class/block.
    async fn get_child(&self, suffix: &str) -> Result<Box<dyn Device>, Error>;
}

/// A nand device.
#[derive(Clone, Debug)]
pub struct NandDevice {
    block_device: BlockDevice,
}

impl NandDevice {
    pub async fn new(path: impl ToString) -> Result<Self, Error> {
        Ok(NandDevice { block_device: BlockDevice::new(path).await? })
    }
}

#[async_trait]
impl Device for NandDevice {
    fn is_nand(&self) -> bool {
        true
    }

    async fn get_block_info(&self) -> Result<fidl_fuchsia_hardware_block::BlockInfo, Error> {
        Err(anyhow!("not supported by nand device"))
    }

    async fn content_format(&mut self) -> Result<DiskFormat, Error> {
        Ok(DiskFormat::Unknown)
    }

    async fn get_child(&self, suffix: &str) -> Result<Box<dyn Device>, Error> {
        const DEV_CLASS_NAND: &str = "/dev/class/nand";
        let dev_class_nand =
            fuchsia_fs::directory::open_in_namespace(DEV_CLASS_NAND, OpenFlags::RIGHT_READABLE)?;
        let child_path = device_watcher::wait_for_device_with(
            &dev_class_nand,
            |device_watcher::DeviceInfo { filename, topological_path }| {
                topological_path.strip_suffix(suffix).and_then(|topological_path| {
                    (topological_path == self.topological_path())
                        .then(|| format!("{}/{}", DEV_CLASS_NAND, filename))
                })
            },
        )
        .await?;
        let nand_device = NandDevice::new(child_path).await?;
        Ok(Box::new(nand_device))
    }

    fn topological_path(&self) -> &str {
        self.block_device.topological_path()
    }

    fn path(&self) -> &str {
        self.block_device.path()
    }

    async fn partition_label(&mut self) -> Result<&str, Error> {
        self.block_device.partition_label().await
    }

    async fn partition_type(&mut self) -> Result<&[u8; 16], Error> {
        self.block_device.partition_type().await
    }

    async fn partition_instance(&mut self) -> Result<&[u8; 16], Error> {
        self.block_device.partition_instance().await
    }

    async fn resize(&mut self, target_size_bytes: u64) -> Result<u64, Error> {
        self.block_device.resize(target_size_bytes).await
    }

    fn client_end(&self) -> Result<ClientEnd<BlockMarker>, Error> {
        self.block_device.client_end()
    }
}

/// A block device.
#[derive(Clone, Debug)]
pub struct BlockDevice {
    // The path of the device in /dev/class/.
    path: String,

    // The topological path.
    topological_path: String,

    // The proxy for the device.  N.B. The device might not support the volume protocol or the
    // composed partition protocol, but it should support the block and node protocols.
    volume_proxy: VolumeAndNodeProxy,

    // Memoized fields.
    content_format: Option<DiskFormat>,
    partition_label: Option<String>,
    partition_type: Option<[u8; 16]>,
    partition_instance: Option<[u8; 16]>,
}

impl BlockDevice {
    pub async fn new(path: impl ToString) -> Result<Self, Error> {
        let path = path.to_string();
        let device_proxy = connect_to_protocol_at_path::<ControllerMarker>(&path)?;
        let topological_path =
            device_proxy.get_topological_path().await?.map_err(zx::Status::from_raw)?;
        Ok(Self::from_proxy(
            VolumeAndNodeProxy::new(device_proxy.into_channel().unwrap()),
            path,
            topological_path,
        ))
    }

    pub fn from_proxy(
        volume_proxy: VolumeAndNodeProxy,
        path: impl ToString,
        topological_path: impl ToString,
    ) -> Self {
        Self {
            path: path.to_string(),
            topological_path: topological_path.to_string(),
            volume_proxy,
            content_format: None,
            partition_label: None,
            partition_type: None,
            partition_instance: None,
        }
    }
}

#[async_trait]
impl Device for BlockDevice {
    async fn get_block_info(&self) -> Result<fidl_fuchsia_hardware_block::BlockInfo, Error> {
        let info = self.volume_proxy.get_info().await?.map_err(zx::Status::from_raw)?;
        Ok(info)
    }

    fn is_nand(&self) -> bool {
        false
    }

    async fn content_format(&mut self) -> Result<DiskFormat, Error> {
        if let Some(format) = self.content_format {
            return Ok(format);
        }
        let block = self.client_end()?;
        let block = block.into_proxy()?;
        return Ok(detect_disk_format(&block).await);
    }

    fn topological_path(&self) -> &str {
        &self.topological_path
    }

    fn path(&self) -> &str {
        &self.path
    }

    async fn partition_label(&mut self) -> Result<&str, Error> {
        if self.partition_label.is_none() {
            let (status, name) = self.volume_proxy.get_name().await?;
            zx::Status::ok(status)?;
            self.partition_label = Some(name.ok_or(anyhow!("Expected name"))?);
        }
        Ok(self.partition_label.as_ref().unwrap())
    }

    async fn partition_type(&mut self) -> Result<&[u8; 16], Error> {
        if self.partition_type.is_none() {
            let (status, partition_type) = self.volume_proxy.get_type_guid().await?;
            zx::Status::ok(status)?;
            self.partition_type = Some(partition_type.ok_or(anyhow!("Expected type"))?.value);
        }
        Ok(self.partition_type.as_ref().unwrap())
    }

    async fn partition_instance(&mut self) -> Result<&[u8; 16], Error> {
        if self.partition_instance.is_none() {
            let (status, instance_guid) = self
                .volume_proxy
                .get_instance_guid()
                .await
                .context("Transport error get_instance_guid")?;
            zx::Status::ok(status).context("get_instance_guid failed")?;
            self.partition_instance =
                Some(instance_guid.ok_or(anyhow!("Expected instance guid"))?.value);
        }
        Ok(self.partition_instance.as_ref().unwrap())
    }

    async fn resize(&mut self, target_size_bytes: u64) -> Result<u64, Error> {
        let volume_proxy = {
            let volume: ClientEnd<fidl_fuchsia_hardware_block_volume::VolumeMarker> =
                self.client_end()?.into_channel().into();
            volume.into_proxy()?
        };
        crate::volume::resize_volume(&volume_proxy, target_size_bytes, false).await
    }

    fn client_end(&self) -> Result<ClientEnd<BlockMarker>, Error> {
        let (client, server) = create_endpoints();
        self.volume_proxy.clone(OpenFlags::CLONE_SAME_RIGHTS, server)?;
        Ok(client.into_channel().into())
    }

    async fn get_child(&self, suffix: &str) -> Result<Box<dyn Device>, Error> {
        const DEV_CLASS_BLOCK: &str = "/dev/class/block";
        let dev_class_block =
            fuchsia_fs::directory::open_in_namespace(DEV_CLASS_BLOCK, OpenFlags::RIGHT_READABLE)?;
        let child_path = device_watcher::wait_for_device_with(
            &dev_class_block,
            |device_watcher::DeviceInfo { filename, topological_path }| {
                topological_path.strip_suffix(suffix).and_then(|topological_path| {
                    (topological_path == self.topological_path)
                        .then(|| format!("{}/{}", DEV_CLASS_BLOCK, filename))
                })
            },
        )
        .await?;

        let block_device = BlockDevice::new(child_path).await?;
        Ok(Box::new(block_device))
    }
}
