// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(fxbug.dev/91407) Remove allow once used.
#![cfg_attr(not(test), allow(dead_code))]

//! The `migration` module exposes structs and traits that can be used to write data migrations for
//! the settings service.
//!
//! # Example:
//! ```
//! let mut builder = MigrationManagerBuilder::new();
//! builder.register(1649199438, Box::new(|file_proxy_generator| async move {
//!     let file_proxy = (file_proxy_generator)("setting_name").await?;
//! }))?;
//! builder.set_dir_proxy(dir_proxy);
//! let migration_manager = builder.build();
//! migration_manager.run_migrations().await?;
//! ```

use anyhow::{anyhow, bail, Context, Error};
use async_trait::async_trait;
use fidl_fuchsia_io::{DirectoryProxy, FileProxy, UnlinkOptions};
use fidl_fuchsia_metrics::MetricEventLoggerProxy;
use fuchsia_fs::directory::{readdir, DirEntry, DirentKind};
use fuchsia_fs::file::WriteError;
use fuchsia_fs::node::{OpenError, RenameError};
use fuchsia_fs::OpenFlags;
use fuchsia_syslog::{fx_log_err, fx_log_warn};
use fuchsia_zircon as zx;
use setui_metrics_registry::{
    SetuiMetricDimensionError as ErrorMetric, SetuiMetricDimensionMigrationId as MigrationIdMetric,
    ACTIVE_MIGRATIONS_METRIC_ID, MIGRATION_ERRORS_METRIC_ID,
};
use std::collections::{BTreeMap, HashSet};

/// Errors that can occur during an individual migration.
#[derive(thiserror::Error, Debug)]
pub(crate) enum MigrationError {
    #[error("The data to be migrated from does not exist")]
    NoData,
    #[error("The disk is full so the migration cannot be performed")]
    DiskFull,
    #[error("An unrecoverable error occurred")]
    Unrecoverable(#[source] Error),
}

impl From<Error> for MigrationError {
    fn from(e: Error) -> Self {
        MigrationError::Unrecoverable(e)
    }
}

/// The migration index is a combination of the migration id first, followed by the cobalt id
/// associated with that same migration id.
type MigrationIndex = (u64, u32);

pub(crate) struct MigrationManager {
    migrations: BTreeMap<MigrationIndex, Box<dyn Migration>>,
    dir_proxy: DirectoryProxy,
}

pub(crate) struct MigrationManagerBuilder {
    migrations: BTreeMap<MigrationIndex, Box<dyn Migration>>,
    id_set: HashSet<u64>,
    cobalt_id_set: HashSet<u32>,
    dir_proxy: Option<DirectoryProxy>,
}

pub(crate) const MIGRATION_FILE_NAME: &str = "migrations.txt";
const TMP_MIGRATION_FILE_NAME: &str = "tmp_migrations.txt";

impl MigrationManagerBuilder {
    /// Construct a new [MigrationManagerBuilder]
    pub(crate) fn new() -> Self {
        Self {
            migrations: BTreeMap::new(),
            id_set: HashSet::new(),
            cobalt_id_set: HashSet::new(),
            dir_proxy: None,
        }
    }

    /// Register a `migration` with a unique `id`. This will fail if another migration with the same
    /// `id` is already registered.
    pub(crate) fn register(&mut self, migration: impl Migration + 'static) -> Result<(), Error> {
        self.register_internal(Box::new(migration))
    }

    fn register_internal(&mut self, migration: Box<dyn Migration>) -> Result<(), Error> {
        let id = migration.id();
        let cobalt_id = migration.cobalt_id();
        if !self.id_set.insert(id) {
            bail!("migration with id {id} already registered");
        }
        if !self.cobalt_id_set.insert(cobalt_id) {
            bail!("migration with cobalt id {cobalt_id} already registered");
        }

        let _ = self.migrations.insert((id, cobalt_id), migration);
        Ok(())
    }

    /// Set the directory where migration data will be tracked.
    pub(crate) fn set_migration_dir(&mut self, dir_proxy: DirectoryProxy) {
        self.dir_proxy = Some(dir_proxy);
    }

