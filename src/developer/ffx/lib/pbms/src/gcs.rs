// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Access utilities for gcs metadata.

use {
    crate::AuthFlowChoice,
    anyhow::{bail, Context, Result},
    errors::ffx_bail,
    gcs::{
        auth,
        client::{
            Client, ClientFactory, DirectoryProgress, FileProgress, ProgressResponse,
            ProgressResult,
        },
        error::GcsError,
        gs_url::split_gs_url,
        token_store::{
            read_boto_refresh_token, write_boto_refresh_token, RefreshAccessType, TokenStore,
        },
    },
    std::path::{Path, PathBuf},
    structured_ui,
};

/// Create a GCS client that only allows access to public buckets.
pub(crate) fn get_gcs_client_without_auth() -> Client {
    let no_auth = TokenStore::new_without_auth();
    let client_factory = ClientFactory::new(no_auth);
    client_factory.create_client()
}

/// Returns the path to the .boto (gsutil) configuration file.
pub(crate) async fn get_boto_path<I>(auth_flow: &AuthFlowChoice, ui: &I) -> Result<Option<PathBuf>>
where
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("get_boto_path");
    if let AuthFlowChoice::Exec(_) = auth_flow {
        // The .boto file is not used for exec.
        return Ok(None);
    }
    // TODO(fxb/89584): Change to using ffx client Id and consent screen.
    let boto: Option<PathBuf> =
        ffx_config::get("flash.gcs.token").await.context("getting flash.gcs.token config value")?;
    match &boto {
        Some(boto_path) => {
            if !boto_path.is_file() {
                tracing::debug!("missing boto file at {:?}", boto_path);
                update_refresh_token(&boto_path, auth_flow, ui)
                    .await
                    .context("Set up refresh token")?
            }
        }
        None => ffx_bail!(
            "GCS authentication configuration value \"flash.gcs.token\" not \
            found. Set this value by running `ffx config set flash.gcs.token <path>` \
            to the path of the .boto file."
        ),
    };

    Ok(boto)
}

/// Returns a GCS client that can access public and private buckets.
///
/// `boto_path` is the path to the .boto (gsutil) configuration file.
pub(crate) fn get_gcs_client_with_auth(
    auth_flow: &AuthFlowChoice,
    boto_path: &Option<PathBuf>,
) -> Result<Client> {
    tracing::debug!("get_gcs_client_with_auth");
    let access_type = match auth_flow {
        AuthFlowChoice::Default
        | AuthFlowChoice::Pkce
        | AuthFlowChoice::Oob
        | AuthFlowChoice::Device => {
            let path =
                boto_path.as_ref().expect("A .boto path is required. Please report as a bug.");
            read_boto_refresh_token(path).context("read boto refresh")?
        }
        AuthFlowChoice::Exec(exec_path) => RefreshAccessType::Exec(exec_path.to_path_buf()),
        AuthFlowChoice::NoAuth => RefreshAccessType::NoAuth,
    };
    let auth = TokenStore::new_with_auth(access_type, /*access_token=*/ None)?;

    let client_factory = ClientFactory::new(auth);
    Ok(client_factory.create_client())
}

/// Return true if the blob is available.
///
/// `gcs_url` is the full GCS url, e.g. "gs://bucket/path/to/file".
/// The resulting data will be written to a directory at `local_dir`.
pub(crate) async fn exists_in_gcs<I>(
    gcs_url: &str,
    auth_flow: &AuthFlowChoice,
    ui: &I,
) -> Result<bool>
where
    I: structured_ui::Interface + Sync,
{
    let client = get_gcs_client_without_auth();
    let (bucket, gcs_path) = split_gs_url(gcs_url).context("Splitting gs URL.")?;
    match client.exists(bucket, gcs_path).await {
        Ok(exists) => Ok(exists),
        Err(_) => exists_in_gcs_with_auth(bucket, gcs_path, auth_flow, ui)
            .await
            .context("checking existence with auth"),
    }
}

