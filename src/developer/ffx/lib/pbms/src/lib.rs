// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Access utilities for product metadata.
//!
//! This is a collection of helper functions wrapping the FMS and GCS libs.
//!
//! The metadata can be loaded from a variety of sources. The initial places are
//! GCS and the local build.
//!
//! Call `product_bundle_urls()` to get a set of URLs for each product bundle.
//!
//! Call `fms_entries_from()` to get FMS entries from a particular repo. The
//! entries include product bundle metadata, physical device specifications, and
//! virtual device specifications. Each FMS entry has a unique name to identify
//! that entry.
//!
//! These FMS entry names are suitable to present to the user. E.g. the name of
//! a product bundle is also the name of the product bundle metadata entry.

use {
    crate::{
        pbms::{
            fetch_product_metadata, get_product_data_from_gcs, local_path_helper,
            path_from_file_url, pb_dir_name, pb_names_from_path, pbm_repo_list,
            CONFIG_STORAGE_PATH, GS_SCHEME,
        },
        repo_info::RepoInfo,
    },
    ::gcs::client::{Client, ProgressResponse},
    anyhow::{bail, Context, Result},
    camino::Utf8Path,
    errors::ffx_bail,
    fms::Entries,
    futures::TryStreamExt as _,
    itertools::Itertools as _,
    sdk,
    sdk_metadata::ProductBundle,
    std::path::{Path, PathBuf},
    std::str::FromStr,
    structured_ui::{Presentation, TableRows},
};

pub use crate::gcs::handle_new_access_token;
pub use crate::pbms::{fetch_data_for_product_bundle_v1, get_product_dir, get_storage_dir};
pub use crate::transfer_manifest::transfer_download;

mod gcs;
mod pbms;
mod repo;
mod repo_info;
pub mod transfer_manifest;

/// Select an Oauth2 authorization flow.
#[derive(PartialEq, Debug, Clone)]
pub enum AuthFlowChoice {
    /// Fail rather than using authentication.
    NoAuth,
    Default,
    Device,
    Exec(PathBuf),
    Oob,
    Pkce,
}

/// Convert common cli switches to AuthFlowChoice.
pub fn select_auth(oob_auth: bool, auth_flow: &AuthFlowChoice) -> &AuthFlowChoice {
    if oob_auth {
        eprintln!("\n\nPlease use `--auth oob` rather than `--oob-auth`\n\n");
        &AuthFlowChoice::Oob
    } else {
        auth_flow
    }
}

/// Convert CLI arg or config strings to AuthFlowChoice.
impl FromStr for AuthFlowChoice {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s.as_ref() {
            "no-auth" => Ok(AuthFlowChoice::NoAuth),
            "default" => Ok(AuthFlowChoice::Default),
            "device-experimental" => Ok(AuthFlowChoice::Device),
            "oob" => Ok(AuthFlowChoice::Oob),
            "pkce" => Ok(AuthFlowChoice::Pkce),
            exec => {
                let path = Path::new(exec);
                if path.is_file() {
                    Ok(AuthFlowChoice::Exec(path.to_path_buf()))
                } else {
                    Err("Unknown auth flow choice. Use one of oob, \
                        device-experimental, pkce, default, a path to an \
                        executable which prints an access token to stdout, or \
                        no-auth to enforce that no auth flow will be used."
                        .to_string())
                }
            }
        }
    }
}

#[derive(PartialEq, Debug, Clone)]
pub enum ListingMode {
    AllBundles,
    GetableBundles,
    ReadyBundlesOnly,
    RemovableBundles,
}

/// Load a product bundle by name, uri, or local path.
/// This is capable of loading both v1 and v2 ProductBundles.
pub async fn load_product_bundle(
    sdk: &ffx_config::Sdk,
    product_bundle: &Option<String>,
    mode: ListingMode,
) -> Result<ProductBundle> {
    tracing::debug!("Loading a product bundle: {:?}", product_bundle);

    //  If `product_bundle` is a local path, load it directly.
    if let Some(path) = product_bundle.as_ref().map(|s| Utf8Path::new(s)).filter(|p| p.exists()) {
        return ProductBundle::try_load_from(path);
    }

    // Otherwise, use the `fms` crate to fetch and parse the product bundle by name or uri.
    let should_print = true;
    let product_url = select_product_bundle(sdk, product_bundle, mode, should_print)
        .await
        .context("Selecting product bundle")?;
    let name = product_url.fragment().expect("Product name is required.");

    let fms_entries =
        fms_entries_from(&product_url, &sdk.get_path_prefix()).await.context("get fms entries")?;
    let product = fms::find_product_bundle(&fms_entries, &Some(name.to_string()))
        .context("problem with product_bundle")?
        .to_owned();
    Ok(ProductBundle::V1(product))
}

