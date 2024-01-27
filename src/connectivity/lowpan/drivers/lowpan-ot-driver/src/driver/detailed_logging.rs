// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use super::*;
use crate::ot::LogLevel;
use anyhow::Error;
use selectors::{parse_component_selector, VerboseError};
use std::cell::{Cell, RefCell};

#[derive(Debug)]
pub struct DetailedLogging {
    /// Store initial log level
    pub log_level_default: LogLevel,

    /// Store the enablement state
    pub detailed_logging_enabled: Cell<bool>,

    /// Store the logging level state
    pub detailed_logging_level: Cell<LogLevel>,

    /// Hold the proxy of Log Settings
    log_settings_proxy: RefCell<Option<fidl_fuchsia_diagnostics::LogSettingsProxy>>,
}

impl DetailedLogging {
    pub fn new() -> Self {
        DetailedLogging {
            log_level_default: ot::LogLevel::Info,
            detailed_logging_enabled: Cell::new(false),
            detailed_logging_level: Cell::new(ot::LogLevel::Info),
            log_settings_proxy: RefCell::new(None),
        }
    }

    pub fn process_detailed_logging_set(
        &self,
        detailed_logging_enabled: Option<bool>,
        detailed_logging_level: Option<LogLevel>,
    ) -> Result<(), Error> {
        if let Some(enabled) = detailed_logging_enabled {
            self.detailed_logging_enabled.set(enabled);
        };
        if let Some(level) = detailed_logging_level {
            self.detailed_logging_level.set(level);
        };
        if self.detailed_logging_enabled.get() {
            let log_settings_client_end = fuchsia_component::client::connect_to_protocol::<
                fidl_fuchsia_diagnostics::LogSettingsMarker,
            >()
            .context("Failed to connect to diagnostics service")?;

            let selectors = vec![fidl_fuchsia_diagnostics::LogInterestSelector {
                selector: parse_component_selector::<VerboseError>("core/lowpan-ot-driver")
                    .context("Failed to parse component selector")?,
                interest: fidl_fuchsia_diagnostics::Interest {
                    min_severity: Some(self.detailed_logging_level.get().into()),
                    ..Default::default()
                },
            }];
            log_settings_client_end.set_interest(&selectors).now_or_never();
            self.log_settings_proxy.replace(Some(log_settings_client_end));

            ot::set_logging_level(self.detailed_logging_level.get());
        } else {
            // Drop the proxy to undo the overridden log level settings
            self.log_settings_proxy.replace(None);
            ot::set_logging_level(self.log_level_default);
        };

        Ok(())
    }

    pub fn process_detailed_logging_get(&self) -> (bool, LogLevel) {
        let detailed_logging_enabled = self.detailed_logging_enabled.replace(false);
        self.detailed_logging_enabled.replace(detailed_logging_enabled);
        let current_log_level = ot::get_logging_level();
        (detailed_logging_enabled, current_log_level)
    }
}
