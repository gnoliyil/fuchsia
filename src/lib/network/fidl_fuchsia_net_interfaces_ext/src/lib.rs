// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/110854): Provide facilities that work with a watcher
// disinterested in all address properties.
//! Extensions for the fuchsia.net.interfaces FIDL library.

#![deny(missing_docs)]

pub mod admin;
mod reachability;

pub use reachability::{is_globally_routable, to_reachability_stream, wait_for_reachability};

use fidl_fuchsia_net_interfaces as fnet_interfaces;
use fidl_table_validation::*;
use fuchsia_zircon_types as zx;
use futures::{Stream, TryStreamExt as _};
use std::collections::{
    btree_map::{self, BTreeMap},
    hash_map::{self, HashMap},
};
use std::convert::TryFrom as _;
use std::num::NonZeroU64;
use thiserror::Error;

// TODO(https://fxbug.dev/66175): Prevent this type from becoming stale.
/// Properties of a network interface.
#[derive(Clone, Debug, Eq, PartialEq, ValidFidlTable)]
#[fidl_table_src(fnet_interfaces::Properties)]
pub struct Properties {
    /// An opaque identifier for the interface. Its value will not be reused
    /// even if the device is removed and subsequently re-added. Immutable.
    pub id: NonZeroU64,
    /// The name of the interface. Immutable.
    pub name: String,
    /// The device class of the interface. Immutable.
    pub device_class: fnet_interfaces::DeviceClass,
    /// The device is enabled and its physical state is online.
    pub online: bool,
    /// The addresses currently assigned to the interface.
    pub addresses: Vec<Address>,
    /// Whether there is a default IPv4 route through this interface.
    pub has_default_ipv4_route: bool,
    /// Whether there is a default IPv6 route through this interface.
    pub has_default_ipv6_route: bool,
}

// TODO(https://fxbug.dev/66175): Prevent this type from becoming stale.
/// An address and its properties.
#[derive(Clone, Debug, Eq, Hash, PartialEq, ValidFidlTable)]
#[fidl_table_src(fnet_interfaces::Address)]
#[fidl_table_validator(AddressValidator)]
pub struct Address {
    /// The address and prefix length.
    pub addr: fidl_fuchsia_net::Subnet,
    /// The time after which the address will no longer be valid.
    ///
    /// Its value must be greater than 0. A value of zx.time.INFINITE indicates
    /// that the address will always be valid.
    // TODO(https://fxbug.dev/75531): Replace with zx::Time once there is support for custom
    // conversion functions.
    pub valid_until: zx::zx_time_t,
    /// The address's assignment state.
    pub assignment_state: fnet_interfaces::AddressAssignmentState,
}

/// Helper struct implementing Address validation.
pub struct AddressValidator;

impl Validate<Address> for AddressValidator {
    type Error = String;
    fn validate(
        Address { addr: _, valid_until, assignment_state: _ }: &Address,
    ) -> Result<(), Self::Error> {
        if *valid_until <= 0 {
            return Err(format!("non-positive value for valid_until={}", *valid_until));
        }
        Ok(())
    }
}

/// Interface watcher event update errors.
#[derive(Error, Debug)]
pub enum UpdateError {
    /// The update attempted to add an already-known added interface into local state.
    #[error("duplicate added event {0:?}")]
    DuplicateAdded(fnet_interfaces::Properties),
    /// The update attempted to add an already-known existing interface into local state.
    #[error("duplicate existing event {0:?}")]
    DuplicateExisting(fnet_interfaces::Properties),
    /// The event contained one or more invalid properties.
    #[error("failed to validate Properties FIDL table: {0}")]
    InvalidProperties(#[from] PropertiesValidationError),
    /// The event contained one or more invalid addresses.
    #[error("failed to validate Address FIDL table: {0}")]
    InvalidAddress(#[from] AddressValidationError),
    /// The event was required to have contained an ID, but did not.
    #[error("changed event with missing ID {0:?}")]
    MissingId(fnet_interfaces::Properties),
    /// The event did not contain any changes.
    #[error("changed event contains no changed fields {0:?}")]
    EmptyChange(fnet_interfaces::Properties),
    /// The update removed the only interface in the local state.
    #[error("interface has been removed")]
    Removed,
    /// The event contained changes for an interface that did not exist in local state.
    #[error("unknown interface changed {0:?}")]
    UnknownChanged(fnet_interfaces::Properties),
    /// The event removed an interface that did not exist in local state.
    #[error("unknown interface with id {0} deleted")]
    UnknownRemoved(u64),
    /// The event included an interface id = 0, which should never happen.
    #[error("encountered 0 interface id")]
    ZeroInterfaceId,
}

/// The result of updating network interface state with an event.
#[derive(Debug, PartialEq)]
pub enum UpdateResult<'a, S> {
    /// The update did not change the local state.
    NoChange,
    /// The update inserted an existing interface into the local state.
    Existing {
        /// The properties,
        properties: &'a Properties,
        /// The state.
        state: &'a mut S,
    },
    /// The update inserted an added interface into the local state.
    Added {
        /// The properties,
        properties: &'a Properties,
        /// The state.
        state: &'a mut S,
    },
    /// The update changed an existing interface in the local state.
    Changed {
        /// The previous values of any properties which changed.
        ///
        /// This is sparsely populated: none of the immutable properties are present (they can
        /// all be found on `current`), and a mutable property is present with its value pre-update
        /// iff it has changed as a result of the update.
        previous: fnet_interfaces::Properties,
        /// The properties of the interface post-update.
        current: &'a Properties,
        /// The state of the interface.
        state: &'a mut S,
    },
    /// The update removed an interface from the local state.
    Removed(PropertiesAndState<S>),
}

/// The properties and state for an interface.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PropertiesAndState<S> {
    /// Properties.
    pub properties: Properties,
    /// State.
    pub state: S,
}