/// For each non-local URL in ffx CONFIG_METADATA, fetch updated info.
pub async fn update_metadata_all<I>(
    sdk: &ffx_config::Sdk,
    output_dir: &Path,
    auth_flow: &AuthFlowChoice,
    ui: &I,
    client: &Client,
) -> Result<()>
where
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("update_metadata_all");
    let repos = pbm_repo_list(sdk).await.context("getting repo list")?;
    async_fs::create_dir_all(&output_dir).await.context("create directory")?;
    for repo_url in repos {
        if repo_url.scheme() != GS_SCHEME {
            // There's no need to fetch local files or unrecognized schemes.
            continue;
        }
        tracing::debug!("update_metadata_all repo_url {:?}", repo_url);
        fetch_product_metadata(
            &repo_url,
            &output_dir.join(pb_dir_name(&repo_url)),
            auth_flow,
            &mut |_d, _f| Ok(ProgressResponse::Continue),
            ui,
            client,
        )
        .await
        .context("fetching product metadata")?;
    }
    Ok(())
}

/// Update metadata from given url.
pub async fn update_metadata_from<I>(
    product_url: &url::Url,
    output_dir: &Path,
    auth_flow: &AuthFlowChoice,
    ui: &I,
    client: &Client,
) -> Result<()>
where
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("update_metadata_from");
    fetch_product_metadata(
        &product_url,
        output_dir,
        &auth_flow,
        &mut |_d, _f| Ok(ProgressResponse::Continue),
        ui,
        client,
    )
    .await
    .context("fetching product metadata")
}

/// Gather a list of PBM reference URLs which include the product bundle entry
/// name.
///
/// Tip: Call `update_metadata()` to update the info (or not, if the intent is
///      to peek at what's there without updating).
pub async fn product_bundle_urls(sdk: &ffx_config::Sdk) -> Result<Vec<url::Url>> {
    tracing::debug!("product_bundle_urls");
    let mut result = Vec::new();

    // Collect product bundle URLs from the file paths in ffx config.
    for repo in pbm_repo_list(&sdk).await.context("getting repo list")? {
        if let Some(path) = path_from_file_url(&repo) {
            let names = pb_names_from_path(&path).context("loading product bundle names")?;
            for name in names {
                let mut product_url = repo.to_owned();
                product_url.set_fragment(Some(&name));
                result.push(product_url);
            }
        }
    }

    let storage_path: PathBuf =
        ffx_config::get(CONFIG_STORAGE_PATH).await.context("getting CONFIG_STORAGE_PATH")?;
    if !storage_path.is_dir() {
        // Early out before calling read_dir.
        return Ok(result);
    }

    // Collect product bundle URLs from the downloaded information. These
    // entries may not be currently referenced in the ffx config. This is where
    // product bundles from old versions will be pulled in, for example.
    let mut dir_entries = async_fs::read_dir(storage_path).await.context("reading vendors dir")?;
    while let Some(dir_entry) = dir_entries.try_next().await.context("reading directory")? {
        if dir_entry.path().is_dir() {
            if let Ok(repo_info) = RepoInfo::load(&dir_entry.path().join("info")) {
                let names = pb_names_from_path(&dir_entry.path().join("product_bundles.json"))?;
                for name in names {
                    let repo = format!("{}#{}", repo_info.metadata_url, name);
                    result.push(
                        url::Url::parse(&repo)
                            .with_context(|| format!("parsing metadata URL {:?}", repo))?,
                    );
                }
            }
        }
    }
    Ok(result)
}

/// Gather all the fms entries from a given product_url.
///
/// If `product_url` is None or not a URL, then an attempt will be made to find
/// default entries.
pub async fn fms_entries_from(product_url: &url::Url, sdk_root: &Path) -> Result<Entries> {
    tracing::debug!("fms_entries_from");
    let path = get_metadata_glob(product_url, sdk_root).await.context("getting metadata")?;
    let mut entries = Entries::new();
    entries.add_from_path(&path).context("adding entries")?;
    Ok(entries)
}

