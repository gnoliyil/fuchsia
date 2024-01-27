// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Service-wide MessageHub definition.
//!
//! The service mod defines a MessageHub (and associated Address, Payload, and
//! Role namespaces) to facilitate service-wide communication. All
//! communication, both intra-component and inter-component, should be sent
//! through this hub. The address space of this MessageHub allows any component
//! to be reached when given a public address. The Role space allows granular
//! message filtering and audience targeting.
//!
//! The static Address and Role definitions below provide a way to reference
//! such values at build time. However, many use-cases that rely on these
//! features can be done through values generated at runtime instead. Care
//! should be taken before expanding either enumeration.
//!
//! Currently, service communication happens in a number of domain-specific
//! message hubs located in the internal mod. Communication from these hubs
//! should migrate here over time.

use crate::agent;
use crate::base::SettingType;
use crate::event;
use crate::handler::base as handler;
use crate::handler::setting_handler as controller;
use crate::job;
#[cfg(test)]
use crate::message::base::MessengerType;
use crate::message::message_hub;
use crate::policy::{self, PolicyType};
use crate::storage;

pub struct MessageHub;

impl MessageHub {
    pub(crate) fn create_hub() -> message::Delegate {
        message_hub::MessageHub::create()
    }
}

pub(crate) mod message {
    use super::Payload;
    use crate::message::{base, delegate, message_client, messenger, receptor};

    pub(crate) type Delegate = delegate::Delegate<Payload>;
    pub(crate) type Audience = base::Audience;
    pub(crate) type Messenger = messenger::MessengerClient<Payload>;
    pub(crate) type MessageError = base::MessageError;
    pub(crate) type MessageEvent = base::MessageEvent<Payload>;
    pub(crate) type MessageClient = message_client::MessageClient<Payload>;
    pub(crate) type MessengerType = base::MessengerType<Payload>;
    pub(crate) type Receptor = receptor::Receptor<Payload>;
    pub(crate) type Signature = base::Signature;
}

/// The `Address` enumeration defines a namespace for entities that can be
/// reached by a predefined name. Care should be taken when adding new child
/// namespaces here. Each address can only belong to a single entity.
/// Most communication can be instead facilitated with a messenger's signature,
/// which is available at messenger creation time.
#[derive(PartialEq, Copy, Clone, Debug, Eq, Hash)]
pub enum Address {
    Handler(SettingType),
    PolicyHandler(PolicyType),
    EventSource(event::Address),
    Storage,
    /// This value is reserved for testing purposes.
    #[cfg(test)]
    Test(u64),
}

/// The types of data that can be sent through the service `MessageHub`. This
/// enumeration is meant to provide a top level definition. Further definitions
/// for particular domains should be located in the appropriate mod.
#[derive(Clone, PartialEq, Debug)]
pub enum Payload {
    /// The Setting type captures communication pertaining to settings,
    /// including requests to access/change settings and the associated
    /// responses.
    Setting(handler::Payload),
    /// The communication to and from a controller to handle requests and
    /// lifetime.
    Controller(controller::Payload),
    /// Policy payloads contain communication to and from policy handlers, including general
    /// requests and responses served by any policy as well as domain-specific requests and
    /// responses defined for individual policies.
    Policy(policy::Payload),
    /// Agent payloads contain communication between the agent authority and individual agents.
    Agent(agent::Payload),
    /// Event payloads contain data about events that occur throughout the system.
    Event(event::Payload),
    /// Job payloads contain information related to new sources of jobs to be executed.
    Job(job::Payload),
    /// Storage payloads contain read and write requests to storage and their responses.
    Storage(storage::Payload),
    /// This value is reserved for testing purposes.
    #[cfg(test)]
    Test(test::Payload),
}

#[cfg(test)]
pub(crate) mod test {
    use crate::audio::types::AudioInfo;
    use crate::payload_convert;

    /// This payload should be expanded to include any sort of data that tests would send that is
    /// outside the scope of production payloads.
    #[derive(PartialEq, Clone, Debug)]
    pub enum Payload {
        Integer(i64),
        Audio(AudioInfo),
    }

    // Conversions for Handler Payload.
    payload_convert!(Test, Payload);
}

/// A trait implemented by payloads for extracting the payload and associated
/// [`message::MessageClient`] from a [`crate::message::base::MessageEvent`].
pub(crate) trait TryFromWithClient<T>: Sized {
    type Error;

    fn try_from_with_client(value: T) -> Result<(Self, message::MessageClient), Self::Error>;
}

/// `Role` defines grouping for responsibilities within the service. Roles allow
/// for addressing a particular audience space. Similar to the other
/// enumerations, details about each role should be defined near to the domain.
#[derive(PartialEq, Copy, Clone, Debug, Eq, Hash)]
pub enum Role {
    Policy(policy::Role),
    Event(event::Role),
}

/// The payload_convert macro helps convert between the domain-specific payloads
/// (variants of [`Payload`]) and the [`Payload`] container(to/from) & MessageHub
/// MessageEvent (from). The first matcher is the [`Payload`] variant name where
/// the payload type can be found. Note that this is the direct variant name
/// and not fully qualified. The second matcher is the domain-specific payload
/// type.
///
/// The macro implements the following in a mod called convert:
/// - Into from domain Payload to service MessageHub Payload
/// - TryFrom from service MessageHub Payload to domain Payload
/// - TryFromWithClient from service MessageHub MessageEvent to domain Payload
///   and client.
/// - TryFromWithClient from a service MessageHub MessageEvent option to domain
///   Payload and client.
#[macro_export]
macro_rules! payload_convert {
    ($service_payload_type:ident, $payload_type:ty) => {
        pub(super) mod convert {
            use super::*;
            use $crate::service;
            use $crate::service::TryFromWithClient;

            use std::convert::TryFrom;

            impl From<$payload_type> for service::Payload {
                fn from(payload: $payload_type) -> service::Payload {
                    paste::paste! {
                        service::Payload::[<$service_payload_type>](payload)
                    }
                }
            }

            impl TryFrom<service::Payload> for $payload_type {
                type Error = String;

                fn try_from(value: service::Payload) -> Result<Self, Self::Error> {
                    paste::paste! {
                        match value {
                            service::Payload::[<$service_payload_type>](payload) => Ok(payload),
                            _=> Err(format!("unexpected payload encountered: {:?}", value)),
                        }
                    }
                }
            }

            impl TryFrom<service::message::MessageEvent> for $payload_type {
                type Error = String;

                fn try_from(value: service::message::MessageEvent) -> Result<Self, Self::Error> {
                    paste::paste! {
                        if let service::message::MessageEvent::Message(payload, _) = value {
                            Payload::try_from(payload)
                        } else {
                            Err(String::from("wrong message type"))
                        }
                    }
                }
            }

            impl TryFromWithClient<service::message::MessageEvent> for $payload_type {
                type Error = String;

                fn try_from_with_client(
                    value: service::message::MessageEvent,
                ) -> Result<(Self, service::message::MessageClient), Self::Error> {
                    if let service::message::MessageEvent::Message(payload, client) = value {
                        Ok((Payload::try_from(payload)?, client))
                    } else {
                        Err(String::from("wrong message type"))
                    }
                }
            }
        }
    };
}

#[cfg(test)]
pub(crate) async fn build_event_listener(delegate: &message::Delegate) -> message::Receptor {
    delegate
        .messenger_builder(MessengerType::Unbound)
        .add_role(Role::Event(event::Role::Sink))
        .build()
        .await
        .expect("Should be able to retrieve receptor")
        .1
}
