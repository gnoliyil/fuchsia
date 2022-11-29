// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[allow(unused)]
use crate::constants::constants::{
    ScreenSplit, ICON_FACTORY_RESET, ICON_REINSTALL_SOFTWARE, IMAGE_DEFAULT_SIZE,
    IMAGE_DEVICE_CONNECT, IMAGE_DEVICE_INSTALL, IMAGE_DEVICE_UPDATING, IMAGE_DEVICE_UPDATING_SIZE,
    IMAGE_PHONE_HOME_APP, IMAGE_PHONE_HOME_APP_SIZE, IMAGE_RESET_FAILED,
};
use crate::font;
use crate::generic_view::{ButtonInfo, GenericSplitViewAssistant};
use crate::keyboard::KeyboardViewAssistant;
use crate::network::NetworkViewAssistant;
use crate::proxy_view_assistant::ProxyMessages;
use crate::text_field::TextVisibility;
use carnelian::{make_message, AppSender, MessageTarget, ViewAssistantPtr, ViewKey};
use recovery_util::ota::state_machine::{Event, Operation, State, StateHandler};
use recovery_util::wlan::NetworkInfo;

pub struct Screens {
    app_sender: AppSender,
    view_key: ViewKey,
}

impl Screens {
    pub fn new(app_sender: AppSender, view_key: ViewKey) -> Self {
        Self { app_sender, view_key }
    }

    pub fn initial_screen(&self) -> ViewAssistantPtr {
        self.factory_reset()
    }

    fn state(&mut self, state: &State) {
        let new_view = match state {
            State::Home => self.factory_reset(),
            State::Failed(operation, error) => self.failed(operation, error),
            State::Reinstall => self.reinstall(),
            State::GetWiFiNetworks => self.select_wifi(&Vec::new()),
            State::SelectWiFi(networks) => self.select_wifi(networks),
            State::EnterWiFi => {
                self.user_entry("Enter Network Name".to_string(), TextVisibility::Always)
            }
            State::EnterPassword(network) => self.user_entry(
                format!("Enter Password for {} ", network),
                TextVisibility::Toggleable(false),
            ),
            // Temporary catch-all until all the other states are added
            _ => self.failed(&Operation::Reinstall, &Some("State not yet implemented".to_string())),
        };
        self.app_sender.queue_message(
            MessageTarget::View(self.view_key),
            make_message(ProxyMessages::ReplaceViewAssistant(Some(new_view))),
        );
    }

    fn factory_reset(&self) -> ViewAssistantPtr {
        let view_assistant_ptr = Box::new(
            GenericSplitViewAssistant::new(
                self.app_sender.clone(),
                self.view_key,
                ScreenSplit::Wide,
                Some("Factory reset".to_string()),
                Some("• A factory reset can fix a device problem\n• This will clear your data from the device and can't be undone".to_string()),
                Some("Still having issues after factory reset?".to_string()),
                vec![ButtonInfo::new("Start factory reset", Some(ICON_FACTORY_RESET), false, false, Event::StartFactoryReset)],
                Some(vec![ButtonInfo::new("Try another option", None, true, true, Event::TryAnotherWay)]),
                None,
                None,
                None,
            )
                .unwrap(),
        );
        view_assistant_ptr
    }

    fn failed(&self, operation: &Operation, error: &Option<String>) -> ViewAssistantPtr {
        let text1 = match operation {
            Operation::FactoryDataReset => "Reset failed",
            Operation::Reinstall => "Recovery failed",
        };
        let error = if let Some(error) = error { error.clone() } else { "".to_string() };
        let text2 = match operation {
            Operation::FactoryDataReset => "An error occurred while\nrecovering".to_string(),
            Operation::Reinstall => error + "\n\nPlease reconnect and try again.",
        };
        let view_assistant_ptr = Box::new(
            GenericSplitViewAssistant::new(
                self.app_sender.clone(),
                self.view_key,
                ScreenSplit::Even,
                Some(text1.to_string()),
                Some(text2.to_string()),
                None,
                vec![
                    ButtonInfo::new("Cancel", None, false, true, Event::Cancel),
                    ButtonInfo::new("Try again", None, false, false, Event::TryAgain),
                ],
                None,
                None,
                Some(IMAGE_RESET_FAILED),
                None,
            )
            .unwrap(),
        );
        view_assistant_ptr
    }

    fn reinstall(&self) -> ViewAssistantPtr {
        let view_assistant_ptr = Box::new(
            GenericSplitViewAssistant::new(
                self.app_sender.clone(),
                self.view_key,
                ScreenSplit::Wide,
                Some("Reinstall Software".to_string()),
                Some(
                    "• Install the latest software to fix bugs and update the system\n\
                     • This will clear your data from the device and can't be undone\n\
                     • You'll need a Wi-Fi connection to proceed"
                        .to_string(),
                ),
                None,
                vec![
                    ButtonInfo::new("Cancel", None, false, true, Event::Cancel),
                    ButtonInfo::new(
                        "Reinstall Software",
                        Some(ICON_REINSTALL_SOFTWARE),
                        false,
                        false,
                        Event::Reinstall,
                    ),
                ],
                Some(vec![ButtonInfo::new("Power off", None, true, true, Event::Cancel)]),
                None,
                None,
                None,
            )
            .unwrap(),
        );
        view_assistant_ptr
    }

    fn select_wifi(&self, networks: &Vec<NetworkInfo>) -> ViewAssistantPtr {
        let network_view = Box::new(
            NetworkViewAssistant::new(self.app_sender.clone(), self.view_key, networks.to_vec())
                .unwrap(),
        );
        network_view
    }

    fn user_entry(&self, text: String, privacy: TextVisibility) -> ViewAssistantPtr {
        let mut keyboard = Box::new(
            KeyboardViewAssistant::new(
                self.app_sender.clone(),
                self.view_key,
                font::get_default_font_face().clone(),
            )
            .unwrap(),
        );
        keyboard.set_field_name(text);
        keyboard.set_text_field(String::new());
        keyboard.set_privacy(privacy);
        keyboard
    }
}

impl StateHandler for Screens {
    fn handle_state(&mut self, state: State) {
        self.state(&state);
    }
}