/// The default behavior for when there are more than one matching
/// product-bundle.
///
/// This function should only be called with an iterator of `url::Url`s that has
/// 2 or more entries, and has already been sorted & reversed.
fn default_pbm_of_many<I>(
    mut urls: I,
    sdk_version: &sdk::SdkVersion,
    looking_for: Option<String>,
    should_print: bool,
) -> Result<url::Url>
where
    I: Iterator<Item = url::Url> + Clone,
{
    let extra_message = if let Some(looking_for) = looking_for {
        format!(" for `{}`", looking_for)
    } else {
        String::new()
    };
    let formatted =
        urls.clone().map(|url| format!("`{}`", url)).collect::<Vec<String>>().join("\n");
    if should_print {
        println!(
            "Multiple product bundles found{extra_message}. To choose a specific product, pass \
            in a full URL from the following:\n{formatted}",
            extra_message = extra_message,
            formatted = formatted
        );
    }
    tracing::info!("Product bundles: {}", formatted);
    // If the user is working in-tree, we want to default to any available locally-built bundles.
    let mut selected: Option<(url::Url, &str)> = if sdk_version == &sdk::SdkVersion::InTree {
        tracing::info!("In-tree selection");
        let mut local_builds = urls.clone().filter(|url| is_locally_built(&url));
        match local_builds.next() {
            Some(first) => Some((first, "the locally-built product bundle")),
            None => None,
        }
    } else {
        None
    };
    // If there is no locally-built bundle, then we want to match the current SDK version.
    if selected.is_none() {
        selected = match sdk_version {
            sdk::SdkVersion::Version(version) => {
                tracing::info!("Sdk version: `{}`", version);
                let mut matching_sdk_version = urls.filter(|url| url.to_string().contains(version));
                let first = matching_sdk_version.next();
                match first {
                    Some(first) => {
                        Some((first, "the first valid product bundle for this SDK version"))
                    }
                    None => {
                        bail!("There were no product-bundles for your sdk version: `{}`", version)
                    }
                }
            }
            sdk::SdkVersion::InTree | sdk::SdkVersion::Unknown => match urls.next() {
                Some(first) => Some((first, "the first valid product bundle in sorted order")),
                None => {
                    bail!(
                        "This function should only be called with an iterator with at least 2 entries."
                    )
                }
            },
        }
    };
    // If we get here, selected is Some(_,_); if we didn't match, we threw an error.
    let (url, description) = selected.unwrap();
    if should_print {
        println!("Defaulting to {}: `{}`", description, url);
    }
    Ok(url)
}

/// Determine if a product bundle url refers to a locally-built bundle.
///
/// Note that this is a heuristic for PBv1 only. It assumes that only a locally-built bundle
/// will be have a source URL with a "file" scheme. The implementation will likely change with
/// PBv2.
pub fn is_locally_built(product_url: &url::Url) -> bool {
    product_url.scheme() == "file"
}

/// Find a product bundle url and name for `product_url`.
///
/// If product_url is
/// - None and there is only one product bundle available, use it.
/// - Some(product name) and a url with that fragment is found, use it.
/// - Some(full product url with fragment) use it.
/// If a match is not found or multiple matches are found, fail with an error
/// message.
///
/// Tip: Call `update_metadata()` to get up to date choices (or not, if the
///      intent is to select from what's already there).
pub async fn select_product_bundle(
    sdk: &ffx_config::Sdk,
    looking_for: &Option<String>,
    mode: ListingMode,
    should_print: bool,
) -> Result<url::Url> {
    tracing::debug!("select_product_bundle");
    let sdk_version = sdk.get_version();
    let sdk_root = sdk.get_path_prefix();
    let mut urls = product_bundle_urls(sdk).await.context("getting product bundle URLs")?;
    // Sort the URLs lexigraphically, then reverse them so the most recent
    // version strings will be first.
    urls.sort();
    urls.reverse();
    let mut iter = urls.into_iter();
    if mode == ListingMode::ReadyBundlesOnly || mode == ListingMode::RemovableBundles {
        // Unfortunately this can't be a filter() because is_pb_ready is async.
        let mut ready = Vec::new();
        for url in iter {
            if is_pb_ready(&url, sdk_root).await? {
                ready.push(url);
            }
        }
        iter = ready.into_iter();
    }
    // The locally built bundle can't be removed or downloaded, so skip it for those cases.
    let iter = iter.filter(|url| {
        mode == ListingMode::AllBundles
            || mode == ListingMode::ReadyBundlesOnly
            || !is_locally_built(&url)
    });
    if let Some(looking_for) = &looking_for {
        let matches = iter.filter(|url| {
            return url.as_str() == looking_for
                || url.fragment().expect("product_urls must have fragment") == looking_for;
        });
        match matches.at_most_one() {
            Ok(Some(m)) => Ok(m),
            Ok(None) => bail!(
                "A product bundle with that name was not found, please check the spelling and try again."
            ),
            Err(matches) => default_pbm_of_many(matches, sdk_version, Some(looking_for.to_string()), should_print),
        }
    } else {
        match iter.at_most_one() {
            Ok(Some(url)) => Ok(url),
            Ok(None) => bail!("There are no product bundles available."),
            Err(urls) => default_pbm_of_many(urls, sdk_version, None, should_print),
        }
    }
}

