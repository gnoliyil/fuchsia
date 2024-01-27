// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::input_device::{Handled, InputDeviceEvent, InputEvent, UnhandledInputEvent};
use crate::input_handler::{InputHandlerStatus, UnhandledInputHandler};
use async_trait::async_trait;
use fidl_fuchsia_ui_input3::{KeyMeaning, Modifiers, NonPrintableKey};
use fuchsia_inspect;
use keymaps::{LockStateKeys, ModifierState};
use std::cell::RefCell;
use std::rc::Rc;

/// Tracks modifier state and decorates passing events with the modifiers.
///
/// This handler should be installed as early as possible in the input pipeline,
/// to ensure that all later stages have the modifiers and lock states available.
/// This holds even for non-keyboard handlers, to allow handling `Ctrl+Click`
/// events, for example.
///
/// One possible exception to this rule would be a hardware key rewriting handler for
/// limited keyboards.
#[derive(Debug)]
pub struct ModifierHandler {
    /// The tracked state of the modifiers.
    modifier_state: RefCell<ModifierState>,

    /// The tracked lock state.
    lock_state: RefCell<LockStateKeys>,

    /// The inventory of this handler's Inspect status.
    pub inspect_status: InputHandlerStatus,
}

#[async_trait(?Send)]
impl UnhandledInputHandler for ModifierHandler {
    async fn handle_unhandled_input_event(
        self: Rc<Self>,
        unhandled_input_event: UnhandledInputEvent,
    ) -> Vec<InputEvent> {
        match unhandled_input_event.clone() {
            UnhandledInputEvent {
                device_event: InputDeviceEvent::Keyboard(mut event),
                device_descriptor,
                event_time,
                trace_id: _,
            } => {
                self.inspect_status.count_received_event(InputEvent::from(unhandled_input_event));
                self.modifier_state.borrow_mut().update(event.get_event_type(), event.get_key());
                self.lock_state.borrow_mut().update(event.get_event_type(), event.get_key());
                event = event
                    .into_with_lock_state(Some(self.lock_state.borrow().get_state()))
                    .into_with_modifiers(Some(self.modifier_state.borrow().get_state()));
                tracing::debug!("modifiers and lock state applied: {:?}", &event);
                vec![InputEvent {
                    device_event: InputDeviceEvent::Keyboard(event),
                    device_descriptor,
                    event_time,
                    handled: Handled::No,
                    trace_id: None,
                }]
            }
            // Pass other events through.
            _ => vec![InputEvent::from(unhandled_input_event)],
        }
    }
}

impl ModifierHandler {
    /// Creates a new [ModifierHandler].
    pub fn new(input_handlers_node: &fuchsia_inspect::Node) -> Rc<Self> {
        let inspect_status = InputHandlerStatus::new(
            input_handlers_node,
            "modifier_handler",
            /* generates_events */ false,
        );
        Rc::new(Self {
            modifier_state: RefCell::new(ModifierState::new()),
            lock_state: RefCell::new(LockStateKeys::new()),
            inspect_status,
        })
    }
}

/// Tracks the state of the modifiers that are tied to the key meaning (as opposed to hardware
/// keys).
#[derive(Debug)]
pub struct ModifierMeaningHandler {
    /// The tracked state of the modifiers.
    modifier_state: RefCell<ModifierState>,

    /// The inventory of this handler's Inspect status.
    pub inspect_status: InputHandlerStatus,
}

impl ModifierMeaningHandler {
    /// Creates a new [ModifierMeaningHandler].
    pub fn new(input_handlers_node: &fuchsia_inspect::Node) -> Rc<Self> {
        let inspect_status = InputHandlerStatus::new(
            input_handlers_node,
            "modifier_meaning_handler",
            /* generates_events */ false,
        );
        Rc::new(Self { modifier_state: RefCell::new(ModifierState::new()), inspect_status })
    }
}

