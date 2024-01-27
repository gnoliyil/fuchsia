// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A safe rust wrapper for creating and using ramdisks.

#![deny(missing_docs)]

#[allow(bad_style)]
mod ramdevice_sys;

use {
    anyhow::{Context as _, Error},
    fidl_fuchsia_hardware_block as fhardware_block, fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    std::{ffi, fs, os::unix::io::AsRawFd as _, ptr},
    zx::HandleBased as _,
};
enum DevRoot {
    Provided(fs::File),
}

const GUID_LEN: usize = 16;
const DEV_PATH: &str = "/dev";
const RAMCTL_PATH: &str = "sys/platform/00:00:2d/ramctl";

/// A type to help construct a [`RamdeviceClient`] from an existing VMO.
pub struct VmoRamdiskClientBuilder {
    vmo: zx::Vmo,
    block_size: Option<u64>,
    dev_root: Option<DevRoot>,
    guid: Option<[u8; GUID_LEN]>,
}

impl VmoRamdiskClientBuilder {
    /// Create a new ramdisk builder with the given VMO handle.
    pub fn new(vmo: zx::Vmo) -> Self {
        Self { vmo, block_size: None, dev_root: None, guid: None }
    }

    /// Set the size of a single block (in bytes)
    pub fn block_size(mut self, block_size: u64) -> Self {
        self.block_size = Some(block_size);
        self
    }

    /// Use the given directory as "/dev" instead of opening "/dev" from the environment.
    pub fn dev_root(mut self, dev_root: fs::File) -> Self {
        self.dev_root = Some(DevRoot::Provided(dev_root));
        self
    }

    /// Initialize the ramdisk with the given GUID, which can be queried from the ramdisk instance.
    pub fn guid(mut self, guid: [u8; GUID_LEN]) -> Self {
        self.guid = Some(guid);
        self
    }

    // This method is not async because its body contains raw pointers, which are not Send.
    fn build_sync(self) -> Result<RamdiskClient, zx::Status> {
        let Self { vmo, block_size, dev_root, guid } = self;
        let vmo_handle = vmo.into_raw();
        let mut ramdisk: *mut ramdevice_sys::ramdisk_client_t = ptr::null_mut();
        let block_size = block_size.unwrap_or(0);
        let type_guid: *const u8 =
            if let Some(guid) = guid.as_ref() { guid.as_ptr() } else { std::ptr::null() };

        let status = if let Some(dev_root) = dev_root {
            let dev_root = match &dev_root {
                DevRoot::Provided(f) => f,
            };
            // Safe because ramdisk_create_at_from_vmo_with_params creates a duplicate fd
            // of the provided dev_root_fd.
            unsafe {
                ramdevice_sys::ramdisk_create_at_from_vmo_with_params(
                    dev_root.as_raw_fd(),
                    vmo_handle,
                    block_size,
                    type_guid,
                    GUID_LEN.try_into().unwrap(),
                    &mut ramdisk,
                )
            }
        } else {
            unsafe {
                ramdevice_sys::ramdisk_create_from_vmo_with_params(
                    vmo_handle,
                    block_size,
                    type_guid,
                    GUID_LEN.try_into().unwrap(),
                    &mut ramdisk,
                )
            }
        };

        // The returned ramdisk is valid iff the FFI method returns ZX_OK.
        zx::Status::ok(status)?;
        Ok(RamdiskClient { ramdisk })
    }

    /// Create the ramdisk.
    pub async fn build(self) -> Result<RamdiskClient, Error> {
        let directory = if let Some(dev_root) = &self.dev_root {
            let dev_root = match &dev_root {
                DevRoot::Provided(f) => f,
            };
            let directory = fdio::clone_channel(dev_root).context("clone channel")?;
            let directory: fidl::endpoints::ClientEnd<fio::DirectoryMarker> = directory.into();
            directory.into_proxy().context("into proxy")?
        } else {
            fuchsia_fs::directory::open_in_namespace(DEV_PATH, fio::OpenFlags::RIGHT_READABLE)
                .with_context(|| format!("open {}", DEV_PATH))?
        };
        let _: fio::NodeProxy =
            device_watcher::recursive_wait_and_open_node(&directory, RAMCTL_PATH)
                .await
                .with_context(|| format!("waiting for {}", RAMCTL_PATH))?;
        self.build_sync().context("build via FFI")
    }
}

/// A type to help construct a [`RamdeviceClient`].
pub struct RamdiskClientBuilder {
    block_size: u64,
    block_count: u64,
    dev_root: Option<DevRoot>,
    guid: Option<[u8; GUID_LEN]>,
}

