// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(missing_docs)]

//! Wrapper types for [`fidl_fuchsia_pkg::PackageCacheProxy`] and its related protocols.

use {
    crate::types::{BlobId, BlobInfo},
    fidl_fuchsia_io as fio, fidl_fuchsia_pkg as fpkg,
    fuchsia_pkg::PackageDirectory,
    fuchsia_zircon_status::Status,
    futures::prelude::*,
    std::sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
};

/// An open connection to a provider of the `fuchsia.pkg.PackageCache`.
#[derive(Debug, Clone)]
pub struct Client {
    proxy: fpkg::PackageCacheProxy,
}

impl Client {
    /// Constructs a client from the given proxy.
    pub fn from_proxy(proxy: fpkg::PackageCacheProxy) -> Self {
        Self { proxy }
    }

    /// Returns a reference to the underlying PackageCacheProxy connection.
    pub fn proxy(&self) -> &fpkg::PackageCacheProxy {
        &self.proxy
    }

    /// Opens the package specified by `meta_far_blob` with the intent to fetch any missing blobs
    /// using the returned [`Get`] type if needed.
    pub fn get(&self, meta_far_blob: BlobInfo) -> Result<Get, fidl::Error> {
        let (needed_blobs, needed_blobs_server_end) =
            fidl::endpoints::create_proxy::<fpkg::NeededBlobsMarker>()?;
        let (pkg_dir, pkg_dir_server_end) = PackageDirectory::create_request()?;

        let get_fut = self.proxy.get(
            &meta_far_blob.into(),
            needed_blobs_server_end,
            Some(pkg_dir_server_end),
        );

        Ok(Get {
            get_fut,
            pkg_dir,
            needed_blobs,
            pkg_present: SharedBoolEvent::new(),
            meta_far: meta_far_blob.blob_id,
        })
    }

    /// Uses PackageCache.Get to obtain the package directory of a package that is already cached
    /// (all blobs are already in blobfs).
    /// Errors if the package is not already cached.
    pub async fn get_already_cached(
        &self,
        meta_far_blob: BlobId,
    ) -> Result<PackageDirectory, GetAlreadyCachedError> {
        let mut get = self
            .get(BlobInfo { blob_id: meta_far_blob, length: 0 })
            .map_err(GetAlreadyCachedError::Get)?;
        if let Some(_) = get
            .open_meta_blob(fpkg::BlobType::Uncompressed)
            .await
            .map_err(GetAlreadyCachedError::OpenMetaBlob)?
        {
            return Err(GetAlreadyCachedError::MissingMetaFar);
        }

        if let Some(missing_blobs) = get
            .get_missing_blobs()
            .try_next()
            .await
            .map_err(GetAlreadyCachedError::GetMissingBlobs)?
        {
            return Err(GetAlreadyCachedError::MissingContentBlobs(missing_blobs));
        }

        get.finish().await.map_err(GetAlreadyCachedError::FinishGet)
    }
}

