// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::base::{SettingInfo, SettingType};
use crate::call;
use crate::factory_reset::types::FactoryResetInfo;
use crate::handler::base::Request;
use crate::handler::setting_handler::controller::Handle;
use crate::handler::setting_handler::persist::{controller, ClientProxy};
use crate::handler::setting_handler::{
    ControllerError, ControllerStateResult, SettingHandlerResult, State,
};
use crate::service_context::ExternalServiceProxy;
use async_trait::async_trait;
use fidl_fuchsia_recovery_policy::{DeviceMarker, DeviceProxy};
use futures::lock::Mutex;
use settings_storage::device_storage::{DeviceStorage, DeviceStorageCompatible};
use settings_storage::storage_factory::StorageAccess;
use std::sync::Arc;

impl DeviceStorageCompatible for FactoryResetInfo {
    const KEY: &'static str = "factory_reset_info";

    fn default_value() -> Self {
        FactoryResetInfo::new(true)
    }
}

impl From<FactoryResetInfo> for SettingInfo {
    fn from(info: FactoryResetInfo) -> SettingInfo {
        SettingInfo::FactoryReset(info)
    }
}

type FactoryResetHandle = Arc<Mutex<FactoryResetManager>>;

/// Handles the mapping between [`Request`]s/[`State`] changes and the
/// [`FactoryResetManager`] logic. Wraps an Arc Mutex of the manager so that each field
/// doesn't need to be individually locked within the manager.
///
/// [`Request`]: crate::handler::base::Request
/// [`State`]: crate::handler::setting_handler::State
pub struct FactoryResetController {
    handle: FactoryResetHandle,
}

impl StorageAccess for FactoryResetController {
    type Storage = DeviceStorage;
    const STORAGE_KEYS: &'static [&'static str] = &[FactoryResetInfo::KEY];
}

/// Keeps track of the current state of factory reset, is responsible for persisting that state to
/// disk and notifying the fuchsia.recovery.policy.Device fidl interface of any changes.
pub struct FactoryResetManager {
    client: ClientProxy,
    is_local_reset_allowed: bool,
    factory_reset_policy_service: ExternalServiceProxy<DeviceProxy>,
}

impl FactoryResetManager {
    async fn from_client(client: ClientProxy) -> Result<FactoryResetHandle, ControllerError> {
        client
            .get_service_context()
            .connect::<DeviceMarker>()
            .await
            .map(|factory_reset_policy_service| {
                Arc::new(Mutex::new(Self {
                    client,
                    is_local_reset_allowed: true,
                    factory_reset_policy_service,
                }))
            })
            .map_err(|_| {
                ControllerError::InitFailure("could not connect to factory reset service".into())
            })
    }

    async fn restore(&mut self) -> SettingHandlerResult {
        self.restore_reset_state(true).await.map(|_| None)
    }

    async fn restore_reset_state(&mut self, send_event: bool) -> ControllerStateResult {
        let info = self.client.read_setting::<FactoryResetInfo>(fuchsia_trace::Id::new()).await;
        self.is_local_reset_allowed = info.is_local_reset_allowed;
        if send_event {
            call!(self.factory_reset_policy_service =>
                set_is_local_reset_allowed(info.is_local_reset_allowed)
            )
            .map_err(|e| {
                ControllerError::ExternalFailure(
                    SettingType::FactoryReset,
                    "factory_reset_policy".into(),
                    "restore_reset_state".into(),
                    format!("{e:?}").into(),
                )
            })?;
        }

        Ok(())
    }

    #[allow(clippy::result_large_err)] // TODO(fxbug.dev/117896)
    fn get(&self) -> SettingHandlerResult {
        Ok(Some(FactoryResetInfo::new(self.is_local_reset_allowed).into()))
    }

    async fn set_local_reset_allowed(
        &mut self,
        is_local_reset_allowed: bool,
    ) -> SettingHandlerResult {
        let id = fuchsia_trace::Id::new();
        let mut info = self.client.read_setting::<FactoryResetInfo>(id).await;
        self.is_local_reset_allowed = is_local_reset_allowed;
        info.is_local_reset_allowed = is_local_reset_allowed;
        call!(self.factory_reset_policy_service =>
            set_is_local_reset_allowed(info.is_local_reset_allowed)
        )
        .map_err(|e| {
            ControllerError::ExternalFailure(
                SettingType::FactoryReset,
                "factory_reset_policy".into(),
                "set_local_reset_allowed".into(),
                format!("{e:?}").into(),
            )
        })?;
        self.client.write_setting(info.into(), id).await.map(|_| None)
    }
}

#[async_trait]
impl controller::Create for FactoryResetController {
    async fn create(client: ClientProxy) -> Result<Self, ControllerError> {
        Ok(Self { handle: FactoryResetManager::from_client(client).await? })
    }
}

#[async_trait]
impl Handle for FactoryResetController {
    async fn handle(&self, request: Request) -> Option<SettingHandlerResult> {
        match request {
            Request::Restore => Some(self.handle.lock().await.restore().await),
            Request::Get => Some(self.handle.lock().await.get()),
            Request::SetLocalResetAllowed(is_local_reset_allowed) => {
                Some(self.handle.lock().await.set_local_reset_allowed(is_local_reset_allowed).await)
            }
            _ => None,
        }
    }

    async fn change_state(&mut self, state: State) -> Option<ControllerStateResult> {
        match state {
            State::Startup => {
                // Restore the factory reset state locally but do not push to
                // the factory reset policy.
                Some(self.handle.lock().await.restore_reset_state(false).await)
            }
            _ => None,
        }
    }
}
