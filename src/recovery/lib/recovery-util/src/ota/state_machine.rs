// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::ota::state_machine::DataSharingConsent::{DontAllow, Unknown};
use crate::wlan::NetworkInfo;
use mockall::automock;
pub use ota_lib::OtaStatus;

// This component maps the current state with a new event to produce a
// new state. The states, events and state logic have all ben derived
// from the Recovery OTA UX design.
// The only state held by the state machine is the current state.

#[derive(Debug, Clone, PartialEq)]
pub enum Operation {
    FactoryDataReset,
    Reinstall,
}

pub(crate) type Network = String;
pub(crate) type NetworkInfos = Vec<NetworkInfo>;
pub(crate) type Password = String;
pub(crate) type PercentProgress = u8;

type Text = String;
type ErrorMessage = String;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DataSharingConsent {
    Allow,
    DontAllow,
    Unknown,
}

impl DataSharingConsent {
    pub fn toggle(&self) -> DataSharingConsent {
        match self {
            Self::Allow => Self::DontAllow,
            Self::DontAllow => Self::Allow,
            Self::Unknown => Self::Unknown,
        }
    }
}

#[derive(Debug, Clone)]
pub enum State {
    Connecting(Network, Password),
    ConnectionFailed(Network, Password),
    EnterPassword(Network),
    EnterWiFi,
    ExecuteReinstall(Option<OtaStatus>),
    FactoryReset,
    FinalizeReinstall(OtaStatus),
    Failed(Operation, Option<ErrorMessage>),
    Home,
    Reinstall,
    ReinstallConfirm { desired: DataSharingConsent, reported: DataSharingConsent },
    GetWiFiNetworks,
    SelectWiFi(NetworkInfos),
}

impl PartialEq for State {
    fn eq(&self, other: &Self) -> bool {
        std::mem::discriminant(self) == std::mem::discriminant(other)
    }
}

#[derive(Debug, Clone)]
pub enum Event {
    AddNetwork,
    Cancel,
    ChooseNetwork,
    Error(ErrorMessage),
    Networks(NetworkInfos),
    OtaStatusReceived(OtaStatus),
    Progress(PercentProgress),
    Reinstall,
    SystemPrivacySetting(DataSharingConsent),
    SendReports(DataSharingConsent),
    StartFactoryReset,
    TryAgain,
    TryAnotherWay,
    UserInput(Text),
    UserInputUnsecuredNetwork(Network),
    WiFiConnected,
}

// This tests only for enum entry not the value contained in the enum.
impl PartialEq for Event {
    fn eq(&self, other: &Self) -> bool {
        std::mem::discriminant(self) == std::mem::discriminant(other)
    }
}

#[cfg_attr(test, automock)]
pub trait StateHandler {
    fn handle_state(&mut self, event: State);
}

#[automock]
pub trait EventProcessor {
    fn process_event(&mut self, event: Event) -> Option<State>;
}

pub struct StateMachine {
    current_state: State,
}

impl StateMachine {
    pub fn new(state: State) -> Self {
        Self { current_state: state }
    }

    fn event(&mut self, event: Event) -> Option<State> {
        #[cfg(feature = "debug_logging")]
        println!("====== SM: state {:?}, event: {:?}", self.current_state, event);
        let new_state = match (&self.current_state, event) {
            // Any cancel or error sends us back to the start.
            (_, Event::Cancel) => Some(State::Home),

            (State::Home, Event::StartFactoryReset) => Some(State::FactoryReset),
            (State::Home, Event::TryAnotherWay) => Some(State::Reinstall),

            (State::FactoryReset, Event::Error(_reason)) => {
                Some(State::Failed(Operation::FactoryDataReset, None))
            }

            (State::Failed(op, _), Event::TryAgain) => match op {
                Operation::FactoryDataReset => Some(State::FactoryReset),
                Operation::Reinstall => Some(State::Reinstall),
            },

            (State::Reinstall, Event::Reinstall) => Some(State::GetWiFiNetworks),

            (State::GetWiFiNetworks, Event::AddNetwork) => Some(State::EnterWiFi),
            (State::GetWiFiNetworks, Event::Networks(networks)) => {
                Some(State::SelectWiFi(networks))
            }

            (State::SelectWiFi(_), Event::UserInput(network)) => {
                Some(State::EnterPassword(network))
            }
            (State::SelectWiFi(_), Event::UserInputUnsecuredNetwork(network)) => {
                Some(State::Connecting(network, String::new()))
            }
            (State::SelectWiFi(_), Event::AddNetwork) => Some(State::EnterWiFi),

            (State::EnterWiFi, Event::UserInput(network)) if network.is_empty() => {
                Some(State::GetWiFiNetworks)
            }
            (State::EnterWiFi, Event::UserInput(network)) => Some(State::EnterPassword(network)),

            (State::EnterPassword(network), Event::UserInput(password)) => {
                Some(State::Connecting(network.clone(), password.clone()))
            }

            (State::Connecting(_, _), Event::WiFiConnected) => {
                Some(State::ReinstallConfirm { desired: DontAllow, reported: Unknown })
            }
            (State::Connecting(network, password), Event::Error(_reason)) => {
                Some(State::ConnectionFailed(network.clone(), password.clone()))
            }

            (State::ConnectionFailed(..), Event::ChooseNetwork) => Some(State::GetWiFiNetworks),
            (State::ConnectionFailed(network, password), Event::TryAgain) => {
                Some(State::Connecting(network.clone(), password.clone()))
            }

            (
                State::ReinstallConfirm { desired: _, reported: _ },
                Event::SystemPrivacySetting(system_setting),
            ) => Some(State::ReinstallConfirm {
                desired: system_setting.clone(),
                reported: system_setting.clone(),
            }),

            (
                State::ReinstallConfirm { desired: _, reported },
                Event::SendReports(desired_setting),
            ) => Some(State::ReinstallConfirm {
                desired: desired_setting,
                reported: reported.clone(),
            }),
            (State::ReinstallConfirm { .. }, Event::Reinstall) => {
                Some(State::ExecuteReinstall(None))
            }
            (State::ExecuteReinstall(_), Event::OtaStatusReceived(status)) => {
                Some(State::FinalizeReinstall(status))
            }
            (State::ExecuteReinstall(_), Event::Error(error)) => {
                Some(State::Failed(Operation::Reinstall, Some(error)))
            }
            // TODO(b/258323217): Add error message to home screen
            (_, Event::Error(_)) => Some(State::Home),
            (state, event) => {
                println!("Error unexpected event {:?} for state {:?}", event, state);
                None
            }
        };
        if new_state.is_some() {
            #[cfg(feature = "debug_logging")]
            println!("====== New state is {:?}", new_state);
            self.current_state = new_state.as_ref().unwrap().clone();
        }
        new_state
    }
}