/// A trait for types holding interface state that can be updated by change events.
pub trait Update<S> {
    /// Update state with the interface change event.
    ///
    /// Returns a bool indicating whether the update caused any changes.
    fn update(&mut self, event: fnet_interfaces::Event)
        -> Result<UpdateResult<'_, S>, UpdateError>;
}

impl<S> Update<S> for PropertiesAndState<S> {
    fn update(
        &mut self,
        event: fnet_interfaces::Event,
    ) -> Result<UpdateResult<'_, S>, UpdateError> {
        let Self { properties, state } = self;
        match event {
            fnet_interfaces::Event::Existing(existing) => {
                let existing = Properties::try_from(existing)?;
                if existing.id == properties.id {
                    return Err(UpdateError::DuplicateExisting(existing.into()));
                }
            }
            fnet_interfaces::Event::Added(added) => {
                let added = Properties::try_from(added)?;
                if added.id == properties.id {
                    return Err(UpdateError::DuplicateAdded(added.into()));
                }
            }
            fnet_interfaces::Event::Changed(mut change) => {
                let fnet_interfaces::Properties {
                    id,
                    name: _,
                    device_class: _,
                    online,
                    has_default_ipv4_route,
                    has_default_ipv6_route,
                    addresses,
                    ..
                } = &mut change;
                if let Some(id) = *id {
                    if properties.id.get() == id {
                        let mut changed = false;
                        macro_rules! swap_if_some {
                            ($field:ident) => {
                                if let Some($field) = $field {
                                    if properties.$field != *$field {
                                        std::mem::swap(&mut properties.$field, $field);
                                        changed = true;
                                    }
                                }
                            };
                        }
                        swap_if_some!(online);
                        swap_if_some!(has_default_ipv4_route);
                        swap_if_some!(has_default_ipv6_route);
                        if let Some(addresses) = addresses {
                            // NB The following iterator comparison assumes that the server is
                            // well-behaved and will not send a permutation of the existing
                            // addresses with no actual changes (additions or removals). Making the
                            // comparison via set equality is possible, but more expensive than
                            // it's worth.
                            // TODO(https://github.com/rust-lang/rust/issues/64295) Use `eq_by` to
                            // compare the iterators once stabilized.
                            if addresses.len() != properties.addresses.len()
                                || !addresses
                                    .iter()
                                    .zip(
                                        properties
                                            .addresses
                                            .iter()
                                            .cloned()
                                            .map(fnet_interfaces::Address::from),
                                    )
                                    .all(|(a, b)| *a == b)
                            {
                                let previous_len = properties.addresses.len();
                                // NB This is equivalent to Vec::try_extend, if such a method
                                // existed.
                                let () = properties.addresses.reserve(addresses.len());
                                for address in addresses.drain(..).map(Address::try_from) {
                                    let () = properties.addresses.push(address?);
                                }
                                let () = addresses.extend(
                                    properties.addresses.drain(..previous_len).map(Into::into),
                                );
                                changed = true;
                            }
                        }
                        if changed {
                            change.id = None;
                            return Ok(UpdateResult::Changed {
                                previous: change,
                                current: properties,
                                state,
                            });
                        } else {
                            return Err(UpdateError::EmptyChange(change));
                        }
                    }
                } else {
                    return Err(UpdateError::MissingId(change));
                }
            }
            fnet_interfaces::Event::Removed(removed_id) => {
                if properties.id.get() == removed_id {
                    return Err(UpdateError::Removed);
                }
            }
            fnet_interfaces::Event::Idle(fnet_interfaces::Empty {}) => {}
        }
        Ok(UpdateResult::NoChange)
    }
}

