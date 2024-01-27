// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::message::action_fuse::ActionFuseHandle;
use crate::message::beacon::Beacon;
use crate::message::message_client::MessageClient;
use crate::message::messenger::MessengerClient;
use crate::message::receptor::Receptor;
use futures::channel::mpsc::UnboundedSender;
use futures::channel::oneshot::Sender;
use std::collections::HashSet;
use std::fmt::Debug;
use std::hash::Hash;
use thiserror::Error;

/// Trait alias for types of data that can be used as the payload in a
/// MessageHub.
pub trait Payload: Clone + Debug + Send + Sync {}
impl<T: Clone + Debug + Send + Sync> Payload for T {}

/// Trait alias for types of data that can be used as an address in a
/// MessageHub.
pub trait Address: Copy + Debug + Eq + Hash + Unpin + Send + Sync {}
impl<T: Copy + Debug + Eq + Hash + Unpin + Send + Sync> Address for T {}

/// A mod for housing common definitions for messengers. Messengers are
/// MessageHub participants, which are capable of sending and receiving
/// messages.
pub(super) mod messenger {
    use super::MessengerType;
    use std::collections::HashSet;

    pub type Roles = HashSet<crate::Role>;

    /// `Descriptor` is a blueprint for creating a messenger. It is sent to the
    /// MessageHub by clients, which interprets the information to build the
    /// messenger.
    #[derive(Clone)]
    pub struct Descriptor {
        /// The type of messenger to be created. This determines how messages
        /// can be directed to a messenger created from this Descriptor.
        /// Please reference [Audience](crate::message::base::Audience) to see how these types map
        /// to audience targets.
        pub messenger_type: MessengerType,
        /// The roles to associate with this messenger. When a messenger
        /// is associated with a given [`Role`], any message directed to that
        /// [`Role`] will be delivered to the messenger.
        pub roles: Roles,
    }
}

/// A MessageEvent defines the data that can be returned through a message
/// receptor.
#[derive(Debug, PartialEq)]
pub enum MessageEvent {
    /// A message that has been delivered, either as a new message directed at to
    /// the recipient's address or a reply to a previously sent message
    /// (dependent on the receptor's context).
    Message(crate::Payload, MessageClient),
    /// A status update for the message that spawned the receptor delivering this
    /// update.
    Status(Status),
}

/// This mod contains the default type definitions for the MessageHub's type
/// parameters when not specified.
pub mod default {
    /// `Address` provides a [`Address`] definition for message hubs not needing
    /// an address.
    #[derive(PartialEq, Copy, Clone, Debug, Eq, Hash)]
    pub enum Address {}
}

#[derive(Error, Debug, Clone)]
pub enum MessageError {
    #[error("Address conflig:{address:?} already exists")]
    AddressConflict { address: crate::Address },
    #[error("Unexpected Error")]
    Unexpected,
}

/// The types of results possible from sending or replying.
#[derive(Copy, Clone, PartialEq, Debug)]
pub enum Status {
    // Sent to some audience, potentially no one.
    Broadcasted,
    // Received by the intended address.
    Received,
    // Could not be delivered to the specified target.
    Undeliverable,
    // Acknowledged by a recipient.
    Acknowledged,
    Timeout,
}

/// The intended recipients for a message.
#[derive(Clone, Debug, PartialEq, Hash, Eq)]
pub enum Audience {
    // All non-broker messengers outside of the sender.
    Broadcast,
    // The messenger at the specified address.
    Address(crate::Address),
    // The messenger with the specified signature.
    Messenger(Signature),
    // A messenger who belongs to the specified role.
    Role(crate::Role),
}

impl Audience {
    pub fn flatten(&self) -> HashSet<Audience> {
        [self.clone()].into()
    }
}

/// An identifier that can be used to send messages directly to a Messenger.
/// Included with Message instances.
#[derive(Copy, Clone, Debug, PartialEq, Hash, Eq)]
pub enum Signature {
    // Messenger at a given address.
    Address(crate::Address),

    // The messenger cannot be directly addressed.
    Anonymous(MessengerId),
}

#[derive(Copy, Clone, Debug)]
pub struct Fingerprint {
    pub id: MessengerId,
    pub signature: Signature,
}

/// The messengers that can participate in messaging
#[derive(Clone, Debug)]
pub enum MessengerType {
    /// An endpoint in the messenger graph. Can have messages specifically
    /// addressed to it and can author new messages.
    Addressable(crate::Address),
    /// A intermediary messenger. Will receive every forwarded message. Brokers
    /// are able to send and reply to messages, but the main purpose is to observe
    /// messages. An optional filter may be specified, which limits the messages
    /// directed to this broker.
    Broker(Option<filter::Filter>),
    /// A messenger that cannot be reached by an address.
    Unbound,
}

