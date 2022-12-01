// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    fxfs::cryptkeeper::CryptKeeper,
    fxfs::log_and_map_err::LogThen,
    state::{State, StateName},
    StorageManager as StorageManagerTrait,
};
use account_common::AccountManagerError;
use async_trait::async_trait;
use fidl::endpoints::{create_proxy, ServerEnd};
use fidl_fuchsia_fs::AdminMarker;
use fidl_fuchsia_fxfs::{MountOptions, VolumeMarker, VolumesMarker};
use fidl_fuchsia_identity_account as faccount;
use fidl_fuchsia_identity_account::Error as AccountApiError;
use fidl_fuchsia_io as fio;
use fuchsia_component::client::{
    connect_to_named_protocol_at_dir_root, connect_to_protocol_at_dir_root,
    connect_to_protocol_at_dir_svc,
};
use futures::lock::Mutex;
use std::sync::Arc;
use tracing::{error, warn};
use typed_builder::TypedBuilder;

mod cryptkeeper;
mod log_and_map_err;

fn new_directory_proxy_pair(
) -> Result<(fio::DirectoryProxy, ServerEnd<fio::DirectoryMarker>), faccount::Error> {
    create_proxy::<fio::DirectoryMarker>()
        .log_error_then("Creating Directory proxy failed", faccount::Error::Resource)
}

struct FxfsInner {
    // Responsible for tearing down the crypt component when dropped.
    cryptkeeper: CryptKeeper,
    // The root directory of the volume.
    root_dir: fio::DirectoryProxy,
    // The outgoing directory for the volume.
    outgoing_dir: fio::DirectoryProxy,
}

impl FxfsInner {
    fn new(
        cryptkeeper: CryptKeeper,
        outgoing_dir: fio::DirectoryProxy,
    ) -> Result<Self, AccountManagerError> {
        let (root_dir, server_end) = new_directory_proxy_pair()?;
        outgoing_dir
            .open(
                fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
                0,
                "root",
                server_end.into_channel().into(),
            )
            .log_warn_then("Failed to open root dir", faccount::Error::Resource)?;

        Ok(Self { cryptkeeper, root_dir, outgoing_dir })
    }
}

// Arguments for instantiating StorageManager.
#[derive(TypedBuilder)]
pub struct Args {
    // The volume label, i.e. the name of the mounted volume:
    // /volumes/<some_label>.
    volume_label: String,

    // The directory of the filesystem.
    filesystem_dir: fio::DirectoryProxy,
}

// An FXFS-backed StorageManager implementation.
pub struct Fxfs {
    // Holds cryptkeeper and directories, but only when |state| is State::Available{..}.
    state: Arc<Mutex<State<FxfsInner>>>,

    // The volume label, i.e. the name of the mounted volume:
    // /volumes/<some_label>.
    volume_label: String,

    // The directory of the filesystem.
    filesystem_dir: fio::DirectoryProxy,
}

impl Fxfs {
    // Creates a new FXFS-backed StorageManager implementation from some
    // instantiating arguments; see |Args| above.
    pub fn new(args: Args) -> Self {
        Self {
            state: Arc::new(Mutex::new(State::Uninitialized)),
            volume_label: args.volume_label,
            filesystem_dir: args.filesystem_dir,
        }
    }
}

#[async_trait]
impl StorageManagerTrait for Fxfs {
    type Key = [u8; 32];

    async fn provision(&self, key: &Self::Key) -> Result<(), AccountManagerError> {
        let mut state_lock = self.state.lock().await;

        if !matches!(&*state_lock, State::Uninitialized) {
            error!(
                "Could not provision storage. Expected state to be Uninitialized, but it \
                was {:?}.",
                StateName::from(&*state_lock)
            );
            return Err(AccountManagerError::new(AccountApiError::Internal));
        }

        let cryptkeeper = CryptKeeper::new_from_key(key).await?;

        let (outgoing_dir, server_end) = new_directory_proxy_pair()?;

        let () = connect_to_protocol_at_dir_root::<VolumesMarker>(&self.filesystem_dir)
            .log_error_then("Connect to Volumes protocol failed", faccount::Error::Resource)?
            .create(&self.volume_label, Some(cryptkeeper.crypt_client_end()?), server_end)
            .await
            .log_warn_then("create FIDL failed", faccount::Error::Resource)?
            .log_warn_then("create failed", faccount::Error::Resource)?;

        state_lock.try_provision(FxfsInner::new(cryptkeeper, outgoing_dir)?).log_info_then(
            "Failed to set FxfsStorageManager state as provisioned",
            faccount::Error::Internal,
        )?;

        Ok(())
    }

