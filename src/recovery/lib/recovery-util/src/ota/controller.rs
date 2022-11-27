// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::ota::state_machine::{Event, EventProcessor, StateHandler};
use fuchsia_async::{self as fasync, Task};
use futures::channel::mpsc;
use futures::SinkExt;
use futures::StreamExt;
#[cfg(test)]
use mockall::automock;

#[derive(Clone)]
pub struct EventSender {
    sender: mpsc::Sender<Event>,
}

#[cfg_attr(test, automock)]
impl EventSender {
    pub fn new(sender: mpsc::Sender<Event>) -> Self {
        Self { sender }
    }
}

#[cfg_attr(test, automock)]
pub trait SendEvent {
    fn send(&mut self, event: Event);
}

impl SendEvent for EventSender {
    fn send(&mut self, event: Event) {
        let mut sender = self.sender.clone();
        let event_clone = event.clone();
        fasync::Task::local(async move {
            if let Err(error) = sender.send(event).await {
                eprintln!("Failed to send event {:?}: {}", event_clone, error);
            }
        })
        .detach();
    }
}

pub struct Controller {
    sender: mpsc::Sender<Event>,
    receiver: Option<mpsc::Receiver<Event>>,
    state_handlers: Option<Vec<Box<dyn StateHandler>>>,
}

impl Controller {
    pub fn new() -> Self {
        let (sender, receiver) = mpsc::channel::<Event>(10);
        Self { sender, receiver: Some(receiver), state_handlers: Some(Vec::new()) }
    }

    pub fn add_state_handler(&mut self, handler: Box<dyn StateHandler>) {
        if let Some(ref mut state_handlers) = self.state_handlers {
            state_handlers.push(handler);
        }
    }

    pub fn get_event_sender(&self) -> EventSender {
        EventSender::new(self.sender.clone())
    }

    pub fn start(&mut self, mut state_machine: Box<dyn EventProcessor>) {
        let mut receiver = self.receiver.take().unwrap();
        let mut state_handlers = self.state_handlers.take().unwrap();
        let main_loop = async move {
            loop {
                match receiver.next().await {
                    Some(event) => {
                        let event_clone = event.clone();
                        let state = state_machine.process_event(event);
                        println!("Controller: event {:?} -> state {:?}", event_clone, state);
                        if let Some(state) = &state {
                            for state_handler in state_handlers.iter_mut() {
                                state_handler.handle_state(state.clone());
                            }
                        }
                    }
                    None => {
                        eprintln!("event reader returned: None")
                    }
                }
            }
        };
        Task::local(main_loop).detach();
    }
}

#[cfg(test)]
mod test {
    use crate::ota::controller::{Controller, SendEvent};
    use crate::ota::state_machine::{
        Event, MockEventProcessor, MockStateHandler, State, StateHandler,
    };
    use fuchsia_async as fasync;
    use mockall::predicate::*;

    #[test]
    fn send_states() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let mut state_machine = MockEventProcessor::new();
        state_machine
            .expect_process_event()
            .with(eq(Event::Cancel))
            .return_const(Some(State::Home));

        let mut state_handler = MockStateHandler::new();
        state_handler.expect_handle_state().with(eq(State::Home)).times(1).return_const(());
        let state_handler: Box<dyn StateHandler> = Box::new(state_handler);

        let mut controller = Controller::new();
        controller.add_state_handler(state_handler);
        let mut event_sender = controller.get_event_sender();
        controller.start(Box::new(state_machine));
        event_sender.send(Event::Cancel);

        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());
    }
}