/// Determine whether the data for `product_url` is downloaded and ready to be
/// used.
pub async fn is_pb_ready(product_url: &url::Url, sdk_root: &Path) -> Result<bool> {
    assert!(product_url.as_str().contains("#"));
    Ok(get_images_dir(product_url, sdk_root).await.context("getting images dir")?.is_dir())
}

/// Download data related to the product.
///
/// The emulator may then be run with the data downloaded.
///
/// If `product_bundle_url` is None and only one viable PBM is available, that entry
/// is used.
///
/// `writer` is used to output user messages.
pub async fn get_product_data<I>(
    product_url: &url::Url,
    output_dir: &std::path::Path,
    auth_flow: &AuthFlowChoice,
    ui: &I,
    client: &Client,
) -> Result<bool>
where
    I: structured_ui::Interface + Sync,
{
    tracing::debug!("get_product_data {:?} to {:?}", product_url, output_dir);
    let mut should_get_data = true;
    let mut note = TableRows::builder();
    if is_locally_built(&product_url) {
        note.title("There's no data download necessary for local products.");
        should_get_data = false;
    } else if product_url.scheme() != GS_SCHEME {
        note.title("Only GCS downloads are supported at this time.");
        should_get_data = false;
    }
    if should_get_data {
        get_product_data_from_gcs(product_url, output_dir, auth_flow, ui, client)
            .await
            .context("reading pbms entries")
            .map(|_| true)
    } else {
        ui.present(&Presentation::Table(note)).expect("Problem presenting the note.");
        Ok(false)
    }
}

/// Determine the path to the product images data.
pub async fn get_images_dir(product_url: &url::Url, sdk_root: &Path) -> Result<PathBuf> {
    assert!(!product_url.as_str().is_empty());
    let name = product_url.fragment().expect("a URI fragment is required");
    assert!(!name.is_empty());
    assert!(!name.contains("/"));
    local_path_helper(product_url, &format!("{}/images", name), /*dir=*/ true, sdk_root).await
}

/// Determine the path to the product packages data.
pub async fn get_packages_dir(product_url: &url::Url, sdk_root: &Path) -> Result<PathBuf> {
    assert!(!product_url.as_str().is_empty());
    let name = product_url.fragment().expect("a URI fragment is required");
    assert!(!name.is_empty());
    assert!(!name.contains("/"));
    local_path_helper(product_url, &format!("{}/packages", name), /*dir=*/ true, sdk_root).await
}

/// Determine the path to the local product metadata directory.
pub async fn get_metadata_dir(product_url: &url::Url, sdk_root: &Path) -> Result<PathBuf> {
    assert!(!product_url.as_str().is_empty());
    assert!(!product_url.fragment().is_none());
    Ok(get_metadata_glob(product_url, sdk_root)
        .await
        .context("getting metadata")?
        .parent()
        .expect("Metadata files should have a parent")
        .to_path_buf())
}

/// Determine the glob path to the product metadata.
///
/// A glob path may have wildcards, such as "file://foo/*.json".
pub async fn get_metadata_glob(product_url: &url::Url, sdk_root: &Path) -> Result<PathBuf> {
    assert!(!product_url.as_str().is_empty());
    assert!(!product_url.fragment().is_none());
    local_path_helper(product_url, "product_bundles.json", /*dir=*/ false, sdk_root).await
}