    /// Construct a [MigrationManager] from the registered migrations and directory proxy. This
    /// method will panic if no directory proxy is registered.
    pub(crate) fn build(self) -> MigrationManager {
        MigrationManager {
            migrations: self.migrations,
            dir_proxy: self.dir_proxy.expect("dir proxy must be provided"),
        }
    }
}

impl MigrationManager {
    // TODO(fxbug.dev/91407): Remove allow once used.
    #[allow(dead_code)]
    /// Run all migrations, and then log the results to cobalt. Will only return an error if there
    /// was an error running the migrations, but not if there's an error logging to cobalt.
    pub(crate) async fn run_tracked_migrations(
        self,
        metric_event_logger_proxy: Option<MetricEventLoggerProxy>,
    ) -> Result<Option<u64>, (Option<u64>, MigrationError)> {
        let migration_result = self.run_migrations().await;
        let (last_migration, migration_result) = match migration_result {
            Ok(last_migration) => (last_migration, Ok(last_migration.map(|m| m.migration_id))),
            Err((last_migration, e)) => {
                (last_migration, Err((last_migration.map(|m| m.migration_id), e)))
            }
        };
        if let Some(metric_event_logger_proxy) = metric_event_logger_proxy {
            if let Err(e) = Self::log_migration_metrics(
                last_migration,
                &migration_result,
                metric_event_logger_proxy,
            )
            .await
            {
                fx_log_err!("Failed to log migration metrics: {:?}", e);
            }
        }

        migration_result
    }

    /// Log the results of the migration to cobalt.
    async fn log_migration_metrics(
        last_migration: Option<LastMigration>,
        migration_result: &Result<Option<u64>, (Option<u64>, MigrationError)>,
        metric_event_logger_proxy: MetricEventLoggerProxy,
    ) -> Result<(), Error> {
        match last_migration {
            None => {
                // When there is no migration id, then the storage is still using stash.
                metric_event_logger_proxy
                    .log_occurrence(
                        ACTIVE_MIGRATIONS_METRIC_ID,
                        1,
                        &[MigrationIdMetric::Stash as u32],
                    )
                    .await
                    .context("failed fidl call")?
                    .map_err(|e| anyhow!("failed to log migration id metric: {:?}", e))?;
            }
            Some(LastMigration { cobalt_id, .. }) => {
                metric_event_logger_proxy
                    .log_occurrence(ACTIVE_MIGRATIONS_METRIC_ID, 1, &[cobalt_id])
                    .await
                    .context("failed fidl call")?
                    .map_err(|e| anyhow!("failed to log migration id metric: {:?}", e))?;
            }
        }

        if let Err((_, e)) = &migration_result {
            let event_code = match e {
                MigrationError::NoData => ErrorMetric::NoData,
                MigrationError::DiskFull => ErrorMetric::DiskFull,
                MigrationError::Unrecoverable(_) => ErrorMetric::Unrecoverable,
            } as u32;
            metric_event_logger_proxy
                .log_occurrence(MIGRATION_ERRORS_METRIC_ID, 1, &[event_code])
                .await
                .context("failed fidl call")?
                .map_err(|e| anyhow!("failed to log migration error metric: {:?}", e))?;
        }
        Ok(())
    }

