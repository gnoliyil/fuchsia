// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::base::{Merge, SettingInfo, SettingType};
use crate::handler::base::Request;
use crate::handler::setting_handler::persist::{controller as data_controller, ClientProxy};
use crate::handler::setting_handler::{
    controller, ControllerError, IntoHandlerResult, SettingHandlerResult,
};
use crate::intl::types::{HourCycle, IntlInfo, LocaleId, TemperatureUnit};
use async_trait::async_trait;
use fuchsia_trace as ftrace;
use rust_icu_uenum as uenum;
use rust_icu_uloc as uloc;
use settings_storage::device_storage::{DeviceStorage, DeviceStorageCompatible};
use settings_storage::storage_factory::StorageAccess;
use std::collections::HashSet;

impl DeviceStorageCompatible for IntlInfo {
    const KEY: &'static str = "intl_info";

    fn default_value() -> Self {
        IntlInfo {
            // `-x-fxdef` is a private use extension and a special marker denoting that the
            // setting is a fallback default, and not actually set through any user action.
            locales: Some(vec![LocaleId { id: "en-US-x-fxdef".to_string() }]),
            temperature_unit: Some(TemperatureUnit::Celsius),
            time_zone_id: Some("UTC".to_string()),
            hour_cycle: Some(HourCycle::H12),
        }
    }
}

impl From<IntlInfo> for SettingInfo {
    fn from(info: IntlInfo) -> SettingInfo {
        SettingInfo::Intl(info)
    }
}

pub struct IntlController {
    client: ClientProxy,
    time_zone_ids: std::collections::HashSet<String>,
}

impl StorageAccess for IntlController {
    type Storage = DeviceStorage;
    const STORAGE_KEYS: &'static [&'static str] = &[IntlInfo::KEY];
}

#[async_trait]
impl data_controller::Create for IntlController {
    async fn create(client: ClientProxy) -> Result<Self, ControllerError> {
        let time_zone_ids = IntlController::load_time_zones();
        Ok(IntlController { client, time_zone_ids })
    }
}

#[async_trait]
impl controller::Handle for IntlController {
    async fn handle(&self, request: Request) -> Option<SettingHandlerResult> {
        match request {
            Request::SetIntlInfo(info) => Some(self.set(info).await),
            Request::Get => Some(
                self.client
                    .read_setting_info::<IntlInfo>(ftrace::Id::new())
                    .await
                    .into_handler_result(),
            ),
            _ => None,
        }
    }
}

/// Controller for processing requests surrounding the Intl protocol, backed by a number of
/// services, including TimeZone.
impl IntlController {
    /// Loads the set of valid time zones from resources.
    fn load_time_zones() -> std::collections::HashSet<String> {
        let _icu_data_loader = icu_data::Loader::new().expect("icu data loaded");

        let time_zone_list = match uenum::open_time_zones() {
            Ok(time_zones) => time_zones,
            Err(err) => {
                tracing::error!("Unable to load time zones: {:?}", err);
                return HashSet::new();
            }
        };

        time_zone_list.flatten().collect()
    }

    async fn set(&self, info: IntlInfo) -> SettingHandlerResult {
        self.validate_intl_info(info.clone())?;

        let id = ftrace::Id::new();
        let current = self.client.read_setting::<IntlInfo>(id).await;
        self.client.write_setting(current.merge(info).into(), id).await.into_handler_result()
    }

    #[allow(clippy::result_large_err)] // TODO(fxbug.dev/117896)
    /// Checks if the given IntlInfo is valid.
    fn validate_intl_info(&self, info: IntlInfo) -> Result<(), ControllerError> {
        if let Some(time_zone_id) = info.time_zone_id {
            // Make sure the given time zone ID is valid.
            if !self.time_zone_ids.contains(time_zone_id.as_str()) {
                return Err(ControllerError::InvalidArgument(
                    SettingType::Intl,
                    "timezone id".into(),
                    time_zone_id.into(),
                ));
            }
        }

        if let Some(time_zone_locale) = info.locales {
            for locale in time_zone_locale {
                // NB: `try_from` doesn't actually do validation, `for_language_tag` does but doesn't
                // actually generate an error, it just ends up falling back to an empty string.
                let loc = uloc::ULoc::for_language_tag(locale.id.as_str());
                match loc {
                    Ok(parsed) => {
                        if parsed.label().is_empty() {
                            tracing::error!("Locale is invalid: {:?}", locale.id);
                            return Err(ControllerError::InvalidArgument(
                                SettingType::Intl,
                                "locale id".into(),
                                locale.id.into(),
                            ));
                        }
                    }
                    Err(err) => {
                        tracing::error!("Error loading locale: {:?}", err);
                        return Err(ControllerError::InvalidArgument(
                            SettingType::Intl,
                            "locale id".into(),
                            locale.id.into(),
                        ));
                    }
                }
            }
        }

        Ok(())
    }
}