impl<S: Default> Update<S> for InterfaceState<S> {
    fn update(
        &mut self,
        event: fnet_interfaces::Event,
    ) -> Result<UpdateResult<'_, S>, UpdateError> {
        fn get_properties<S>(state: &mut InterfaceState<S>) -> &mut PropertiesAndState<S> {
            match state {
                InterfaceState::Known(properties) => properties,
                InterfaceState::Unknown(id) => unreachable!(
                    "matched `Unknown({})` immediately after assigning with `Known`",
                    id
                ),
            }
        }
        match self {
            InterfaceState::Unknown(id) => match event {
                fnet_interfaces::Event::Existing(existing) => {
                    let properties = Properties::try_from(existing)?;
                    if properties.id.get() == *id {
                        *self = InterfaceState::Known(PropertiesAndState {
                            properties,
                            state: S::default(),
                        });
                        let PropertiesAndState { properties, state } = get_properties(self);
                        return Ok(UpdateResult::Existing { properties, state });
                    }
                }
                fnet_interfaces::Event::Added(added) => {
                    let properties = Properties::try_from(added)?;
                    if properties.id.get() == *id {
                        *self = InterfaceState::Known(PropertiesAndState {
                            properties,
                            state: S::default(),
                        });
                        let PropertiesAndState { properties, state } = get_properties(self);
                        return Ok(UpdateResult::Added { properties, state });
                    }
                }
                fnet_interfaces::Event::Changed(change) => {
                    if let Some(change_id) = change.id {
                        if change_id == *id {
                            return Err(UpdateError::UnknownChanged(change));
                        }
                    } else {
                        return Err(UpdateError::MissingId(change));
                    }
                }
                fnet_interfaces::Event::Removed(removed_id) => {
                    if removed_id == *id {
                        return Err(UpdateError::UnknownRemoved(removed_id));
                    }
                }
                fnet_interfaces::Event::Idle(fnet_interfaces::Empty {}) => {}
            },
            InterfaceState::Known(properties) => return properties.update(event),
        }
        Ok(UpdateResult::NoChange)
    }
}

trait TryFromMaybeNonzero: Sized {
    fn try_from_maybe_nonzero(value: u64) -> Result<Self, UpdateError>;
}

impl TryFromMaybeNonzero for u64 {
    fn try_from_maybe_nonzero(value: u64) -> Result<Self, UpdateError> {
        Ok(value)
    }
}

impl TryFromMaybeNonzero for NonZeroU64 {
    fn try_from_maybe_nonzero(value: u64) -> Result<Self, UpdateError> {
        NonZeroU64::new(value).ok_or(UpdateError::ZeroInterfaceId)
    }
}