    /// Run all registered migrations. On success will return final migration number if there are
    /// any registered migrations. On error it will return the most recent migration number that was
    /// able to complete, along with the migration error.
    async fn run_migrations(
        mut self,
    ) -> Result<Option<LastMigration>, (Option<LastMigration>, MigrationError)> {
        let last_migration = {
            let migration_file = fuchsia_fs::directory::open_file(
                &self.dir_proxy,
                MIGRATION_FILE_NAME,
                OpenFlags::NOT_DIRECTORY | OpenFlags::RIGHT_READABLE | OpenFlags::RIGHT_WRITABLE,
            )
            .await;
            match migration_file {
                Err(e) => {
                    if let OpenError::OpenError(zx::Status::NOT_FOUND) = e {
                        if let Some(last_migration) = self
                            .initialize_migrations()
                            .await
                            .context("failed to initialize migrations")
                            .map_err(|e| (None, e.into()))?
                        {
                            last_migration
                        } else {
                            // There are no migrations to run.
                            return Ok(None);
                        }
                    } else {
                        return Err((
                            None,
                            Error::from(e).context("failed to load migrations").into(),
                        ));
                    }
                }
                Ok(migration_file) => {
                    let migration_data = fuchsia_fs::read_file(&migration_file)
                        .await
                        .context("failed to read migrations.txt")
                        .map_err(|e| (None, e.into()))?;
                    let migration_id = migration_data
                        .parse()
                        .context("Failed to parse migration id from migrations.txt")
                        .map_err(|e| (None, e.into()))?;
                    if let Some(&(_, cobalt_id)) =
                        self.migrations.keys().find(|(id, _)| *id == migration_id)
                    {
                        LastMigration { migration_id, cobalt_id }
                    } else {
                        fx_log_warn!("Unknown migration {migration_id}, reverting to default data");

                        // We don't know which migrations to run. Some future build may have
                        // run migrations, then something caused a boot loop, and the system
                        // reverted back to this build. This build doesn't know about the
                        // future migrations, so it doesn't know what state the data is in.
                        // Report an error but use the last known migration this build is aware of.
                        // TODO(fxbug.dev/116822) Remove light migration fallback once proper
                        // fallback handling is in place. This is safe for now since Light settings
                        // are always overwritten. The follow-up must be compatible with this
                        // fallback.
                        let last_migration = LastMigration {
                            migration_id: 1653667210,
                            cobalt_id: MigrationIdMetric::V1653667210 as u32,
                        };
                        Self::write_migration_file(&self.dir_proxy, last_migration.migration_id)
                            .await
                            .map_err(|e| (Some(last_migration), e))?;

                        return Err((
                            Some(last_migration),
                            MigrationError::Unrecoverable(anyhow!(format!(
                                "Unknown migration {migration_id}. Using migration with id {}",
                                last_migration.migration_id
                            ))),
                        ));
                    }
                }
            }
        };

        // Track the last migration so we know whether to update the migration file.
        let mut new_last_migration = last_migration;

        // Skip over migrations already previously completed.
        for ((id, cobalt_id), migration) in self
            .migrations
            .into_iter()
            .skip_while(|&((id, _), _)| id != last_migration.migration_id)
            .skip(1)
        {
            Self::run_single_migration(
                &self.dir_proxy,
                new_last_migration.migration_id,
                &*migration,
            )
            .await
            .map_err(|e| {
                let e = match e {
                    MigrationError::NoData => MigrationError::NoData,
                    MigrationError::Unrecoverable(e) => {
                        MigrationError::Unrecoverable(e.context(format!("Migration {id} failed")))
                    }
                    _ => e,
                };
                (Some(new_last_migration), e)
            })?;
            new_last_migration = LastMigration { migration_id: id, cobalt_id };
        }

        // Remove old files that don't match the current pattern.
        let file_suffix = format!("_{}.pfidl", new_last_migration.migration_id);
        let entries: Vec<DirEntry> = readdir(&self.dir_proxy)
            .await
            .context("another error")
            .map_err(|e| (Some(new_last_migration), e.into()))?;
        let files = entries.into_iter().filter_map(|entry| {
            if let DirentKind::File = entry.kind {
                if entry.name == MIGRATION_FILE_NAME || entry.name.ends_with(&file_suffix) {
                    None
                } else {
                    Some(entry.name)
                }
            } else {
                None
            }
        });

        for file in files {
            self.dir_proxy
                .unlink(&file, UnlinkOptions::EMPTY)
                .await
                .context("failed to remove old file from file system")
                .map_err(|e| (Some(new_last_migration), e.into()))?
                .map_err(zx::Status::from_raw)
                .context("another error")
                .map_err(|e| (Some(new_last_migration), e.into()))?;
        }

        Ok(Some(new_last_migration))
    }

    async fn write_migration_file(
        dir_proxy: &DirectoryProxy,
        migration_id: u64,
    ) -> Result<(), MigrationError> {
        // Scope is important. tmp_migration_file needs to be out of scope when the file is
        // renamed.
        {
            let tmp_migration_file = fuchsia_fs::directory::open_file(
                dir_proxy,
                TMP_MIGRATION_FILE_NAME,
                OpenFlags::NOT_DIRECTORY
                    | OpenFlags::CREATE
                    | OpenFlags::RIGHT_READABLE
                    | OpenFlags::RIGHT_WRITABLE,
            )
            .await
            .context("unable to create migrations file")?;
            if let Err(e) =
                fuchsia_fs::file::write(&tmp_migration_file, migration_id.to_string()).await
            {
                return Err(match e {
                    WriteError::WriteError(zx::Status::NO_SPACE) => MigrationError::DiskFull,
                    _ => Error::from(e).context("failed to write tmp migration").into(),
                });
            };
            if let Err(e) = tmp_migration_file
                .close()
                .await
                .map_err(Error::from)
                .context("failed to close")?
                .map_err(zx::Status::from_raw)
            {
                return Err(match e {
                    zx::Status::NO_SPACE => MigrationError::DiskFull,
                    _ => anyhow!("{e:?}").context("failed to properly close migration file").into(),
                });
            }
        }

        if let Err(e) =
            fuchsia_fs::directory::rename(dir_proxy, TMP_MIGRATION_FILE_NAME, MIGRATION_FILE_NAME)
                .await
        {
            return Err(match e {
                RenameError::RenameError(zx::Status::NO_SPACE) => MigrationError::DiskFull,
                _ => Error::from(e).context("failed to rename tmp to migrations.txt").into(),
            });
        };

        if let Err(e) = dir_proxy
            .sync()
            .await
            .map_err(Error::from)
            .context("failed to sync dir")?
            .map_err(zx::Status::from_raw)
        {
            match e {
                // This is only returned when the directory is backed by a VFS, so this is fine to
                // ignore.
                zx::Status::NOT_SUPPORTED => {}
                zx::Status::NO_SPACE => return Err(MigrationError::DiskFull),
                _ => {
                    return Err(anyhow!("{e:?}")
                        .context("failed to sync directory for migration id")
                        .into())
                }
            }
        }

        Ok(())
    }

