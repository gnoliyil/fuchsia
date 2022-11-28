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
use crate::keyboard::KeyboardViewAssistant;
use crate::proxy_view_assistant::ProxyMessages;
use crate::text_field::TextVisibility;
use carnelian::{make_message, AppSender, MessageTarget, ViewAssistantPtr, ViewKey};
use recovery_util::ota::state_machine::{State, StateHandler};

pub struct Screens {
    app_sender: AppSender,
    view_key: ViewKey,
}

impl Screens {
    pub fn new(app_sender: AppSender, view_key: ViewKey) -> Self {
        Self { app_sender, view_key }
    }

    pub fn initial_screen(&self) -> ViewAssistantPtr {
        // Temporary start page until we get to the others in later CLs
        self.user_entry("Start Page".to_string(), TextVisibility::Always)
    }

    fn state(&mut self, state: &State) {
        let new_view = match state {
            State::EnterWiFi => {
                self.user_entry("Enter Network Name".to_string(), TextVisibility::Always)
            }
            State::EnterPassword(network) => self.user_entry(
                format!("Enter Password for {} ", network),
                TextVisibility::Toggleable(false),
            ),
            // Temporary catch-all until all the other states are added
            _ => self.user_entry("No state recognised".to_string(), TextVisibility::Always),
        };
        self.app_sender.queue_message(
            MessageTarget::View(self.view_key),
            make_message(ProxyMessages::ReplaceViewAssistant(Some(new_view))),
        );
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
