// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Download blob data from Google Cloud Storage (GCS).

use {
    crate::token_store::TokenStore,
    anyhow::{bail, Context, Result},
    fuchsia_hyper::{new_https_client, HttpsClient},
    hyper::{body::HttpBody as _, header::CONTENT_LENGTH, Body, Response, StatusCode},
    std::{
        fs::{create_dir_all, File},
        io::Write,
        path::Path,
        sync::Arc,
    },
};

/// A snapshot of the progress.
#[derive(Clone, Debug, PartialEq)]
pub struct ProgressState<'a> {
    /// Label for this layer of progress, e.g. current URL.
    pub name: &'a str,

    /// The current step of the progress.
    pub at: u64,

    /// The total steps.
    pub of: u64,

    /// The type of `at` and `of`, such as "steps", "files", "bytes".
    pub units: &'a str,
}
/// This types promote self-documenting code.
pub type OverallProgress<'a> = ProgressState<'a>;
pub type DirectoryProgress<'a> = ProgressState<'a>;
pub type FileProgress<'a> = ProgressState<'a>;

/// Allow the user an opportunity to cancel an operation.
#[derive(Debug, PartialEq)]
pub enum ProgressResponse {
    /// Keep going.
    Continue,

    /// The user (or high level code) asks that the operation halt. This isn't
    /// an error in itself. Simply stop (e.g. break from iteration) and return.
    Cancel,
}

pub type ProgressResult<E = anyhow::Error> = std::result::Result<ProgressResponse, E>;

/// Throttle an action to N times per second.
pub struct Throttle {
    prior_update: std::time::Instant,
    interval: std::time::Duration,
}

impl Throttle {
    /// `is_ready()` will return true after each `interval` amount of time.
    pub fn from_duration(interval: std::time::Duration) -> Self {
        let prior_update = std::time::Instant::now();
        let prior_update = prior_update.checked_sub(interval).expect("reasonable interval");
        // A std::time::Instant cannot be created at zero or epoch start, so
        // the interval is deducted twice to create an instant that will trigger
        // on the first call to is_ready().
        // See Rust issue std::time::Instant #40910.
        let prior_update = prior_update.checked_sub(interval).expect("reasonable interval");
        Throttle { prior_update, interval }
    }

    /// Determine whether enough time has elapsed.
    ///
    /// Returns true at most N times per second, where N is the value passed
    /// into `new()`.
    pub fn is_ready(&mut self) -> bool {
        let now = std::time::Instant::now();
        if now.duration_since(self.prior_update) >= self.interval {
            self.prior_update = now;
            return true;
        }
        return false;
    }
}

/// An https client capable of fetching objects from GCS.
#[derive(Clone, Debug)]
pub struct Client {
    /// Base client used for HTTP/S IO.
    https: HttpsClient,
    token_store: Arc<TokenStore>,
}

impl Client {
    /// An https client that uses a `token_store` to authenticate with GCS.
    ///
    /// This is sufficient for downloading public or private data blobs from
    /// GCS. Reuse a client (or clones of a single client) for best results.
    ///
    /// If more than one client is desired (to download in parallel, for
    /// example), clone an existing client. This will cause them to share
    /// access token information. It's preferable to share a single token_store
    /// to share the access token stored therein, to avoid unnecessary network
    /// traffic or repeatedly asking the user for access.
    ///
    /// The shared access token is thread/async safe to encourage cloning for
    /// shared instance. (Cloning the mutex to the token, not the token).
    pub fn initial() -> Result<Self> {
        Ok(Self { https: new_https_client(), token_store: Arc::new(TokenStore::new()?) })
    }

    /// Allow access to public and private (auth-required) GCS data.
    pub fn initial_with_urls(api_base: &str, storage_base: &str) -> Result<Self> {
        Ok(Self {
            https: new_https_client(),
            token_store: Arc::new(TokenStore::new_with_urls(api_base, storage_base)?),
        })
    }

    /// Set the access token for all clients cloned from this client.
    pub async fn set_access_token(&self, access: String) {
        self.token_store.set_access_token(access).await;
    }

    /// Similar to path::exists(), return true if the blob is available.
    pub async fn exists(&self, bucket: &str, prefix: &str) -> Result<bool> {
        self.token_store.exists(&self.https, bucket, prefix).await
    }