pub mod filter {
    use super::{Audience, Message, MessageType, Signature};
    use core::fmt::{Debug, Formatter};
    use std::sync::Arc;

    /// `Condition` allows specifying a filter condition that must be true
    /// for a filter to match.
    #[derive(Clone)]
    pub enum Condition {
        /// Matches on the message's intended audience as specified by the
        /// sender.
        Audience(Audience),
        /// Matches on the author's signature.
        Author(Signature),
        /// Matches on a custom closure that may evaluate the sent message.
        #[allow(clippy::type_complexity)]
        Custom(Arc<dyn Fn(&Message) -> bool + Send + Sync>),
        /// Matches on another filter and its conditions.
        Filter(Filter),
    }

    /// We must implement Debug since the `Condition::Custom` does not provide
    /// a `Debug` implementation.
    impl Debug for Condition {
        fn fmt(&self, f: &mut Formatter<'_>) -> core::fmt::Result {
            let condition = match self {
                Condition::Audience(audience) => format!("audience:{audience:?}"),
                Condition::Author(signature) => format!("author:{signature:?}"),
                Condition::Custom(_) => "custom".to_string(),
                Condition::Filter(filter) => format!("filter:{filter:?}"),
            };

            write!(f, "Condition: {condition:?}")
        }
    }

    /// `Conjugation` dictates how multiple conditions are combined to determine
    /// a match.
    #[derive(Clone, Debug, PartialEq)]
    pub enum Conjugation {
        /// All conditions must match.
        All,
        /// Any condition may declare a match.
        Any,
    }

    /// `Builder` provides a convenient way to construct a [`Filter`] based on
    /// a number of conditions.
    pub struct Builder {
        conjugation: Conjugation,
        conditions: Vec<Condition>,
    }

    impl Builder {
        pub(crate) fn new(condition: Condition, conjugation: Conjugation) -> Self {
            Self { conjugation, conditions: vec![condition] }
        }

        /// Shorthand method to create a filter based on a single condition.
        pub(crate) fn single(condition: Condition) -> Filter {
            Builder::new(condition, Conjugation::All).build()
        }

        /// Adds an additional condition to the filter under construction.
        pub(crate) fn append(mut self, condition: Condition) -> Self {
            self.conditions.push(condition);

            self
        }

        pub(crate) fn build(self) -> Filter {
            Filter { conjugation: self.conjugation, conditions: self.conditions }
        }
    }

    /// `Filter` is used by the `MessageHub` to determine whether an incoming
    /// message should be directed to associated broker.
    #[derive(Clone, Debug)]
    pub struct Filter {
        conjugation: Conjugation,
        conditions: Vec<Condition>,
    }

    impl Filter {
        pub(crate) fn matches(&self, message: &Message) -> bool {
            for condition in &self.conditions {
                let match_found = match condition {
                    Condition::Audience(audience) => matches!(
                            message.get_type(),
                            MessageType::Origin(target) if target == audience),
                    Condition::Custom(check_fn) => (check_fn)(message),
                    Condition::Filter(filter) => filter.matches(message),
                    Condition::Author(signature) => message.get_author().eq(signature),
                };
                if match_found {
                    if self.conjugation == Conjugation::Any {
                        return true;
                    }
                } else if self.conjugation == Conjugation::All {
                    return false;
                }
            }

            self.conjugation == Conjugation::All
        }
    }
}

/// MessageType captures details about the Message's source.
#[derive(Clone, Debug)]
pub enum MessageType {
    /// A completely new message that is intended for the specified audience.
    Origin(Audience),
    /// A response to a previously received message. Note that the value must
    /// be boxed to mitigate recursive sizing issues as MessageType is held by
    /// Message.
    Reply(Box<Message>),
}

/// `Attribution` describes the relationship of the message path in relation
/// to the author.
#[derive(Clone, Debug)]
pub enum Attribution {
    /// `Source` attributed messages are the original messages to be sent on a
    /// path. For example, a source attribution for an origin message type will
    /// be authored by the original sender. In a reply message type, a source
    /// attribution means the reply was authored by the original message's
    /// intended target.
    Source(MessageType),
    /// `Derived` attributed messages are messages that have been modified by
    /// someone in the message path. They follow the same trajectory (audience
    /// or return path), but their message has been altered. The supplied
    /// signature is the messenger that modified the specified message.
    Derived(Box<Message>, Signature),
}