/// Return true if the blob is available, using auth.
///
/// Fallback from using `exists_in_gcs()` without auth.
async fn exists_in_gcs_with_auth<I>(
    gcs_bucket: &str,
    gcs_path: &str,
    auth_flow: &AuthFlowChoice,
    ui: &I,
) -> Result<bool>
where
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("exists_in_gcs_with_auth");
    let boto_path = get_boto_path(auth_flow, ui).await?;

    loop {
        let client = get_gcs_client_with_auth(auth_flow, &boto_path)?;
        match client.exists(gcs_bucket, gcs_path).await {
            Ok(exists) => return Ok(exists),
            Err(e) => match e.downcast_ref::<GcsError>() {
                Some(GcsError::NeedNewRefreshToken) => {
                    tracing::debug!("exists_in_gcs_with_auth got NeedNewRefreshToken");
                    if let Some(path) = &boto_path {
                        update_refresh_token(&path, auth_flow, ui)
                            .await
                            .context("Updating refresh token")?
                    }
                }
                Some(GcsError::NotFound(_, _)) => {
                    // Ok(false) should be returned rather than NotFound.
                    unreachable!();
                }
                Some(_) | None => bail!(
                    "Cannot get product bundle container while \
                    downloading from gs://{}/{}, error {:?}",
                    gcs_bucket,
                    gcs_path,
                    e,
                ),
            },
        }
    }
}

/// Download from a given `gcs_url`.
///
/// `gcs_url` is the full GCS url, e.g. "gs://bucket/path/to/file".
/// The resulting data will be written to a directory at `local_dir`.
pub(crate) async fn fetch_from_gcs<F, I>(
    gcs_url: &str,
    local_dir: &Path,
    auth_flow: &AuthFlowChoice,
    progress: &F,
    ui: &I,
) -> Result<()>
where
    F: Fn(DirectoryProgress<'_>, FileProgress<'_>) -> ProgressResult,
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("fetch_from_gcs {:?}", gcs_url);
    let client = get_gcs_client_without_auth();
    let (bucket, gcs_path) = split_gs_url(gcs_url).context("Splitting gs URL.")?;
    if !client.fetch_all(bucket, gcs_path, &local_dir, progress).await.is_ok() {
        tracing::debug!("Failed without auth, trying auth {:?}", gcs_url);
        fetch_from_gcs_with_auth(bucket, gcs_path, local_dir, auth_flow, progress, ui)
            .await
            .context("fetching with auth")?;
    }
    Ok(())
}

/// Download from a given `gcs_url` using auth.
///
/// Fallback from using `fetch_from_gcs()` without auth.
async fn fetch_from_gcs_with_auth<F, I>(
    gcs_bucket: &str,
    gcs_path: &str,
    local_dir: &Path,
    auth_flow: &AuthFlowChoice,
    progress: &F,
    ui: &I,
) -> Result<()>
where
    F: Fn(DirectoryProgress<'_>, FileProgress<'_>) -> ProgressResult,
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("fetch_from_gcs_with_auth");
    let boto_path = get_boto_path(auth_flow, ui).await?;

    loop {
        let client = get_gcs_client_with_auth(auth_flow, &boto_path)?;
        tracing::debug!("gcs_bucket {:?}, gcs_path {:?}", gcs_bucket, gcs_path);
        match client
            .fetch_all(gcs_bucket, gcs_path, &local_dir, progress)
            .await
            .context("fetch all")
        {
            Ok(()) => break,
            Err(e) => match e.downcast_ref::<GcsError>() {
                Some(GcsError::NeedNewRefreshToken) => {
                    tracing::debug!("fetch_from_gcs_with_auth got NeedNewRefreshToken");
                    if let Some(path) = &boto_path {
                        update_refresh_token(&path, auth_flow, ui)
                            .await
                            .context("Updating refresh token")?
                    }
                }
                Some(GcsError::NotFound(b, p)) => {
                    tracing::warn!("[gs://{}/{} not found]", b, p);
                    break;
                }
                Some(_) | None => bail!(
                    "Cannot get data from gs://{}/{}, saving to {:?}, error {:?}",
                    gcs_bucket,
                    gcs_path,
                    local_dir,
                    e,
                ),
            },
        }
    }
    Ok(())
}

/// Download a single file from `gcs_url` to an in-ram string.
///
/// `gcs_url` is the full GCS url, e.g. "gs://bucket/path/to/file".
pub(crate) async fn string_from_gcs<F, I>(
    gcs_url: &str,
    auth_flow: &AuthFlowChoice,
    progress: &F,
    ui: &I,
) -> Result<String>
where
    F: Fn(FileProgress<'_>) -> ProgressResult,
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("string_from_gcs {:?}", gcs_url);
    let client = get_gcs_client_without_auth();
    let (bucket, gcs_path) = split_gs_url(gcs_url).context("Splitting gs URL.")?;
    let mut result = Vec::new();
    if client.write(bucket, gcs_path, &mut result, progress).await.is_ok() {
        return Ok(String::from_utf8_lossy(&result).to_string());
    }
    tracing::debug!("Failed without auth, trying auth {:?}", gcs_url);
    string_from_gcs_with_auth(bucket, gcs_path, auth_flow, progress, ui)
        .await
        .context("fetching to string with auth")
}