    /// Save content of matching objects (blob) from GCS to local location
    /// `output_dir`.
    pub async fn fetch_all<P, F>(
        &self,
        bucket: &str,
        prefix: &str,
        output_dir: P,
        progress: &F,
    ) -> Result<()>
    where
        P: AsRef<Path>,
        F: Fn(DirectoryProgress<'_>, FileProgress<'_>) -> ProgressResult,
    {
        let objects = self
            .token_store
            .list(&self.https, bucket, prefix, /*limit=*/ None)
            .await
            .context("listing with token store")?;
        let output_dir = output_dir.as_ref();
        let mut count = 0;
        let total = objects.len() as u64;
        for object in objects {
            if let Some(relative_path) = object.strip_prefix(prefix) {
                let start = std::time::Instant::now();
                // Strip leading slash, if present.
                let relative_path = if relative_path.starts_with("/") {
                    &relative_path[1..]
                } else {
                    relative_path
                };
                let output_path = if relative_path.is_empty() {
                    // The `relative_path` is empty with then specified prefix
                    // is a file.
                    output_dir.join(Path::new(prefix).file_name().expect("Prefix file name."))
                } else {
                    output_dir.join(relative_path)
                };

                if let Some(parent) = output_path.parent() {
                    create_dir_all(&parent)
                        .with_context(|| format!("creating dir all for {:?}", parent))?;
                }
                let mut file = File::create(&output_path).context("create file")?;
                let url = format!("gs://{}/", bucket);
                count += 1;
                let dir_progress =
                    ProgressState { name: &url, at: count, of: total, units: "files" };
                self.write(bucket, &object, &mut file, &|file_progress| {
                    assert!(
                        file_progress.at <= file_progress.of,
                        "At {} of {}",
                        file_progress.at,
                        file_progress.of
                    );
                    progress(dir_progress.clone(), file_progress)
                })
                .await
                .context("write object")?;
                use std::io::{Seek, SeekFrom};
                let file_size = file.seek(SeekFrom::End(0)).context("getting file size")?;
                tracing::debug!(
                    "Wrote gs://{}/{} to {:?}, {} bytes in {} seconds.",
                    bucket,
                    object,
                    output_path,
                    file_size,
                    start.elapsed().as_secs_f32()
                );
            }
        }
        Ok(())
    }