macro_rules! impl_map {
    ($map_type:ident, $map_mod:tt) => {
        impl<K, S> Update<S> for $map_type<K, PropertiesAndState<S>>
        where
            K: TryFromMaybeNonzero + Copy + From<NonZeroU64> + Eq + Ord + std::hash::Hash,
            S: Default,
        {
            fn update(
                &mut self,
                event: fnet_interfaces::Event,
            ) -> Result<UpdateResult<'_, S>, UpdateError> {
                match event {
                    fnet_interfaces::Event::Existing(existing) => {
                        let existing = Properties::try_from(existing)?;
                        match self.entry(existing.id.into()) {
                            $map_mod::Entry::Occupied(_) => {
                                Err(UpdateError::DuplicateExisting(existing.into()))
                            }
                            $map_mod::Entry::Vacant(entry) => {
                                let PropertiesAndState { properties, state } =
                                    entry.insert(PropertiesAndState {
                                        properties: existing,
                                        state: S::default(),
                                    });
                                Ok(UpdateResult::Existing { properties, state })
                            }
                        }
                    }
                    fnet_interfaces::Event::Added(added) => {
                        let added = Properties::try_from(added)?;
                        match self.entry(added.id.into()) {
                            $map_mod::Entry::Occupied(_) => {
                                Err(UpdateError::DuplicateAdded(added.into()))
                            }
                            $map_mod::Entry::Vacant(entry) => {
                                let PropertiesAndState { properties, state } =
                                    entry.insert(PropertiesAndState {
                                        properties: added,
                                        state: S::default(),
                                    });
                                Ok(UpdateResult::Added { properties, state })
                            }
                        }
                    }
                    fnet_interfaces::Event::Changed(change) => {
                        let id = if let Some(id) = change.id {
                            id
                        } else {
                            return Err(UpdateError::MissingId(change));
                        };
                        if let Some(properties) = self.get_mut(&K::try_from_maybe_nonzero(id)?) {
                            properties.update(fnet_interfaces::Event::Changed(change))
                        } else {
                            Err(UpdateError::UnknownChanged(change))
                        }
                    }
                    fnet_interfaces::Event::Removed(removed_id) => {
                        if let Some(properties) =
                            self.remove(&K::try_from_maybe_nonzero(removed_id)?)
                        {
                            Ok(UpdateResult::Removed(properties))
                        } else {
                            Err(UpdateError::UnknownRemoved(removed_id))
                        }
                    }
                    fnet_interfaces::Event::Idle(fnet_interfaces::Empty {}) => {
                        Ok(UpdateResult::NoChange)
                    }
                }
            }
        }
    };
}

impl_map!(BTreeMap, btree_map);
impl_map!(HashMap, hash_map);

/// Interface watcher operational errors.
#[derive(Error, Debug)]
pub enum WatcherOperationError<S: std::fmt::Debug, B: Update<S> + std::fmt::Debug> {
    /// Watcher event stream yielded an error.
    #[error("event stream error: {0}")]
    EventStream(fidl::Error),
    /// Watcher event stream yielded an event that could not be applied to the local state.
    #[error("failed to update: {0}")]
    Update(UpdateError),
    /// Watcher event stream ended unexpectedly.
    #[error("watcher event stream ended unexpectedly, final state: {final_state:?}")]
    UnexpectedEnd {
        /// The local state at the time of the watcher event stream's end.
        final_state: B,
        /// Marker for the state held alongside interface properties.
        marker: std::marker::PhantomData<S>,
    },
    /// Watcher event stream yielded an event with unexpected type.
    #[error("unexpected event type: {0:?}")]
    UnexpectedEvent(fnet_interfaces::Event),
}

/// Interface watcher creation errors.
#[derive(Error, Debug)]
pub enum WatcherCreationError {
    /// Proxy creation failed.
    #[error("failed to create interface watcher proxy: {0}")]
    CreateProxy(fidl::Error),
    /// Watcher acquisition failed.
    #[error("failed to get interface watcher: {0}")]
    GetWatcher(fidl::Error),
}

/// Wait for a condition on interface state to be satisfied.
///
/// Note that `stream` must be created from a watcher with interest in all
/// fields, such as one created from [`event_stream_from_state`].
///
/// With the initial state in `init`, take events from `stream` and update the state, calling
/// `predicate` whenever the state changes. When `predicate` returns `Some(T)`, yield `Ok(T)`.
///
/// Since the state passed via `init` is mutably updated for every event, when this function
/// returns successfully, the state can be used as the initial state in a subsequent call with a
/// stream of events from the same watcher.
pub async fn wait_interface<S, B, St, F, T>(
    stream: St,
    init: &mut B,
    mut predicate: F,
) -> Result<T, WatcherOperationError<S, B>>
where
    S: std::fmt::Debug + Default,
    B: Update<S> + Clone + std::fmt::Debug,
    St: Stream<Item = Result<fnet_interfaces::Event, fidl::Error>>,
    F: FnMut(&B) -> Option<T>,
{
    async_utils::fold::try_fold_while(
        stream.map_err(WatcherOperationError::EventStream),
        init,
        |acc, event| {
            futures::future::ready(match acc.update(event) {
                Ok(changed) => match changed {
                    UpdateResult::Existing { .. }
                    | UpdateResult::Added { .. }
                    | UpdateResult::Changed { .. }
                    | UpdateResult::Removed(_) => {
                        if let Some(rtn) = predicate(acc) {
                            Ok(async_utils::fold::FoldWhile::Done(rtn))
                        } else {
                            Ok(async_utils::fold::FoldWhile::Continue(acc))
                        }
                    }
                    UpdateResult::NoChange => Ok(async_utils::fold::FoldWhile::Continue(acc)),
                },
                Err(e) => Err(WatcherOperationError::Update(e)),
            })
        },
    )
    .await?
    .short_circuited()
    .map_err(|final_state| WatcherOperationError::UnexpectedEnd {
        final_state: final_state.clone(),
        marker: Default::default(),
    })
}