/// Remove prior output directory, if necessary.
pub async fn make_way_for_output(local_dir: &Path, force: bool) -> Result<()> {
    tracing::debug!("make_way_for_output {:?}, force {}", local_dir, force);
    if local_dir.exists() {
        tracing::debug!("local_dir.exists {:?}", local_dir);
        if std::fs::read_dir(&local_dir).expect("reading dir").next().is_none() {
            tracing::debug!("local_dir is empty (which is good) {:?}", local_dir);
            return Ok(());
        } else if force {
            if local_dir == Path::new("") || local_dir == Path::new("/") {
                ffx_bail!(
                    "The output directory is {:?} which looks like a mistake. \
                    Please try a different output directory path.",
                    local_dir
                );
            }
            if !local_dir.join("product_bundle.json").exists() {
                ffx_bail!(
                    "The directory does not resemble an old product \
                    bundle. For caution's sake, please remove the output \
                    directory {:?} by hand and try again.",
                    local_dir
                );
            }
            async_fs::remove_dir_all(&local_dir)
                .await
                .with_context(|| format!("removing output dir {:?}", local_dir))?;
            tracing::debug!("Removed all of {:?}", local_dir);
        } else {
            ffx_bail!(
                "The output directory already exists. Please provide \
                another directory to write to, or use --force to overwrite the \
                contents of {:?}.",
                local_dir
            );
        }
    }
    assert!(
        !local_dir.exists(),
        "The local_dir exists in make_way_for_output, please report this as a bug."
    );
    async_fs::create_dir_all(&local_dir)
        .await
        .with_context(|| format!("creating directory {:?}", local_dir))?;
    tracing::debug!("local_dir dir ready.");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pbms::CONFIG_METADATA;
    use ffx_config::ConfigLevel;
    use serde_json;
    use std::{fs::File, io::Write};
    use tempfile::TempDir;

    const CORE_JSON: &str = include_str!("../test_data/test_core.json");
    const IMAGES_JSON: &str = include_str!("../test_data/test_images.json");
    const PRODUCT_BUNDLE_JSON: &str = include_str!("../test_data/test_product_bundle.json");

    fn create_test_intree_sdk() -> TempDir {
        let temp_dir = TempDir::new().expect("temp dir");
        let temp_path = temp_dir.path();

        let manifest_path = temp_dir.path().join("sdk/manifest");
        std::fs::create_dir_all(&manifest_path).expect("create dir");
        let mut core_file = File::create(manifest_path.join("core")).expect("create core");
        core_file.write_all(CORE_JSON.as_bytes()).expect("write core file");

        let mut images_file =
            File::create(temp_path.join("images.json")).expect("create images file");
        images_file.write_all(IMAGES_JSON.as_bytes()).expect("write images file");

        let mut pbm_file =
            File::create(temp_path.join("product_bundle.json")).expect("create images file");
        pbm_file.write_all(PRODUCT_BUNDLE_JSON.as_bytes()).expect("write pbm file");

        temp_dir
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_pbms() {
        let env = ffx_config::test_init().await.expect("create test config");

        let sdk_root_dir = create_test_intree_sdk();
        let sdk_root = sdk_root_dir.path().to_str().expect("path to str");
        ffx_config::query("sdk.root")
            .level(Some(ConfigLevel::User))
            .set(sdk_root.into())
            .await
            .expect("set sdk root path");
        ffx_config::query("sdk.type")
            .level(Some(ConfigLevel::User))
            .set("in-tree".into())
            .await
            .expect("set sdk type");
        ffx_config::query(CONFIG_METADATA)
            .level(Some(ConfigLevel::User))
            .set(serde_json::json!(["{sdk.root}/*.json"]))
            .await
            .expect("set pbms metadata");

        let output_dir = TempDir::new().expect("output directory");
        let mut input = Box::new(std::io::stdin());
        let mut output = Vec::new();
        let mut err_out = Vec::new();
        let ui = structured_ui::TextUi::new(&mut input, &mut output, &mut err_out);
        let client_factory = ::gcs::client::ClientFactory::new().expect("creating client factory");
        let client = client_factory.create_client();

        let sdk = env.context.get_sdk().await.expect("Loading configured sdk");
        update_metadata_all(&sdk, output_dir.path(), &AuthFlowChoice::Default, &ui, &client)
            .await
            .expect("updating metadata");
        let urls = product_bundle_urls(&sdk).await.expect("get pb urls");
        assert!(!urls.is_empty());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_make_way_for_output() {
        let test_dir = tempfile::TempDir::new().expect("temp dir");

        make_way_for_output(&test_dir.path(), /*force=*/ false).await.expect("empty dir is okay");

        std::fs::create_dir(&test_dir.path().join("foo")).expect("make_dir foo");
        std::fs::File::create(test_dir.path().join("info")).expect("create info");
        std::fs::File::create(test_dir.path().join("product_bundle.json"))
            .expect("create product_bundle.json");
        make_way_for_output(&test_dir.path(), /*force=*/ true).await.expect("rm dir is okay");

        let test_dir = tempfile::TempDir::new().expect("temp dir");
        std::fs::create_dir(&test_dir.path().join("foo")).expect("make_dir foo");
        assert!(make_way_for_output(&test_dir.path(), /*force=*/ false).await.is_err());
    }
}