    async fn run_single_migration(
        dir_proxy: &DirectoryProxy,
        old_id: u64,
        migration: &dyn Migration,
    ) -> Result<(), MigrationError> {
        let new_id = migration.id();
        let file_generator = FileGenerator::new(old_id, new_id, Clone::clone(dir_proxy));
        migration.migrate(file_generator).await?;
        Self::write_migration_file(dir_proxy, new_id).await
    }

    /// Runs the initial migration. If it fails due to a [MigrationError::NoData] error, then it
    /// will return the last migration to initialize all of the data sources.
    async fn initialize_migrations(&mut self) -> Result<Option<LastMigration>, MigrationError> {
        // Try to run the first migration. If it fails because there's no data in stash then we can
        // use default values and skip to the final migration number. If it fails because the disk
        // is full, propagate the error up so the main client can decide how to gracefully fallback.
        let &(id, cobalt_id) = if let Some(key) = self.migrations.keys().next() {
            key
        } else {
            return Ok(None);
        };
        let migration = self.migrations.get(&(id, cobalt_id)).unwrap();
        if let Err(migration_error) =
            Self::run_single_migration(&self.dir_proxy, 0, &**migration).await
        {
            match migration_error {
                MigrationError::NoData => {
                    // There was no previous data. We just need to use the default value for all
                    // data and use the most recent migration as the migration number.
                    let migration_id = self
                        .migrations
                        .keys()
                        .last()
                        .copied()
                        .map(|(id, cobalt_id)| LastMigration { migration_id: id, cobalt_id });
                    if let Some(last_migration) = migration_id.as_ref() {
                        Self::write_migration_file(&self.dir_proxy, last_migration.migration_id)
                            .await?;
                    }
                    return Ok(migration_id);
                }
                MigrationError::DiskFull => return Err(MigrationError::DiskFull),
                MigrationError::Unrecoverable(e) => {
                    return Err(MigrationError::Unrecoverable(
                        e.context("Failed to run initial migration"),
                    ))
                }
            }
        }

        Ok(Some(LastMigration { migration_id: id, cobalt_id }))
    }
}

pub(crate) struct FileGenerator {
    // TODO(fxbug.dev/91407) Remove allow once used.
    #[allow(dead_code)]
    old_id: u64,
    new_id: u64,
    dir_proxy: DirectoryProxy,
}

impl FileGenerator {
    pub(crate) fn new(old_id: u64, new_id: u64, dir_proxy: DirectoryProxy) -> Self {
        Self { old_id, new_id, dir_proxy }
    }

    // TODO(fxbug.dev/91407) Remove allow once used.
    #[allow(dead_code)]
    pub(crate) async fn old_file(
        &self,
        file_name: impl AsRef<str>,
    ) -> Result<FileProxy, OpenError> {
        let file_name = file_name.as_ref();
        let id = self.new_id;
        fuchsia_fs::directory::open_file(
            &self.dir_proxy,
            &format!("{file_name}_{id}.pfidl"),
            OpenFlags::NOT_DIRECTORY | OpenFlags::RIGHT_READABLE,
        )
        .await
    }

    pub(crate) async fn new_file(
        &self,
        file_name: impl AsRef<str>,
    ) -> Result<FileProxy, OpenError> {
        let file_name = file_name.as_ref();
        let id = self.new_id;
        fuchsia_fs::directory::open_file(
            &self.dir_proxy,
            &format!("{file_name}_{id}.pfidl"),
            OpenFlags::NOT_DIRECTORY
                | OpenFlags::CREATE
                | OpenFlags::RIGHT_READABLE
                | OpenFlags::RIGHT_WRITABLE,
        )
        .await
    }
}

#[async_trait]
pub(crate) trait Migration {
    fn id(&self) -> u64;
    fn cobalt_id(&self) -> u32;
    async fn migrate(&self, file_generator: FileGenerator) -> Result<(), MigrationError>;
}

#[derive(Copy, Clone, Debug)]
pub(crate) struct LastMigration {
    migration_id: u64,
    cobalt_id: u32,
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use fasync::Task;
    use fidl::endpoints::{create_proxy, create_proxy_and_stream, ServerEnd};
    use fidl::Vmo;
    use fidl_fuchsia_io::{DirectoryMarker, DirectoryProxy};
    use fidl_fuchsia_metrics::{
        MetricEventLoggerMarker, MetricEventLoggerRequest, MetricEventLoggerRequestStream,
    };
    use fuchsia_async as fasync;
    use fuchsia_fs::OpenFlags;
    use futures::future::BoxFuture;
    use futures::lock::Mutex;
    use futures::{FutureExt, StreamExt};
    use std::collections::HashMap;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use test_case::test_case;
    use vfs::directory::entry::DirectoryEntry;
    use vfs::directory::mutable::simple::tree_constructor;
    use vfs::execution_scope::ExecutionScope;
    use vfs::file::vmo::asynchronous::{read_write, simple_init_vmo_with_capacity};
    use vfs::mut_pseudo_directory;

