// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::fuchsia::{
        directory::FxDirectory,
        errors::map_to_status,
        node::{FxNode, OpenedNode},
        paged_object_handle::PagedObjectHandle,
        pager::{default_page_in, PagerBacked, PagerPacketReceiverRegistration},
        volume::{info_to_filesystem_info, FxVolume},
    },
    anyhow::Error,
    async_trait::async_trait,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io as fio,
    fuchsia_zircon::{self as zx, HandleBased, Status},
    futures::future::BoxFuture,
    fxfs::{
        async_enter,
        filesystem::SyncOptions,
        log::*,
        object_handle::{ObjectHandle, ObjectProperties, ReadObjectHandle},
        object_store::{
            transaction::{lock_keys, LockKey, Options},
            DataObjectHandle, ObjectDescriptor, Timestamp,
        },
        round::round_up,
    },
    std::{
        ops::Range,
        sync::{
            atomic::{AtomicUsize, Ordering},
            Arc,
        },
    },
    storage_device::buffer,
    vfs::{
        attributes,
        common::rights_to_posix_mode_bits,
        directory::{
            entry::{DirectoryEntry, EntryInfo},
            entry_container::MutableDirectory,
        },
        execution_scope::ExecutionScope,
        file::{File, FileOptions, GetVmo, StreamIoConnection, SyncMode},
        name::Name,
        path::Path,
        ObjectRequestRef, ProtocolsExt, ToObjectRequest,
    },
};

// When the top bit of the open count is set, it means the file has been deleted and when the count
// drops to zero, it will be tombstoned.  Once it has dropped to zero, it cannot be opened again
// (assertions will fire).
const PURGED: usize = 1 << (usize::BITS - 1);

/// FxFile represents an open connection to a file.
pub struct FxFile {
    handle: PagedObjectHandle,
    open_count: AtomicUsize,
}

impl FxFile {
    pub fn new(handle: DataObjectHandle<FxVolume>) -> Arc<Self> {
        let file = Arc::new(Self {
            handle: PagedObjectHandle::new(handle),
            open_count: AtomicUsize::new(0),
        });

        file.handle.owner().pager().register_file(&file);
        file
    }

    pub fn open_count(&self) -> usize {
        self.open_count.load(Ordering::Relaxed)
    }

    pub fn create_connection_async(
        this: OpenedNode<FxFile>,
        scope: ExecutionScope,
        flags: impl ProtocolsExt,
        object_request: ObjectRequestRef<'_>,
    ) -> Result<BoxFuture<'static, ()>, zx::Status> {
        if let Some(rights) = flags.rights() {
            if rights.intersects(fio::Operations::READ_BYTES | fio::Operations::WRITE_BYTES) {
                if let Some(fut) = this.handle.pre_fetch_keys() {
                    this.handle.owner().scope().spawn(fut);
                }
            }
        }
        object_request.create_connection(
            scope.clone(),
            this.take(),
            flags,
            StreamIoConnection::create,
        )
    }

    /// Marks the file as being purged.  Returns true if there are no open references.
    pub fn mark_purged(&self) -> bool {
        let mut old = self.open_count.load(Ordering::Relaxed);
        loop {
            assert_eq!(old & PURGED, 0);
            match self.open_count.compare_exchange_weak(
                old,
                old | PURGED,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => return old == 0,
                Err(x) => old = x,
            }
        }
    }

    pub async fn flush(&self) -> Result<(), Error> {
        self.handle.flush().await
    }

    pub fn get_block_size(&self) -> u64 {
        self.handle.block_size()
    }

    pub async fn is_allocated(&self, start_offset: u64) -> Result<(bool, u64), Status> {
        self.handle.uncached_handle().is_allocated(start_offset).await.map_err(map_to_status)
    }

    // TODO(fxbug.dev/89873): might be better to have a cached/uncached mode for file and call
    // this when in uncached mode
    pub async fn write_at_uncached(&self, offset: u64, content: &[u8]) -> Result<u64, Status> {
        let mut buf = self.handle.uncached_handle().allocate_buffer(content.len());
        buf.as_mut_slice().copy_from_slice(content);
        let _ = self
            .handle
            .uncached_handle()
            .overwrite(offset, buf.as_mut(), true)
            .await
            .map_err(map_to_status)?;
        Ok(content.len() as u64)
    }

    // TODO(fxbug.dev/89873): might be better to have a cached/uncached mode for file and call
    // this when in uncached mode
    pub async fn read_at_uncached(&self, offset: u64, buffer: &mut [u8]) -> Result<u64, Status> {
        let mut buf = self.handle.uncached_handle().allocate_buffer(buffer.len());
        buf.as_mut_slice().fill(0);
        let bytes_read = self
            .handle
            .uncached_handle()
            .read(offset, buf.as_mut())
            .await
            .map_err(map_to_status)?;
        buffer.copy_from_slice(buf.as_slice());
        Ok(bytes_read as u64)
    }

    pub async fn get_size_uncached(&self) -> u64 {
        self.handle.uncached_handle().get_size()
    }

    fn open_count_sub_one_and_maybe_flush(self: Arc<Self>, flush_on_last: bool) {
        let old = if flush_on_last {
            let mut old = self.open_count.load(Ordering::Relaxed);
            loop {
                assert!(old & !PURGED > 0);
                if old == 1 && self.handle.needs_flush() {
                    // Spawn a task to do the flush.
                    self.handle.owner().clone().spawn(async move {
                        // Avoid infinite loops for errors.
                        let can_flush_again = self.handle.flush().await.is_ok();
                        self.open_count_sub_one_and_maybe_flush(can_flush_again);
                    });
                    return;
                }
                match self.open_count.compare_exchange_weak(
                    old,
                    old - 1,
                    Ordering::Relaxed,
                    Ordering::Relaxed,
                ) {
                    Ok(_) => break,
                    Err(x) => old = x,
                }
            }
            old
        } else {
            let old = self.open_count.fetch_sub(1, Ordering::Relaxed);
            assert!(old & !PURGED > 0);
            old
        };
        if old == PURGED + 1 {
            let store = self.handle.store();
            store
                .filesystem()
                .graveyard()
                .queue_tombstone(store.store_object_id(), self.object_id());
        }
    }
}

