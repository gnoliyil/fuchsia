// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{ConfigMap, Environment, EnvironmentContext};
use anyhow::{Context, Result};
use std::{cell::Cell, collections::HashMap, sync::Arc};
use tempfile::{NamedTempFile, TempDir};
use tracing::level_filters::LevelFilter;

use super::ExecutableKind;

/// A structure that holds information about the test config environment for the duration
/// of a test. This object must continue to exist for the duration of the test, or the test
/// may fail.
#[must_use = "This object must be held for the duration of a test (ie. `let _env = ffx_config::test_init()`) for it to operate correctly."]
pub struct TestEnv {
    pub env_file: NamedTempFile,
    pub context: EnvironmentContext,
    pub isolate_root: TempDir,
    pub user_file: NamedTempFile,
    pub global_file: NamedTempFile,
    pub log_subscriber: Arc<dyn tracing::Subscriber + Send + Sync>,
    _guard: async_lock::MutexGuardArc<()>,
}

impl TestEnv {
    async fn new(_guard: async_lock::MutexGuardArc<()>) -> Result<Self> {
        let env_file = NamedTempFile::new().context("tmp access failed")?;
        let user_file = NamedTempFile::new().context("tmp access failed")?;
        let global_file = NamedTempFile::new().context("tmp access failed")?;
        let isolate_root = tempfile::tempdir()?;

        // Point the configs at temporary files.
        let user_file_path = user_file.path().to_owned();
        let global_file_path = global_file.path().to_owned();
        let context = EnvironmentContext::isolated(
            ExecutableKind::Test,
            isolate_root.path().to_owned(),
            HashMap::from_iter(std::env::vars()),
            ConfigMap::default(),
            Some(env_file.path().to_owned()),
            None,
        )?;
        let log_subscriber: Arc<dyn tracing::Subscriber + Send + Sync> = Arc::new(
            crate::logging::configure_subscribers(
                &context,
                Some(crate::logging::StdioOptions { test_writer: true }),
                false,
                LevelFilter::DEBUG,
            )
            .await,
        );

        // Dropping the subscriber guard causes test flakes as the tracing library panics when
        // closing an instrumentation span on a different subscriber.
        // To mitigate this, only drop the guards at thread exit.
        // See https://github.com/tokio-rs/tracing/issues/1656 for more details.
        let log_guard = tracing::subscriber::set_default(Arc::clone(&log_subscriber));

        thread_local! {
            static GUARD_STASH: Cell<Option<tracing::subscriber::DefaultGuard>> =
                const { Cell::new(None) };
        }

        GUARD_STASH.with(move |guard| guard.set(Some(log_guard)));

        let test_env = TestEnv {
            env_file,
            context,
            user_file,
            global_file,
            isolate_root,
            log_subscriber,
            _guard,
        };

        let mut env = Environment::new_empty(test_env.context.clone());
        env.set_user(Some(&user_file_path));
        env.set_global(Some(&global_file_path));
        env.save().await.context("saving env file")?;

        Ok(test_env)
    }

    pub async fn load(&self) -> Environment {
        self.context.load().await.expect("opening test env file")
    }
}

impl Drop for TestEnv {
    fn drop(&mut self) {
        // after the test, wipe out all the test configuration we set up. Explode if things aren't as we
        // expect them.
        let mut env = crate::ENV.lock().expect("Poisoned lock");
        let env_prev = env.clone();
        *env = None;
        drop(env);

        if let Some(env_prev) = env_prev {
            assert_eq!(
                env_prev,
                self.context,
                "environment context changed from isolated environment to {other:?} during test run somehow.",
                other = env_prev
            );
        }

        // since we're not running in async context during drop, we can't clear the cache unfortunately.
    }
}

lazy_static::lazy_static! {
    static ref TEST_LOCK: Arc<async_lock::Mutex<()>> = Arc::default();
}
/// When running tests we usually just want to initialize a blank slate configuration, so
/// use this for tests. You must hold the returned object object for the duration of the test, not doing so
/// will result in strange behaviour.
pub async fn test_init() -> Result<TestEnv> {
    let env = TestEnv::new(TEST_LOCK.lock_arc().await).await?;

    // force an overwrite of the configuration setup
    crate::init(&env.context).await?;

    Ok(env)
}