/// The core messaging unit. A Message may be annotated by messengers, but is
/// not associated with a particular Messenger instance.
#[derive(Clone, Debug)]
pub struct Message {
    author: Fingerprint,
    payload: crate::Payload,
    attribution: Attribution,
    // The return path is generated while the message is passed from messenger
    // to messenger on the way to the intended recipient. It indicates the
    // messengers that would like to be informed of replies to this message.
    // The message author is always the last element in this vector. New
    // participants are pushed to the front.
    return_path: Vec<Beacon>,
}

impl Message {
    /// Returns a new Message instance. Only the MessageHub can mint new messages.
    pub(super) fn new(
        author: Fingerprint,
        payload: crate::Payload,
        attribution: Attribution,
    ) -> Message {
        let mut return_path = vec![];

        // A derived message adopts the return path of the original message.
        if let Attribution::Derived(message, _) = &attribution {
            return_path.extend(message.get_return_path().iter().cloned());
        }

        Message { author, payload, attribution, return_path }
    }

    /// Adds an entity to be notified on any replies.
    pub(super) fn add_participant(&mut self, participant: Beacon) {
        self.return_path.insert(0, participant);
    }

    /// Returns the Signatures of messengers who have modified this message
    /// through propagation.
    #[cfg(test)]
    pub(super) fn get_modifiers(&self) -> Vec<Signature> {
        let mut modifiers = vec![];

        if let Attribution::Derived(origin, signature) = &self.attribution {
            modifiers.push(*signature);
            modifiers.extend(origin.get_modifiers());
        }

        modifiers
    }

    pub(crate) fn get_author(&self) -> Signature {
        match &self.attribution {
            Attribution::Source(_) => self.author.signature,
            Attribution::Derived(message, _) => message.get_author(),
        }
    }

    /// Binds the action fuse to the author's receptor. The fuse will fire
    /// once that receptor is released.
    pub(super) async fn bind_to_author(&mut self, fuse: ActionFuseHandle) {
        if let Some(beacon) = self.return_path.last_mut() {
            beacon.add_fuse(fuse).await;
        }
    }

    /// Returns the list of participants for the reply return path.
    pub(super) fn get_return_path(&self) -> &Vec<Beacon> {
        &self.return_path
    }

    /// Returns the message's attribution, which identifies whether it has been modified by a source
    /// other than the original author.
    pub(crate) fn get_attribution(&self) -> &Attribution {
        &self.attribution
    }

    /// Returns the message's type.
    pub(crate) fn get_type(&self) -> &MessageType {
        match &self.attribution {
            Attribution::Source(message_type) => message_type,
            Attribution::Derived(message, _) => message.get_type(),
        }
    }

    /// Returns a reference to the message's payload.
    pub(crate) fn payload(&self) -> &crate::Payload {
        &self.payload
    }

    /// Delivers the supplied status to all participants in the return path.
    pub(super) async fn report_status(&self, status: Status) {
        for beacon in &self.return_path {
            // It's ok if the other end is already closed and does not get an update, so we can
            // safely ignore the result here.
            let _ = beacon.status(status).await;
        }
    }
}

/// Type definition for a sender handed by the MessageHub to messengers to
/// send actions.
pub(super) type ActionSender = UnboundedSender<(Fingerprint, MessageAction, Option<Beacon>)>;

/// An internal identifier used by the MessageHub to identify messengers.
pub(super) type MessengerId = usize;

/// An internal identifier used by the `MessageHub` to identify `MessageClient`.
pub(super) type MessageClientId = usize;

pub(super) type CreateMessengerResult = Result<(MessengerClient, Receptor), MessageError>;

/// Callback for handing back a messenger
pub(super) type MessengerSender = Sender<CreateMessengerResult>;

/// Callback for checking on messenger presence
#[cfg(test)]
pub(super) type MessengerPresenceSender = Sender<MessengerPresenceResult>;
#[cfg(test)]
pub(super) type MessengerPresenceResult = Result<bool, MessageError>;

/// Type definition for a sender handed by the MessageHub to spawned components
/// (messenger factories and messengers) to control messengers.
pub(super) type MessengerActionSender = UnboundedSender<MessengerAction>;

/// Internal representation of possible actions around a messenger.
pub(super) enum MessengerAction {
    /// Creates a top level messenger
    Create(messenger::Descriptor, MessengerSender, MessengerActionSender),
    #[cfg(test)]
    /// Check whether a messenger exists for the given [`Signature`]
    CheckPresence(Signature, MessengerPresenceSender),
    /// Deletes a messenger by its [`Signature`]
    DeleteBySignature(Signature),
}

/// Internal representation for possible actions on a message.
#[derive(Debug)]
pub(super) enum MessageAction {
    // A new message sent to the specified audience.
    Send(crate::Payload, Attribution),
    // The message has been forwarded by the current holder.
    Forward(Message),
}