impl Drop for FxFile {
    fn drop(&mut self) {
        let volume = self.handle.owner();
        volume.cache().remove(self);
    }
}

#[async_trait]
impl FxNode for FxFile {
    fn object_id(&self) -> u64 {
        self.handle.object_id()
    }

    fn parent(&self) -> Option<Arc<FxDirectory>> {
        unreachable!(); // Add a parent back-reference if needed.
    }

    fn set_parent(&self, _parent: Arc<FxDirectory>) {
        // NOP
    }

    fn open_count_add_one(&self) {
        let old = self.open_count.fetch_add(1, Ordering::Relaxed);
        assert!(old != PURGED && old != PURGED - 1);
    }

    fn open_count_sub_one(self: Arc<Self>) {
        self.open_count_sub_one_and_maybe_flush(true);
    }

    async fn get_properties(&self) -> Result<ObjectProperties, Error> {
        self.handle.get_properties().await
    }

    fn object_descriptor(&self) -> ObjectDescriptor {
        ObjectDescriptor::File
    }

    fn terminate(&self) {
        self.handle.pager_packet_receiver_registration().stop_watching_for_zero_children();
    }
}

impl DirectoryEntry for FxFile {
    fn open(
        self: Arc<Self>,
        scope: ExecutionScope,
        flags: fio::OpenFlags,
        path: Path,
        server_end: ServerEnd<fio::NodeMarker>,
    ) {
        flags.to_object_request(server_end).spawn(&scope.clone(), move |object_request| {
            Box::pin(async move {
                if !path.is_empty() {
                    return Err(Status::NOT_FILE);
                }
                Self::create_connection_async(OpenedNode::new(self), scope, flags, object_request)
            })
        });
    }

    fn entry_info(&self) -> EntryInfo {
        EntryInfo::new(self.object_id(), fio::DirentType::File)
    }
}

#[async_trait]
impl vfs::node::Node for FxFile {
    async fn get_attrs(&self) -> Result<fio::NodeAttributes, Status> {
        let props = self.get_properties().await.map_err(map_to_status)?;
        Ok(fio::NodeAttributes {
            mode: fio::MODE_TYPE_FILE
                | rights_to_posix_mode_bits(/*r*/ true, /*w*/ true, /*x*/ false),
            id: self.handle.object_id(),
            content_size: props.data_attribute_size,
            storage_size: props.allocated_size,
            link_count: props.refs,
            creation_time: props.creation_time.as_nanos(),
            modification_time: props.modification_time.as_nanos(),
        })
    }

    async fn get_attributes(
        &self,
        requested_attributes: fio::NodeAttributesQuery,
    ) -> Result<fio::NodeAttributes2, zx::Status> {
        let props = self.get_properties().await.map_err(map_to_status)?;
        Ok(attributes!(
            requested_attributes,
            Mutable {
                creation_time: props.creation_time.as_nanos(),
                modification_time: props.modification_time.as_nanos(),
                access_time: props.access_time.as_nanos(),
                mode: props.posix_attributes.map(|a| a.mode).unwrap_or(0),
                uid: props.posix_attributes.map(|a| a.uid).unwrap_or(0),
                gid: props.posix_attributes.map(|a| a.gid).unwrap_or(0),
                rdev: props.posix_attributes.map(|a| a.rdev).unwrap_or(0),
            },
            Immutable {
                protocols: fio::NodeProtocolKinds::FILE,
                abilities: fio::Operations::GET_ATTRIBUTES
                    | fio::Operations::UPDATE_ATTRIBUTES
                    | fio::Operations::READ_BYTES
                    | fio::Operations::WRITE_BYTES,
                content_size: props.data_attribute_size,
                storage_size: props.allocated_size,
                link_count: props.refs,
                id: self.handle.object_id(),
                change_time: props.change_time.as_nanos(),
            }
        ))
    }

    fn close(self: Arc<Self>) {
        self.open_count_sub_one();
    }

    async fn link_into(
        self: Arc<Self>,
        destination_dir: Arc<dyn MutableDirectory>,
        name: Name,
    ) -> Result<(), zx::Status> {
        let dir = destination_dir.into_any().downcast::<FxDirectory>().unwrap();
        let store = self.handle.store();
        let object_id = self.object_id();
        let transaction = store
            .filesystem()
            .clone()
            .new_transaction(
                lock_keys![
                    LockKey::object(store.store_object_id(), object_id),
                    LockKey::object(store.store_object_id(), dir.object_id()),
                ],
                Options::default(),
            )
            .await
            .map_err(map_to_status)?;
        // Check that we're not unlinked.
        if self.open_count.load(Ordering::Relaxed) & PURGED != 0 {
            return Err(zx::Status::NOT_FOUND);
        }
        dir.link_object(transaction, &name, object_id, ObjectDescriptor::File).await
    }
}

#[async_trait]
impl File for FxFile {
    fn writable(&self) -> bool {
        true
    }

    async fn open_file(&self, _options: &FileOptions) -> Result<(), Status> {
        Ok(())
    }