#[async_trait(?Send)]
impl UnhandledInputHandler for ModifierMeaningHandler {
    async fn handle_unhandled_input_event(
        self: Rc<Self>,
        unhandled_input_event: UnhandledInputEvent,
    ) -> Vec<InputEvent> {
        match unhandled_input_event.clone() {
            UnhandledInputEvent {
                device_event: InputDeviceEvent::Keyboard(mut event),
                device_descriptor,
                event_time,
                trace_id: _,
            } if event.get_key_meaning()
                == Some(KeyMeaning::NonPrintableKey(NonPrintableKey::AltGraph)) =>
            {
                self.inspect_status.count_received_event(InputEvent::from(unhandled_input_event));
                // The "obvious" rewrite of this if and the match guard above is
                // unstable, so doing it this way.
                if let Some(key_meaning) = event.get_key_meaning() {
                    self.modifier_state
                        .borrow_mut()
                        .update_with_key_meaning(event.get_event_type(), key_meaning);
                    let new_modifier = event.get_modifiers().unwrap_or(Modifiers::empty())
                        | self.modifier_state.borrow().get_state();
                    event = event.into_with_modifiers(Some(new_modifier));
                    tracing::debug!("additinal modifiers and lock state applied: {:?}", &event);
                }
                vec![InputEvent {
                    device_event: InputDeviceEvent::Keyboard(event),
                    device_descriptor,
                    event_time,
                    handled: Handled::No,
                    trace_id: None,
                }]
            }
            // Pass other events through.
            _ => vec![InputEvent::from(unhandled_input_event)],
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::input_device::{Handled, InputDeviceDescriptor, InputDeviceEvent, InputEvent};
    use crate::input_handler::InputHandler;
    use crate::keyboard_binding::{self, KeyboardEvent};
    use crate::testing_utilities;
    use fidl_fuchsia_input::Key;
    use fidl_fuchsia_ui_input3::{KeyEventType, LockState, Modifiers};
    use fuchsia_async as fasync;
    use fuchsia_inspect;
    use fuchsia_zircon as zx;
    use pretty_assertions::assert_eq;

    fn get_unhandled_input_event(event: KeyboardEvent) -> UnhandledInputEvent {
        UnhandledInputEvent {
            device_event: InputDeviceEvent::Keyboard(event),
            event_time: zx::Time::from_nanos(42),
            device_descriptor: InputDeviceDescriptor::Fake,
            trace_id: None,
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_decoration() {
        let inspector = fuchsia_inspect::Inspector::default();
        let test_node = inspector.root().create_child("test_node");
        let handler = ModifierHandler::new(&test_node);
        let input_event =
            get_unhandled_input_event(KeyboardEvent::new(Key::CapsLock, KeyEventType::Pressed));
        let result = handler.handle_unhandled_input_event(input_event.clone()).await;

        // This handler decorates, but does not handle the key. Hence,
        // the key remains unhandled.
        let expected = InputEvent::from(get_unhandled_input_event(
            KeyboardEvent::new(Key::CapsLock, KeyEventType::Pressed)
                .into_with_modifiers(Some(Modifiers::CAPS_LOCK))
                .into_with_lock_state(Some(LockState::CAPS_LOCK)),
        ));
        assert_eq!(vec![expected], result);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_key_meaning_decoration() {
        let inspector = fuchsia_inspect::Inspector::default();
        let test_node = inspector.root().create_child("test_node");
        let handler = ModifierMeaningHandler::new(&test_node);
        {
            let input_event = get_unhandled_input_event(
                KeyboardEvent::new(Key::RightAlt, KeyEventType::Pressed)
                    .into_with_key_meaning(Some(KeyMeaning::NonPrintableKey(
                        NonPrintableKey::AltGraph,
                    )))
                    .into_with_modifiers(Some(Modifiers::CAPS_LOCK)),
            );
            let result = handler.clone().handle_unhandled_input_event(input_event.clone()).await;
            let expected = InputEvent::from(get_unhandled_input_event(
                KeyboardEvent::new(Key::RightAlt, KeyEventType::Pressed)
                    .into_with_key_meaning(Some(KeyMeaning::NonPrintableKey(
                        NonPrintableKey::AltGraph,
                    )))
                    .into_with_modifiers(Some(Modifiers::ALT_GRAPH | Modifiers::CAPS_LOCK)),
            ));
            assert_eq!(vec![expected], result);
        }
        {
            let input_event = get_unhandled_input_event(
                KeyboardEvent::new(Key::RightAlt, KeyEventType::Released)
                    .into_with_key_meaning(Some(KeyMeaning::NonPrintableKey(
                        NonPrintableKey::AltGraph,
                    )))
                    .into_with_modifiers(Some(Modifiers::CAPS_LOCK)),
            );
            let handler = handler.clone();
            let result = handler.handle_unhandled_input_event(input_event.clone()).await;
            let expected = InputEvent::from(get_unhandled_input_event(
                KeyboardEvent::new(Key::RightAlt, KeyEventType::Released)
                    .into_with_key_meaning(Some(KeyMeaning::NonPrintableKey(
                        NonPrintableKey::AltGraph,
                    )))
                    .into_with_modifiers(Some(Modifiers::CAPS_LOCK)),
            ));
            assert_eq!(vec![expected], result);
        }
    }

    // CapsLock  """"""\______/""""""""""\_______/"""
    // Modifiers ------CCCCCCCC----------CCCCCCCCC---
    // LockState ------CCCCCCCCCCCCCCCCCCCCCCCCCCC---
    //
    // C == CapsLock
    #[fasync::run_singlethreaded(test)]
    async fn test_modifier_press_lock_release() {
        let input_events = vec![
            get_unhandled_input_event(KeyboardEvent::new(Key::CapsLock, KeyEventType::Pressed)),
            get_unhandled_input_event(KeyboardEvent::new(Key::CapsLock, KeyEventType::Released)),
            get_unhandled_input_event(KeyboardEvent::new(Key::CapsLock, KeyEventType::Pressed)),
            get_unhandled_input_event(KeyboardEvent::new(Key::CapsLock, KeyEventType::Released)),
        ];

        let inspector = fuchsia_inspect::Inspector::default();
        let test_node = inspector.root().create_child("test_node");
        let handler = ModifierHandler::new(&test_node);
        let clone_handler = move || handler.clone();
        let result = futures::future::join_all(
            input_events
                .into_iter()
                .map(|e| async { clone_handler().handle_unhandled_input_event(e).await }),
        )
        .await
        .into_iter()
        .flatten()
        .collect::<Vec<InputEvent>>();

        let expected = IntoIterator::into_iter([
            get_unhandled_input_event(
                KeyboardEvent::new(Key::CapsLock, KeyEventType::Pressed)
                    .into_with_modifiers(Some(Modifiers::CAPS_LOCK))
                    .into_with_lock_state(Some(LockState::CAPS_LOCK)),
            ),
            get_unhandled_input_event(
                KeyboardEvent::new(Key::CapsLock, KeyEventType::Released)
                    .into_with_modifiers(Some(Modifiers::from_bits_allow_unknown(0)))
                    .into_with_lock_state(Some(LockState::CAPS_LOCK)),
            ),
            get_unhandled_input_event(
                KeyboardEvent::new(Key::CapsLock, KeyEventType::Pressed)
                    .into_with_modifiers(Some(Modifiers::CAPS_LOCK))
                    .into_with_lock_state(Some(LockState::CAPS_LOCK)),
            ),
            get_unhandled_input_event(
                KeyboardEvent::new(Key::CapsLock, KeyEventType::Released)
                    .into_with_modifiers(Some(Modifiers::from_bits_allow_unknown(0)))
                    .into_with_lock_state(Some(LockState::from_bits_allow_unknown(0))),
            ),
        ])
        .map(InputEvent::from)
        .collect::<Vec<_>>();

        assert_eq!(expected, result);
    }

    // CapsLock  """"""\______/"""""""""""""""""""
    // A         """""""""""""""""""\________/""""
    // Modifiers ------CCCCCCCC-------------------
    // LockState ------CCCCCCCCCCCCCCCCCCCCCCCCCCC
    //
    // C == CapsLock
    #[fasync::run_singlethreaded(test)]
    async fn repeated_modifier_key() {
        let input_events = vec![
            get_unhandled_input_event(KeyboardEvent::new(Key::CapsLock, KeyEventType::Pressed)),
            get_unhandled_input_event(KeyboardEvent::new(Key::CapsLock, KeyEventType::Released)),
            get_unhandled_input_event(KeyboardEvent::new(Key::A, KeyEventType::Pressed)),
            get_unhandled_input_event(KeyboardEvent::new(Key::A, KeyEventType::Released)),
        ];

        let inspector = fuchsia_inspect::Inspector::default();
        let test_node = inspector.root().create_child("test_node");
        let handler = ModifierHandler::new(&test_node);
        let clone_handler = move || handler.clone();
        let result = futures::future::join_all(
            input_events
                .into_iter()
                .map(|e| async { clone_handler().handle_unhandled_input_event(e).await }),
        )
        .await
        .into_iter()
        .flatten()
        .collect::<Vec<InputEvent>>();

        let expected = IntoIterator::into_iter([
            get_unhandled_input_event(
                KeyboardEvent::new(Key::CapsLock, KeyEventType::Pressed)
                    .into_with_modifiers(Some(Modifiers::CAPS_LOCK))
                    .into_with_lock_state(Some(LockState::CAPS_LOCK)),
            ),
            get_unhandled_input_event(
                KeyboardEvent::new(Key::CapsLock, KeyEventType::Released)
                    .into_with_modifiers(Some(Modifiers::from_bits_allow_unknown(0)))
                    .into_with_lock_state(Some(LockState::CAPS_LOCK)),
            ),
            get_unhandled_input_event(
                KeyboardEvent::new(Key::A, KeyEventType::Pressed)
                    .into_with_modifiers(Some(Modifiers::from_bits_allow_unknown(0)))
                    .into_with_lock_state(Some(LockState::CAPS_LOCK)),
            ),
            get_unhandled_input_event(
                KeyboardEvent::new(Key::A, KeyEventType::Released)
                    .into_with_modifiers(Some(Modifiers::from_bits_allow_unknown(0)))
                    .into_with_lock_state(Some(LockState::CAPS_LOCK)),
            ),
        ])
        .map(InputEvent::from)
        .collect::<Vec<_>>();
        assert_eq!(expected, result);
    }

    #[fuchsia::test]
    fn modifier_handlers_initialized_with_inspect_node() {
        let inspector = fuchsia_inspect::Inspector::default();
        let fake_handlers_node = inspector.root().create_child("input_handlers_node");
        let _modifier_handler = ModifierHandler::new(&fake_handlers_node);
        let _modifier_meaning_handler = ModifierMeaningHandler::new(&fake_handlers_node);
        fuchsia_inspect::assert_data_tree!(inspector, root: {
            input_handlers_node: {
                modifier_handler: {
                    events_received_count: 0u64,
                    events_handled_count: 0u64,
                    last_received_timestamp_ns: 0u64,
                    "fuchsia.inspect.Health": {
                        status: "STARTING_UP",
                        // Timestamp value is unpredictable and not relevant in this context,
                        // so we only assert that the property is present.
                        start_timestamp_nanos: fuchsia_inspect::AnyProperty
                    },
                },
                modifier_meaning_handler: {
                    events_received_count: 0u64,
                    events_handled_count: 0u64,
                    last_received_timestamp_ns: 0u64,
                    "fuchsia.inspect.Health": {
                        status: "STARTING_UP",
                        // Timestamp value is unpredictable and not relevant in this context,
                        // so we only assert that the property is present.
                        start_timestamp_nanos: fuchsia_inspect::AnyProperty
                    },
                }
            }
        });
    }

    #[fasync::run_singlethreaded(test)]
    async fn modifier_handler_inspect_counts_events() {
        let inspector = fuchsia_inspect::Inspector::default();
        let fake_handlers_node = inspector.root().create_child("input_handlers_node");
        let modifier_handler = ModifierHandler::new(&fake_handlers_node);
        let modifier_meaning_handler = ModifierMeaningHandler::new(&fake_handlers_node);
        let device_descriptor =
            InputDeviceDescriptor::Keyboard(keyboard_binding::KeyboardDeviceDescriptor {
                keys: vec![Key::A, Key::B, Key::RightAlt],
                ..Default::default()
            });
        let (_, event_time_u64) = testing_utilities::event_times();
        let input_events = vec![
            testing_utilities::create_keyboard_event_with_time(
                Key::A,
                fidl_fuchsia_ui_input3::KeyEventType::Pressed,
                None,
                event_time_u64,
                &device_descriptor,
                /* keymap= */ None,
            ),
            // Should not count received events that have already been handled.
            testing_utilities::create_keyboard_event_with_handled(
                Key::B,
                fidl_fuchsia_ui_input3::KeyEventType::Pressed,
                None,
                event_time_u64,
                &device_descriptor,
                /* keymap= */ None,
                /* key_meaning= */ None,
                Handled::Yes,
            ),
            testing_utilities::create_keyboard_event_with_time(
                Key::A,
                fidl_fuchsia_ui_input3::KeyEventType::Released,
                None,
                event_time_u64,
                &device_descriptor,
                /* keymap= */ None,
            ),
            // Should not count non-keyboard input events.
            testing_utilities::create_fake_input_event(event_time_u64),
            // Only event that should be counted by ModifierMeaningHandler.
            testing_utilities::create_keyboard_event_with_key_meaning(
                Key::RightAlt,
                fidl_fuchsia_ui_input3::KeyEventType::Pressed,
                None,
                event_time_u64,
                &device_descriptor,
                /* keymap= */ None,
                /* key_meaning= */
                Some(KeyMeaning::NonPrintableKey(NonPrintableKey::AltGraph)),
            ),
        ];

        for input_event in input_events {
            let _ = modifier_handler.clone().handle_input_event(input_event.clone()).await;
            let _ = modifier_meaning_handler.clone().handle_input_event(input_event).await;
        }

        let last_event_timestamp: u64 = event_time_u64.into_nanos().try_into().unwrap();

        fuchsia_inspect::assert_data_tree!(inspector, root: {
            input_handlers_node: {
                modifier_handler: {
                    events_received_count: 3u64,
                    events_handled_count: 0u64,
                    last_received_timestamp_ns: last_event_timestamp,
                    "fuchsia.inspect.Health": {
                        status: "STARTING_UP",
                        // Timestamp value is unpredictable and not relevant in this context,
                        // so we only assert that the property is present.
                        start_timestamp_nanos: fuchsia_inspect::AnyProperty
                    },
                },
                modifier_meaning_handler: {
                    events_received_count: 1u64,
                    events_handled_count: 0u64,
                    last_received_timestamp_ns: last_event_timestamp,
                    "fuchsia.inspect.Health": {
                        status: "STARTING_UP",
                        // Timestamp value is unpredictable and not relevant in this context,
                        // so we only assert that the property is present.
                        start_timestamp_nanos: fuchsia_inspect::AnyProperty
                    },
                }
            }
        });
    }
}
