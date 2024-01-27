// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A tool to:
//! - acquire and display product bundle information (metadata)
//! - acquire related data files, such as disk partition images (data)

use ::gcs::client::Client;
use anyhow::{bail, Context, Result};
use async_trait::async_trait;
use errors::ffx_bail;
use ffx_config::ConfigLevel;
use ffx_product_bundle_args::{
    CreateCommand, GetCommand, ListCommand, ProductBundleCommand, RemoveCommand, SubCommand,
};
use fho::{FfxMain, FfxTool, SimpleWriter};
use fidl;
use fidl_fuchsia_developer_ffx::{RepositoryIteratorMarker, RepositoryRegistryProxy};
use fidl_fuchsia_developer_ffx_ext::{RepositoryConfig, RepositoryError, RepositorySpec};
use fuchsia_url::RepositoryUrl;
use pbms::{
    is_locally_built, is_pb_ready, pbv1_get, product_bundle_urls, select_auth,
    select_product_bundle, update_metadata_all, ListingMode,
};
use std::{
    collections::BTreeSet,
    convert::TryInto,
    fs::{read_dir, remove_dir_all},
    io::{stderr, stdin, stdout},
    path::{Component, Path},
};
use structured_ui::{self, Presentation, Response, SimplePresentation, TableRows, TextUi};
use url::Url;

mod create;

const CONFIG_METADATA: &str = "pbms.metadata";

#[derive(FfxTool)]
pub struct ProductBundleTool {
    #[command]
    cmd: ProductBundleCommand,
    #[with(fho::daemon_protocol())]
    repos: RepositoryRegistryProxy,
}

fho::embedded_plugin!(ProductBundleTool);

/// Provide functionality to list product-bundle metadata, fetch metadata, and
/// pull images and related data.
#[async_trait(?Send)]
impl FfxMain for ProductBundleTool {
    type Writer = SimpleWriter;

    // TODO(fxbug.dev/127955) use the writer when possible.
    async fn main(self, _writer: Self::Writer) -> fho::Result<()> {
        let mut input = stdin();
        let mut output = stdout();
        let mut err_out = stderr();
        let mut ui = TextUi::new(&mut input, &mut output, &mut err_out);
        product_bundle_plugin_impl(self.cmd, &mut ui, self.repos).await?;
        Ok(())
    }
}

/// Dispatch to a sub-command.
pub async fn product_bundle_plugin_impl<I>(
    command: ProductBundleCommand,
    ui: &mut I,
    repos: RepositoryRegistryProxy,
) -> Result<()>
where
    I: structured_ui::Interface + Sync,
{
    match &command.sub {
        SubCommand::List(cmd) => pb_list(ui, &cmd).await,
        SubCommand::Get(cmd) => pb_get(ui, &cmd, &repos).await,
        SubCommand::Create(cmd) => pb_create(&cmd).await,
        SubCommand::Remove(cmd) => pb_remove(ui, &cmd, &repos).await,
    }
}

async fn check_for_custom_metadata<I>(ui: &mut I, title: &str)
where
    I: structured_ui::Interface + Sync,
{
    let mut note = TableRows::builder();
    note.title(title);
    match ffx_config::query(CONFIG_METADATA)
        .level(Some(ConfigLevel::User))
        .get::<Vec<String>>()
        .await
    {
        Ok(v) => {
            if !v.is_empty() {
                note.note(
                    "\nIt looks like you have a custom search path in your FFX configuration, \n\
                    which may prevent the tool from finding product bundles. Try:\n\n    \
                        ffx config get pbms.metadata \n\n\
                    to review your current configuration.\n",
                );
            }
        }
        // If the config doesn't return an array, we assume there's no custom config set and bail.
        Err(_) => (),
    }
    ui.present(&Presentation::Table(note)).expect("Problem presenting the custom metadata note.");
}