/// The local state of an interface's properties.
#[derive(Clone, Debug, PartialEq)]
pub enum InterfaceState<S> {
    /// Not yet known.
    Unknown(u64),
    /// Locally known.
    Known(PropertiesAndState<S>),
}

/// Wait for a condition on a specific interface to be satisfied.
///
/// Note that `stream` must be created from a watcher with interest in all
/// fields, such as one created from [`event_stream_from_state`].
///
/// With the initial state in `init`, take events from `stream` and update the state, calling
/// `predicate` whenever the state changes. When `predicate` returns `Some(T)`, yield `Ok(T)`.
///
/// Since the state passed via `init` is mutably updated for every event, when this function
/// returns successfully, the state can be used as the initial state in a subsequent call with a
/// stream of events from the same watcher.
pub async fn wait_interface_with_id<S, St, F, T>(
    stream: St,
    init: &mut InterfaceState<S>,
    mut predicate: F,
) -> Result<T, WatcherOperationError<S, InterfaceState<S>>>
where
    S: Default + Clone + std::fmt::Debug,
    St: Stream<Item = Result<fnet_interfaces::Event, fidl::Error>>,
    F: FnMut(&PropertiesAndState<S>) -> Option<T>,
{
    wait_interface(stream, init, |state| {
        match state {
            InterfaceState::Known(properties) => predicate(properties),
            // NB This is technically unreachable because a successful update will always change
            // `Unknown` to `Known` (and `Known` will stay `Known`).
            InterfaceState::Unknown(_) => None,
        }
    })
    .await
}

/// Read Existing interface events from `stream`, updating `init` until the Idle event is detected,
/// returning the resulting state.
///
/// Note that `stream` must be created from a watcher with interest in all
/// fields, such as one created from [`event_stream_from_state`].
pub async fn existing<S, St, B>(stream: St, init: B) -> Result<B, WatcherOperationError<S, B>>
where
    S: std::fmt::Debug,
    St: futures::Stream<Item = Result<fnet_interfaces::Event, fidl::Error>>,
    B: Update<S> + std::fmt::Debug,
{
    async_utils::fold::try_fold_while(
        stream.map_err(WatcherOperationError::EventStream),
        init,
        |mut acc, event| {
            futures::future::ready(match event {
                fnet_interfaces::Event::Existing(_) => match acc.update(event) {
                    Ok::<UpdateResult<'_, _>, _>(_) => {
                        Ok(async_utils::fold::FoldWhile::Continue(acc))
                    }
                    Err(e) => Err(WatcherOperationError::Update(e)),
                },
                fnet_interfaces::Event::Idle(fnet_interfaces::Empty {}) => {
                    Ok(async_utils::fold::FoldWhile::Done(acc))
                }
                fnet_interfaces::Event::Added(_)
                | fnet_interfaces::Event::Removed(_)
                | fnet_interfaces::Event::Changed(_) => {
                    Err(WatcherOperationError::UnexpectedEvent(event))
                }
            })
        },
    )
    .await?
    .short_circuited()
    .map_err(|acc| WatcherOperationError::UnexpectedEnd {
        final_state: acc,
        marker: Default::default(),
    })
}

/// The kind of addresses included from the watcher.
pub enum IncludedAddresses {
    /// All addresses are returned from the watcher.
    All,
    /// Only assigned addresses are returned rom the watcher.
    OnlyAssigned,
}

