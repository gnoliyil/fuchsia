// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use carnelian::{
    app::Config, drawing::DisplayRotation, App, AppAssistant, AppAssistantPtr, AppSender,
    AssistantCreator, AssistantCreatorFunc, ViewAssistantPtr, ViewKey,
};
use recovery_ui::{font, proxy_view_assistant::ProxyViewAssistant};
use recovery_ui_config::Config as UiConfig;
use recovery_util::ota::controller::Controller;
use recovery_util::ota::state_machine::{State, StateMachine};

#[cfg(feature = "debug_console")]
use recovery_ui::console::ConsoleViewAssistant;
use recovery_ui::screens::Screens;
use recovery_util::ota::action::Action;

struct RecoveryAppAssistant {
    app_sender: AppSender,
    display_rotation: DisplayRotation,
}

impl RecoveryAppAssistant {
    pub fn new(app_sender: &AppSender) -> Self {
        let display_rotation = Self::get_display_rotation();
        Self { app_sender: app_sender.clone(), display_rotation }
    }

    fn get_display_rotation() -> DisplayRotation {
        let config = UiConfig::take_from_startup_handle();
        match config.display_rotation {
            0 => DisplayRotation::Deg0,
            180 => DisplayRotation::Deg180,
            // Carnelian uses an inverted z-axis for rotation
            90 => DisplayRotation::Deg270,
            270 => DisplayRotation::Deg90,
            val => {
                eprintln!("Invalid display_rotation {}, defaulting to 90 degrees", val);
                DisplayRotation::Deg90
            }
        }
    }
}

impl AppAssistant for RecoveryAppAssistant {
    fn setup(&mut self) -> Result<(), Error> {
        // This line is required for the CQ build and test system, specifically boot_test.go
        println!("recovery: AppAssistant setup");
        Ok(())
    }

    // Create the State Machine
    fn create_view_assistant(&mut self, view_key: ViewKey) -> Result<ViewAssistantPtr, Error> {
        // TODO(b/244744635) Add a structured initialization flow for the recovery component
        // This initial state will change back to State::Home when the generic view is added
        let state_machine = Box::new(StateMachine::new(State::EnterWiFi));
        let mut controller = Controller::new();
        let event_sender = controller.get_event_sender();

        let action = Action::new(event_sender.clone());
        controller.add_state_handler(Box::new(action));

        let screens = Screens::new(self.app_sender.clone(), view_key);
        let first_screen = screens.initial_screen();
        controller.add_state_handler(Box::new(screens));

        controller.start(state_machine);

        let font_face = font::get_default_font_face();
        #[cfg(feature = "debug_console")]
        let console_view_assistant_ptr = Box::new(ConsoleViewAssistant::new(font_face.clone())?);
        #[cfg(not(feature = "debug_console"))]
        let console_view_assistant_ptr = None;
        let proxy_ptr = Box::new(ProxyViewAssistant::new(
            Some(Box::new(event_sender)),
            Some(console_view_assistant_ptr),
            first_screen,
        )?);
        Ok(proxy_ptr)
    }

    fn filter_config(&mut self, config: &mut Config) {
        config.display_rotation = self.display_rotation;
    }
}

fn make_app_assistant_fut() -> impl FnOnce(&AppSender) -> AssistantCreator<'_> {
    move |app_sender: &AppSender| {
        let f = async move {
            // Route stdout to debuglog, allowing log lines to appear over serial.
            stdout_to_debuglog::init().await.unwrap_or_else(|error| {
                eprintln!("Failed to initialize debuglog: {:?}", error);
            });
            println!("Starting New recovery UI");

            let assistant = Box::new(RecoveryAppAssistant::new(app_sender));
            Ok::<AppAssistantPtr, Error>(assistant)
        };
        Box::pin(f)
    }
}

fn make_app_assistant() -> AssistantCreatorFunc {
    Box::new(make_app_assistant_fut())
}

#[allow(unused)]
pub fn main() -> Result<(), Error> {
    App::run(make_app_assistant())
}