    async fn truncate(&self, length: u64) -> Result<(), Status> {
        self.handle.truncate(length).await.map_err(map_to_status)?;
        Ok(())
    }

    // Returns a VMO handle that supports paging.
    async fn get_backing_memory(&self, flags: fio::VmoFlags) -> Result<zx::Vmo, Status> {
        // We do not support executable VMO handles.
        if flags.contains(fio::VmoFlags::EXECUTE) {
            error!("get_backing_memory does not support execute rights!");
            return Err(Status::NOT_SUPPORTED);
        }

        let vmo = self.handle.vmo();
        let mut rights = zx::Rights::BASIC | zx::Rights::MAP | zx::Rights::GET_PROPERTY;
        if flags.contains(fio::VmoFlags::READ) {
            rights |= zx::Rights::READ;
        }
        if flags.contains(fio::VmoFlags::WRITE) {
            rights |= zx::Rights::WRITE;
        }

        let child_vmo = if flags.contains(fio::VmoFlags::PRIVATE_CLONE) {
            // Allow for the VMO's content size and name to be changed even without ZX_RIGHT_WRITE.
            rights |= zx::Rights::SET_PROPERTY;
            let mut child_options = zx::VmoChildOptions::SNAPSHOT_AT_LEAST_ON_WRITE;
            if flags.contains(fio::VmoFlags::WRITE) {
                child_options |= zx::VmoChildOptions::RESIZABLE;
                rights |= zx::Rights::RESIZE;
            }
            vmo.create_child(child_options, 0, vmo.get_content_size()?)?
        } else {
            vmo.create_child(zx::VmoChildOptions::REFERENCE, 0, 0)?
        };

        let child_vmo = child_vmo.replace_handle(rights)?;
        if self.handle.owner().pager().watch_for_zero_children(self).map_err(map_to_status)? {
            // Take an open count so that we keep this object alive if it is unlinked.
            self.open_count_add_one();
        }
        Ok(child_vmo)
    }

    async fn get_size(&self) -> Result<u64, Status> {
        Ok(self.handle.get_size())
    }

    async fn set_attrs(
        &self,
        flags: fio::NodeAttributeFlags,
        attrs: fio::NodeAttributes,
    ) -> Result<(), Status> {
        let crtime = flags
            .contains(fio::NodeAttributeFlags::CREATION_TIME)
            .then(|| Timestamp::from_nanos(attrs.creation_time));
        let mtime = flags
            .contains(fio::NodeAttributeFlags::MODIFICATION_TIME)
            .then(|| Timestamp::from_nanos(attrs.modification_time));
        if let (None, None) = (crtime.as_ref(), mtime.as_ref()) {
            return Ok(());
        }
        self.handle.write_timestamps(crtime, mtime).await.map_err(map_to_status)?;
        Ok(())
    }

    async fn update_attributes(
        &self,
        attributes: fio::MutableNodeAttributes,
    ) -> Result<(), Status> {
        let empty_attributes = fio::MutableNodeAttributes { ..Default::default() };
        if attributes == empty_attributes {
            return Ok(());
        }
        self.handle.update_attributes(&attributes).await.map_err(map_to_status)?;
        Ok(())
    }

    async fn list_extended_attributes(&self) -> Result<Vec<Vec<u8>>, Status> {
        self.handle.store_handle().list_extended_attributes().await.map_err(map_to_status)
    }

    async fn get_extended_attribute(&self, name: Vec<u8>) -> Result<Vec<u8>, Status> {
        self.handle.store_handle().get_extended_attribute(name).await.map_err(map_to_status)
    }

    async fn set_extended_attribute(
        &self,
        name: Vec<u8>,
        value: Vec<u8>,
        mode: fio::SetExtendedAttributeMode,
    ) -> Result<(), Status> {
        self.handle
            .store_handle()
            .set_extended_attribute(name, value, mode.into())
            .await
            .map_err(map_to_status)
    }

    async fn remove_extended_attribute(&self, name: Vec<u8>) -> Result<(), Status> {
        self.handle.store_handle().remove_extended_attribute(name).await.map_err(map_to_status)
    }

    async fn sync(&self, mode: SyncMode) -> Result<(), Status> {
        self.handle.flush().await.map_err(map_to_status)?;

        // TODO(fxbug.dev/96085): at the moment, this doesn't send a flush to the device, which
        // doesn't match minfs.
        if mode == SyncMode::Normal {
            self.handle
                .store()
                .filesystem()
                .sync(SyncOptions::default())
                .await
                .map_err(map_to_status)?;
        }

        Ok(())
    }

    fn query_filesystem(&self) -> Result<fio::FilesystemInfo, Status> {
        let store = self.handle.store();
        Ok(info_to_filesystem_info(
            store.filesystem().get_info(),
            store.filesystem().block_size(),
            store.object_count(),
            self.handle.owner().id(),
        ))
    }
}

#[async_trait]
impl PagerBacked for FxFile {
    fn pager(&self) -> &crate::pager::Pager {
        self.handle.owner().pager()
    }

    fn pager_packet_receiver_registration(&self) -> &PagerPacketReceiverRegistration {
        &self.handle.pager_packet_receiver_registration()
    }

    fn vmo(&self) -> &zx::Vmo {
        self.handle.vmo()
    }

    fn page_in(self: Arc<Self>, mut range: Range<u64>) {
        async_enter!("page_in");
        assert!(range.end < i64::MAX as u64);
        let aligned_size =
            round_up(self.handle.uncached_size(), zx::system_get_page_size()).unwrap();
        // Round up to 128 KiB reads with a minimum of 64 KiB readahead.
        range.end = std::cmp::min(
            range.start + round_up(range.end - range.start + 65536, 131072u64).unwrap(),
            aligned_size,
        );
        default_page_in(self, range)
    }