    /// Save content of a stored object (blob) from GCS at location `output`.
    ///
    /// Wraps call to `self.write` which wraps `self.stream()`.
    pub async fn fetch_with_progress<P, F>(
        &self,
        bucket: &str,
        object: &str,
        output: P,
        progress: &F,
    ) -> ProgressResult
    where
        P: AsRef<Path>,
        F: Fn(ProgressState<'_>) -> ProgressResult,
    {
        let mut file = File::create(output.as_ref())?;
        self.write(bucket, object, &mut file, progress).await
    }

    /// As `fetch_with_progress()` without a progress callback.
    pub async fn fetch_without_progress<P>(
        &self,
        bucket: &str,
        object: &str,
        output: P,
    ) -> Result<()>
    where
        P: AsRef<Path>,
    {
        let mut file = File::create(output.as_ref())?;
        self.write(bucket, object, &mut file, &mut |_| Ok(ProgressResponse::Continue)).await?;
        Ok(())
    }

    /// Reads content of a stored object (blob) from GCS.
    pub async fn stream(&self, bucket: &str, object: &str) -> Result<Response<Body>> {
        self.token_store.download(&self.https, bucket, object).await
    }

    /// Write content of a stored object (blob) from GCS to writer.
    ///
    /// Wraps call to `self.stream`.
    pub async fn write<W, F>(
        &self,
        bucket: &str,
        object: &str,
        writer: &mut W,
        progress: &F,
    ) -> ProgressResult
    where
        W: Write + Sync,
        F: Fn(FileProgress<'_>) -> ProgressResult,
    {
        let mut res = self
            .stream(bucket, object)
            .await
            .with_context(|| format!("gcs streaming from {:?} {:?}", bucket, object))?;
        if res.status() == StatusCode::OK {
            let mut at: u64 = 0;
            let length = if res.headers().contains_key(CONTENT_LENGTH) {
                res.headers()
                    .get(CONTENT_LENGTH)
                    .context("getting content length")?
                    .to_str()?
                    .parse::<u64>()
                    .context("parsing content length as u64")?
            } else if res.headers().contains_key("x-goog-stored-content-length") {
                // The size of gzipped files is a guess.
                res.headers()["x-goog-stored-content-length"]
                    .to_str()
                    .context("getting x-goog content length as str")?
                    .parse::<u64>()
                    .context("parsing x-goog content length as u64")?
                    * 3
            } else {
                println!("missing content-length in {}: res.headers() {:?}", object, res.headers());
                bail!("missing content-length in header");
            };
            let mut of = length;
            // Throttle the progress UI updates to avoid burning CPU on changes
            // the user will have trouble seeing anyway. Without throttling,
            // around 20% of the execution time can be spent updating the
            // progress UI. The throttle makes the overhead negligible.
            let mut throttle = Throttle::from_duration(std::time::Duration::from_millis(500));
            while let Some(next) = res.data().await {
                let chunk = next.context("next chunk")?;
                writer.write_all(&chunk).context("write chunk")?;
                at += chunk.len() as u64;
                if at > of {
                    of = at;
                }
                if throttle.is_ready() {
                    match progress(ProgressState { name: object, at, of, units: "bytes" })
                        .context("rendering progress")?
                    {
                        ProgressResponse::Cancel => return Ok(ProgressResponse::Cancel),
                        _ => (),
                    }
                }
            }
            return Ok(ProgressResponse::Continue);
        }
        bail!("Failed to fetch file, result status: {:?}", res.status());
    }

    /// List objects in `bucket` with matching `prefix`.
    pub async fn list(&self, bucket: &str, prefix: &str) -> Result<Vec<String>> {
        self.token_store.list(&self.https, bucket, prefix, /*limit=*/ None).await
    }
}

#[cfg(test)]
mod test {
    use {super::*, std::fs::read_to_string};

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_client_factory_no_auth() {
        let client = Client::initial().expect("creating client");
        let res =
            client.stream("for_testing_does_not_exist", "face_test_object").await.expect("stream");
        assert_eq!(res.status(), 404);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_client_clone() {
        let client_a = Client::initial().expect("creating client");
        client_a.set_access_token("fake_token".to_string()).await;
        let client_b = client_a.clone();
        let client_c = Client::initial().expect("creating client");
        assert_eq!("fake_token", *client_a.token_store.access_token.lock().await);
        assert_eq!("fake_token", *client_b.token_store.access_token.lock().await);
        assert_eq!("", *client_c.token_store.access_token.lock().await);
    }

    /// This test relies on a local file which is not present on test bots, so
    /// it is marked "ignore".
    /// This can be run with `fx test gcs_lib_test -- --ignored`.
    #[ignore]
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_client_with_auth() {
        let client = Client::initial().expect("creating client");

        // Try downloading something that doesn't exist.
        let res =
            client.stream("for_testing_does_not_exist", "fake_test_object").await.expect("stream");
        assert_eq!(res.status(), 404);

        // Fetch something that does exist.
        let out_dir = tempfile::tempdir().unwrap();
        let out_file = out_dir.path().join("downloaded");
        client
            .fetch_without_progress("fuchsia", "development/LATEST_LINUX", &out_file)
            .await
            .expect("fetch");
        assert!(out_file.exists());
        let fetched = read_to_string(out_file).expect("read out_file");
        assert!(!fetched.is_empty());

        // Write the same data.
        let mut written = Vec::new();
        client
            .write("fuchsia", "development/LATEST_LINUX", &mut written, &mut |_| {
                Ok(ProgressResponse::Continue)
            })
            .await
            .expect("write");
        // The data is expected to be small (less than a KiB). For a non-test
        // keeping the whole file in memory may be impractical.
        let written = String::from_utf8(written).expect("streamed string");
        assert!(!written.is_empty());

        // Compare the fetched and written data.
        assert_eq!(fetched, written);

        // Stream the same data.
        let res = client.stream("fuchsia", "development/LATEST_LINUX").await.expect("stream");
        assert_eq!(res.status(), 200);
        // The data is expected to be small (less than a KiB). For a non-test
        // keeping the whole file in memory may be impractical.
        let streamed_bytes = hyper::body::to_bytes(res.into_body()).await.expect("streamed bytes");
        let streamed = String::from_utf8(streamed_bytes.to_vec()).expect("streamed string");

        // Compare the fetched and streamed data.
        assert_eq!(fetched, streamed);
    }
}