#[derive(thiserror::Error, Debug)]
#[allow(missing_docs)]
pub enum GetAlreadyCachedError {
    #[error("calling get")]
    Get(#[source] fidl::Error),

    #[error("opening meta blob")]
    OpenMetaBlob(#[source] OpenBlobError),

    #[error("meta.far blob not cached")]
    MissingMetaFar,

    #[error("getting missing blobs")]
    GetMissingBlobs(#[source] ListMissingBlobsError),

    #[error("content blobs not cached {0:?}")]
    MissingContentBlobs(Vec<BlobInfo>),

    #[error("finishing get")]
    FinishGet(#[source] GetError),
}

impl GetAlreadyCachedError {
    /// Returns true if the get failed because the package was not cached.
    pub fn was_not_cached(&self) -> bool {
        use GetAlreadyCachedError::*;
        match self {
            Get(..) | OpenMetaBlob(..) | GetMissingBlobs(..) | FinishGet(..) => false,
            MissingMetaFar | MissingContentBlobs(..) => true,
        }
    }
}

#[derive(Debug, Clone)]
struct SharedBoolEvent(Arc<AtomicBool>);

impl SharedBoolEvent {
    fn new() -> Self {
        Self(Arc::new(AtomicBool::new(false)))
    }

    fn get(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }

    fn set(&self) {
        self.0.store(true, Ordering::SeqCst)
    }
}

async fn open_blob(
    needed_blobs: &fpkg::NeededBlobsProxy,
    kind: OpenKind,
    blob_id: BlobId,
    blob_type: fpkg::BlobType,
    pkg_present: Option<&SharedBoolEvent>,
) -> Result<Option<NeededBlob>, OpenBlobError> {
    let open_fut = match kind {
        OpenKind::Meta => needed_blobs.open_meta_blob(blob_type),
        OpenKind::Content => needed_blobs.open_blob(&blob_id.into(), blob_type),
    };
    match open_fut.await {
        Err(fidl::Error::ClientChannelClosed { status: Status::OK, .. }) => {
            if let Some(pkg_present) = pkg_present {
                pkg_present.set();
            }
            Ok(None)
        }
        res => {
            if let Some(blob) = res?? {
                let proxy = blob.into_proxy()?;
                Ok(Some(NeededBlob {
                    blob: Blob {
                        proxy: Clone::clone(&proxy),
                        needed_blobs: needed_blobs.clone(),
                        blob_id,
                        state: NeedsTruncate,
                    },
                    closer: BlobCloser { proxy, closed: false },
                }))
            } else {
                Ok(None)
            }
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum OpenKind {
    Meta,
    Content,
}

/// A deferred call to [`Get::open_meta_blob`] or [`Get::open_blob`].
#[derive(Debug)]
pub struct DeferredOpenBlob {
    needed_blobs: fpkg::NeededBlobsProxy,
    kind: OpenKind,
    blob_id: BlobId,
    pkg_present: Option<SharedBoolEvent>,
}

impl DeferredOpenBlob {
    /// Opens the blob for write, if it is still needed. The blob's data can be provided using the
    /// returned NeededBlob.
    pub async fn open(
        &self,
        blob_type: fpkg::BlobType,
    ) -> Result<Option<NeededBlob>, OpenBlobError> {
        open_blob(&self.needed_blobs, self.kind, self.blob_id, blob_type, self.pkg_present.as_ref())
            .await
    }
    fn proxy_cmp_key(&self) -> u32 {
        use fidl::{endpoints::Proxy, AsHandleRef};
        self.needed_blobs.as_channel().raw_handle()
    }
}

impl std::cmp::PartialEq for DeferredOpenBlob {
    fn eq(&self, other: &Self) -> bool {
        self.proxy_cmp_key() == other.proxy_cmp_key() && self.kind == other.kind
    }
}

impl std::cmp::Eq for DeferredOpenBlob {}

/// A pending `fuchsia.pkg/PackageCache.Get()` request. Clients must, in order:
/// 1. open/write the meta blob, if Some(NeededBlob) is provided by that API
/// 2. enumerate all missing content blobs
/// 3. open/write all missing content blobs, if Some(NeededBlob) is provided by that API
/// 4. finish() to complete the Get() request.
#[derive(Debug)]
pub struct Get {
    get_fut: fidl::client::QueryResponseFut<Result<(), i32>>,
    needed_blobs: fpkg::NeededBlobsProxy,
    pkg_dir: PackageDirectory,
    pkg_present: SharedBoolEvent,
    meta_far: BlobId,
}

impl Get {
    /// Returns an independent object that can be used to open the meta blob for write.  See
    /// [`Self::open_meta_blob`].
    pub fn make_open_meta_blob(&mut self) -> DeferredOpenBlob {
        DeferredOpenBlob {
            needed_blobs: self.needed_blobs.clone(),
            kind: OpenKind::Meta,
            blob_id: self.meta_far,
            pkg_present: Some(self.pkg_present.clone()),
        }
    }

    /// Opens the meta blob for write, if it is still needed. The blob's data can be provided using
    /// the returned NeededBlob.
    pub async fn open_meta_blob(
        &mut self,
        blob_type: fpkg::BlobType,
    ) -> Result<Option<NeededBlob>, OpenBlobError> {
        open_blob(
            &self.needed_blobs,
            OpenKind::Meta,
            self.meta_far,
            blob_type,
            Some(&self.pkg_present),
        )
        .await
    }

    fn start_get_missing_blobs(
        &mut self,
    ) -> Result<Option<fpkg::BlobInfoIteratorProxy>, fidl::Error> {
        if self.pkg_present.get() {
            return Ok(None);
        }

        let (blob_iterator, blob_iterator_server_end) =
            fidl::endpoints::create_proxy::<fpkg::BlobInfoIteratorMarker>()?;

        self.needed_blobs.get_missing_blobs(blob_iterator_server_end)?;
        Ok(Some(blob_iterator))
    }

    /// Determines the set of blobs that the caller must open/write to complete this `Get()`
    /// operation.
    /// The returned stream will never yield an empty `Vec`.
    /// Callers should process the missing blobs (via `make_open_blob` or `open_blob`) concurrently
    /// with reading the stream to guarantee stream termination.
    pub fn get_missing_blobs(
        &mut self,
    ) -> impl Stream<Item = Result<Vec<BlobInfo>, ListMissingBlobsError>> + Unpin {
        match self.start_get_missing_blobs() {
            Ok(option_iter) => match option_iter {
                Some(iterator) => crate::fidl_iterator_to_stream(iterator)
                    .map_ok(|v| v.into_iter().map(BlobInfo::from).collect())
                    .map_err(ListMissingBlobsError::CallNextOnBlobIterator)
                    .left_stream(),
                None => futures::stream::empty().right_stream(),
            }
            .left_stream(),
            Err(e) => {
                futures::stream::iter(Some(Err(ListMissingBlobsError::CallGetMissingBlobs(e))))
                    .right_stream()
            }
        }
    }

    /// Returns an independent object that can be used to open the `content_blob` for write.  See
    /// [`Self::open_blob`].
    pub fn make_open_blob(&mut self, content_blob: BlobId) -> DeferredOpenBlob {
        DeferredOpenBlob {
            needed_blobs: self.needed_blobs.clone(),
            kind: OpenKind::Content,
            blob_id: content_blob,
            pkg_present: None,
        }
    }

    /// Opens `content_blob` for write, if it is still needed. The blob's data can be provided
    /// using the returned NeededBlob.
    pub async fn open_blob(
        &mut self,
        content_blob: BlobId,
        blob_type: fpkg::BlobType,
    ) -> Result<Option<NeededBlob>, OpenBlobError> {
        open_blob(&self.needed_blobs, OpenKind::Content, content_blob, blob_type, None).await
    }

    /// Notifies the endpoint that all blobs have been written and wait for the response to the
    /// pending `Get()` request, returning the cached [`PackageDirectory`].
    pub async fn finish(self) -> Result<PackageDirectory, GetError> {
        drop(self.needed_blobs);
        let () = self.get_fut.await?.map_err(Status::from_raw)?;
        Ok(self.pkg_dir)
    }

    /// Aborts this caching operation for the package.
    pub async fn abort(self) {
        self.needed_blobs.abort().map(|_: Result<(), fidl::Error>| ()).await;
        // The package is not guaranteed to be removed from the dynamic index after abort
        // returns, we have to wait until finish returns (to prevent a resolve retry from
        // racing). The finish call will return an error that just tells us that we called
        // abort, so we ignore it.
        let _ = self.get_fut.await;
    }
}

/// A blob that needs to be written.
#[derive(Debug)]
pub struct NeededBlob {
    /// Typestate wrapper around the blob. Clients must first call truncate(), then write() until
    /// all data is provided.
    pub blob: Blob<NeedsTruncate>,

    /// Helper object that can close the blob independent of what state `blob` is in.
    pub closer: BlobCloser,
}

/// A handle to a blob that must be explicitly closed to prevent future opens of the same blob from
/// racing with this blob closing.
#[derive(Debug)]
#[must_use = "Subsequent opens of this blob may race with closing this one"]
pub struct BlobCloser {
    proxy: fio::FileProxy,
    closed: bool,
}

impl BlobCloser {
    /// Close the blob, silently ignoring errors.
    pub async fn close(mut self) {
        let _ = self.proxy.close().await;
        self.closed = true;
    }
}

impl Drop for BlobCloser {
    fn drop(&mut self) {
        if !self.closed {
            // Dropped without waiting on close. We can at least send the close request here, but
            // there could be a race with another attempt to open the blob.
            let _ = self.proxy.close();
        }
    }
}

/// The successful result of truncating a blob.
#[derive(Debug)]
pub enum TruncateBlobSuccess {
    /// The blob contents need to be written.
    NeedsData(Blob<NeedsData>),

    /// The blob is fully written (it was the empty blob) and now a
    /// fuchsia.pkg.NeededBlobs.BlobWritten message should be sent.
    AllWritten(Blob<NeedsBlobWritten>),
}

/// The successful result of writing some data to a blob.
#[derive(Debug)]
pub enum BlobWriteSuccess {
    /// There is still more data to write.
    NeedsData(Blob<NeedsData>),

    /// The blob is fully written and now a fuchsia.pkg.NeededBlobs.BlobWritten
    /// message should be sent.
    AllWritten(Blob<NeedsBlobWritten>),
}

/// State for a blob that can be truncated.
#[derive(Debug)]
pub struct NeedsTruncate;

/// State for a blob that can be written to.
#[derive(Debug)]
pub struct NeedsData {
    size: u64,
    written: u64,
}

/// State for a blob that has been fully written but that needs a
/// fuchsia.pkg.NeededBlobs.BlobWritten message sent to pkg-cache.
#[derive(Debug)]
pub struct NeedsBlobWritten;

/// A blob in the process of being written.
#[derive(Debug)]
#[must_use]
pub struct Blob<S> {
    proxy: fio::FileProxy,
    needed_blobs: fpkg::NeededBlobsProxy,
    blob_id: BlobId,
    state: S,
}

impl Blob<NeedsTruncate> {
    /// Truncates the blob to the given size. On success, the blob enters the writable state.
    pub async fn truncate(self, size: u64) -> Result<TruncateBlobSuccess, TruncateBlobError> {
        let () =
            self.proxy.resize(size).await?.map_err(Status::from_raw).map_err(
                |status| match status {
                    Status::NO_SPACE => TruncateBlobError::NoSpace,
                    status => TruncateBlobError::UnexpectedResponse(status),
                },
            )?;

        let Self { proxy, needed_blobs, blob_id, state: _ } = self;

        Ok(if size == 0 {
            TruncateBlobSuccess::AllWritten(Blob {
                proxy,
                needed_blobs,
                blob_id,
                state: NeedsBlobWritten,
            })
        } else {
            TruncateBlobSuccess::NeedsData(Blob {
                proxy,
                needed_blobs,
                blob_id,
                state: NeedsData { size, written: 0 },
            })
        })
    }
}

impl Blob<NeedsData> {
    /// Writes all of the given buffer to the blob.
    ///
    /// # Panics
    ///
    /// Panics if a write is attempted with a buf larger than the remaining blob size.
    pub fn write(
        self,
        buf: &[u8],
    ) -> impl Future<Output = Result<BlobWriteSuccess, WriteBlobError>> + '_ {
        self.write_with_trace_callbacks(buf, |_| {}, || {})
    }

    /// Writes all of the given buffer to the blob.
    /// Calls `after_write` after each fuchsia.io/File.Write message is sent with the number of
    /// bytes sent.
    /// Calls `after_write_ack` after each fuchsia.io/File.Write message is acknowledged.
    ///
    /// # Panics
    ///
    /// Panics if a write is attempted with a buf larger than the remaining blob size.
    pub async fn write_with_trace_callbacks(
        mut self,
        mut buf: &[u8],
        after_write: impl Fn(u64),
        after_write_ack: impl Fn(),
    ) -> Result<BlobWriteSuccess, WriteBlobError> {
        assert!(self.state.written + buf.len() as u64 <= self.state.size);

        while !buf.is_empty() {
            // Don't try to write more than MAX_BUF bytes at a time.
            let limit = buf.len().min(fio::MAX_BUF as usize);
            let written = self.write_some(&buf[..limit], &after_write, &after_write_ack).await?;
            buf = &buf[written..];
        }

        if self.state.written == self.state.size {
            let Self { proxy, needed_blobs, blob_id, state: _ } = self;
            Ok(BlobWriteSuccess::AllWritten(Blob {
                proxy,
                needed_blobs,
                blob_id,
                state: NeedsBlobWritten,
            }))
        } else {
            Ok(BlobWriteSuccess::NeedsData(self))
        }
    }

    /// Writes some of the given buffer to the blob.
    ///
    /// Returns the number of bytes written (which may be less than the buffer's size) or the error
    /// encountered during the write.
    async fn write_some(
        &mut self,
        buf: &[u8],
        after_write: &impl Fn(u64),
        after_write_ack: &impl Fn(),
    ) -> Result<usize, WriteBlobError> {
        let result_fut = self.proxy.write(buf);
        after_write(buf.len() as u64);

        let result = result_fut.await;
        after_write_ack();

        let result = result?.map_err(Status::from_raw);

        let actual = match result {
            Ok(actual) => actual,
            Err(Status::IO_DATA_INTEGRITY) => {
                return Err(WriteBlobError::Corrupt);
            }
            Err(Status::NO_SPACE) => {
                return Err(WriteBlobError::NoSpace);
            }
            Err(status) => {
                return Err(WriteBlobError::UnexpectedResponse(status));
            }
        };

        if actual > buf.len() as u64 {
            return Err(WriteBlobError::Overwrite);
        }

        self.state.written += actual;
        Ok(actual as usize)
    }
}

impl Blob<NeedsBlobWritten> {
    /// Tells pkg-cache that the blob has been successfully written and can now be read.
    pub async fn blob_written(self) -> Result<(), BlobWrittenError> {
        Ok(self.needed_blobs.blob_written(&self.blob_id.into()).await??)
    }
}

/// An error encountered while opening a package.
#[derive(Debug, thiserror::Error)]
#[allow(missing_docs)]
pub enum OpenError {
    #[error("the package does not exist")]
    NotFound,

    #[error("Open() responded with an unexpected status")]
    UnexpectedResponse(#[source] Status),

    #[error("transport error")]
    Fidl(#[from] fidl::Error),
}
/// An error encountered while caching a package.
#[derive(Debug, thiserror::Error)]
#[allow(missing_docs)]
pub enum GetError {
    #[error("Get() responded with an unexpected status")]
    UnexpectedResponse(#[from] Status),

    #[error("transport error")]
    Fidl(#[from] fidl::Error),
}

/// An error encountered while opening a metadata or content blob for write.
#[derive(Debug, thiserror::Error)]
#[allow(missing_docs)]
pub enum OpenBlobError {
    #[error("there is insufficient storage space available to persist this blob")]
    OutOfSpace,

    #[error("this blob is already open for write by another cache operation")]
    ConcurrentWrite,

    #[error("an unspecified error occurred during underlying I/O")]
    UnspecifiedIo,

    #[error("an unspecified error occurred")]
    Internal,

    #[error("transport error")]
    Fidl(#[from] fidl::Error),
}

impl From<fpkg::OpenBlobError> for OpenBlobError {
    fn from(e: fpkg::OpenBlobError) -> Self {
        match e {
            fpkg::OpenBlobError::OutOfSpace => OpenBlobError::OutOfSpace,
            fpkg::OpenBlobError::ConcurrentWrite => OpenBlobError::ConcurrentWrite,
            fpkg::OpenBlobError::UnspecifiedIo => OpenBlobError::UnspecifiedIo,
            fpkg::OpenBlobError::Internal => OpenBlobError::Internal,
        }
    }
}

/// An error encountered while enumerating missing content blobs.
#[derive(Debug, thiserror::Error)]
#[allow(missing_docs)]
pub enum ListMissingBlobsError {
    #[error("while obtaining the missing blobs fidl iterator")]
    CallGetMissingBlobs(#[source] fidl::Error),

    #[error("while obtaining the next chunk of blobs from the fidl iterator")]
    CallNextOnBlobIterator(#[source] fidl::Error),
}

/// An error encountered while truncating a blob
#[derive(Debug, thiserror::Error)]
#[allow(missing_docs)]
pub enum TruncateBlobError {
    #[error("insufficient storage space is available")]
    NoSpace,

    #[error("Truncate() responded with an unexpected status")]
    UnexpectedResponse(#[source] Status),

    #[error("transport error")]
    Fidl(#[from] fidl::Error),
}

/// An error encountered while writing a blob.
#[derive(Debug, thiserror::Error)]
#[allow(missing_docs)]
pub enum WriteBlobError {
    #[error("file endpoint reported it wrote more bytes than were actually provided to the file endpoint")]
    Overwrite,

    #[error("the written data was corrupt")]
    Corrupt,

    #[error("insufficient storage space is available")]
    NoSpace,

    #[error("Write() responded with an unexpected status")]
    UnexpectedResponse(#[source] Status),

    #[error("transport error")]
    Fidl(#[from] fidl::Error),
}

/// An error encountered while sending the BlobWritten message.
#[derive(Debug, thiserror::Error)]
#[allow(missing_docs)]
pub enum BlobWrittenError {
    #[error("pkg-cache could not find the blob after it was successfully written")]
    MissingAfterWritten,

    #[error("NeededBlobs.BlobWritten was called before the blob was opened")]
    UnopenedBlob,

    #[error("transport error")]
    Fidl(#[from] fidl::Error),
}

impl From<fpkg::BlobWrittenError> for BlobWrittenError {
    fn from(e: fpkg::BlobWrittenError) -> Self {
        match e {
            fpkg::BlobWrittenError::NotWritten => BlobWrittenError::MissingAfterWritten,
            fpkg::BlobWrittenError::UnopenedBlob => BlobWrittenError::UnopenedBlob,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use fidl::endpoints::{ClientEnd, ControlHandle as _, RequestStream as _};
    use fidl_fuchsia_pkg::{
        BlobInfoIteratorRequest, NeededBlobsRequest, NeededBlobsRequestStream,
        PackageCacheGetResponder, PackageCacheMarker, PackageCacheRequest,
        PackageCacheRequestStream,
    };

    struct MockPackageCache {
        stream: PackageCacheRequestStream,
    }

    impl MockPackageCache {
        fn new() -> (Client, Self) {
            let (proxy, stream) =
                fidl::endpoints::create_proxy_and_stream::<PackageCacheMarker>().unwrap();
            (Client::from_proxy(proxy), Self { stream })
        }

        async fn expect_get(&mut self, blob_info: BlobInfo) -> PendingGet {
            match self.stream.next().await {
                Some(Ok(PackageCacheRequest::Get {
                    meta_far_blob,
                    needed_blobs,
                    dir,
                    responder,
                })) => {
                    assert_eq!(BlobInfo::from(meta_far_blob), blob_info);
                    let needed_blobs = needed_blobs.into_stream().unwrap();
                    let dir = dir.unwrap().into_stream().unwrap();

                    PendingGet { stream: needed_blobs, dir, responder }
                }
                r => panic!("Unexpected request: {:?}", r),
            }
        }

        async fn expect_closed(mut self) {
            assert_matches!(self.stream.next().await, None);
        }
    }

    struct PendingGet {
        stream: NeededBlobsRequestStream,
        dir: fio::DirectoryRequestStream,
        responder: PackageCacheGetResponder,
    }

    impl PendingGet {
        async fn new() -> (Get, PendingGet) {
            let (client, mut server) = MockPackageCache::new();

            let get = client.get(blob_info(42)).unwrap();
            let pending_get = server.expect_get(blob_info(42)).await;
            (get, pending_get)
        }

        fn finish_hold_stream_open(self) -> (NeededBlobsRequestStream, PackageDirProvider) {
            self.stream.control_handle().shutdown_with_epitaph(Status::OK);
            self.responder.send(Ok(())).unwrap();
            (self.stream, PackageDirProvider { stream: self.dir })
        }

        fn finish(self) -> PackageDirProvider {
            self.stream.control_handle().shutdown_with_epitaph(Status::OK);
            self.responder.send(Ok(())).unwrap();
            PackageDirProvider { stream: self.dir }
        }

        #[cfg(target_os = "fuchsia")]
        fn fail_the_get(self) {
            self.responder
                .send(Err(Status::IO_INVALID.into_raw()))
                .expect("client should be waiting");
        }

        async fn expect_open_meta_blob(
            mut self,
            res: Result<Option<ClientEnd<fio::FileMarker>>, fpkg::OpenBlobError>,
        ) -> Self {
            match self.stream.next().await {
                Some(Ok(NeededBlobsRequest::OpenMetaBlob { blob_type, responder })) => {
                    assert_eq!(blob_type, fpkg::BlobType::Uncompressed);
                    responder.send(res).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn expect_open_blob(
            mut self,
            expected_blob_id: BlobId,
            res: Result<Option<ClientEnd<fio::FileMarker>>, fpkg::OpenBlobError>,
        ) -> Self {
            match self.stream.next().await {
                Some(Ok(NeededBlobsRequest::OpenBlob { blob_id, blob_type, responder })) => {
                    assert_eq!(BlobId::from(blob_id), expected_blob_id);
                    assert_eq!(blob_type, fpkg::BlobType::Uncompressed);
                    responder.send(res).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn expect_get_missing_blobs(mut self, response_chunks: Vec<Vec<BlobInfo>>) -> Self {
            match self.stream.next().await {
                Some(Ok(NeededBlobsRequest::GetMissingBlobs { iterator, control_handle: _ })) => {
                    let mut stream = iterator.into_stream().unwrap();

                    // Respond to each next request with the next chunk.
                    for chunk in response_chunks {
                        let chunk = chunk
                            .into_iter()
                            .map(fidl_fuchsia_pkg::BlobInfo::from)
                            .collect::<Vec<_>>();

                        let BlobInfoIteratorRequest::Next { responder } =
                            stream.next().await.unwrap().unwrap();
                        responder.send(&chunk).unwrap();
                    }

                    // Then respond with an empty chunk.
                    let BlobInfoIteratorRequest::Next { responder } =
                        stream.next().await.unwrap().unwrap();
                    responder.send(&[]).unwrap();

                    // Expect the client to stop asking.
                    assert_matches!(stream.next().await, None);
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn expect_get_missing_blobs_client_closes_channel(
            mut self,
            response_chunks: Vec<Vec<BlobInfo>>,
        ) -> Self {
            match self.stream.next().await {
                Some(Ok(NeededBlobsRequest::GetMissingBlobs { iterator, control_handle: _ })) => {
                    let mut stream = iterator.into_stream().unwrap();

                    // Respond to each next request with the next chunk.
                    for chunk in response_chunks {
                        let chunk = chunk
                            .into_iter()
                            .map(fidl_fuchsia_pkg::BlobInfo::from)
                            .collect::<Vec<_>>();

                        let BlobInfoIteratorRequest::Next { responder } =
                            stream.next().await.unwrap().unwrap();
                        responder.send(&chunk).unwrap();
                    }

                    // The client closes the channel before we can respond with an empty chunk.
                    assert_matches!(stream.next().await, None);
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn expect_get_missing_blobs_inject_iterator_error(mut self) -> Self {
            match self.stream.next().await {
                Some(Ok(NeededBlobsRequest::GetMissingBlobs { iterator, control_handle: _ })) => {
                    iterator
                        .into_stream_and_control_handle()
                        .unwrap()
                        .1
                        .shutdown_with_epitaph(Status::ADDRESS_IN_USE);
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        #[cfg(target_os = "fuchsia")]
        async fn expect_abort(mut self) -> Self {
            match self.stream.next().await {
                Some(Ok(NeededBlobsRequest::Abort { responder })) => {
                    responder.send().unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }
    }

    struct PackageDirProvider {
        stream: fio::DirectoryRequestStream,
    }

    impl PackageDirProvider {
        fn close_pkg_dir(self) {
            self.stream.control_handle().shutdown_with_epitaph(Status::NOT_EMPTY);
        }
    }

    fn blob_id(n: u8) -> BlobId {
        BlobId::from([n; 32])
    }

    fn blob_info(n: u8) -> BlobInfo {
        BlobInfo { blob_id: blob_id(n), length: 0 }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn constructor() {
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PackageCacheMarker>().unwrap();
        let client = Client::from_proxy(proxy);

        drop(stream);
        assert_matches!(client.proxy().sync().await, Err(_));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn get_present_package() {
        let (client, mut server) = MockPackageCache::new();

        let ((), ()) = future::join(
            async {
                server.expect_get(blob_info(2)).await.finish().close_pkg_dir();
                server.expect_closed().await;
            },
            async move {
                let mut get = client.get(blob_info(2)).unwrap();

                assert_matches!(
                    get.open_meta_blob(fpkg::BlobType::Uncompressed).await.unwrap(),
                    None
                );
                assert_eq!(get.get_missing_blobs().try_concat().await.unwrap(), vec![]);
                let pkg_dir = get.finish().await.unwrap();

                assert_matches!(
                    pkg_dir.into_proxy().take_event_stream().next().await,
                    Some(Err(fidl::Error::ClientChannelClosed { status: Status::NOT_EMPTY, .. }))
                );
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn get_present_package_handles_slow_stream_close() {
        let (client, mut server) = MockPackageCache::new();

        let (send, recv) = futures::channel::oneshot::channel::<()>();

        let ((), ()) = future::join(
            async {
                let (needed_blobs_stream, pkg_dir) =
                    server.expect_get(blob_info(2)).await.finish_hold_stream_open();
                pkg_dir.close_pkg_dir();

                // wait until `send` is dropped to drop the request stream.
                let _ = recv.await;
                drop(needed_blobs_stream);
            },
            async move {
                let mut get = client.get(blob_info(2)).unwrap();

                assert_matches!(
                    get.open_meta_blob(fpkg::BlobType::Uncompressed).await.unwrap(),
                    None
                );

                // ensure sending the request doesn't fail, then unblock closing the channel, then
                // ensure the get_missing_blobs call detects the closed iterator as success instead
                // of a PEER_CLOSED error.
                let missing_blobs_stream = get.get_missing_blobs();
                drop(send);
                assert_eq!(missing_blobs_stream.try_concat().await.unwrap(), vec![]);
                let pkg_dir = get.finish().await.unwrap();

                assert_matches!(
                    pkg_dir.into_proxy().take_event_stream().next().await,
                    Some(Err(fidl::Error::ClientChannelClosed { status: Status::NOT_EMPTY, .. }))
                );
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn needed_blobs_open_meta_far() {
        let (mut get, pending_get) = PendingGet::new().await;

        let ((), ()) = future::join(
            async {
                pending_get
                    .expect_open_meta_blob(Ok(None))
                    .await
                    .expect_open_meta_blob(Ok(Some(fidl::endpoints::create_endpoints().0)))
                    .await
                    .expect_open_meta_blob(Ok(None))
                    .await
                    .expect_open_meta_blob(Ok(Some(fidl::endpoints::create_endpoints().0)))
                    .await
                    .expect_open_meta_blob(Err(fpkg::OpenBlobError::OutOfSpace))
                    .await
                    .expect_open_meta_blob(Err(fpkg::OpenBlobError::UnspecifiedIo))
                    .await;
            },
            async {
                {
                    let opener = get.make_open_meta_blob();
                    assert_matches!(opener.open(fpkg::BlobType::Uncompressed).await.unwrap(), None);
                    assert_matches!(
                        opener.open(fpkg::BlobType::Uncompressed).await.unwrap(),
                        Some(_)
                    );
                }
                assert_matches!(
                    get.open_meta_blob(fpkg::BlobType::Uncompressed).await.unwrap(),
                    None
                );
                assert_matches!(
                    get.open_meta_blob(fpkg::BlobType::Uncompressed).await.unwrap(),
                    Some(_)
                );
                assert_matches!(
                    get.open_meta_blob(fpkg::BlobType::Uncompressed).await,
                    Err(OpenBlobError::OutOfSpace)
                );
                assert_matches!(
                    get.open_meta_blob(fpkg::BlobType::Uncompressed).await,
                    Err(OpenBlobError::UnspecifiedIo)
                );
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn needed_blobs_open_content_blob() {
        let (mut get, pending_get) = PendingGet::new().await;

        let ((), ()) = future::join(
            async {
                pending_get
                    .expect_open_blob(blob_id(2), Ok(None))
                    .await
                    .expect_open_blob(blob_id(2), Ok(Some(fidl::endpoints::create_endpoints().0)))
                    .await
                    .expect_open_blob(blob_id(10), Ok(None))
                    .await
                    .expect_open_blob(blob_id(11), Ok(Some(fidl::endpoints::create_endpoints().0)))
                    .await
                    .expect_open_blob(blob_id(12), Err(fpkg::OpenBlobError::OutOfSpace))
                    .await
                    .expect_open_blob(blob_id(13), Err(fpkg::OpenBlobError::UnspecifiedIo))
                    .await;
            },
            async {
                {
                    let opener = get.make_open_blob(blob_id(2));
                    assert_matches!(opener.open(fpkg::BlobType::Uncompressed).await.unwrap(), None);
                    assert_matches!(
                        opener.open(fpkg::BlobType::Uncompressed).await.unwrap(),
                        Some(_)
                    );
                }
                assert_matches!(
                    get.open_blob(blob_id(10), fpkg::BlobType::Uncompressed).await.unwrap(),
                    None
                );
                assert_matches!(
                    get.open_blob(blob_id(11), fpkg::BlobType::Uncompressed).await.unwrap(),
                    Some(_)
                );
                assert_matches!(
                    get.open_blob(blob_id(12), fpkg::BlobType::Uncompressed).await,
                    Err(OpenBlobError::OutOfSpace)
                );
                assert_matches!(
                    get.open_blob(blob_id(13), fpkg::BlobType::Uncompressed).await,
                    Err(OpenBlobError::UnspecifiedIo)
                );
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn needed_blobs_get_missing_blobs_on_closed_ok() {
        let (mut get, pending_get) = PendingGet::new().await;
        let _ = pending_get.finish();

        assert_matches!(get.open_meta_blob(fpkg::BlobType::Uncompressed).await, Ok(None));
        assert_eq!(get.get_missing_blobs().try_concat().await.unwrap(), vec![]);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn needed_blobs_get_missing_blobs() {
        let (mut get, pending_get) = PendingGet::new().await;

        let ((), ()) = future::join(
            async {
                pending_get
                    .expect_get_missing_blobs(vec![
                        vec![blob_info(1), blob_info(2)],
                        vec![blob_info(3)],
                    ])
                    .await;
            },
            async {
                assert_eq!(
                    get.get_missing_blobs().try_concat().await.unwrap(),
                    vec![blob_info(1), blob_info(2), blob_info(3)]
                );
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn needed_blobs_get_missing_blobs_fail_to_obtain_iterator() {
        let (mut get, pending_get) = PendingGet::new().await;
        drop(pending_get);

        assert_matches!(
            get.get_missing_blobs().try_concat().await,
            Err(ListMissingBlobsError::CallNextOnBlobIterator(
                fidl::Error::ClientChannelClosed{status, ..})
            )
                if status == Status::PEER_CLOSED
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn needed_blobs_get_missing_blobs_iterator_contains_error() {
        let (mut get, pending_get) = PendingGet::new().await;

        let (_, ()) =
            future::join(pending_get.expect_get_missing_blobs_inject_iterator_error(), async {
                assert_matches!(
                    get.get_missing_blobs().try_concat().await,
                    Err(ListMissingBlobsError::CallNextOnBlobIterator(
                        fidl::Error::ClientChannelClosed{status, ..}
                    ))
                        if status == Status::ADDRESS_IN_USE
                );
            })
            .await;
    }

    #[cfg(target_os = "fuchsia")]
    #[test]
    fn needed_blobs_abort() {
        use futures::{future::Either, pin_mut};
        use std::task::Poll;

        let mut executor = fuchsia_async::TestExecutor::new_with_fake_time();

        let fut = async {
            let (get, pending_get) = PendingGet::new().await;

            let abort_fut = get.abort().boxed();
            let expect_abort_fut = pending_get.expect_abort();
            pin_mut!(expect_abort_fut);

            match futures::future::select(abort_fut, expect_abort_fut).await {
                Either::Left(((), _expect_abort_fut)) => {
                    panic!("abort should wait for the get future to complete")
                }
                Either::Right((pending_get, abort_fut)) => (abort_fut, pending_get),
            }
        };
        pin_mut!(fut);

        let (mut abort_fut, pending_get) = match executor.run_until_stalled(&mut fut) {
            Poll::Pending => panic!("should complete"),
            Poll::Ready((abort_fut, pending_get)) => (abort_fut, pending_get),
        };

        // NeededBlobs.Abort should wait until PackageCache.Get returns
        assert_matches!(executor.run_until_stalled(&mut abort_fut), Poll::Pending);
        pending_get.fail_the_get();
        assert_matches!(executor.run_until_stalled(&mut abort_fut), Poll::Ready(()));
    }

    struct MockNeededBlob {
        blob: fio::FileRequestStream,
        needed_blobs: fpkg::NeededBlobsRequestStream,
    }

    impl MockNeededBlob {
        fn mock_hash() -> BlobId {
            [7; 32].into()
        }

        fn new() -> (NeededBlob, Self) {
            let (blob_proxy, blob) =
                fidl::endpoints::create_proxy_and_stream::<fio::FileMarker>().unwrap();
            let (needed_blobs_proxy, needed_blobs) =
                fidl::endpoints::create_proxy_and_stream::<fpkg::NeededBlobsMarker>().unwrap();
            (
                NeededBlob {
                    blob: Blob {
                        proxy: Clone::clone(&blob_proxy),
                        needed_blobs: needed_blobs_proxy,
                        blob_id: Self::mock_hash(),
                        state: NeedsTruncate,
                    },
                    closer: BlobCloser { proxy: blob_proxy, closed: false },
                },
                Self { blob, needed_blobs },
            )
        }

        async fn fail_truncate(mut self) -> Self {
            match self.blob.next().await {
                Some(Ok(fio::FileRequest::Resize { length: _, responder })) => {
                    responder.send(Err(Status::NO_SPACE.into_raw())).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn expect_truncate(mut self, expected_length: u64) -> Self {
            match self.blob.next().await {
                Some(Ok(fio::FileRequest::Resize { length, responder })) => {
                    assert_eq!(length, expected_length);
                    responder.send(Ok(())).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn fail_write(mut self) -> Self {
            match self.blob.next().await {
                Some(Ok(fio::FileRequest::Write { data: _, responder })) => {
                    responder.send(Err(Status::NO_SPACE.into_raw())).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn expect_write(mut self, expected_payload: &[u8]) -> Self {
            match self.blob.next().await {
                Some(Ok(fio::FileRequest::Write { data, responder })) => {
                    assert_eq!(data, expected_payload);
                    responder.send(Ok(data.len() as u64)).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn expect_write_partial(
            mut self,
            expected_payload: &[u8],
            bytes_to_consume: u64,
        ) -> Self {
            match self.blob.next().await {
                Some(Ok(fio::FileRequest::Write { data, responder })) => {
                    assert_eq!(data, expected_payload);
                    responder.send(Ok(bytes_to_consume)).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }

        async fn expect_close(mut self) {
            match self.blob.next().await {
                Some(Ok(fio::FileRequest::Close { responder })) => {
                    responder.send(Ok(())).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
        }

        async fn expect_blob_written(mut self) -> Self {
            match self.needed_blobs.next().await {
                Some(Ok(fpkg::NeededBlobsRequest::BlobWritten { blob_id, responder })) => {
                    assert_eq!(blob_id, Self::mock_hash().into());
                    responder.send(Ok(())).unwrap();
                }
                r => panic!("Unexpected request: {:?}", r),
            }
            self
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn empty_blob_write() {
        let (NeededBlob { blob, closer }, blob_server) = MockNeededBlob::new();

        let ((), ()) = future::join(
            async {
                blob_server
                    .expect_truncate(0)
                    .await
                    .expect_blob_written()
                    .await
                    .expect_close()
                    .await;
            },
            async {
                let blob = match blob.truncate(0).await.unwrap() {
                    TruncateBlobSuccess::AllWritten(blob) => blob,
                    other => panic!("empty blob shouldn't need bytes {other:?}"),
                };
                let () = blob.blob_written().await.unwrap();
                closer.close().await;
            },
        )
        .await;
    }

    impl TruncateBlobSuccess {
        fn unwrap_needs_data(self) -> Blob<NeedsData> {
            match self {
                TruncateBlobSuccess::NeedsData(blob) => blob,
                TruncateBlobSuccess::AllWritten(_) => panic!("blob should need data"),
            }
        }
    }

    impl BlobWriteSuccess {
        fn unwrap_needs_data(self) -> Blob<NeedsData> {
            match self {
                BlobWriteSuccess::NeedsData(blob) => blob,
                BlobWriteSuccess::AllWritten(_) => panic!("blob should need data"),
            }
        }

        fn unwrap_all_written(self) -> Blob<NeedsBlobWritten> {
            match self {
                BlobWriteSuccess::NeedsData(_) => panic!("blob should be completely written"),
                BlobWriteSuccess::AllWritten(blob) => blob,
            }
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn small_blob_write() {
        let (NeededBlob { blob, closer }, blob_server) = MockNeededBlob::new();

        let ((), ()) = future::join(
            async {
                blob_server
                    .expect_truncate(4)
                    .await
                    .expect_write(b"test")
                    .await
                    .expect_blob_written()
                    .await
                    .expect_close()
                    .await;
            },
            async {
                let blob = blob.truncate(4).await.unwrap().unwrap_needs_data();
                let blob = blob.write(b"test").await.unwrap().unwrap_all_written();
                let () = blob.blob_written().await.unwrap();
                closer.close().await;
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn blob_truncate_no_space() {
        let (NeededBlob { blob, closer }, blob_server) = MockNeededBlob::new();

        let ((), ()) = future::join(
            async {
                blob_server.fail_truncate().await;
            },
            async {
                assert_matches!(blob.truncate(4).await, Err(TruncateBlobError::NoSpace));
                closer.close().await;
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn blob_write_no_space() {
        let (NeededBlob { blob, closer }, blob_server) = MockNeededBlob::new();

        let ((), ()) = future::join(
            async {
                blob_server.expect_truncate(4).await.fail_write().await;
            },
            async {
                let blob = blob.truncate(4).await.unwrap().unwrap_needs_data();
                assert_matches!(blob.write(b"test").await, Err(WriteBlobError::NoSpace));
                closer.close().await;
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn blob_write_server_partial_write() {
        let (NeededBlob { blob, closer }, blob_server) = MockNeededBlob::new();

        let ((), ()) = future::join(
            async {
                blob_server
                    .expect_truncate(6)
                    .await
                    .expect_write_partial(b"abc123", 3)
                    .await
                    .expect_write(b"123")
                    .await
                    .expect_blob_written()
                    .await;
            },
            async {
                let blob = blob.truncate(6).await.unwrap().unwrap_needs_data();
                let blob = blob.write(b"abc123").await.unwrap().unwrap_all_written();
                let () = blob.blob_written().await.unwrap();
                closer.close().await;
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn blob_write_client_partial_write() {
        let (NeededBlob { blob, closer }, blob_server) = MockNeededBlob::new();

        let ((), ()) = future::join(
            async {
                blob_server
                    .expect_truncate(6)
                    .await
                    .expect_write(b"abc")
                    .await
                    .expect_write(b"123")
                    .await
                    .expect_blob_written()
                    .await;
            },
            async {
                let blob = blob.truncate(6).await.unwrap().unwrap_needs_data();
                let blob = blob.write(b"abc").await.unwrap().unwrap_needs_data();
                let blob = blob.write(b"123").await.unwrap().unwrap_all_written();
                let () = blob.blob_written().await.unwrap();
                closer.close().await;
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn blob_write_chunkize_payload() {
        const CHUNK_SIZE: usize = fio::MAX_BUF as usize;

        let (NeededBlob { blob, closer }, blob_server) = MockNeededBlob::new();

        let ((), ()) = future::join(
            async {
                blob_server
                    .expect_truncate(3 * CHUNK_SIZE as u64)
                    .await
                    .expect_write(&[0u8; CHUNK_SIZE])
                    .await
                    .expect_write(&[1u8; CHUNK_SIZE])
                    .await
                    .expect_write(&[2u8; CHUNK_SIZE])
                    .await
                    .expect_blob_written()
                    .await;
            },
            async {
                let payload =
                    (0..3).flat_map(|n| std::iter::repeat(n).take(CHUNK_SIZE)).collect::<Vec<u8>>();
                let blob = blob.truncate(3 * CHUNK_SIZE as u64).await.unwrap().unwrap_needs_data();
                let blob = blob.write(&payload).await.unwrap().unwrap_all_written();
                let () = blob.blob_written().await.unwrap();
                closer.close().await;
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn get_already_cached_success() {
        let (client, mut server) = MockPackageCache::new();

        let ((), ()) = future::join(
            async {
                server.expect_get(blob_info(2)).await.finish().close_pkg_dir();
                server.expect_closed().await;
            },
            async move {
                let pkg_dir = client.get_already_cached(blob_id(2)).await.unwrap();

                assert_matches!(
                    pkg_dir.into_proxy().take_event_stream().next().await,
                    Some(Err(fidl::Error::ClientChannelClosed { status: Status::NOT_EMPTY, .. }))
                );
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn get_already_cached_missing_meta_far() {
        let (client, mut server) = MockPackageCache::new();

        let ((), ()) = future::join(
            async {
                server
                    .expect_get(blob_info(2))
                    .await
                    .expect_open_meta_blob(Ok(Some(fidl::endpoints::create_endpoints().0)))
                    .await;
            },
            async move {
                assert_matches!(
                    client.get_already_cached(blob_id(2)).await,
                    Err(GetAlreadyCachedError::MissingMetaFar)
                );
            },
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn get_already_cached_missing_content_blob() {
        let (client, mut server) = MockPackageCache::new();

        let ((), ()) = future::join(
            async {
                server
                    .expect_get(blob_info(2))
                    .await
                    .expect_open_meta_blob(Ok(None))
                    .await
                    .expect_get_missing_blobs_client_closes_channel(vec![vec![BlobInfo {
                        blob_id: [0; 32].into(),
                        length: 0,
                    }]])
                    .await;
            },
            async move {
                assert_matches!(
                    client.get_already_cached(blob_id(2)).await,
                    Err(GetAlreadyCachedError::MissingContentBlobs(v))
                        if v == vec![BlobInfo {
                            blob_id: [0; 32].into(),
                            length: 0,
                        }]
                );
            },
        )
        .await;
    }
}