    #[async_trait]
    impl<T> Migration for (u64, u32, T)
    where
        T: Fn(FileGenerator) -> BoxFuture<'static, Result<(), MigrationError>> + Send + Sync,
    {
        fn id(&self) -> u64 {
            self.0
        }

        fn cobalt_id(&self) -> u32 {
            self.1
        }

        async fn migrate(&self, file_generator: FileGenerator) -> Result<(), MigrationError> {
            (self.2)(file_generator).await
        }
    }

    const ID_LEN: u64 = 15;
    const ID: u64 = 20_220_130_120_000;
    const COBALT_ID: u32 = 99;
    const ID2: u64 = 20_220_523_120_000;
    const COBALT_ID2: u32 = 100;
    const DATA_FILE_NAME: &str = "test_20220130120000.pfidl";

    #[test]
    fn cannot_register_same_id_twice() {
        let mut builder = MigrationManagerBuilder::new();
        builder
            .register((ID, COBALT_ID, Box::new(|_| async move { Ok(()) }.boxed())))
            .expect("should register once");
        let result = builder
            .register((ID, COBALT_ID, Box::new(|_| async move { Ok(()) }.boxed())))
            .map_err(|e| format!("{e:}"));
        assert!(result.is_err());
        assert_eq!(result.unwrap_err(), "migration with id 20220130120000 already registered");
    }

    #[test]
    #[should_panic(expected = "dir proxy must be provided")]
    fn cannot_build_migration_manager_without_dir_proxy() {
        let builder = MigrationManagerBuilder::new();
        let _ = builder.build();
    }

    // Needs to be async to create the proxy and stream.
    #[fasync::run_until_stalled(test)]
    async fn can_build_migration_manager_without_migrations() {
        let (proxy, _) = fidl::endpoints::create_proxy_and_stream::<DirectoryMarker>().unwrap();
        let mut builder = MigrationManagerBuilder::new();
        builder.set_migration_dir(proxy);
        let _migration_manager = builder.build();
    }

    fn serve_vfs_dir(
        root: Arc<impl DirectoryEntry>,
    ) -> (DirectoryProxy, Arc<Mutex<HashMap<String, Vmo>>>) {
        let vmo_map = Arc::new(Mutex::new(HashMap::new()));
        let fs_scope = ExecutionScope::build()
            .entry_constructor(tree_constructor(move |_, _| {
                Ok(read_write(simple_init_vmo_with_capacity(b"", 100)))
            }))
            .new();
        let (client, server) = create_proxy::<DirectoryMarker>().unwrap();
        root.open(
            fs_scope,
            OpenFlags::RIGHT_READABLE | OpenFlags::RIGHT_WRITABLE,
            0,
            vfs::path::Path::dot(),
            ServerEnd::new(server.into_channel()),
        );
        (client, vmo_map)
    }