impl RamdiskClientBuilder {
    /// Create a new ramdisk builder with the given block_size and block_count.
    pub fn new(block_size: u64, block_count: u64) -> Self {
        Self { block_size, block_count, dev_root: None, guid: None }
    }

    /// Use the given directory as "/dev" instead of opening "/dev" from the environment.
    pub fn dev_root(mut self, dev_root: fs::File) -> Self {
        self.dev_root = Some(DevRoot::Provided(dev_root));
        self
    }

    /// Initialize the ramdisk with the given GUID, which can be queried from the ramdisk instance.
    pub fn guid(mut self, guid: [u8; GUID_LEN]) -> Self {
        self.guid = Some(guid);
        self
    }

    // This method is not async because its body contains raw pointers, which are not Send.
    fn build_sync(self) -> Result<RamdiskClient, zx::Status> {
        let Self { block_size, block_count, dev_root, guid } = self;
        let type_guid: *const u8 =
            if let Some(guid) = guid.as_ref() { guid.as_ptr() } else { std::ptr::null() };
        let mut ramdisk: *mut ramdevice_sys::ramdisk_client_t = ptr::null_mut();

        let status = if let Some(dev_root) = dev_root {
            let dev_root = match &dev_root {
                DevRoot::Provided(f) => f,
            };
            // Safe because ramdisk_create_at creates a duplicate fd of the provided dev_root_fd.
            unsafe {
                ramdevice_sys::ramdisk_create_at_with_guid(
                    dev_root.as_raw_fd(),
                    block_size,
                    block_count,
                    type_guid,
                    GUID_LEN.try_into().unwrap(),
                    &mut ramdisk,
                )
            }
        } else {
            unsafe {
                ramdevice_sys::ramdisk_create_with_guid(
                    block_size,
                    block_count,
                    type_guid,
                    GUID_LEN.try_into().unwrap(),
                    &mut ramdisk,
                )
            }
        };

        // The returned ramdisk is valid iff the FFI method returns ZX_OK.
        zx::Status::ok(status)?;
        Ok(RamdiskClient { ramdisk })
    }

    /// Create the ramdisk.
    pub async fn build(self) -> Result<RamdiskClient, Error> {
        let directory = if let Some(dev_root) = &self.dev_root {
            let dev_root = match &dev_root {
                DevRoot::Provided(f) => f,
            };
            let directory = fdio::clone_channel(dev_root).context("clone channel")?;
            let directory: fidl::endpoints::ClientEnd<fio::DirectoryMarker> = directory.into();
            directory.into_proxy().context("into proxy")?
        } else {
            fuchsia_fs::directory::open_in_namespace(DEV_PATH, fio::OpenFlags::RIGHT_READABLE)
                .with_context(|| format!("open {}", DEV_PATH))?
        };
        let _: fio::NodeProxy =
            device_watcher::recursive_wait_and_open_node(&directory, RAMCTL_PATH)
                .await
                .with_context(|| format!("waiting for {}", RAMCTL_PATH))?;
        self.build_sync().context("build via FFI")
    }
}

/// A client for managing a ramdisk. This can be created with the [`RamdiskClient::create`]
/// function or through the type returned by [`RamdiskClient::builder`] to specify additional
/// options.
pub struct RamdiskClient {
    // we own this pointer - nothing in the ramdisk library keeps it, and we don't pass it anywhere,
    // and the only valid way to get one is to have been the thing that made the ramdisk in the
    // first place.
    ramdisk: *mut ramdevice_sys::ramdisk_client_t,
}

impl RamdiskClient {
    /// Create a new ramdisk builder with the given block_size and block_count.
    pub fn builder(block_size: u64, block_count: u64) -> RamdiskClientBuilder {
        RamdiskClientBuilder::new(block_size, block_count)
    }

    /// Create a new ramdisk.
    pub async fn create(block_size: u64, block_count: u64) -> Result<Self, Error> {
        Self::builder(block_size, block_count).build().await
    }

    /// Get the device path of the associated ramdisk. Note that if this ramdisk was created with a
    /// custom dev_root, the returned path will be relative to that handle.
    pub fn get_path(&self) -> &str {
        let Self { ramdisk } = self;
        unsafe {
            let raw_path = ramdevice_sys::ramdisk_get_path(*ramdisk);
            // We can trust this path to be valid UTF-8
            ffi::CStr::from_ptr(raw_path).to_str().expect("ramdisk path was not utf8?")
        }
    }