    fn mark_dirty(self: Arc<Self>, range: Range<u64>) {
        self.handle.owner().clone().spawn(async move {
            async_enter!("mark_dirty");
            self.handle.mark_dirty(range).await;
        });
    }

    fn on_zero_children(self: Arc<Self>) {
        // Drop the open count that we took in `get_backing_memory`.
        self.open_count_sub_one();
    }

    fn read_alignment(&self) -> u64 {
        self.handle.block_size()
    }
    fn byte_size(&self) -> u64 {
        self.handle.uncached_size()
    }
    async fn aligned_read(&self, range: Range<u64>) -> Result<(buffer::Buffer<'_>, usize), Error> {
        let buffer = self.handle.read_uncached(range).await?;
        let buffer_len = buffer.len();
        Ok((buffer, buffer_len))
    }
}

impl GetVmo for FxFile {
    fn get_vmo(&self) -> &zx::Vmo {
        self.vmo()
    }
}

#[cfg(test)]
mod tests {
    use {
        crate::fuchsia::testing::{
            close_file_checked, open_file_checked, TestFixture, TestFixtureOptions,
        },
        anyhow::format_err,
        fidl_fuchsia_io as fio, fuchsia_async as fasync,
        fuchsia_fs::file,
        fuchsia_zircon::Status,
        futures::join,
        fxfs::object_handle::INVALID_OBJECT_ID,
        std::sync::{
            atomic::{self, AtomicBool},
            Arc,
        },
        storage_device::{fake_device::FakeDevice, DeviceHolder},
        vfs::common::rights_to_posix_mode_bits,
    };

    #[fuchsia::test(threads = 10)]
    async fn test_empty_file() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE | fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let buf = file
            .read(fio::MAX_BUF)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("read failed");
        assert!(buf.is_empty());

        let (status, attrs) = file.get_attr().await.expect("FIDL call failed");
        Status::ok(status).expect("get_attr failed");
        assert_ne!(attrs.id, INVALID_OBJECT_ID);
        assert_eq!(
            attrs.mode,
            fio::MODE_TYPE_FILE | rights_to_posix_mode_bits(/*r*/ true, /*w*/ true, /*x*/ false)
        );
        assert_eq!(attrs.content_size, 0u64);
        assert_eq!(attrs.storage_size, 0u64);
        assert_eq!(attrs.link_count, 1u64);
        assert_ne!(attrs.creation_time, 0u64);
        assert_ne!(attrs.modification_time, 0u64);
        assert_eq!(attrs.creation_time, attrs.modification_time);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_set_attrs() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let (status, initial_attrs) = file.get_attr().await.expect("FIDL call failed");
        Status::ok(status).expect("get_attr failed");

        let crtime = initial_attrs.creation_time ^ 1u64;
        let mtime = initial_attrs.modification_time ^ 1u64;

        let mut attrs = initial_attrs.clone();
        attrs.creation_time = crtime;
        attrs.modification_time = mtime;
        let status = file
            .set_attr(fio::NodeAttributeFlags::CREATION_TIME, &attrs)
            .await
            .expect("FIDL call failed");
        Status::ok(status).expect("set_attr failed");

        let mut expected_attrs = initial_attrs.clone();
        expected_attrs.creation_time = crtime; // Only crtime is updated so far.
        let (status, attrs) = file.get_attr().await.expect("FIDL call failed");
        Status::ok(status).expect("get_attr failed");
        assert_eq!(expected_attrs, attrs);

        let mut attrs = initial_attrs.clone();
        attrs.creation_time = 0u64; // This should be ignored since we don't set the flag.
        attrs.modification_time = mtime;
        let status = file
            .set_attr(fio::NodeAttributeFlags::MODIFICATION_TIME, &attrs)
            .await
            .expect("FIDL call failed");
        Status::ok(status).expect("set_attr failed");

        let mut expected_attrs = initial_attrs.clone();
        expected_attrs.creation_time = crtime;
        expected_attrs.modification_time = mtime;
        let (status, attrs) = file.get_attr().await.expect("FIDL call failed");
        Status::ok(status).expect("get_attr failed");
        assert_eq!(expected_attrs, attrs);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_write_read() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let inputs = vec!["hello, ", "world!"];
        let expected_output = "hello, world!";
        for input in inputs {
            let bytes_written = file
                .write(input.as_bytes())
                .await
                .expect("write failed")
                .map_err(Status::from_raw)
                .expect("File write was successful");
            assert_eq!(bytes_written as usize, input.as_bytes().len());
        }

        let buf = file
            .read_at(fio::MAX_BUF, 0)
            .await
            .expect("read_at failed")
            .map_err(Status::from_raw)
            .expect("File read was successful");
        assert_eq!(buf.len(), expected_output.as_bytes().len());
        assert!(buf.iter().eq(expected_output.as_bytes().iter()));

        let (status, attrs) = file.get_attr().await.expect("FIDL call failed");
        Status::ok(status).expect("get_attr failed");
        assert_eq!(attrs.content_size, expected_output.as_bytes().len() as u64);
        // We haven't synced yet, but the pending writes should have blocks reserved still.
        assert_eq!(attrs.storage_size, fixture.fs().block_size() as u64);

        let () = file
            .sync()
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("sync failed");