    // Test for initial migration
    #[fasync::run_until_stalled(test)]
    async fn if_no_migration_file_runs_initial_migration() {
        let fs = mut_pseudo_directory! {};
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        let migration_ran = Arc::new(AtomicBool::new(false));

        builder
            .register((
                ID,
                COBALT_ID,
                Box::new({
                    let migration_ran = Arc::clone(&migration_ran);
                    move |_| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(Clone::clone(&directory));
        let migration_manager = builder.build();

        let result = migration_manager.run_migrations().await;
        assert_matches!(
            result,
            Ok(Some(LastMigration { migration_id: id, cobalt_id }))
                if id == ID && cobalt_id == COBALT_ID
        );
        let migration_file = fuchsia_fs::directory::open_file(
            &directory,
            MIGRATION_FILE_NAME,
            OpenFlags::RIGHT_READABLE,
        )
        .await
        .expect("migration file should exist");
        let migration_number =
            fuchsia_fs::read_file(&migration_file).await.expect("should be able to read file");
        let migration_number: u64 = dbg!(migration_number).parse().expect("should be a number");
        assert_eq!(migration_number, ID);
    }

    // Test for initial migration
    #[fasync::run_until_stalled(test)]
    async fn if_no_migration_file_and_no_data_uses_defaults() {
        let fs = mut_pseudo_directory! {};
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        let migration_ran = Arc::new(AtomicBool::new(false));

        builder
            .register((
                ID,
                COBALT_ID,
                Box::new({
                    let migration_ran = Arc::clone(&migration_ran);
                    move |_| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            Err(MigrationError::NoData)
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(Clone::clone(&directory));
        let migration_manager = builder.build();

        let result = migration_manager.run_migrations().await;
        assert_matches!(
            result,
            Ok(Some(LastMigration { migration_id: id, cobalt_id }))
                if id == ID && cobalt_id == COBALT_ID
        );
        let migration_file = fuchsia_fs::directory::open_file(
            &directory,
            MIGRATION_FILE_NAME,
            OpenFlags::RIGHT_READABLE,
        )
        .await
        .expect("migration file should exist");
        let migration_number =
            fuchsia_fs::read_file(&migration_file).await.expect("should be able to read file");
        let migration_number: u64 = dbg!(migration_number).parse().expect("should be a number");
        assert_eq!(migration_number, ID);
    }

    #[fasync::run_until_stalled(test)]
    async fn if_no_migration_file_and_no_migrations_no_update() {
        let fs = mut_pseudo_directory! {};
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        builder.set_migration_dir(Clone::clone(&directory));
        let migration_manager = builder.build();

        let result = migration_manager.run_migrations().await;
        assert_matches!(result, Ok(None));
        let open_result = fuchsia_fs::directory::open_file(
            &directory,
            MIGRATION_FILE_NAME,
            OpenFlags::RIGHT_READABLE,
        )
        .await;
        assert_matches!(
            open_result,
            Err(OpenError::OpenError(status)) if status == zx::Status::NOT_FOUND
        );
    }

    #[fasync::run_until_stalled(test)]
    async fn if_no_migration_file_but_data_runs_migrations() {
        let fs = mut_pseudo_directory! {};
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        let migration_1_ran = Arc::new(AtomicBool::new(false));
        let migration_2_ran = Arc::new(AtomicBool::new(false));

        builder
            .register((
                ID,
                COBALT_ID,
                Box::new({
                    let migration_ran = Arc::clone(&migration_1_ran);
                    move |_| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");

        builder
            .register((
                ID2,
                COBALT_ID2,
                Box::new({
                    let migration_ran = Arc::clone(&migration_2_ran);
                    move |_| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(Clone::clone(&directory));
        let migration_manager = builder.build();

        let result = migration_manager.run_migrations().await;
        assert_matches!(
            result,
            Ok(Some(LastMigration { migration_id: id, cobalt_id }))
                if id == ID2 && cobalt_id == COBALT_ID2
        );
        assert!(migration_1_ran.load(Ordering::SeqCst));
        assert!(migration_2_ran.load(Ordering::SeqCst));
    }

    #[fasync::run_until_stalled(test)]
    async fn if_migration_file_and_up_to_date_no_migrations_run() {
        let fs = mut_pseudo_directory! {
            MIGRATION_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(ID.to_string().as_bytes(), ID_LEN)
            ),
            DATA_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(b"", 1)
            ),
        };
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        let migration_ran = Arc::new(AtomicBool::new(false));

        builder
            .register((
                ID,
                COBALT_ID,
                Box::new({
                    let migration_ran = Arc::clone(&migration_ran);
                    move |_| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(directory);
        let migration_manager = builder.build();

        let result = migration_manager.run_migrations().await;
        assert_matches!(
            result,
            Ok(Some(LastMigration { migration_id: id, cobalt_id }))
                if id == ID && cobalt_id == COBALT_ID
        );
        assert!(!migration_ran.load(Ordering::SeqCst));
    }

    #[fasync::run_until_stalled(test)]
    async fn migration_file_exists_but_missing_data() {
        let fs = mut_pseudo_directory! {
            MIGRATION_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(ID.to_string().as_bytes(), ID_LEN)
            ),
        };
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        let initial_migration_ran = Arc::new(AtomicBool::new(false));

        builder
            .register((
                ID,
                COBALT_ID,
                Box::new({
                    let initial_migration_ran = Arc::clone(&initial_migration_ran);
                    move |_| {
                        let initial_migration_ran = Arc::clone(&initial_migration_ran);
                        async move {
                            initial_migration_ran.store(true, Ordering::SeqCst);
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");

        let second_migration_ran = Arc::new(AtomicBool::new(false));
        builder
            .register((
                ID2,
                COBALT_ID2,
                Box::new({
                    let second_migration_ran = Arc::clone(&second_migration_ran);
                    move |_| {
                        let second_migration_ran = Arc::clone(&second_migration_ran);
                        async move {
                            second_migration_ran.store(true, Ordering::SeqCst);
                            Err(MigrationError::NoData)
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(directory);
        let migration_manager = builder.build();

        let result = migration_manager.run_migrations().await;
        assert_matches!(
            result,
            Err((Some(LastMigration { migration_id: id, cobalt_id }), MigrationError::NoData))
                if id == ID && cobalt_id == COBALT_ID
        );
        assert!(!initial_migration_ran.load(Ordering::SeqCst));
        assert!(second_migration_ran.load(Ordering::SeqCst));
    }

    // Tests that a migration newer than the latest one tracked in the migration file can run.
    #[fasync::run_until_stalled(test)]
    async fn migration_file_exists_and_newer_migrations_should_update() {
        let fs = mut_pseudo_directory! {
            MIGRATION_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(ID.to_string().as_bytes(), ID_LEN)
            ),
            DATA_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(b"", 1)
            ),
        };
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        let migration_ran = Arc::new(AtomicBool::new(false));

        builder
            .register((
                ID,
                COBALT_ID,
                Box::new({
                    let migration_ran = Arc::clone(&migration_ran);
                    move |_| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");

        const NEW_CONTENTS: &[u8] = b"test2";
        builder
            .register((
                ID2,
                COBALT_ID2,
                Box::new({
                    let migration_ran = Arc::clone(&migration_ran);
                    move |file_generator: FileGenerator| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            let file = file_generator.new_file("test").await.expect("can get file");
                            fuchsia_fs::file::write(&file, NEW_CONTENTS)
                                .await
                                .expect("can wite file");
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(Clone::clone(&directory));
        let migration_manager = builder.build();

        let result = migration_manager.run_migrations().await;
        assert_matches!(
            result,
            Ok(Some(LastMigration { migration_id: id, cobalt_id }))
                if id == ID2 && cobalt_id == COBALT_ID2
        );

        let migration_file = fuchsia_fs::directory::open_file(
            &directory,
            MIGRATION_FILE_NAME,
            OpenFlags::RIGHT_READABLE,
        )
        .await
        .expect("migration file should exist");
        let migration_number: u64 = fuchsia_fs::read_file(&migration_file)
            .await
            .expect("should be able to read file")
            .parse()
            .expect("should be a number");
        assert_eq!(migration_number, ID2);

        let data_file = fuchsia_fs::directory::open_file(
            &directory,
            &format!("test_{ID2}.pfidl"),
            OpenFlags::RIGHT_READABLE,
        )
        .await
        .expect("migration file should exist");
        let data = fuchsia_fs::file::read(&data_file).await.expect("should be able to read file");
        assert_eq!(data, NEW_CONTENTS);
    }

    #[fasync::run_until_stalled(test)]
    async fn migration_file_unknown_id_uses_light_migration() {
        const LIGHT_MIGRATION: u64 = 1653667210;
        const UNKNOWN_ID: u64 = u64::MAX;
        let unknown_id_str = UNKNOWN_ID.to_string();
        let fs = mut_pseudo_directory! {
            MIGRATION_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(unknown_id_str.as_bytes(), unknown_id_str.len() as u64)
            ),
            DATA_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(b"", 1)
            ),
        };
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        let migration_ran = Arc::new(AtomicBool::new(false));

        builder
            .register((
                ID,
                COBALT_ID,
                Box::new({
                    let migration_ran = Arc::clone(&migration_ran);
                    move |_| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(Clone::clone(&directory));
        let migration_manager = builder.build();

        let result = migration_manager.run_migrations().await;
        assert_matches!(result, Err((Some(LastMigration {
                    migration_id, cobalt_id
                }), MigrationError::Unrecoverable(_)))
            if migration_id == LIGHT_MIGRATION && cobalt_id == MigrationIdMetric::V1653667210 as u32);

        let migration_file = fuchsia_fs::directory::open_file(
            &directory,
            MIGRATION_FILE_NAME,
            OpenFlags::RIGHT_READABLE,
        )
        .await
        .expect("migration file should exist");
        let migration_number: u64 = fuchsia_fs::read_file(&migration_file)
            .await
            .expect("should be able to read file")
            .parse()
            .expect("should be a number");
        assert_eq!(migration_number, LIGHT_MIGRATION);
    }

    /// Setup the event logger, It will track whether the expected metric id and event get
    /// triggered.
    fn setup_event_logger(
        mut stream: MetricEventLoggerRequestStream,
        triggered: Arc<AtomicBool>,
        expected_metric_ids: Vec<u32>,
        expected_event_codes: Vec<u32>,
    ) -> Task<()> {
        fasync::Task::spawn(async move {
            let mut index = 0;
            while let Some(Ok(request)) = stream.next().await {
                if let MetricEventLoggerRequest::LogOccurrence {
                    metric_id,
                    count,
                    responder,
                    event_codes,
                } = request
                {
                    triggered.store(true, Ordering::SeqCst);
                    assert_eq!(metric_id, expected_metric_ids[index]);
                    assert_eq!(count, 1);
                    assert_eq!(event_codes[0], expected_event_codes[index]);
                    let _ = responder.send(&mut Ok(()));
                    index += 1;
                }
            }
        })
    }

    #[fasync::run_until_stalled(test)]
    async fn logs_migration_on_success() {
        let (proxy, stream) = create_proxy_and_stream::<MetricEventLoggerMarker>().unwrap();

        let triggered = Arc::new(AtomicBool::new(false));
        let join_handle = setup_event_logger(
            stream,
            Arc::clone(&triggered),
            vec![ACTIVE_MIGRATIONS_METRIC_ID],
            vec![COBALT_ID],
        );

        let fs = mut_pseudo_directory! {
            MIGRATION_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(ID.to_string().as_bytes(), ID_LEN)
            ),
            DATA_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(b"", 1)
            ),
        };
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        let migration_ran = Arc::new(AtomicBool::new(false));

        builder
            .register((
                ID,
                COBALT_ID,
                Box::new({
                    let migration_ran = Arc::clone(&migration_ran);
                    move |_| {
                        let migration_ran = Arc::clone(&migration_ran);
                        async move {
                            migration_ran.store(true, Ordering::SeqCst);
                            Ok(())
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(directory);
        let migration_manager = builder.build();

        let result = migration_manager.run_tracked_migrations(Some(proxy)).await;
        assert_matches!(result, Ok(Some(ID)));

        join_handle.await;
        assert!(triggered.load(Ordering::SeqCst));
    }

    #[fasync::run_until_stalled(test)]
    async fn logs_stash_on_no_migrations() {
        let (proxy, stream) = create_proxy_and_stream::<MetricEventLoggerMarker>().unwrap();

        let triggered = Arc::new(AtomicBool::new(false));
        let join_handle = setup_event_logger(
            stream,
            Arc::clone(&triggered),
            vec![ACTIVE_MIGRATIONS_METRIC_ID],
            vec![MigrationIdMetric::Stash as u32],
        );

        let fs = mut_pseudo_directory! {};
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();
        builder.set_migration_dir(Clone::clone(&directory));
        let migration_manager = builder.build();

        let result = migration_manager.run_tracked_migrations(Some(proxy)).await;
        assert_matches!(result, Ok(None));

        join_handle.await;
        assert!(triggered.load(Ordering::SeqCst));
    }

    #[allow(clippy::unused_unit)]
    #[test_case(MigrationError::NoData, MigrationError::NoData, ErrorMetric::NoData)]
    #[test_case(MigrationError::DiskFull, MigrationError::DiskFull, ErrorMetric::DiskFull)]
    #[test_case(
        MigrationError::Unrecoverable(anyhow!("abc")),
        MigrationError::Unrecoverable(anyhow!("abc")),
        ErrorMetric::Unrecoverable
    )]
    #[fasync::run_until_stalled(test)]
    async fn logs_error_on_migration_error(
        generated_error: MigrationError,
        expected_error: MigrationError,
        expected_event_code: ErrorMetric,
    ) {
        let generated_error = Arc::new(Mutex::new(Some(generated_error)));
        let (proxy, stream) = create_proxy_and_stream::<MetricEventLoggerMarker>().unwrap();

        let triggered = Arc::new(AtomicBool::new(false));
        let join_handle = setup_event_logger(
            stream,
            Arc::clone(&triggered),
            vec![ACTIVE_MIGRATIONS_METRIC_ID, MIGRATION_ERRORS_METRIC_ID],
            vec![COBALT_ID, expected_event_code as u32],
        );

        let fs = mut_pseudo_directory! {
            MIGRATION_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(ID.to_string().as_bytes(), ID_LEN)
            ),
            DATA_FILE_NAME => read_write(
                simple_init_vmo_with_capacity(b"", 1)
            ),
        };
        let (directory, _vmo_map) = serve_vfs_dir(fs);
        let mut builder = MigrationManagerBuilder::new();

        builder
            .register((ID, COBALT_ID, Box::new(move |_| async move { Ok(()) }.boxed())))
            .expect("can register");

        builder
            .register((
                ID2,
                COBALT_ID2,
                Box::new({
                    let generated_error = Arc::clone(&generated_error);
                    move |_| {
                        let generated_error = Arc::clone(&generated_error);
                        async move {
                            let generated_error = generated_error.lock().await.take().unwrap();
                            Err(generated_error)
                        }
                        .boxed()
                    }
                }),
            ))
            .expect("can register");
        builder.set_migration_dir(directory);
        let migration_manager = builder.build();

        let result = migration_manager.run_tracked_migrations(Some(proxy)).await;
        assert!(matches!(
            (result, expected_error),
            (Err((Some(id), MigrationError::NoData)), MigrationError::NoData)
                | (Err((Some(id), MigrationError::DiskFull)), MigrationError::DiskFull)
                | (
                    Err((Some(id), MigrationError::Unrecoverable(_))),
                    MigrationError::Unrecoverable(_)
                ) if id == ID
        ));

        join_handle.await;
        assert!(triggered.load(Ordering::SeqCst));
    }

    // TODO(fxbug.dev/91407) Test for disk full behavior
    // TODO(fxbug.dev/91407) Tests for power outage simulation (drop future after various write
    // stages)
    // TODO(fxbug.dev/91407) Test that migration file is accurate even after a partial failure, e.g.
    // simulate disk full after certain number of migrations)
}
