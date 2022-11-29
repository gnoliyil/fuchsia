// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::ota::actions::factory_reset::FactoryResetAction;
use crate::ota::actions::get_wifi_networks::GetWifiNetworksAction;
use crate::ota::actions::set_sharing_consent::SetSharingConsentAction;
use crate::ota::actions::wifi_connect::WifiConnectAction;
use crate::ota::controller::EventSender;
use crate::ota::state_machine::{State, StateHandler};

/// This file initiates all the non-ui, background actions that are required to satisfy
/// the OTA UX UI slides. In general some states cause a background task to be run which
/// may  produce one or more state machine events. Actions may reboot the device.
pub struct Action {
    event_sender: EventSender,
}

impl Action {
    pub fn new(event_sender: EventSender) -> Self {
        Self { event_sender }
    }
}

impl StateHandler for Action {
    fn handle_state(&mut self, state: State) {
        let event_sender = Box::new(self.event_sender.clone());
        // There are six states that will need background actions
        // They will be added in future CLs
        match state {
            State::GetWiFiNetworks => GetWifiNetworksAction::run(event_sender),
            State::Connecting(network, password) => {
                WifiConnectAction::run(event_sender, network, password)
            }
            State::SetPrivacy(user_data_sharing_consent) => {
                SetSharingConsentAction::run(event_sender, user_data_sharing_consent)
            }
            State::FactoryReset => FactoryResetAction::run(event_sender),
            // We ignore all other sates
            _ => {}
        };
    }
}