        let (status, attrs) = file.get_attr().await.expect("FIDL call failed");
        Status::ok(status).expect("get_attr failed");
        assert_eq!(attrs.content_size, expected_output.as_bytes().len() as u64);
        assert_eq!(attrs.storage_size, fixture.fs().block_size() as u64);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_page_in() {
        let input = "hello, world!";
        let reused_device = {
            let fixture = TestFixture::new().await;
            let root = fixture.root();

            let file = open_file_checked(
                &root,
                fio::OpenFlags::CREATE
                    | fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::RIGHT_WRITABLE
                    | fio::OpenFlags::NOT_DIRECTORY,
                "foo",
            )
            .await;

            let bytes_written = file
                .write(input.as_bytes())
                .await
                .expect("write failed")
                .map_err(Status::from_raw)
                .expect("File write was successful");
            assert_eq!(bytes_written as usize, input.as_bytes().len());
            assert!(file.sync().await.expect("Sync failed").is_ok());

            close_file_checked(file).await;
            fixture.close().await
        };

        let fixture = TestFixture::open(
            reused_device,
            TestFixtureOptions {
                format: false,
                as_blob: false,
                encrypted: true,
                serve_volume: false,
            },
        )
        .await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let vmo =
            file.get_backing_memory(fio::VmoFlags::READ).await.expect("Fidl failure").unwrap();
        let mut readback = vec![0; input.as_bytes().len()];
        assert!(vmo.read(&mut readback, 0).is_ok());
        assert_eq!(input.as_bytes(), readback);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_page_in_io_error() {
        let mut device = FakeDevice::new(8192, 512);
        let succeed_requests = Arc::new(AtomicBool::new(true));
        let succeed_requests_clone = succeed_requests.clone();
        device.set_op_callback(Box::new(move |_| {
            if succeed_requests_clone.load(atomic::Ordering::Relaxed) {
                Ok(())
            } else {
                Err(format_err!("Fake error."))
            }
        }));

        let input = "hello, world!";
        let reused_device = {
            let fixture = TestFixture::open(
                DeviceHolder::new(device),
                TestFixtureOptions {
                    format: true,
                    as_blob: false,
                    encrypted: true,
                    serve_volume: false,
                },
            )
            .await;
            let root = fixture.root();

            let file = open_file_checked(
                &root,
                fio::OpenFlags::CREATE
                    | fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::RIGHT_WRITABLE
                    | fio::OpenFlags::NOT_DIRECTORY,
                "foo",
            )
            .await;

            let bytes_written = file
                .write(input.as_bytes())
                .await
                .expect("write failed")
                .map_err(Status::from_raw)
                .expect("File write was successful");
            assert_eq!(bytes_written as usize, input.as_bytes().len());

            close_file_checked(file).await;
            fixture.close().await
        };

        let fixture = TestFixture::open(
            reused_device,
            TestFixtureOptions {
                format: false,
                as_blob: false,
                encrypted: true,
                serve_volume: false,
            },
        )
        .await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let vmo =
            file.get_backing_memory(fio::VmoFlags::READ).await.expect("Fidl failure").unwrap();
        succeed_requests.store(false, atomic::Ordering::Relaxed);
        let mut readback = vec![0; input.as_bytes().len()];
        assert!(vmo.read(&mut readback, 0).is_err());

        succeed_requests.store(true, atomic::Ordering::Relaxed);
        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_writes_persist() {
        let mut device = DeviceHolder::new(FakeDevice::new(8192, 512));
        for i in 0..2 {
            let fixture = TestFixture::open(
                device,
                TestFixtureOptions {
                    format: i == 0,
                    as_blob: false,
                    encrypted: true,
                    serve_volume: false,
                },
            )
            .await;
            let root = fixture.root();

            let flags = if i == 0 {
                fio::OpenFlags::CREATE
                    | fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::RIGHT_WRITABLE
            } else {
                fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE
            };
            let file = open_file_checked(&root, flags | fio::OpenFlags::NOT_DIRECTORY, "foo").await;

            if i == 0 {
                let _: u64 = file
                    .write(&vec![0xaa as u8; 8192])
                    .await
                    .expect("FIDL call failed")
                    .map_err(Status::from_raw)
                    .expect("File write was successful");
            } else {
                let buf = file
                    .read(8192)
                    .await
                    .expect("FIDL call failed")
                    .map_err(Status::from_raw)
                    .expect("File read was successful");
                assert_eq!(buf, vec![0xaa as u8; 8192]);
            }

            let (status, attrs) = file.get_attr().await.expect("FIDL call failed");
            Status::ok(status).expect("get_attr failed");
            assert_eq!(attrs.content_size, 8192u64);
            assert_eq!(attrs.storage_size, 8192u64);

            close_file_checked(file).await;
            device = fixture.close().await;
        }
    }

    #[fuchsia::test(threads = 10)]
    async fn test_append() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let inputs = vec!["hello, ", "world!"];
        let expected_output = "hello, world!";
        for input in inputs {
            let file = open_file_checked(
                &root,
                fio::OpenFlags::CREATE
                    | fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::RIGHT_WRITABLE
                    | fio::OpenFlags::APPEND
                    | fio::OpenFlags::NOT_DIRECTORY,
                "foo",
            )
            .await;

            let bytes_written = file
                .write(input.as_bytes())
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("File write was successful");
            assert_eq!(bytes_written as usize, input.as_bytes().len());
            close_file_checked(file).await;
        }

        let file = open_file_checked(
            &root,
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;
        let buf = file
            .read_at(fio::MAX_BUF, 0)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("File read was successful");
        assert_eq!(buf.len(), expected_output.as_bytes().len());
        assert_eq!(&buf[..], expected_output.as_bytes());

        let (status, attrs) = file.get_attr().await.expect("FIDL call failed");
        Status::ok(status).expect("get_attr failed");
        assert_eq!(attrs.content_size, expected_output.as_bytes().len() as u64);
        assert_eq!(attrs.storage_size, fixture.fs().block_size() as u64);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_seek() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let input = "hello, world!";
        let _: u64 = file
            .write(input.as_bytes())
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("File write was successful");

        {
            let offset = file
                .seek(fio::SeekOrigin::Start, 0)
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("seek was successful");
            assert_eq!(offset, 0);
            let buf = file
                .read(5)
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("File read was successful");
            assert!(buf.iter().eq("hello".as_bytes().into_iter()));
        }
        {
            let offset = file
                .seek(fio::SeekOrigin::Current, 2)
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("seek was successful");
            assert_eq!(offset, 7);
            let buf = file
                .read(5)
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("File read was successful");
            assert!(buf.iter().eq("world".as_bytes().into_iter()));
        }
        {
            let offset = file
                .seek(fio::SeekOrigin::Current, -5)
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("seek was successful");
            assert_eq!(offset, 7);
            let buf = file
                .read(5)
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("File read was successful");
            assert!(buf.iter().eq("world".as_bytes().into_iter()));
        }
        {
            let offset = file
                .seek(fio::SeekOrigin::End, -1)
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("seek was successful");
            assert_eq!(offset, 12);
            let buf = file
                .read(1)
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("File read was successful");
            assert!(buf.iter().eq("!".as_bytes().into_iter()));
        }

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_resize_extend() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let input = "hello, world!";
        let len: usize = 16 * 1024;

        let _: u64 = file
            .write(input.as_bytes())
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("File write was successful");

        let offset = file
            .seek(fio::SeekOrigin::Start, 0)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("Seek was successful");
        assert_eq!(offset, 0);

        let () = file
            .resize(len as u64)
            .await
            .expect("resize failed")
            .map_err(Status::from_raw)
            .expect("resize error");

        let mut expected_buf = vec![0 as u8; len];
        expected_buf[..input.as_bytes().len()].copy_from_slice(input.as_bytes());

        let buf = file::read(&file).await.expect("File read was successful");
        assert_eq!(buf.len(), len);
        assert_eq!(buf, expected_buf);

        // Write something at the end of the gap.
        expected_buf[len - 1..].copy_from_slice("a".as_bytes());

        let _: u64 = file
            .write_at("a".as_bytes(), (len - 1) as u64)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("File write was successful");

        let offset = file
            .seek(fio::SeekOrigin::Start, 0)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("Seek was successful");
        assert_eq!(offset, 0);

        let buf = file::read(&file).await.expect("File read was successful");
        assert_eq!(buf.len(), len);
        assert_eq!(buf, expected_buf);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_resize_shrink() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let len: usize = 2 * 1024;
        let input = {
            let mut v = vec![0 as u8; len];
            for i in 0..v.len() {
                v[i] = ('a' as u8) + (i % 13) as u8;
            }
            v
        };
        let short_len: usize = 513;

        file::write(&file, &input).await.expect("File write was successful");

        let () = file
            .resize(short_len as u64)
            .await
            .expect("resize failed")
            .map_err(Status::from_raw)
            .expect("resize error");

        let offset = file
            .seek(fio::SeekOrigin::Start, 0)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("Seek was successful");
        assert_eq!(offset, 0);

        let buf = file::read(&file).await.expect("File read was successful");
        assert_eq!(buf.len(), short_len);
        assert_eq!(buf, input[..short_len]);

        // Resize to the original length and verify the data's zeroed.
        let () = file
            .resize(len as u64)
            .await
            .expect("resize failed")
            .map_err(Status::from_raw)
            .expect("resize error");

        let expected_buf = {
            let mut v = vec![0 as u8; len];
            v[..short_len].copy_from_slice(&input[..short_len]);
            v
        };

        let offset = file
            .seek(fio::SeekOrigin::Start, 0)
            .await
            .expect("seek failed")
            .map_err(Status::from_raw)
            .expect("Seek was successful");
        assert_eq!(offset, 0);

        let buf = file::read(&file).await.expect("File read was successful");
        assert_eq!(buf.len(), len);
        assert_eq!(buf, expected_buf);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_resize_shrink_repeated() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let orig_len: usize = 4 * 1024;
        let mut len = orig_len;
        let input = {
            let mut v = vec![0 as u8; len];
            for i in 0..v.len() {
                v[i] = ('a' as u8) + (i % 13) as u8;
            }
            v
        };
        let short_len: usize = 513;

        file::write(&file, &input).await.expect("File write was successful");

        while len > short_len {
            len -= std::cmp::min(len - short_len, 512);
            let () = file
                .resize(len as u64)
                .await
                .expect("resize failed")
                .map_err(Status::from_raw)
                .expect("resize error");
        }

        let offset = file
            .seek(fio::SeekOrigin::Start, 0)
            .await
            .expect("Seek failed")
            .map_err(Status::from_raw)
            .expect("Seek was successful");
        assert_eq!(offset, 0);

        let buf = file::read(&file).await.expect("File read was successful");
        assert_eq!(buf.len(), short_len);
        assert_eq!(buf, input[..short_len]);

        // Resize to the original length and verify the data's zeroed.
        let () = file
            .resize(orig_len as u64)
            .await
            .expect("resize failed")
            .map_err(Status::from_raw)
            .expect("resize error");

        let expected_buf = {
            let mut v = vec![0 as u8; orig_len];
            v[..short_len].copy_from_slice(&input[..short_len]);
            v
        };

        let offset = file
            .seek(fio::SeekOrigin::Start, 0)
            .await
            .expect("seek failed")
            .map_err(Status::from_raw)
            .expect("Seek was successful");
        assert_eq!(offset, 0);

        let buf = file::read(&file).await.expect("File read was successful");
        assert_eq!(buf.len(), orig_len);
        assert_eq!(buf, expected_buf);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_unlink_with_open_race() {
        let fixture = Arc::new(TestFixture::new().await);
        let fixture1 = fixture.clone();
        let fixture2 = fixture.clone();
        let fixture3 = fixture.clone();
        let done = Arc::new(AtomicBool::new(false));
        let done1 = done.clone();
        let done2 = done.clone();
        join!(
            fasync::Task::spawn(async move {
                let root = fixture1.root();
                while !done1.load(atomic::Ordering::Relaxed) {
                    let file = open_file_checked(
                        &root,
                        fio::OpenFlags::CREATE
                            | fio::OpenFlags::RIGHT_READABLE
                            | fio::OpenFlags::RIGHT_WRITABLE
                            | fio::OpenFlags::NOT_DIRECTORY,
                        "foo",
                    )
                    .await;
                    let _: u64 = file
                        .write(b"hello")
                        .await
                        .expect("write failed")
                        .map_err(Status::from_raw)
                        .expect("write error");
                }
            }),
            fasync::Task::spawn(async move {
                let root = fixture2.root();
                while !done2.load(atomic::Ordering::Relaxed) {
                    let file = open_file_checked(
                        &root,
                        fio::OpenFlags::CREATE
                            | fio::OpenFlags::RIGHT_READABLE
                            | fio::OpenFlags::RIGHT_WRITABLE
                            | fio::OpenFlags::NOT_DIRECTORY,
                        "foo",
                    )
                    .await;
                    let _: u64 = file
                        .write(b"hello")
                        .await
                        .expect("write failed")
                        .map_err(Status::from_raw)
                        .expect("write error");
                }
            }),
            fasync::Task::spawn(async move {
                let root = fixture3.root();
                for _ in 0..300 {
                    let file = open_file_checked(
                        &root,
                        fio::OpenFlags::CREATE
                            | fio::OpenFlags::RIGHT_READABLE
                            | fio::OpenFlags::RIGHT_WRITABLE
                            | fio::OpenFlags::NOT_DIRECTORY,
                        "foo",
                    )
                    .await;
                    assert_eq!(
                        file.close().await.expect("FIDL call failed").map_err(Status::from_raw),
                        Ok(())
                    );
                    root.unlink("foo", &fio::UnlinkOptions::default())
                        .await
                        .expect("FIDL call failed")
                        .expect("unlink failed");
                }
                done.store(true, atomic::Ordering::Relaxed);
            })
        );

        Arc::try_unwrap(fixture).unwrap_or_else(|_| panic!()).close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_get_backing_memory_shared_vmo_right_write() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        file.resize(4096)
            .await
            .expect("resize failed")
            .map_err(Status::from_raw)
            .expect("resize error");

        let vmo = file
            .get_backing_memory(fio::VmoFlags::SHARED_BUFFER | fio::VmoFlags::READ)
            .await
            .expect("Failed to make FIDL call")
            .map_err(Status::from_raw)
            .expect("Failed to get VMO");
        let err = vmo.write(&[0, 1, 2, 3], 0).expect_err("VMO should not be writable");
        assert_eq!(Status::ACCESS_DENIED, err);

        let vmo = file
            .get_backing_memory(
                fio::VmoFlags::SHARED_BUFFER | fio::VmoFlags::READ | fio::VmoFlags::WRITE,
            )
            .await
            .expect("Failed to make FIDL call")
            .map_err(Status::from_raw)
            .expect("Failed to get VMO");
        vmo.write(&[0, 1, 2, 3], 0).expect("VMO should be writable");

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_get_backing_memory_shared_vmo_right_read() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        file.resize(4096)
            .await
            .expect("resize failed")
            .map_err(Status::from_raw)
            .expect("resize error");

        let mut data = [0u8; 4];
        let vmo = file
            .get_backing_memory(fio::VmoFlags::SHARED_BUFFER)
            .await
            .expect("Failed to make FIDL call")
            .map_err(Status::from_raw)
            .expect("Failed to get VMO");
        let err = vmo.read(&mut data, 0).expect_err("VMO should not be readable");
        assert_eq!(Status::ACCESS_DENIED, err);

        let vmo = file
            .get_backing_memory(fio::VmoFlags::SHARED_BUFFER | fio::VmoFlags::READ)
            .await
            .expect("Failed to make FIDL call")
            .map_err(Status::from_raw)
            .expect("Failed to get VMO");
        vmo.read(&mut data, 0).expect("VMO should be readable");

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_get_backing_memory_shared_vmo_resize() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let vmo = file
            .get_backing_memory(
                fio::VmoFlags::SHARED_BUFFER | fio::VmoFlags::READ | fio::VmoFlags::WRITE,
            )
            .await
            .expect("Failed to make FIDL call")
            .map_err(Status::from_raw)
            .expect("Failed to get VMO");

        let err = vmo.set_size(10).expect_err("VMO should not be resizable");
        assert_eq!(Status::ACCESS_DENIED, err);

        let err =
            vmo.set_content_size(&10).expect_err("content size should not be directly modifiable");
        assert_eq!(Status::ACCESS_DENIED, err);

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn test_get_backing_memory_private_vmo_resize() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let vmo = file
            .get_backing_memory(
                fio::VmoFlags::PRIVATE_CLONE | fio::VmoFlags::READ | fio::VmoFlags::WRITE,
            )
            .await
            .expect("Failed to make FIDL call")
            .map_err(Status::from_raw)
            .expect("Failed to get VMO");
        vmo.set_size(10).expect("VMO should be resizable");
        vmo.set_content_size(&20).expect("content size should be modifiable");

        let vmo = file
            .get_backing_memory(fio::VmoFlags::PRIVATE_CLONE | fio::VmoFlags::READ)
            .await
            .expect("Failed to make FIDL call")
            .map_err(Status::from_raw)
            .expect("Failed to get VMO");
        let err = vmo.set_size(10).expect_err("VMO should not be resizable");
        assert_eq!(err, Status::ACCESS_DENIED);
        vmo.set_content_size(&20).expect("content is still modifiable");

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test(threads = 10)]
    async fn extended_attributes() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        let name = b"security.selinux";
        let value_vec = b"bar".to_vec();

        {
            let (iterator_client, iterator_server) =
                fidl::endpoints::create_proxy::<fio::ExtendedAttributeIteratorMarker>().unwrap();
            file.list_extended_attributes(iterator_server).expect("Failed to make FIDL call");
            let (chunk, last) = iterator_client
                .get_next()
                .await
                .expect("Failed to make FIDL call")
                .expect("Failed to get next iterator chunk");
            assert!(last);
            assert_eq!(chunk, Vec::<Vec<u8>>::new());
        }
        assert_eq!(
            file.get_extended_attribute(name)
                .await
                .expect("Failed to make FIDL call")
                .expect_err("Got successful message back for missing attribute"),
            Status::NOT_FOUND.into_raw(),
        );

        file.set_extended_attribute(
            name,
            fio::ExtendedAttributeValue::Bytes(value_vec.clone()),
            fio::SetExtendedAttributeMode::Set,
        )
        .await
        .expect("Failed to make FIDL call")
        .expect("Failed to set extended attribute");

        {
            let (iterator_client, iterator_server) =
                fidl::endpoints::create_proxy::<fio::ExtendedAttributeIteratorMarker>().unwrap();
            file.list_extended_attributes(iterator_server).expect("Failed to make FIDL call");
            let (chunk, last) = iterator_client
                .get_next()
                .await
                .expect("Failed to make FIDL call")
                .expect("Failed to get next iterator chunk");
            assert!(last);
            assert_eq!(chunk, vec![name]);
        }
        assert_eq!(
            file.get_extended_attribute(name)
                .await
                .expect("Failed to make FIDL call")
                .expect("Failed to get extended attribute"),
            fio::ExtendedAttributeValue::Bytes(value_vec)
        );

        file.remove_extended_attribute(name)
            .await
            .expect("Failed to make FIDL call")
            .expect("Failed to remove extended attribute");

        {
            let (iterator_client, iterator_server) =
                fidl::endpoints::create_proxy::<fio::ExtendedAttributeIteratorMarker>().unwrap();
            file.list_extended_attributes(iterator_server).expect("Failed to make FIDL call");
            let (chunk, last) = iterator_client
                .get_next()
                .await
                .expect("Failed to make FIDL call")
                .expect("Failed to get next iterator chunk");
            assert!(last);
            assert_eq!(chunk, Vec::<Vec<u8>>::new());
        }
        assert_eq!(
            file.get_extended_attribute(name)
                .await
                .expect("Failed to make FIDL call")
                .expect_err("Got successful message back for missing attribute"),
            Status::NOT_FOUND.into_raw(),
        );

        close_file_checked(file).await;
        fixture.close().await;
    }

    #[fuchsia::test]
    async fn test_flush_when_closed_from_on_zero_children() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        file.resize(50).await.expect("resize (FIDL) failed").expect("resize failed");

        {
            let vmo = file
                .get_backing_memory(fio::VmoFlags::READ | fio::VmoFlags::WRITE)
                .await
                .expect("get_backing_memory (FIDL) failed")
                .map_err(Status::from_raw)
                .expect("get_backing_memory failed");

            std::mem::drop(file);

            fasync::unblock(move || vmo.write(b"hello", 0).expect("write failed")).await;
        }

        fixture.close().await;
    }

    #[fuchsia::test]
    async fn test_update_attributes_also_updates_ctime() {
        let fixture = TestFixture::new().await;
        let root = fixture.root();

        let file = open_file_checked(
            &root,
            fio::OpenFlags::CREATE
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::NOT_DIRECTORY,
            "foo",
        )
        .await;

        // Writing to file should update ctime
        file.write("hello, world!".as_bytes())
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("write failed");
        let (_mutable_attributes, immutable_attributes) = file
            .get_attributes(fio::NodeAttributesQuery::CHANGE_TIME)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("get_attributes failed");
        let ctime_after_write = immutable_attributes.change_time;

        // Updating file attributes updates ctime as well
        file.update_attributes(&fio::MutableNodeAttributes {
            mode: Some(111),
            gid: Some(222),
            ..Default::default()
        })
        .await
        .expect("FIDL call failed")
        .map_err(Status::from_raw)
        .expect("update_attributes failed");
        let (_mutable_attributes, immutable_attributes) = file
            .get_attributes(fio::NodeAttributesQuery::CHANGE_TIME)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("get_attributes failed");
        let ctime_after_update = immutable_attributes.change_time;
        assert!(ctime_after_update > ctime_after_write);

        // Flush metadata
        file.sync()
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("sync failed");
        let (_mutable_attributes, immutable_attributes) = file
            .get_attributes(fio::NodeAttributesQuery::CHANGE_TIME)
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("get_attributes failed");
        let ctime_after_sync = immutable_attributes.change_time;
        assert_eq!(ctime_after_sync, ctime_after_update);
        fixture.close().await;
    }
}