/// Initialize a watcher with interest in all fields and return its events as a
/// stream.
///
/// If `include_non_assigned_addresses` is true, then all addresses will be
/// returned, not just assigned addresses.
pub fn event_stream_from_state(
    interface_state: &fnet_interfaces::StateProxy,
    included_addresses: IncludedAddresses,
) -> Result<impl Stream<Item = Result<fnet_interfaces::Event, fidl::Error>>, WatcherCreationError> {
    let (watcher, server) = ::fidl::endpoints::create_proxy::<fnet_interfaces::WatcherMarker>()
        .map_err(WatcherCreationError::CreateProxy)?;
    let () = interface_state
        .get_watcher(
            // Register interest in all fields so that the strong validation
            // witness type can be used and the stream returned is compatible
            // with other methods in this crate.
            &fnet_interfaces::WatcherOptions {
                address_properties_interest: Some(
                    fnet_interfaces::AddressPropertiesInterest::VALID_UNTIL
                        | fnet_interfaces::AddressPropertiesInterest::PREFERRED_LIFETIME_INFO,
                ),
                include_non_assigned_addresses: Some(match included_addresses {
                    IncludedAddresses::All => true,
                    IncludedAddresses::OnlyAssigned => false,
                }),
                ..Default::default()
            },
            server,
        )
        .map_err(WatcherCreationError::GetWatcher)?;
    Ok(futures::stream::try_unfold(watcher, |watcher| async {
        Ok(Some((watcher.watch().await?, watcher)))
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use fidl_fuchsia_net as fnet;
    use futures::{task::Poll, FutureExt as _};
    use net_declare::fidl_subnet;
    use std::{cell::RefCell, convert::TryInto as _, pin::Pin, rc::Rc};
    use test_case::test_case;

    fn fidl_properties(id: u64) -> fnet_interfaces::Properties {
        fnet_interfaces::Properties {
            id: Some(id),
            name: Some("test1".to_string()),
            device_class: Some(fnet_interfaces::DeviceClass::Loopback(fnet_interfaces::Empty {})),
            online: Some(false),
            has_default_ipv4_route: Some(false),
            has_default_ipv6_route: Some(false),
            addresses: Some(vec![fidl_address(ADDR, zx::ZX_TIME_INFINITE)]),
            ..Default::default()
        }
    }

    fn validated_properties(id: u64) -> PropertiesAndState<()> {
        PropertiesAndState {
            properties: fidl_properties(id).try_into().expect("failed to validate FIDL Properties"),
            state: (),
        }
    }

    fn properties_delta(id: u64) -> fnet_interfaces::Properties {
        fnet_interfaces::Properties {
            id: Some(id),
            name: None,
            device_class: None,
            online: Some(true),
            has_default_ipv4_route: Some(true),
            has_default_ipv6_route: Some(true),
            addresses: Some(vec![fidl_address(ADDR2, zx::ZX_TIME_INFINITE)]),
            ..Default::default()
        }
    }

    fn fidl_properties_after_change(id: u64) -> fnet_interfaces::Properties {
        fnet_interfaces::Properties {
            id: Some(id),
            name: Some("test1".to_string()),
            device_class: Some(fnet_interfaces::DeviceClass::Loopback(fnet_interfaces::Empty {})),
            online: Some(true),
            has_default_ipv4_route: Some(true),
            has_default_ipv6_route: Some(true),
            addresses: Some(vec![fidl_address(ADDR2, zx::ZX_TIME_INFINITE)]),
            ..Default::default()
        }
    }

    fn validated_properties_after_change(id: u64) -> PropertiesAndState<()> {
        PropertiesAndState {
            properties: fidl_properties_after_change(id)
                .try_into()
                .expect("failed to validate FIDL Properties"),
            state: (),
        }
    }

    fn fidl_address(addr: fnet::Subnet, valid_until: zx::zx_time_t) -> fnet_interfaces::Address {
        fnet_interfaces::Address {
            addr: Some(addr),
            valid_until: Some(valid_until),
            assignment_state: Some(fnet_interfaces::AddressAssignmentState::Assigned),
            ..Default::default()
        }
    }

    const ID: u64 = 1;
    const ID2: u64 = 2;
    const ADDR: fnet::Subnet = fidl_subnet!("1.2.3.4/24");
    const ADDR2: fnet::Subnet = fidl_subnet!("5.6.7.8/24");

    #[test_case(
        &mut std::iter::once((ID, validated_properties(ID))).collect::<HashMap<_, _>>();
        "hashmap"
    )]
    #[test_case(&mut InterfaceState::Known(validated_properties(ID)); "interface_state_known")]
    #[test_case(&mut validated_properties(ID); "properties")]
    fn test_duplicate_error(state: &mut impl Update<()>) {
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Added(fidl_properties(ID))),
            Err(UpdateError::DuplicateAdded(added)) if added == fidl_properties(ID)
        );
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Existing(fidl_properties(ID))),
            Err(UpdateError::DuplicateExisting(existing)) if existing == fidl_properties(ID)
        );
    }

    #[test_case(&mut HashMap::<u64, _>::new(); "hashmap")]
    #[test_case(&mut InterfaceState::Unknown(ID); "interface_state_unknown")]
    fn test_unknown_error(state: &mut impl Update<()>) {
        let unknown =
            fnet_interfaces::Properties { id: Some(ID), online: Some(true), ..Default::default() };
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Changed(unknown.clone())),
            Err(UpdateError::UnknownChanged(changed)) if changed == unknown
        );
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Removed(ID)),
            Err(UpdateError::UnknownRemoved(id)) if id == ID
        );
    }

    #[test_case(&mut InterfaceState::Known(validated_properties(ID)); "interface_state_known")]
    #[test_case(&mut validated_properties(ID); "properties")]
    fn test_removed_error(state: &mut impl Update<()>) {
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Removed(ID)),
            Err(UpdateError::Removed)
        );
    }

    #[test_case(&mut HashMap::<u64, _>::new(); "hashmap")]
    #[test_case(&mut InterfaceState::Unknown(ID); "interface_state_unknown")]
    #[test_case(&mut InterfaceState::Known(validated_properties(ID)); "interface_state_known")]
    #[test_case(&mut validated_properties(ID); "properties")]
    fn test_missing_id_error(state: &mut impl Update<()>) {
        let missing_id = fnet_interfaces::Properties { online: Some(true), ..Default::default() };
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Changed(missing_id.clone())),
            Err(UpdateError::MissingId(properties)) if properties == missing_id
        );
    }

    #[test_case(
        &mut std::iter::once((ID, validated_properties(ID))).collect::<HashMap<_, _>>();
        "hashmap"
    )]
    #[test_case(&mut InterfaceState::Known(validated_properties(ID)); "interface_state_known")]
    #[test_case(&mut validated_properties(ID); "properties")]
    fn test_empty_change_error(state: &mut impl Update<()>) {
        let empty_change = fnet_interfaces::Properties { id: Some(ID), ..Default::default() };
        let net_zero_change =
            fnet_interfaces::Properties { name: None, device_class: None, ..fidl_properties(ID) };
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Changed(empty_change.clone())),
            Err(UpdateError::EmptyChange(properties)) if properties == empty_change
        );
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Changed(net_zero_change.clone())),
            Err(UpdateError::EmptyChange(properties)) if properties == net_zero_change
        );
    }

    #[test_case(
        &mut std::iter::once((ID, validated_properties(ID))).collect::<HashMap<_, _>>();
        "hashmap"
    )]
    #[test_case(&mut InterfaceState::Known(validated_properties(ID)); "interface_state_known")]
    #[test_case(&mut validated_properties(ID); "properties")]
    fn test_update_changed_result(state: &mut impl Update<()>) {
        let want_previous = fnet_interfaces::Properties {
            online: Some(false),
            has_default_ipv4_route: Some(false),
            has_default_ipv6_route: Some(false),
            addresses: Some(vec![fidl_address(ADDR, zx::ZX_TIME_INFINITE)]),
            ..Default::default()
        };
        assert_matches::assert_matches!(
            state.update(fnet_interfaces::Event::Changed(properties_delta(ID).clone())),
            Ok(UpdateResult::Changed { previous, current, state: _ }) => {
                assert_eq!(previous, want_previous);
                let PropertiesAndState { properties, state: () } =
                    validated_properties_after_change(ID);
                assert_eq!(*current, properties);
            }
        );
    }

    #[derive(Clone)]
    struct EventStream(Rc<RefCell<Vec<fnet_interfaces::Event>>>);

    impl Stream for EventStream {
        type Item = Result<fnet_interfaces::Event, fidl::Error>;

        fn poll_next(
            self: Pin<&mut Self>,
            _cx: &mut futures::task::Context<'_>,
        ) -> Poll<Option<Self::Item>> {
            let EventStream(events_vec) = &*self;
            if events_vec.borrow().is_empty() {
                Poll::Ready(None)
            } else {
                Poll::Ready(Some(Ok(events_vec.borrow_mut().remove(0))))
            }
        }
    }

    fn test_event_stream() -> EventStream {
        EventStream(Rc::new(RefCell::new(vec![
            fnet_interfaces::Event::Existing(fidl_properties(ID)),
            fnet_interfaces::Event::Idle(fnet_interfaces::Empty {}),
            fnet_interfaces::Event::Added(fidl_properties(ID2)),
            fnet_interfaces::Event::Changed(properties_delta(ID)),
            fnet_interfaces::Event::Changed(properties_delta(ID2)),
            fnet_interfaces::Event::Removed(ID),
            fnet_interfaces::Event::Removed(ID2),
        ])))
    }

    #[test]
    fn test_wait_one_interface() {
        let event_stream = test_event_stream();
        let mut state = InterfaceState::Unknown(ID);
        for want in &[validated_properties(ID), validated_properties_after_change(ID)] {
            let () = wait_interface_with_id(event_stream.clone(), &mut state, |got| {
                assert_eq!(got, want);
                Some(())
            })
            .now_or_never()
            .expect("wait_interface_with_id did not complete immediately")
            .expect("wait_interface_with_id error");
            assert_matches!(state, InterfaceState::Known(ref got) if got == want);
        }
    }

    fn test_wait_interface<'a, B>(state: &mut B, want_states: impl IntoIterator<Item = &'a B>)
    where
        B: 'a + Update<()> + Clone + std::fmt::Debug + std::cmp::PartialEq,
    {
        let event_stream = test_event_stream();
        for want in want_states.into_iter() {
            let () = wait_interface(event_stream.clone(), state, |got| {
                assert_eq!(got, want);
                Some(())
            })
            .now_or_never()
            .expect("wait_interface did not complete immediately")
            .expect("wait_interface error");
            assert_eq!(state, want);
        }
    }

    #[test]
    fn test_wait_interface_hashmap() {
        test_wait_interface(
            &mut HashMap::new(),
            &[
                std::iter::once((ID, validated_properties(ID))).collect::<HashMap<_, _>>(),
                [(ID, validated_properties(ID)), (ID2, validated_properties(ID2))]
                    .iter()
                    .cloned()
                    .collect::<HashMap<_, _>>(),
                [(ID, validated_properties_after_change(ID)), (ID2, validated_properties(ID2))]
                    .iter()
                    .cloned()
                    .collect::<HashMap<_, _>>(),
                [
                    (ID, validated_properties_after_change(ID)),
                    (ID2, validated_properties_after_change(ID2)),
                ]
                .iter()
                .cloned()
                .collect::<HashMap<_, _>>(),
                std::iter::once((ID2, validated_properties_after_change(ID2)))
                    .collect::<HashMap<_, _>>(),
                HashMap::new(),
            ],
        );
    }

    #[test]
    fn test_wait_interface_interface_state() {
        test_wait_interface(
            &mut InterfaceState::Unknown(ID),
            &[
                InterfaceState::Known(validated_properties(ID)),
                InterfaceState::Known(validated_properties_after_change(ID)),
            ],
        );
    }

    const ID_NON_EXISTENT: u64 = 0xffffffff;
    #[test_case(
        InterfaceState::Unknown(ID_NON_EXISTENT),
        InterfaceState::Unknown(ID_NON_EXISTENT);
        "interface_state_unknown_different_id"
    )]
    #[test_case(
        InterfaceState::Unknown(ID),
        InterfaceState::Known(validated_properties(ID));
        "interface_state_unknown")]
    #[test_case(
        HashMap::new(),
        [(ID, validated_properties(ID)), (ID2, validated_properties(ID2))]
            .iter()
            .cloned()
            .collect::<HashMap<_, _>>();
        "hashmap"
    )]
    fn test_existing<B>(state: B, want: B)
    where
        B: Update<()> + std::fmt::Debug + std::cmp::PartialEq,
    {
        let events = [
            fnet_interfaces::Event::Existing(fidl_properties(ID)),
            fnet_interfaces::Event::Existing(fidl_properties(ID2)),
            fnet_interfaces::Event::Idle(fnet_interfaces::Empty {}),
        ];
        let event_stream = futures::stream::iter(events.iter().cloned().map(Ok));
        assert_eq!(
            existing(event_stream, state)
                .now_or_never()
                .expect("existing did not complete immediately")
                .expect("existing returned error"),
            want,
        );
    }

    #[test]
    fn test_address_validator() {
        let valid = fidl_address(ADDR, 42);
        let fidl_fuchsia_net_interfaces::Address { addr, valid_until, assignment_state, .. } =
            valid.clone();
        assert_eq!(
            Address::try_from(valid).expect("failed to create address from valid fidl table"),
            Address {
                addr: addr.expect("addr field missing from valid address"),
                valid_until: valid_until.expect("valid_until field missing from valid address"),
                assignment_state: assignment_state
                    .expect("assignment_state missing from valid address"),
            }
        );
        let invalid = fidl_address(ADDR, 0);
        assert_matches!(
            Address::try_from(invalid),
            Err(AddressValidationError::Logical(e @ String { .. }))
                if e == "non-positive value for valid_until=0"
        );
    }
}