    /// Get an open channel to the underlying ramdevice.
    pub fn open(
        &self,
    ) -> Result<fidl::endpoints::ClientEnd<fhardware_block::BlockMarker>, zx::Status> {
        let Self { ramdisk } = self;
        let handle = unsafe { ramdevice_sys::ramdisk_get_block_interface(*ramdisk) };
        // TODO(https://fxbug.dev/89811): Remove this when we're not restricted
        // to this FFI interface. Unfortunately there is no public API that
        // allows us to clone an unowned handle. We could expose this function
        // from fdio::fdio_sys, but it seems better to keep this hack localized.
        //
        // TODO(https://fxbug.dev/112484): this relies on multiplexing.
        extern "C" {
            pub fn fdio_service_clone(node: zx::sys::zx_handle_t) -> zx::sys::zx_handle_t;
        }
        let handle = unsafe { fdio_service_clone(handle) };
        match handle {
            zx::sys::ZX_HANDLE_INVALID => Err(zx::Status::NOT_SUPPORTED),
            handle => {
                let handle = unsafe { zx::Handle::from_raw(handle) };
                Ok(zx::Channel::from(handle).into())
            }
        }
    }

    /// Remove the underlying ramdisk. This deallocates all resources for this ramdisk, which will
    /// remove all data written to the associated ramdisk.
    pub fn destroy(self) -> Result<(), zx::Status> {
        let Self { ramdisk } = self;
        // we are doing the same thing as the `Drop` impl, so tell rust not to drop it
        let status = unsafe { ramdevice_sys::ramdisk_destroy(ramdisk) };
        std::mem::forget(self);
        zx::Status::ok(status)
    }
}

/// This struct has exclusive ownership of the ramdisk pointer.
/// It is safe to move this struct between threads.
unsafe impl Send for RamdiskClient {}

/// The only methods that can be invoked from a reference of RamdiskClient
/// are open() and get_path(). Both these functions are non-destructive and
/// can be called from multiple threads.
/// This implies that it is safe to share a reference to RamdiskClient between threads.
unsafe impl Sync for RamdiskClient {}

impl Drop for RamdiskClient {
    fn drop(&mut self) {
        let Self { ramdisk } = self;
        let _ = unsafe { ramdevice_sys::ramdisk_destroy(*ramdisk) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Note that if these tests flake, all downstream tests that depend on this crate may too.

    const TEST_GUID: [u8; GUID_LEN] = [
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
        0x10,
    ];

    #[fuchsia::test]
    async fn create_get_path_destroy() {
        // just make sure all the functions are hooked up properly.
        let ramdisk =
            RamdiskClient::builder(512, 2048).build().await.expect("failed to create ramdisk");
        let _path = ramdisk.get_path();
        assert_eq!(ramdisk.destroy(), Ok(()));
    }

    #[fuchsia::test]
    async fn create_with_dev_root_and_guid_get_path_destroy() {
        let devroot = std::fs::File::open("/dev").unwrap();
        let ramdisk = RamdiskClient::builder(512, 2048)
            .dev_root(devroot)
            .guid(TEST_GUID)
            .build()
            .await
            .expect("failed to create ramdisk");
        let _path = ramdisk.get_path();
        assert_eq!(ramdisk.destroy(), Ok(()));
    }

    #[fuchsia::test]
    async fn create_with_guid_get_path_destroy() {
        let ramdisk = RamdiskClient::builder(512, 2048)
            .guid(TEST_GUID)
            .build()
            .await
            .expect("failed to create ramdisk");
        let _path = ramdisk.get_path();
        assert_eq!(ramdisk.destroy(), Ok(()));
    }

    #[fuchsia::test]
    async fn create_open_destroy() {
        let ramdisk = RamdiskClient::create(512, 2048).await.unwrap();
        let _: fidl::endpoints::ClientEnd<fhardware_block::BlockMarker> = ramdisk.open().unwrap();
        assert_eq!(ramdisk.destroy(), Ok(()));
    }

    #[fuchsia::test]
    async fn create_describe_destroy() {
        let ramdisk = RamdiskClient::create(512, 2048).await.unwrap();
        let client_end = ramdisk.open().unwrap();

        // Ask it to describe itself using the Node interface.
        //
        // TODO(https://fxbug.dev/112484): this relies on multiplexing.
        let client_end =
            fidl::endpoints::ClientEnd::<fio::NodeMarker>::new(client_end.into_channel());
        let proxy = client_end.into_proxy().unwrap();
        let protocol = proxy.query().await.expect("failed to get node info");
        assert_eq!(protocol, fio::NODE_PROTOCOL_NAME.as_bytes());

        assert_eq!(ramdisk.destroy(), Ok(()));
    }
}