#[automock]
impl EventProcessor for StateMachine {
    fn process_event(&mut self, event: Event) -> Option<State> {
        self.event(event)
    }
}

#[cfg(test)]
mod test {
    // TODO(b/258049697): Tests to check the expected flow through more than one state.
    // c.f. https://cs.opensource.google/fuchsia/fuchsia/+/main:src/recovery/system/src/fdr.rs;l=183.

    use super::OtaStatus;
    use crate::ota::state_machine::DataSharingConsent::{DontAllow, Unknown};
    use crate::ota::state_machine::{DataSharingConsent, Event, Operation, State, StateMachine};
    use lazy_static::lazy_static;

    lazy_static! {
        static ref STATES: Vec<State> = vec![
            State::Connecting("Network".to_string(), "Password".to_string()),
            State::ConnectionFailed("Network".to_string(), "Password".to_string()),
            State::EnterPassword("Network".to_string()),
            State::EnterWiFi,
            State::ExecuteReinstall(None),
            State::FactoryReset,
            State::Failed(Operation::Reinstall, Some("Error message".to_string())),
            State::FinalizeReinstall(OtaStatus::Succeeded),
            State::GetWiFiNetworks,
            State::Home,
            State::Reinstall,
            State::SelectWiFi(vec![]),
        ];
        static ref EVENTS: Vec<Event> = vec![
            Event::AddNetwork,
            Event::Cancel,
            Event::ChooseNetwork,
            Event::Error("Error".to_string()),
            Event::Networks(Vec::new()),
            Event::OtaStatusReceived(OtaStatus::Succeeded),
            Event::Progress(0),
            Event::Reinstall,
            Event::SendReports(DataSharingConsent::Allow),
            Event::StartFactoryReset,
            Event::TryAgain,
            Event::TryAnotherWay,
            Event::UserInput("User Input".to_string()),
            Event::UserInputUnsecuredNetwork("Network".to_string()),
            Event::WiFiConnected,
        ];
    }
    // TODO(b/258049617): Enable this when variant_count is in the allowed features list
    // This will enable a check to make sure all events and states are used
    // #[test]
    // fn check_test_validity() {
    //     assert_eq!(std::mem::variant_count::<State>(), STATES.len());
    //     assert_eq!(std::mem::variant_count::<Event>(), EVENTS.len());
    // }

    #[test]
    fn ensure_all_state_and_event_combos_can_not_crash_state_machine() {
        let mut sm = StateMachine::new(State::Home);
        // Generate all possible combinations of States and Events
        for state in STATES.iter() {
            for event in EVENTS.iter() {
                // Set the current state here because sm.event() can change it
                sm.current_state = state.clone();
                let _new_state = sm.event(event.clone());
                if let Some(new_state) = _new_state {
                    assert_eq!(new_state, sm.current_state);
                }
            }
        }
    }

    #[test]
    fn run_through_a_sucessful_user_flow() {
        let mut sm = StateMachine::new(State::Home);
        let mut state = sm.event(Event::TryAnotherWay).unwrap();
        assert_eq!(state, State::Reinstall);
        state = sm.event(Event::Reinstall).unwrap();
        assert_eq!(state, State::GetWiFiNetworks);
        state = sm.event(Event::AddNetwork).unwrap();
        assert_eq!(state, State::EnterWiFi);
        state = sm.event(Event::UserInput("Network".to_string())).unwrap();
        assert_eq!(state, State::EnterPassword("Network".to_string()));
        state = sm.event(Event::UserInput("Password".to_string())).unwrap();
        assert_eq!(state, State::Connecting("Network".to_string(), "Password".to_string()));
        state = sm.event(Event::WiFiConnected).unwrap();
        assert_eq!(state, State::ReinstallConfirm { desired: DontAllow, reported: Unknown });
        state = sm.event(Event::Reinstall).unwrap();
        assert_eq!(state, State::ExecuteReinstall(None));
        state = sm.event(Event::OtaStatusReceived(OtaStatus::Succeeded)).unwrap();
        assert_eq!(state, State::FinalizeReinstall(OtaStatus::Succeeded));
    }
}