    async fn unlock_storage(&self, key: &Self::Key) -> Result<(), AccountManagerError> {
        let mut state_lock = self.state.lock().await;

        if !matches!(&*state_lock, State::Uninitialized | State::Locked) {
            error!(
                "Could not unlock account. Expected state to be Locked, but it was {:?}.",
                StateName::from(&*state_lock)
            );
            return Err(AccountManagerError::new(AccountApiError::Internal));
        }

        let cryptkeeper = CryptKeeper::new_from_key(key).await?;

        let (exposed_dir, server_end) = new_directory_proxy_pair()?;

        let () = connect_to_named_protocol_at_dir_root::<VolumeMarker>(
            /*directory= */ &self.filesystem_dir,
            /*filename= */ &format!("volumes/{}", self.volume_label),
        )
        .log_error_then("Connect to Volume protocol failed", faccount::Error::Resource)?
        .mount(server_end, &mut MountOptions { crypt: Some(cryptkeeper.crypt_client_end()?) })
        .await
        .log_warn_then("mount FIDL failed", faccount::Error::Resource)?
        .log_warn_then("mount failed", faccount::Error::Resource)?;

        state_lock.try_unlock(FxfsInner::new(cryptkeeper, exposed_dir)?).log_info_then(
            "Failed to set FxfsStorageManager state as unlocked",
            faccount::Error::Internal,
        )?;

        Ok(())
    }

    async fn lock_storage(&self) -> Result<(), AccountManagerError> {
        let mut state_lock = self.state.lock().await;

        if !matches!(&*state_lock, State::Available { .. }) {
            error!(
                "Could not lock storage. Expected state to be Available, but it was {:?}.",
                StateName::from(&*state_lock)
            );
            return Err(AccountManagerError::new(AccountApiError::Internal));
        }

        let fxfs_inner: FxfsInner = state_lock.try_lock().log_info_then(
            "Failed to set FxfsStorageManager state as locked",
            faccount::Error::Internal,
        )?;

        match connect_to_protocol_at_dir_svc::<AdminMarker>(&fxfs_inner.outgoing_dir) {
            Ok(proxy) => {
                if let Err(e) = proxy.shutdown().await {
                    error!("shutdown failed: {:?}", e);
                    return Err(AccountManagerError::new(AccountApiError::Internal));
                }
            }
            Err(e) => {
                error!("Connect to Admin protocol failed: {:?}", e);
                return Err(AccountManagerError::new(AccountApiError::Internal));
            }
        }

        let () = fxfs_inner
            .cryptkeeper
            .destroy()
            .await
            .log_warn_then("Forgetting keys on lock failed", faccount::Error::Resource)?;

        Ok(())
    }

    async fn destroy(&self) -> Result<(), AccountManagerError> {
        let mut state_lock = self.state.lock().await;

        if matches!(&*state_lock, State::Uninitialized) {
            error!(
                "Could not destroy storage. Expected state to not already be Uninitialized, \
                but it was."
            );
            return Err(AccountManagerError::new(AccountApiError::Internal));
        }

        let fxfs_inner: FxfsInner = match state_lock.try_destroy().log_info_then(
            "Failed to destroy FxfsStorageManager state",
            faccount::Error::Internal,
        )? {
            Some(fxfs_inner) => fxfs_inner,
            None => {
                warn!(
                    "Did not need to not destroy FxfsStorageManager. Has it already been \
                    destroyed?"
                );
                return Ok(());
            }
        };

        // First shut down the FXFS filesystem which contains this volume by
        // calling Admin.shutdown on the filesystem directory (as opposed to the
        // outgoing directory for the volume itself, as we do in
        // ::lock_storage() above).
        let mut failed = false;
        match connect_to_protocol_at_dir_root::<AdminMarker>(&self.filesystem_dir) {
            Ok(proxy) => {
                if let Err(e) = proxy.shutdown().await {
                    error!("shutdown failed: {:?}", e);
                    failed = true;
                }
            }
            Err(e) => {
                error!("Connect to Admin protocol failed: {:?}", e);
                failed = true;
            }
        }

        // Then destroy the crypt component permanently, forgetting the keys.
        if let Err(e) = fxfs_inner.cryptkeeper.destroy().await {
            error!("Destroy crypt component failed: {:?}", e);
            failed = true;
        }

        if failed {
            error!("[FxfsStorageManager::destroy()] failed to lock.");
            return Err(AccountManagerError::new(AccountApiError::Resource));
        }

        Ok(())
    }

    async fn get_root_dir(&self) -> Result<fio::DirectoryProxy, AccountManagerError> {
        let state_lock = self.state.lock().await;
        if let Some(FxfsInner { root_dir, .. }) = state_lock.get_internals() {
            Ok(fuchsia_fs::clone_directory(root_dir, fio::OpenFlags::CLONE_SAME_RIGHTS)
                .log_warn_then("failed to clone root directory", faccount::Error::Resource)?)
        } else {
            Err(AccountManagerError::new(AccountApiError::Internal))
        }
    }
}