/// `ffx product-bundle remove` sub-command.
async fn pb_remove<I>(
    ui: &mut I,
    cmd: &RemoveCommand,
    repos: &RepositoryRegistryProxy,
) -> Result<()>
where
    I: structured_ui::Interface + Sync,
{
    println!("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
    println!("@");
    println!("@  `ffx product-bundle remove` is deprecated");
    println!("@");
    println!("@  When using `ffx product download` in the future, please use `rm` instead.");
    println!("@");
    println!("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
    tracing::debug!("pb_remove");
    let sdk = ffx_config::global_env_context()
        .context("loading global environment context")?
        .get_sdk()
        .await
        .context("getting sdk env context")?;
    let mut pbs_to_remove = Vec::new();
    if cmd.all {
        let entries = product_bundle_urls(&sdk).await.context("list pbms")?;
        for url in entries {
            if !is_locally_built(&url) && is_pb_ready(&url, sdk.get_path_prefix()).await? {
                pbs_to_remove.push(url);
            }
        }
    } else {
        // We use the default matching functionality.
        let should_print = true;
        match select_product_bundle(
            &sdk,
            &cmd.product_bundle_name,
            ListingMode::RemovableBundles,
            should_print,
        )
        .await
        {
            Ok(url) => {
                if !is_locally_built(&url) && is_pb_ready(&url, sdk.get_path_prefix()).await? {
                    pbs_to_remove.push(url);
                }
            }
            Err(e) => {
                println!("Couldn't determine which bundle to remove: {:?}", e);
                return Ok(());
            }
        }
    }
    if pbs_to_remove.len() > 0 {
        pb_remove_all(&sdk, ui, pbs_to_remove, cmd.force, repos).await
    } else {
        // Nothing to remove.
        check_for_custom_metadata(ui, "There are no product bundles to remove.").await;
        Ok(())
    }
}

async fn get_repos(repos_proxy: &RepositoryRegistryProxy) -> Result<Vec<RepositoryConfig>> {
    let (client, server) = fidl::endpoints::create_endpoints::<RepositoryIteratorMarker>();
    repos_proxy.list_repositories(server).context("listing repositories")?;
    let client = client.into_proxy().context("creating repository iterator proxy")?;

    let mut repos = vec![];
    loop {
        let batch = client.next().await.context("fetching next batch of repositories")?;
        if batch.is_empty() {
            break;
        }

        for repo in batch {
            repos.push(repo.try_into().context("converting repository config")?);
        }
    }

    repos.sort();
    Ok(repos)
}

/// Removes a set of product bundle directories, with user confirmation if "force" is false.
async fn pb_remove_all<I>(
    sdk: &ffx_config::Sdk,
    ui: &mut I,
    pbs_to_remove: Vec<Url>,
    force: bool,
    repos: &RepositoryRegistryProxy,
) -> Result<()>
where
    I: structured_ui::Interface + Sync,
{
    let mut confirmation = true;
    if !force {
        let mut table = TableRows::builder();
        table.title(format!(
            "This will delete the following {} product bundle(s):",
            pbs_to_remove.len()
        ));
        for url in &pbs_to_remove {
            table.row(vec![format!("    {}", url)]);
        }
        ui.present(&Presentation::Table(table))?;

        let mut question = SimplePresentation::builder();
        question.prompt("Are you sure you wish to proceed? (y/n)");
        confirmation = match ui.present(&Presentation::StringPrompt(question))? {
            Response::Choice(str) => {
                let response = str.to_lowercase();
                let response = response.trim_end();
                response == "y" || response == "yes"
            }
            _ => false,
        };
    }
    if confirmation {
        let all_repos = get_repos(&repos).await?;
        for url in pbs_to_remove {
            // Resolve the directory for the target bundle.
            let root_dir = pbms::get_product_dir(&url).await.context("Couldn't get directory")?;
            assert_ne!(root_dir.components().count(), 0, "An empty PBMS path is not allowed.");
            assert_ne!(
                root_dir.components().collect::<Vec<_>>(),
                &[Component::RootDir],
                "Refusing to delete from the root of the filesystem."
            );
            assert!(
                !root_dir.components().into_iter().any(|x| x == Component::ParentDir),
                "Directory traversal is not allowed."
            );
            let name = url.fragment().expect("URL with trailing product_name fragment.");

            // If there is a repository for the bundle...
            let repo_name = name.replace('_', "-");
            if let Ok(repo_path) = pbms::get_packages_dir(&url, sdk.get_path_prefix()).await {
                if all_repos.iter().any(|r| {
                    // The name has to match...
                    r.name == repo_name &&
                    // It has to be a Pm-style repo, since that's what we add in `get`...
                    match &r.spec {
                        // And the local path has to match, to make sure it's the right bundle of
                        // that name...
                        RepositorySpec::Pm { path, .. } => {
                            path.clone().into_std_path_buf() == repo_path
                        }
                        _ => false,
                    }
                }) {
                    // If all those match, we remove it.
                    repos
                        .remove_repository(&repo_name)
                        .await
                        .context("communicating with ffx daemon")?;
                    tracing::info!("Removed repository named '{}'", repo_name);
                }
            }

            // Delete the bundle directory.
            let product_dir = root_dir.join(name);
            println!("Removing product bundle '{}'.", url);
            tracing::debug!("Removing product bundle '{}'.", url);
            remove_dir_all(&product_dir).context("removing product directory")?;

            // If there are no more bundles in this directory, delete it too.
            // Note how we make an exception for directories whose name starts with ".tmp",
            // since any such directory in the pbms storage path would be a failed
            // download in a previous operation that wasn't cleaned up properly.
            if !read_dir(&root_dir).context("reading root dir)")?.any(|d| {
                let d = d.expect("intermittent IO error, please try the command again.");
                d.path().is_dir() && !d.file_name().to_str().unwrap_or("").starts_with(".tmp")
            }) {
                remove_dir_all(&root_dir).context("removing root directory")?;
            }
        }
    } else {
        println!("Cancelling product bundle removal.");
    }
    Ok(())
}

/// `ffx product-bundle list` sub-command.
async fn pb_list<I>(ui: &mut I, cmd: &ListCommand) -> Result<()>
where
    I: structured_ui::Interface + Sync,
{
    println!("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
    println!("@");
    println!("@  `ffx product-bundle list` is deprecated");
    println!("@");
    println!("@  Please use `ffx product lookup` instead.");
    println!("@");
    println!("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
    tracing::debug!("pb_list");
    let sdk = ffx_config::global_env_context()
        .context("loading global environment context")?
        .get_sdk()
        .await
        .context("getting sdk env context")?;
    if !cmd.cached {
        let storage_dir = pbms::get_storage_dir().await?;
        let client = Client::initial()?;
        update_metadata_all(&sdk, &storage_dir, select_auth(cmd.oob_auth, &cmd.auth), ui, &client)
            .await?;
    }
    let mut entries = product_bundle_urls(&sdk).await.context("list pbms")?;
    if entries.is_empty() {
        check_for_custom_metadata(ui, "No product bundles found.").await;
        return Ok(());
    }
    entries.sort();
    entries.reverse();
    let mut table = TableRows::builder();
    for entry in entries {
        let ready = if is_pb_ready(&entry, sdk.get_path_prefix()).await? { "*" } else { " " };
        table.row(vec![format!("{}", ready), format!("{}", entry)]);
    }
    table.note(
        "\
        \n*No need to fetch with `ffx product-bundle get ...`. \
        The '*' is not part of the name.\
        \n",
    );
    ui.present(&Presentation::Table(table.clone()))?;
    Ok(())
}

/// `ffx product-bundle get` sub-command.
///
/// This command is broken up into a few pieces:
///
/// First, we determine the URL of the target product bundle. This can be provided by the user
/// as a fully-qualified URL, or as a fragment (a.k.a. short-name) to match against the current
/// version's available product bundles. If a unique URL cannot be determined based on the user's
/// input or the available list, the command fails.
///
/// We then generate a name for the package repository, and ensure another repository with that
/// name is not already registered. The user can provide a custom name for the package repository,
/// or else the tool will default to the product bundle's short-name. If the selected repository
/// name is unavailable, the command fails unless the user has specified the --force-repo flag; in
/// that case, the existing package repository will be overwritten by a new package repository for
/// the product bundle being downloaded. The previous repository can be restored by running the
/// product-bundle get command again for the previously targeted product bundle.
///
/// The product bundle images and packages are then retrieved. The files are initially downloaded
/// to a temporary directory to ensure atomicity of the download operation, and moved to the final
/// location on success. If the final location already exists, we assume the contents are valid and
/// skip the download step unless the user has specified the --force flag. Skipping the download is
/// not an error, but if the download fails for any other reason, the command fails.
///
/// Once the download step is complete, the final step is to set up the package repository for the
/// downloaded product bundle based on the repository name determined earlier in the command.
async fn pb_get<I>(ui: &mut I, cmd: &GetCommand, repos: &RepositoryRegistryProxy) -> Result<()>
where
    I: structured_ui::Interface + Sync,
{
    println!("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
    println!("@");
    println!("@  `ffx product-bundle get` is deprecated");
    println!("@");
    println!("@  Please use `ffx product lookup`, `ffx product download`, and `ffx repository add` instead.");
    println!("@");
    println!("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
    let start = std::time::Instant::now();
    tracing::debug!("pb_get {:?}", cmd.product_bundle_name);
    let sdk = ffx_config::global_env_context()
        .context("loading global environment context")?
        .get_sdk()
        .await
        .context("getting sdk env context")?;
    let product_url = match determine_pbm_url(&sdk, cmd, ui).await {
        Ok(url) => url,
        Err(e) => {
            let mut note = TableRows::builder();
            note.title(format!("{:?}", e));
            ui.present(&Presentation::Table(note)).expect("Problem presenting the note.");
            // We can't make any progress without a Url.
            return Ok(());
        }
    };

    let repo_name = match get_repository_name(&sdk, cmd, repos, &product_url).await {
        Ok(name) => name,
        Err(e) => {
            let mut note = TableRows::builder();
            note.title(format!("{:?}", e));
            ui.present(&Presentation::Table(note)).expect("Problem presenting the note.");
            // Don't attempt to download or setup-repo if the repo_name conflicts.
            return Ok(());
        }
    };

    if pbv1_get::bundled_exists_locally(&sdk, &product_url).await? {
        if cmd.force {
            if let Err(e) = pb_remove_all(&sdk, ui, vec![product_url.clone()], true, repos).await {
                let mut note = TableRows::builder();
                let message =
                    format!("Unexpected error removing the existing product bundle: {:?}", e);
                note.title(message);
                ui.present(&Presentation::Table(note)).expect("Problem presenting the note.");
                return Ok(());
            }
        } else {
            let mut note = TableRows::builder();
            note.title("This product bundle is already downloaded. Use --force to replace it.");
            ui.present(&Presentation::Table(note)).expect("Problem presenting the note.");
            return Ok(());
        }
    }

    match pbv1_get::download_product_bundle(&sdk, ui, cmd.oob_auth, &cmd.auth, &product_url).await {
        Ok(true) => tracing::debug!("Product Bundle downloaded successfully."),
        Ok(false) => tracing::debug!("Product Bundle download skipped."),
        Err(e) => {
            let mut note = TableRows::builder();
            note.title(format!("{:?}", e));
            ui.present(&Presentation::Table(note)).expect("Problem presenting the note.");
            // Don't proceed to setup-repo if the download failed.
            return Ok(());
        }
    }

    match set_up_package_repository(&repo_name, repos, &product_url, sdk.get_path_prefix()).await {
        Ok(()) => {
            tracing::debug!("Repository '{}' added for '{}'.", &repo_name, product_url);
            let mut note = TableRows::builder();
            note.title(format!("Repository added for {}.", &repo_name));
            ui.present(&Presentation::Table(note)).expect("Problem presenting the note.");
        }
        Err(e) => ffx_bail!("Error adding repository {}: {:?}", &repo_name, e),
    }

    tracing::debug!(
        "Total fx product-bundle get runtime {} seconds.",
        start.elapsed().as_secs_f32()
    );
    Ok(())
}

/// Determines the name used for the package repository for this product bundle, starting with
/// any name provided by the user, then the short-name of the product bundle being retrieved,
/// or finally defaulting to "devhost". Returns an Err() if the selected repository name is an
/// invalid domain name or is already in use by another repository.
async fn get_repository_name(
    sdk: &ffx_config::Sdk,
    cmd: &GetCommand,
    repos: &RepositoryRegistryProxy,
    product_url: &Url,
) -> Result<String> {
    let repo_name = if let Some(repo_name) = &cmd.repository {
        repo_name.clone()
    } else if let Some(product_name) = product_url.fragment() {
        // FIXME(103661): Repository names must be a valid domain name, and cannot contain
        // '_'. We might be able to expand our support for [opaque hosts], which supports
        // arbitrary ASCII codepoints. Until then, replace any '_' with '-'.
        //
        // [opaque hosts]: https://url.spec.whatwg.org/#opaque-host
        let repo_name = product_name.replace('_', "-");

        if repo_name != product_name {
            tracing::info!(
                "Repository names cannot contain '_'. Replacing with '-' in {}",
                product_name
            );
        }

        repo_name
    } else {
        // Otherwise use the standard default.
        "devhost".to_string()
    };

    // Make sure the repository name is valid.
    if let Err(err) = RepositoryUrl::parse_host(repo_name.clone()) {
        bail!("invalid repository name {}: {}", repo_name, err);
    }

    // If a repo with the selected name already exists, that's an error unless
    // the user told us to replace it, or if it's already associated with a
    // bundle that matches the product_url.
    let repo_list = get_repos(repos).await?;
    let repo_path = pbms::get_packages_dir(product_url, sdk.get_path_prefix()).await?;
    if let Some(r) = repo_list.iter().find(|r| r.name == repo_name) {
        let replace_anyway =
            // --force-repo means "Replace this repo if it exists".
            cmd.force_repo ||
            // If it's already associated with the matching bundle.
            match &r.spec {
                RepositorySpec::Pm { path, .. } => {
                    path.clone().into_std_path_buf() == repo_path
                }
                _ => false,
            };
        if !replace_anyway {
            bail!(
                "A package repository already exists with the name '{}'. \
                Specify an alternative name using the --repository flag.",
                repo_name
            );
        }
    }
    Ok(repo_name)
}

/// Sets up a package server repository for the product bundle being downloaded. This is
/// equivalent to calling `ffx repository add-from-pm` with the product bundle's storage path.
/// If no packages are available as part of the product bundle, this is a no-op. Otherwise,
/// this returns the result of the `add-from-pm` call.
async fn set_up_package_repository(
    repo_name: &str,
    repos: &RepositoryRegistryProxy,
    product_url: &Url,
    sdk_root: &Path,
) -> Result<()> {
    // Register a repository with the daemon if we downloaded any packaging artifacts.
    if let Ok(repo_path) = pbms::get_packages_dir(&product_url, sdk_root).await {
        if repo_path.exists() {
            let repo_path = repo_path
                .canonicalize()
                .with_context(|| format!("canonicalizing {:?}", repo_path))?;
            let repo_spec =
                RepositorySpec::Pm { path: repo_path.try_into()?, aliases: BTreeSet::new() };
            repos
                .add_repository(&repo_name, &repo_spec.into())
                .await
                .context("communicating with ffx daemon")?
                .map_err(RepositoryError::from)
                .with_context(|| format!("registering repository {}", repo_name))?;

            tracing::info!("Created repository named '{}'", repo_name);
        }
    }
    Ok(())
}

/// Convert cli args to a full URL pointing to product bundle metadata
async fn determine_pbm_url<I>(
    sdk: &ffx_config::Sdk,
    cmd: &GetCommand,
    ui: &mut I,
) -> Result<url::Url>
where
    I: structured_ui::Interface + Sync,
{
    if !cmd.cached {
        let base_dir = pbms::get_storage_dir().await?;
        let client = Client::initial()?;
        update_metadata_all(sdk, &base_dir, select_auth(cmd.oob_auth, &cmd.auth), ui, &client)
            .await?;
    }
    let should_print = true;
    select_product_bundle(sdk, &cmd.product_bundle_name, ListingMode::GetableBundles, should_print)
        .await
}

/// `ffx product-bundle create` sub-command.
async fn pb_create(cmd: &CreateCommand) -> Result<()> {
    println!("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
    println!("@");
    println!("@  `ffx product-bundle create` is deprecated");
    println!("@");
    println!("@  Please use `ffx product create` instead.");
    println!("@");
    println!("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
    create::create_product_bundle(cmd).await
}

#[cfg(test)]
mod test {}