/// Download a single file from `gcs_url` to an in-ram string.
///
/// Fallback from using `string_from_gcs()` without auth.
async fn string_from_gcs_with_auth<F, I>(
    gcs_bucket: &str,
    gcs_path: &str,
    auth_flow: &AuthFlowChoice,
    progress: &F,
    ui: &I,
) -> Result<String>
where
    F: Fn(FileProgress<'_>) -> ProgressResult,
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("string_from_gcs_with_auth");
    let boto_path = get_boto_path(auth_flow, ui).await?;

    let mut result = Vec::new();
    loop {
        let client = get_gcs_client_with_auth(auth_flow, &boto_path)
            .context("creating gcs client with auth")?;
        tracing::debug!("gcs_bucket {:?}, gcs_path {:?}", gcs_bucket, gcs_path);
        match client
            .write(gcs_bucket, gcs_path, &mut result, progress)
            .await
            .context("writing to string")
        {
            Ok(ProgressResponse::Continue) => break,
            Ok(ProgressResponse::Cancel) => {
                tracing::info!("ProgressResponse requesting cancel, exiting");
                std::process::exit(1);
            }
            Err(e) => match e.downcast_ref::<GcsError>() {
                Some(GcsError::NeedNewRefreshToken) => {
                    tracing::debug!("string_from_gcs_with_auth got NeedNewRefreshToken");
                    if let Some(path) = &boto_path {
                        update_refresh_token(&path, auth_flow, ui)
                            .await
                            .context("Updating refresh token")?
                    }
                }
                Some(GcsError::NotFound(b, p)) => {
                    tracing::warn!("[gs://{}/{} not found]", b, p);
                    break;
                }
                Some(gcs_err) => bail!(
                    "Cannot get data from gs://{}/{} to string, error {:?}, {:?}",
                    gcs_bucket,
                    gcs_path,
                    e,
                    gcs_err,
                ),
                None => bail!(
                    "Cannot get data from gs://{}/{} to string (Non-GcsError), error {:?}",
                    gcs_bucket,
                    gcs_path,
                    e,
                ),
            },
        }
    }
    Ok(String::from_utf8_lossy(&result).to_string())
}

/// Prompt the user to visit the OAUTH2 permissions web page and enter a new
/// authorization code, then convert that to a refresh token and write that
/// refresh token to the ~/.boto file.
async fn update_refresh_token<I>(boto_path: &Path, auth_flow: &AuthFlowChoice, ui: &I) -> Result<()>
where
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("update_refresh_token {:?}", boto_path);
    println!("\nThe refresh token in the {:?} file needs to be updated.", boto_path);
    let refresh_token = match auth_flow {
        AuthFlowChoice::Default | AuthFlowChoice::Pkce => {
            auth::pkce::new_refresh_token(ui).await.context("get refresh token")?
        }
        AuthFlowChoice::Oob => {
            auth::oob::new_refresh_token().await.context("get oob refresh token")?
        }
        AuthFlowChoice::Device => {
            auth::device::new_refresh_token(ui).await.context("get device refresh token")?
        }
        AuthFlowChoice::Exec(_) => {
            bail!("There's no refresh token used with an executable for auth.");
        }
        AuthFlowChoice::NoAuth => {
            bail!("The refresh token should not be updated when no-auth is used.");
        }
    };
    tracing::debug!("Writing boto file {:?}", boto_path);
    write_boto_refresh_token(boto_path, &refresh_token)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use {super::*, tempfile::NamedTempFile};

    // TODO(fxbug.dev/92773): This test requires mocks for interactivity and
    // https. The test is currently disabled.
    #[ignore]
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_update_refresh_token() {
        let temp_file = NamedTempFile::new().expect("temp file");
        let ui = structured_ui::MockUi::new();
        update_refresh_token(&temp_file.path(), &AuthFlowChoice::Default, &ui)
            .await
            .expect("set refresh token");
    }
}
