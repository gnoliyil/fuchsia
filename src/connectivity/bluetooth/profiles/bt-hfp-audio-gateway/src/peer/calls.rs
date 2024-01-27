// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::calls::{
        call_list::CallList,
        types::{CallHeld, CallIndicators, CallIndicatorsUpdates},
    },
    crate::{
        error::{CallError, Error},
        inspect::CallEntryInspect,
        peer::procedure::{dtmf::DtmfCode, hold::CallHoldAction},
    },
    async_utils::{
        hanging_get::client::HangingGetStream,
        stream::{StreamItem, StreamMap, StreamWithEpitaph, Tagged, WithEpitaph, WithTag},
    },
    fidl_fuchsia_bluetooth_hfp::{
        CallAction as FidlCallAction, CallProxy, NextCall, PeerHandlerProxy, RedialLast,
        TransferActive,
    },
    fuchsia_async as fasync, fuchsia_inspect as inspect,
    fuchsia_inspect_derive::{AttachError, Inspect},
    futures::stream::{FusedStream, Stream, StreamExt},
    std::{
        convert::{TryFrom, TryInto},
        pin::Pin,
        task::{Context, Poll},
    },
    tracing::{debug, info, warn},
};

pub use {fidl_fuchsia_bluetooth_hfp::CallState, number::Number, types::Direction};

/// Defines the types associated with a phone number.
pub mod number;

/// Defines the collection used to identify and store calls.
mod call_list;

/// Defines commonly used types when interacting with a call.
pub mod types;

mod pending;

/// The index associated with a call, that is guaranteed to be unique for the lifetime of the call,
/// but will be recycled after the call is released.
pub type CallIdx = usize;

/// Internal state and resources associated with a single call.
struct CallEntry {
    /// Proxy associated with this call.
    proxy: CallProxy,
    /// The remote party's number.
    number: Number,
    /// Current state.
    state: CallState,
    /// Time of the last update to the call's `state`.
    state_updated_at: fasync::Time,
    /// Direction of the call.
    direction: Direction,
    /// Inspect node
    inspect: CallEntryInspect,
}

impl TryFrom<NextCall> for CallEntry {
    type Error = Error;

    fn try_from(next: NextCall) -> Result<Self, Self::Error> {
        match next {
            NextCall {
                call: Some(c), remote: Some(n), state: Some(s), direction: Some(d), ..
            } => {
                let proxy = c.into_proxy()?;
                Ok(CallEntry::new(proxy, n.into(), s, d.into()))
            }
            _ => Err(Error::MissingParameter("Missing fidl in NextCall table".into())),
        }
    }
}

impl Inspect for &mut CallEntry {
    fn iattach(self, parent: &inspect::Node, name: impl AsRef<str>) -> Result<(), AttachError> {
        let _ = self.inspect.iattach(parent, name)?;
        self.inspect.set_number(self.number.clone());
        self.inspect.set_call_direction(self.direction);
        self.inspect.set_call_state(self.state);
        Ok(())
    }
}

impl CallEntry {
    pub fn new(proxy: CallProxy, number: Number, state: CallState, direction: Direction) -> Self {
        let state_updated_at = fasync::Time::now();
        Self {
            proxy,
            number,
            state,
            state_updated_at,
            direction,
            inspect: CallEntryInspect::default(),
        }
    }

    /// Update the state. `state_updated_at` is changed only if self.state != state.
    pub fn set_state(&mut self, state: CallState) {
        if self.state != state {
            self.state_updated_at = fasync::Time::now();
            self.state = state;
            self.inspect.set_call_state(state);
        }
    }

    // TODO (fxb/64550): Remove when call requests are initiated
    #[allow(unused)]
    pub fn is_active(&self) -> bool {
        self.state == CallState::OngoingActive
    }
}

/// The current state of a call.
#[derive(Debug, Clone, PartialEq)]
pub struct Call {
    /// Unique identifier associated with a call for the lifetime of the call.
    /// Once the state is `Terminated` or `TransferredToAg` the index may be reused by another
    /// call.
    pub index: CallIdx,
    /// Remote party's number.
    pub number: Number,
    /// Current state.
    pub state: CallState,
    /// Direction of the call.
    pub direction: Direction,
}

impl Call {
    pub fn new(index: CallIdx, number: Number, state: CallState, direction: Direction) -> Self {
        Self { index, number, state, direction }
    }
}

/// Return the `fn` that should be used to request that the Call Manager put a call into `state`.
///
/// `state` must be a state that is valid to request of the Call Manager, `OngoingActive`,
/// `OngoingHeld`, `Terminated`, or `TransferredToAg`.
fn call_state_to_request(
    state: CallState,
) -> Result<Box<dyn FnOnce(&CallProxy) -> Result<(), fidl::Error>>, anyhow::Error> {
    Ok(match state {
        CallState::OngoingActive => Box::new(CallProxy::request_active),
        CallState::OngoingHeld => Box::new(CallProxy::request_hold),
        CallState::Terminated => Box::new(CallProxy::request_terminate),
        CallState::TransferredToAg => Box::new(CallProxy::request_transfer_audio),
        _ => anyhow::bail!("{:?} is not a valid state to request of the Call Manager", state),
    })
}

/// A stream of updates to the state of calls. Each update contains the `Number` and the
/// `CallState`. When the channel for a given call is closed, an epitaph is returned with the
/// `Number`.
/// The `Number` uniquely identifies a call. TODO (fxbug.dev/64558): Handle multi-party calls.
type CallStateUpdates = StreamMap<
    CallIdx,
    StreamWithEpitaph<Tagged<CallIdx, HangingGetStream<CallProxy, CallState>>, CallIdx>,
>;

/// Manages the state of all ongoing calls reported by the call manager.
///
/// ### Fetching Call information.
///
/// `Calls` provides methods for interacting with specific calls or listing all calls. These methods
/// can be used to query information about a call such as the associated `Number` or the
/// `CallState`.
///
///
/// ### Requesting Action on Calls
///
/// `Calls` can be used to request that a new call be made or for a call's state to be changed. Any
/// request to change the state of a call cannot be handled directly by `Calls`. Instead the request
/// will be forwarded to the Call Manager service for handling.
///
/// The state of a call is not updated to reflect a request until the Call Manager notifies `Calls`
/// that a change has taken effect. A client of `Calls` cannot assume that a request succeeded
/// until the status has been updated in `Calls`.
///
///
/// ### Calls as Stream of Call Status Updates.
///
/// Clients that are interested in changes to Call Status Updates should poll `Calls`' Stream
/// implementation. A Vec of `Indicator`s will be produced as a stream item whenever the status
/// of at least one call related `Indicator` has changed since the last stream item was produced.
///
/// The returned Vec has both Ordering and Uniqueness guarantees.
///
/// It is guaranteed to be ordered such that Indicator::Call values come before Indicator::CallSetup
/// values and Indicator::CallSetup values come before Indicator::CallHeld values.
///
/// This Vec is also is guaranteed to have no more than one value for each `Indicator` variant
/// which represents the current state of the calls as reported by the Call Manager.
pub(crate) struct Calls {
    /// A Stream of new calls.
    new_calls: Option<HangingGetStream<PeerHandlerProxy, NextCall>>,
    /// Store the current state and associated resources of every Call.
    current_calls: CallList<CallEntry>,
    /// A Stream of all updates to the state of ongoing calls.
    call_updates: CallStateUpdates,
    /// Tracks groups of pending call state changes that have been requested
    /// by the HF but have not been confirmed by the Call Manager.
    pending: pending::CallChanges,
    /// The last set of indicator values returned from Polling Calls as a Stream.
    reported_indicators: CallIndicators,
    /// The Calls Stream terminated state.
    terminated: bool,
    /// Root inspect for all Calls
    inspect_node: inspect::Node,
}

impl Inspect for &mut Calls {
    fn iattach(self, parent: &inspect::Node, name: impl AsRef<str>) -> Result<(), AttachError> {
        self.inspect_node = parent.create_child(name.as_ref());
        Ok(())
    }
}

impl Calls {
    pub fn new(proxy: Option<PeerHandlerProxy>) -> Self {
        let new_calls = proxy.map(|proxy| {
            HangingGetStream::new_with_fn_ptr(proxy, PeerHandlerProxy::watch_next_call)
        });
        Self {
            new_calls,
            current_calls: CallList::default(),
            call_updates: CallStateUpdates::empty(),
            pending: Default::default(),
            reported_indicators: CallIndicators::default(),
            terminated: false,
            inspect_node: Default::default(),
        }
    }

    /// Insert a new call. Returns the index of the call inserted.
    fn handle_new_call(&mut self, call: NextCall) -> Result<CallIdx, Error> {
        let mut call: CallEntry = call.try_into()?;
        if let Err(e) = call.iattach(&self.inspect_node, inspect::unique_name("call_")) {
            warn!("Failed to attach CallEntryInspect to Calls: {}", e)
        }
        let proxy = call.proxy.clone();
        let index = self.current_calls.insert(call);
        let call_state = HangingGetStream::new_with_fn_ptr(proxy, CallProxy::watch_state);
        if self.call_updates.insert(index, call_state.tagged(index).with_epitaph(index)).is_some() {
            warn!("Replaced call at {} with a new call?", index)
        }
        self.pending.should_report = true;
        self.terminated = false;
        Ok(index)
    }

    /// Check and mark this stream "terminated" if the appropriate conditions are met.
    /// Specifically, Calls is terminated if both `new_calls` and `call_updates` cannot produce any
    /// new values.
    fn check_termination_condition(&mut self) {
        self.terminated = self.new_calls.is_none() && self.call_updates.inner().is_empty();
    }

    /// Run `f`, passing the CallProxy associated with the given `number` into `f` and removing
    /// the CallProxy from the map if an error is returned by running `f`.
    ///
    /// Successful call requests are recorded as pending. If `txn` is Some, they are recorded
    /// to that Transaction, otherwise they are recorded directly to a new Transaction.
    ///
    /// Panics: Invalid call `state` passed to `send_call_state` will result in a panic.
    fn send_call_request(
        &mut self,
        index: CallIdx,
        state: CallState,
        txn: Option<&mut pending::Transaction>,
    ) -> Result<(), CallError> {
        let call = self.current_calls.get(index).ok_or(CallError::UnknownIndexError(index))?;
        let request = call_state_to_request(state).expect("Invalid call state request");

        match (request)(&call.proxy) {
            Ok(()) => {
                // Record the call as "pending" if the request to the Call Manager succeeds.
                match txn {
                    Some(txn) => txn.pending_call(index, state),
                    None => self.pending.pending_call(index, state),
                }
            }
            Err(e) => {
                // TODO(fxbug.dev/113160): Warn unconditionally once one-ways hide PEER_CLOSED.
                if !e.is_closed() {
                    warn!("Error making request on Call channel for call {:?}: {}", index, e);
                }
                // Call channel is unreachable, so we expect to record a terminated state.
                self.pending.pending_call(index, CallState::Terminated);
                self.remove_call(index);
            }
        }

        Ok(())
    }

    /// Send a request to the call manager to place the call on hold.
    fn request_hold(&mut self, index: CallIdx) -> Result<(), CallError> {
        let desired_state = CallState::OngoingHeld;
        self.send_call_request(index, desired_state, None)
    }

    /// Send a request to the call manager to make the call active.
    /// Only one call can be active at a time. Before requesting the call at `index` be made
    /// active, all active calls are either terminated or placed on hold depending on
    /// `terminate_others`.
    pub fn request_active(
        &mut self,
        index: CallIdx,
        terminate_others: bool,
    ) -> Result<(), CallError> {
        let desired_state = CallState::OngoingActive;
        let desired_others =
            if terminate_others { CallState::Terminated } else { CallState::OngoingHeld };

        // Collect active_calls into a Vec to avoid double borrowing self.
        let active_calls: Vec<_> = self
            .current_calls
            .calls()
            .filter_map(|(i, call)| (i != index && call.is_active()).then(|| i))
            .collect();

        let mut txn = pending::Transaction::default();

        for i in active_calls {
            // Failures are ignored.
            let _ = self.send_call_request(i, desired_others, Some(&mut txn));
        }

        self.send_call_request(index, desired_state, Some(&mut txn))?;
        self.pending.pending_txn(txn);
        Ok(())
    }

    /// Send a request to the call manager to terminate the call.
    pub fn request_terminate(&mut self, index: CallIdx) -> Result<(), CallError> {
        let desired_state = CallState::Terminated;
        self.send_call_request(index, desired_state, None)
    }

    /// Send a request to the call manager to transfer the audio of the call from the
    /// headset to the fuchsia device.
    fn request_transfer_audio(&mut self, index: CallIdx) -> Result<(), CallError> {
        let desired_state = CallState::TransferredToAg;
        self.send_call_request(index, desired_state, None)
    }

    /// Send a dtmf code to the call manager for active call,
    pub async fn send_dtmf_code(&mut self, code: DtmfCode) -> Result<(), ()> {
        let idx = self.oldest_by_state(CallState::OngoingActive).ok_or(())?.0;
        let call = self.current_calls.get(idx).expect("Index was just determined to be present");
        match call.proxy.send_dtmf_code(code.into()).await {
            Ok(Ok(())) => Ok(()),
            Ok(Err(status)) => {
                info!("Dtmf Code could not be sent to Call {}: {}", idx, status);
                Err(())
            }
            Err(e) => {
                self.remove_call(idx);
                if e.is_closed() {
                    info!("Dtmf Code could not be sent: Call {} removed", idx);
                } else {
                    warn!("Dtmf Code could not be sent: Call {} removed. Error: {}", idx, e);
                }
                Err(())
            }
        }
    }

    /// Return a Vec of the current call state for every call that `Calls` is tracking.
    pub fn current_calls(&self) -> Vec<Call> {
        self.current_calls
            .calls()
            .map(|(index, call)| Call::new(index, call.number.clone(), call.state, call.direction))
            .collect()
    }

    /// Return true if at least one call is in the Active state.
    pub fn is_call_active(&self) -> bool {
        self.calls().any(|c| c.1.state == CallState::OngoingActive)
    }

    /// Return true if at least one call is in the TransferredToAg state.
    pub fn is_call_transferred_to_ag(&self) -> bool {
        self.calls().any(|c| c.1.state == CallState::TransferredToAg)
    }

    /// Return true if at least one call is in the Held state.
    #[cfg(test)]
    pub fn is_call_held(&self) -> bool {
        self.calls().any(|c| c.1.state == CallState::OngoingHeld)
    }

    /// Remove all references to the call assigned to `index`.
    /// Removing a call signals that it has been terminated by the Call Manager
    /// so a CallState::Terminated is recorded to pending calls.
    fn remove_call(&mut self, index: CallIdx) {
        self.pending.record(index, CallState::Terminated);
        drop(self.call_updates.remove(&index));
        drop(self.current_calls.remove(index));
    }

    /// Iterator over all calls in `state`.
    fn all_by_state(
        &self,
        state: CallState,
    ) -> impl Iterator<Item = (CallIdx, &CallEntry)> + Clone {
        self.calls().filter(move |c| c.1.state == state)
    }

    /// Operations that act on calls in a particular state should act on the call that was put into
    /// that state first. `oldest_by_state` allows querying for a call that meets that criteria.
    fn oldest_by_state(&self, state: CallState) -> Option<(CallIdx, &CallEntry)> {
        self.all_by_state(state).min_by_key(|c| c.1.state_updated_at)
    }

    /// Find the first state in the list of `states` that contains a call in that state, and return
    /// the oldest call in that state if there are more than one call in the state.
    fn first_of_oldest_by_states(&self, states: &[CallState]) -> Option<(CallIdx, &CallEntry)> {
        states.iter().filter_map(|state| self.oldest_by_state(*state)).next()
    }

    /// Return the Call that has been in the IncomingRinging call state the longest if at least one
    /// exists.
    pub fn ringing(&self) -> Option<Call> {
        self.oldest_by_state(CallState::IncomingRinging)
            .map(|(idx, call)| Call::new(idx, call.number.clone(), call.state, call.direction))
    }

    /// Get the Call that has been waiting the longest. Returns None if there are no waiting calls.
    pub fn waiting(&self) -> Option<Call> {
        self.oldest_by_state(CallState::IncomingWaiting)
            .map(|(idx, call)| Call::new(idx, call.number.clone(), call.state, call.direction))
    }

    /// Answer a single call based on the following policy:
    ///
    ///   * The oldest Incoming Ringing call is answered.
    ///   * If there are no Incoming Ringing calls, the oldest Incoming Waiting call is answered.
    ///   * If there are no calls in any of the previously specified states, return an Error.
    // Incoming Waiting is prioritized last because implementations should be using the AT+CHLD
    // command to manage waiting calls instead of the ATA command.
    pub fn answer(&mut self) -> Result<(), CallError> {
        use CallState::*;
        let states = vec![IncomingRinging, IncomingWaiting];
        let idx = self.first_of_oldest_by_states(&states).ok_or(CallError::None(states))?.0;
        self.request_active(idx, true)
    }

    /// Transfer the current OngoingActive call to the AG.
    pub fn transfer_to_ag(&mut self) -> Result<(), CallError> {
        let states = vec![CallState::OngoingActive];
        let idx = self.first_of_oldest_by_states(&states).ok_or(CallError::None(states))?.0;
        self.request_transfer_audio(idx)
    }

    /// Transfer the current TransferredToAg call to the HF.
    pub fn transfer_to_hf(&mut self) -> Result<(), CallError> {
        let states = vec![CallState::TransferredToAg];
        let idx = self.first_of_oldest_by_states(&states).ok_or(CallError::None(states))?.0;
        self.request_active(idx, /* terminate others */ false)
    }

    /// Terminate a call.
    ///
    /// The AT+CHUP command does not specify a particular call to be terminated. When there are
    /// multiple calls that could be terminated, the Audio Gateway must make a determination as to
    /// which call to terminate. This prioritization is based on the perceived urgency of a call in
    /// a given state.
    ///
    /// Terminate a single call based on the following policy:
    ///
    ///   * The oldest Incoming Ringing call is terminated.
    ///   * If there are no Incoming Ringing calls, the oldest Outgoing Dialing call is terminated.
    ///   * If there are no Outgoing Dialing calls, the oldest Outgoing Alerting call is terminated.
    ///   * If there are no Outgoing Alerting calls, the oldest Ongoing Active call is terminated.
    ///   * If there are no Ongoing Active calls, the oldest Ongoing Held call is terminated.
    ///   * If there are no Ongoing Held calls, the oldest Incoming Waiting call is terminated.
    ///   * If there are no calls in any of the previously specified states, return an Error.
    // If it becomes desirable for this policy to be configurable, it should be pulled out into the
    // component's configuration data.
    //
    // Incoming Waiting is prioritized last because implementations should be using the AT+CHLD
    // command to manage waiting calls instead of the AT+CHUP command.
    pub fn hang_up(&mut self) -> Result<(), CallError> {
        use CallState::*;
        let states = vec![
            IncomingRinging,
            OutgoingDialing,
            OutgoingAlerting,
            OngoingActive,
            OngoingHeld,
            IncomingWaiting,
        ];
        let idx = self.first_of_oldest_by_states(&states).ok_or(CallError::None(states))?.0;
        self.request_terminate(idx)
    }

    /// Perform the supplementary call service related to held or waiting calls specified by
    /// `action` received from the HF. See HFP v1.8, Section 4.22 for more information.
    ///
    /// Waiting calls are prioritized over held calls when an action operates on a specific call.
    /// Per 4.34.2 AT+CHLD: "Where both a held and a waiting call exist, the above procedures shall
    /// apply to the waiting call (i.e., not to the held call) in conflicting situation."
    ///
    /// Returns an error if the CallIdx associated with a specific action is invalid.
    pub fn hold(&mut self, action: CallHoldAction) -> Result<(), CallError> {
        use {CallHoldAction::*, CallState::*};
        match action {
            ReleaseAllHeld => {
                let waiting_calls: Vec<_> =
                    self.all_by_state(IncomingWaiting).map(|c| c.0).collect();
                let held_calls: Vec<_> = self.all_by_state(OngoingHeld).map(|c| c.0).collect();

                // Terminated state on incoming waiting calls is prioritized over held calls.
                let to_release = if !waiting_calls.is_empty() { waiting_calls } else { held_calls };

                for idx in to_release {
                    let _ = self.request_terminate(idx);
                }
            }
            ReleaseAllActive => {
                // Active state on incoming waiting calls is prioritized over held calls.
                let idx = self
                    .oldest_by_state(IncomingWaiting)
                    .or_else(|| self.oldest_by_state(OngoingHeld))
                    .ok_or(CallError::None(vec![IncomingWaiting, OngoingHeld]))?
                    .0;

                self.request_active(idx, true)?;
            }
            ReleaseSpecified(idx) => self.request_terminate(idx)?,
            HoldActiveAndAccept => {
                // Active state on incoming waiting calls is prioritized over held calls.
                let idx = self
                    .oldest_by_state(IncomingWaiting)
                    .or_else(|| self.oldest_by_state(OngoingHeld))
                    .ok_or(CallError::None(vec![IncomingWaiting, OngoingHeld]))?
                    .0;

                self.request_active(idx, false)?;
            }
            HoldAllExceptSpecified(idx) => self.request_active(idx, false)?,
        }
        Ok(())
    }

    // If the HF makes an outgoing call while another call is active, this call is put on hold.
    // This behavior is implicitly implied by making the outgoing call rather than controlled by the
    // HF, so there is no CallHoldAction.
    // See HFP v1.8 4.22.2.
    pub fn hold_active(&mut self) -> Result<(), CallError> {
        let active_calls =
            self.all_by_state(CallState::OngoingActive).map(|c| c.0).collect::<Vec<_>>();
        for idx in active_calls {
            self.request_hold(idx)?
        }
        Ok(())
    }

    /// Returns true if the state of any calls requires ringing and there are no
    /// ongoing calls.
    pub fn should_ring(&self) -> bool {
        !self.ongoing_call() && self.calls().any(|c| c.1.state == CallState::IncomingRinging)
    }

    /// Returns true if any calls are ongoing.
    pub fn ongoing_call(&self) -> bool {
        self.is_call_active() || self.calls().any(|c| c.1.state == CallState::OngoingHeld)
    }

    /// Get the current `CallIndicators` based on the state of all calls.
    pub fn indicators(&mut self) -> CallIndicators {
        let mut calls = self.calls().collect::<Vec<_>>();
        calls.sort_by_key(|c| c.1.state_updated_at);
        CallIndicators::find(calls.into_iter().rev().map(|c| c.1.state))
    }

    /// Return an iterator of the calls and associated call indices.
    fn calls(&self) -> impl Iterator<Item = (CallIdx, &CallEntry)> + Clone {
        self.current_calls.calls()
    }

    /// Helper function to poll the new_calls hanging get for any available items.
    /// Polls until there are no more items ready. Updates internal state with the
    /// available items.
    fn poll_and_consume_new_calls(&mut self, cx: &mut Context<'_>) {
        // Loop until pending or self.new_calls is set to None.
        while let Some(new_calls) = &mut self.new_calls {
            match new_calls.poll_next_unpin(cx) {
                Poll::Ready(Some(Ok(next_call))) => {
                    if let Err(e) = self.handle_new_call(next_call) {
                        warn!("Ignoring next call value. Error: {}", e);
                    }
                }
                Poll::Ready(Some(Err(_e))) => self.new_calls = None,
                Poll::Ready(None) => self.new_calls = None,
                Poll::Pending => break,
            }
        }
    }

    /// Helper function to poll the call_updates collection for call state changes.
    /// Polls until there are no more updates ready. Updates internal state with
    /// available items.
    fn poll_and_consume_call_updates(&mut self, cx: &mut Context<'_>) {
        // Loop until self.call_updates is terminated or the stream is exhausted.
        while !self.call_updates.is_terminated() {
            match self.call_updates.poll_next_unpin(cx) {
                Poll::Ready(Some(item)) => match item {
                    StreamItem::Item((index, Ok(state))) => {
                        if let Some(call) = self.current_calls.get_mut(index) {
                            self.pending.record(index, state);
                            call.set_state(state);
                        } else {
                            self.remove_call(index);
                        }
                    }
                    StreamItem::Item((index, Err(e))) => {
                        info!("Call {} channel closed with error: {}", index, e);
                        self.remove_call(index);
                    }
                    StreamItem::Epitaph(index) => {
                        debug!("Call {} channel closed", index);
                        self.remove_call(index);
                    }
                },
                Poll::Ready(None) | Poll::Pending => break,
            }
        }
    }

    /// Compute the call indicators to send after changes to calls have been polled and the call state updated.
    /// TODO(fxb/96764) Move this to call_changes.
    fn call_indicators_updates(
        &mut self,
        call_was_active: bool,
        call_was_transferred: bool,
        old_call_held_index: Option<CallIdx>,
    ) -> Option<CallIndicatorsUpdates> {
        let call_is_active = self.is_call_active();
        let call_is_transferred = self.is_call_transferred_to_ag();

        let new_call_held_index = self.oldest_by_state(CallState::OngoingHeld).map(|(idx, _)| idx);

        let mut report_call_transferred = false;
        let mut call_indicators_updates = Default::default();

        if self.pending.report_now() {
            let previous = self.reported_indicators;
            self.reported_indicators = self.indicators();
            // Generate a list of all the indicators that have changed as a result of the
            // new call state.
            call_indicators_updates = self.reported_indicators.difference(previous);
            // Determine whether a call has been transferred to or from the AG/HF, in which case
            // we'll need to report this so the task can update the SCO state even if the
            // indicators are empty.
            report_call_transferred = (call_was_active && call_is_transferred)
                || (call_was_transferred && call_is_active);
        }

        // If the active call has been swapped with a held call, force an indicator to be produced
        // to communcate this, even if one wouldn't be otherwise.
        if call_indicators_updates.callheld == None
            && old_call_held_index.is_some()
            && new_call_held_index.is_some()
            && call_was_active
            && call_is_active
            && old_call_held_index != new_call_held_index
        {
            call_indicators_updates.callheld = Some(CallHeld::HeldAndActive)
        }

        if !call_indicators_updates.is_empty() || report_call_transferred {
            Some(call_indicators_updates)
        } else {
            None
        }
    }
}

impl Unpin for Calls {}

impl Stream for Calls {
    type Item = CallIndicatorsUpdates;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.check_termination_condition();

        if self.terminated {
            return Poll::Ready(None);
        }

        let call_was_active = self.is_call_active();
        let call_was_transferred = self.is_call_transferred_to_ag();

        let old_call_held_index = self.oldest_by_state(CallState::OngoingHeld).map(|(idx, _)| idx);

        // Update the state of all new and ongoing calls.
        self.poll_and_consume_new_calls(cx);
        self.poll_and_consume_call_updates(cx);

        match self.call_indicators_updates(
            call_was_active,
            call_was_transferred,
            old_call_held_index,
        ) {
            Some(call_indicators_updates) => Poll::Ready(Some(call_indicators_updates)),
            None => Poll::Pending,
        }
    }
}

impl FusedStream for Calls {
    fn is_terminated(&self) -> bool {
        self.terminated
    }
}

/// Call actions that can be requested of the AG.
#[derive(Debug)]
pub enum CallAction {
    InitiateByNumber(String),
    InitiateByMemoryLocation(String),
    InitiateByRedialLast,
    TransferActive,
}

impl From<CallAction> for FidlCallAction {
    fn from(call_action: CallAction) -> Self {
        match call_action {
            CallAction::InitiateByNumber(number) => Self::DialFromNumber(number),
            CallAction::InitiateByMemoryLocation(location) => Self::DialFromLocation(location),
            CallAction::InitiateByRedialLast => Self::RedialLast(RedialLast),
            CallAction::TransferActive => Self::TransferActive(TransferActive),
        }
    }
}

impl From<FidlCallAction> for CallAction {
    fn from(fidl_call_action: FidlCallAction) -> Self {
        match fidl_call_action {
            FidlCallAction::DialFromNumber(number) => Self::InitiateByNumber(number),
            FidlCallAction::DialFromLocation(location) => Self::InitiateByMemoryLocation(location),
            FidlCallAction::RedialLast(RedialLast) => Self::InitiateByRedialLast,
            FidlCallAction::TransferActive(TransferActive) => Self::TransferActive,
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        assert_matches::assert_matches,
        async_utils::PollExt,
        fidl::endpoints::ClientEnd,
        fidl_fuchsia_bluetooth_hfp::{
            CallDirection, CallMarker, CallRequest, CallRequestStream, CallWatchStateResponder,
            PeerHandlerMarker, PeerHandlerRequest, PeerHandlerRequestStream,
        },
        fuchsia_async as fasync,
    };

    #[fuchsia::test]
    fn call_is_active() {
        // executor must be created before fidl endpoints can be created
        let _exec = fasync::TestExecutor::new();
        let (proxy, _) = fidl::endpoints::create_proxy::<CallMarker>().unwrap();

        let mut call = CallEntry::new(
            proxy,
            "1".into(),
            CallState::IncomingRinging,
            Direction::MobileTerminated,
        );

        assert!(!call.is_active());
        call.set_state(CallState::OngoingActive);
        assert!(call.is_active());
        call.set_state(CallState::Terminated);
        assert!(!call.is_active());
    }

    /// Create a new NextCall fidl response object from an optional `client_end` and a remote party
    /// `number`. Uses the most common test values for `state` and `direction`.
    fn new_next_call_fidl(
        client_end: impl Into<Option<ClientEnd<CallMarker>>>,
        number: impl Into<String>,
    ) -> NextCall {
        NextCall {
            call: client_end.into(),
            remote: Some(number.into()),
            state: Some(CallState::OngoingActive),
            direction: Some(CallDirection::MobileTerminated),
            ..NextCall::EMPTY
        }
    }

    /// The most common test setup includes an initialized Calls instance and an ongoing call.
    /// This helper function sets up a `Calls` instance in that state and returns the associated
    /// endpoints.
    fn setup_ongoing_call() -> (Calls, PeerHandlerRequestStream, CallRequestStream, CallIdx, Number)
    {
        let (proxy, peer_stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();
        let mut calls = Calls::new(Some(proxy));
        let (client_end, call_stream) = fidl::endpoints::create_request_stream().unwrap();
        let num = Number::from("1");
        let mut next_call = new_next_call_fidl(client_end, num.clone());
        next_call.state = Some(CallState::IncomingRinging);
        let _ = calls.handle_new_call(next_call).expect("success handling new call");
        let expected = CallIndicators {
            call: types::Call::None,
            callsetup: types::CallSetup::Incoming,
            callwaiting: false,
            callheld: types::CallHeld::None,
        };
        assert_eq!(calls.indicators(), expected);
        (calls, peer_stream, call_stream, 1, num)
    }

    #[fuchsia::test]
    fn calls_should_ring_succeeds() {
        let mut exec = fasync::TestExecutor::new();

        let (mut calls, mut peer_handler, mut call_stream, _idx, _num) = setup_ongoing_call();
        assert!(calls.should_ring());

        let _ = assert_calls_indicators(&mut exec, &mut calls);

        update_call(&mut exec, &mut call_stream, CallState::OngoingActive);
        let _ = assert_calls_indicators(&mut exec, &mut calls);

        // Call is no longer ringing after call state has changed
        assert!(!calls.should_ring());

        let _call2_stream = new_call(
            &mut exec,
            &mut peer_handler,
            "2",
            CallState::IncomingRinging,
            CallDirection::MobileTerminated,
        );
        poll_calls_until_pending(&mut exec, &mut calls);

        // The calls state should not be ringing despite an IncomingRinging call because there
        // is an ongoing call present.
        assert!(!calls.should_ring());

        // Remove the ongoing call
        drop(call_stream);
        poll_calls_until_pending(&mut exec, &mut calls);

        // Call is ringing now that the ongoing call was removed.
        assert!(calls.should_ring());
    }

    #[fasync::run_until_stalled(test)]
    async fn call_requests_send_requests_to_server() {
        let (mut calls, _peer_handler, mut call_stream, idx, _num) = setup_ongoing_call();

        calls.request_hold(idx).expect("valid index");
        assert_matches!(call_stream.next().await, Some(Ok(CallRequest::RequestHold { .. })));

        // Make a second call that is active
        let (client_end, mut call_stream_2) = fidl::endpoints::create_request_stream().unwrap();
        let next_call = new_next_call_fidl(client_end, "2");
        let _ = calls.handle_new_call(next_call).expect("success handling new call");

        // Sending a RequestActive for the first call will send a RequestTerminate for the second
        // call when `true` is passed to request_active.
        calls.request_active(idx, true).expect("valid index");
        assert_matches!(call_stream.next().await, Some(Ok(CallRequest::RequestActive { .. })));
        assert_matches!(call_stream_2.next().await, Some(Ok(CallRequest::RequestTerminate { .. })));

        // Place the first call back on hold, so that we can activate it again.
        calls.request_hold(idx).expect("valid index");
        assert_matches!(call_stream.next().await, Some(Ok(CallRequest::RequestHold { .. })));

        // Make a third call that is active
        let (client_end, mut call_stream_3) = fidl::endpoints::create_request_stream().unwrap();
        let next_call = new_next_call_fidl(client_end, "3");
        let _ = calls.handle_new_call(next_call).expect("success handling new call");

        // Sending a RequestActive for the first call will send a RequestHold for the third
        // call when `false` is passed to request_active.
        calls.request_active(idx, false).expect("valid index");
        assert_matches!(call_stream.next().await, Some(Ok(CallRequest::RequestActive { .. })));
        assert_matches!(call_stream_3.next().await, Some(Ok(CallRequest::RequestHold { .. })));

        calls.request_terminate(idx).expect("valid index");
        assert_matches!(call_stream.next().await, Some(Ok(CallRequest::RequestTerminate { .. })));

        calls.request_transfer_audio(idx).expect("valid index");
        assert_matches!(
            call_stream.next().await,
            Some(Ok(CallRequest::RequestTransferAudio { .. }))
        );
    }

    #[fasync::run_until_stalled(test)]
    async fn call_requests_invalid_index_return_error() {
        let (mut calls, _peer_handler, _call_stream, _idx, _num) = setup_ongoing_call();

        let invalid = 2;
        let result = calls.request_hold(invalid);
        assert_eq!(result, Err(CallError::UnknownIndexError(invalid.clone())));

        let result = calls.request_active(invalid, false);
        assert_eq!(result, Err(CallError::UnknownIndexError(invalid.clone())));

        let result = calls.request_terminate(invalid);
        assert_eq!(result, Err(CallError::UnknownIndexError(invalid.clone())));

        let result = calls.request_transfer_audio(invalid);
        assert_eq!(result, Err(CallError::UnknownIndexError(invalid.clone())));
    }

    #[fuchsia::test]
    fn call_requests_manager_closed_clears_call() {
        let mut exec = fasync::TestExecutor::new();

        let (mut calls, _peer_handler, call_stream, idx, num) = setup_ongoing_call();
        drop(call_stream);

        let call =
            calls.current_calls().into_iter().find(|info| info.index == idx && info.number == num);
        assert!(call.is_some(), "Call must exist in list of calls");

        // Since we dropped `call_stream`, the WatchState hanging get will
        // return with PEER_CLOSED, and the call entry will be removed.
        poll_calls_until_pending(&mut exec, &mut calls);

        let call =
            calls.current_calls().into_iter().find(|info| info.index == idx && info.number == num);
        assert!(call.is_none(), "Call must not exist in list of calls");
    }

    /// Make a new call, manually driving async execution.
    #[track_caller]
    fn new_call(
        exec: &mut fasync::TestExecutor,
        stream: &mut PeerHandlerRequestStream,
        num: &str,
        state: CallState,
        direction: CallDirection,
    ) -> CallRequestStream {
        // Get WatchNextCall request.
        let responder = match exec.run_until_stalled(&mut stream.next()) {
            Poll::Ready(Some(Ok(PeerHandlerRequest::WatchNextCall { responder, .. }))) => responder,
            result => panic!("Unexpected result: {:?}", result),
        };
        // Respond with a call.
        let (client, call) = fidl::endpoints::create_request_stream::<CallMarker>().unwrap();
        let next_call = NextCall {
            call: Some(client),
            remote: Some(num.to_string()),
            state: Some(state),
            direction: Some(direction),
            ..NextCall::EMPTY
        };
        responder.send(next_call).expect("response to succeed");
        call
    }

    /// Expects a WatchState call to be the next pending item on `stream`.
    /// Returns the responder or panics.
    #[track_caller]
    fn wait_for_call_state(
        exec: &mut fasync::TestExecutor,
        stream: &mut CallRequestStream,
    ) -> CallRequest {
        match exec.run_until_stalled(&mut stream.next()) {
            Poll::Ready(Some(Ok(req))) => req,
            result => panic!("Unexpected call request result: {:?}", result),
        }
    }

    /// Expects a WatchState call to be the next pending item on `stream`.
    /// Returns the responder or panics.
    #[track_caller]
    fn watch_state_responder(
        exec: &mut fasync::TestExecutor,
        stream: &mut CallRequestStream,
    ) -> CallWatchStateResponder {
        match exec.run_until_stalled(&mut stream.next()) {
            Poll::Ready(Some(Ok(CallRequest::WatchState { responder, .. }))) => responder,
            result => panic!("Unexpected result: {:?}", result),
        }
    }

    /// Update call state, manually driving async execution.
    /// Expects a WatchState call to be the next pending item on `stream`.
    #[track_caller]
    fn update_call(
        exec: &mut fasync::TestExecutor,
        stream: &mut CallRequestStream,
        state: CallState,
    ) {
        let responder = watch_state_responder(exec, stream);
        // Respond with a call state.
        responder.send(state).expect("response to succeed");
    }

    #[track_caller]
    fn assert_dtmf_code(
        exec: &mut fasync::TestExecutor,
        stream: &mut CallRequestStream,
        expected: DtmfCode,
        mut response: Result<(), i32>,
    ) {
        let expected = expected.into();
        // Get SendDtmfCode request for call
        let responder = match exec.run_until_stalled(&mut stream.next()) {
            Poll::Ready(Some(Ok(CallRequest::SendDtmfCode { responder, code })))
                if code == expected =>
            {
                responder
            }
            result => panic!("Unexpected request: {:?}", result),
        };
        // Respond with a call state.
        responder.send(&mut response).expect("response to succeed");
    }

    /// Get the next CallRequest::WatchState hanging get responder from `stream` and return it.
    #[track_caller]
    fn get_watch_state(
        exec: &mut fasync::TestExecutor,
        stream: &mut CallRequestStream,
    ) -> CallWatchStateResponder {
        // Get WatchState request for call
        match exec.run_until_stalled(&mut stream.next()) {
            Poll::Ready(Some(Ok(CallRequest::WatchState { responder, .. }))) => return responder,
            result => panic!("Unexpected result: {:?}", result),
        };
    }

    /// Assert the Calls stream is pending, manually driving async execution.
    #[track_caller]
    fn assert_calls_pending(exec: &mut fasync::TestExecutor, calls: &mut Calls) {
        match exec.run_until_stalled(&mut calls.next()) {
            Poll::Pending => {}
            x => panic!("Expected pending, got {:?}", x),
        }
    }

    /// Return the next item from the Calls stream, manually driving async execution.
    /// Panics if the stream does not produce some item.
    #[track_caller]
    fn assert_calls_indicators(
        exec: &mut fasync::TestExecutor,
        calls: &mut Calls,
    ) -> CallIndicatorsUpdates {
        let result = exec.run_until_stalled(&mut calls.next());
        match result {
            Poll::Ready(Some(ind)) => ind,
            result => panic!("Unexpected result: {:?}", result),
        }
    }

    /// Poll the calls  driving async execution, until there are no more updates and and we receive
    /// Pending.  Returns the most recent indicators if any were sent.
    #[track_caller]
    fn poll_calls_until_pending(exec: &mut fasync::TestExecutor, calls: &mut Calls) {
        loop {
            match exec.run_until_stalled(&mut calls.next()) {
                Poll::Ready(Some(_ind)) => return,
                Poll::Pending => return,
                Poll::Ready(None) => panic!("Calls terminated before expected!"),
            }
        }
    }

    #[fuchsia::test]
    fn calls_is_call_active() {
        let mut exec = fasync::TestExecutor::new();

        let (proxy, mut peer_stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();
        let mut calls = Calls::new(Some(proxy));

        // No active call when there are no calls.
        assert!(!calls.is_call_active());

        poll_calls_until_pending(&mut exec, &mut calls);
        let mut call_stream = new_call(
            &mut exec,
            &mut peer_stream,
            "1",
            CallState::IncomingRinging,
            CallDirection::MobileTerminated,
        );
        poll_calls_until_pending(&mut exec, &mut calls);

        // When we have an ringing call, we are still not active yet.

        assert!(!calls.is_call_active());

        poll_calls_until_pending(&mut exec, &mut calls);
        update_call(&mut exec, &mut call_stream, CallState::OngoingActive);
        poll_calls_until_pending(&mut exec, &mut calls);

        assert!(calls.is_call_active());

        poll_calls_until_pending(&mut exec, &mut calls);
        update_call(&mut exec, &mut call_stream, CallState::OngoingHeld);
        poll_calls_until_pending(&mut exec, &mut calls);

        assert!(!calls.is_call_active());
    }

    #[fuchsia::test]
    fn calls_transfers_produce_indicators() {
        let mut exec = fasync::TestExecutor::new();

        let (proxy, mut peer_stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();
        let mut calls = Calls::new(Some(proxy));

        // No active call when there are no calls.
        assert!(!calls.is_call_active());

        poll_calls_until_pending(&mut exec, &mut calls);
        let mut call_stream = new_call(
            &mut exec,
            &mut peer_stream,
            "1",
            CallState::IncomingRinging,
            CallDirection::MobileTerminated,
        );
        poll_calls_until_pending(&mut exec, &mut calls);

        // Answer call
        update_call(&mut exec, &mut call_stream, CallState::OngoingActive);
        poll_calls_until_pending(&mut exec, &mut calls);
        assert!(calls.is_call_active());
        assert!(!calls.is_call_transferred_to_ag());

        // Call transfer to AG produces an empty indicator update.
        update_call(&mut exec, &mut call_stream, CallState::TransferredToAg);
        let transferred_update = assert_calls_indicators(&mut exec, &mut calls);
        assert!(!calls.is_call_active());
        assert!(calls.is_call_transferred_to_ag());
        assert!(transferred_update.is_empty());

        // Call transfer to HF produces an empty indicator update.
        update_call(&mut exec, &mut call_stream, CallState::OngoingActive);
        let active_update = assert_calls_indicators(&mut exec, &mut calls);
        assert!(calls.is_call_active());
        assert!(!calls.is_call_transferred_to_ag());
        assert!(active_update.is_empty());
    }

    #[fuchsia::test]
    fn calls_swap_produce_indicators() {
        let mut exec = fasync::TestExecutor::new();

        let (proxy, mut peer_stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();
        let mut calls = Calls::new(Some(proxy));

        // No active call when there are no calls.
        assert!(!calls.is_call_active());

        // Create new call
        poll_calls_until_pending(&mut exec, &mut calls);
        let mut call_stream_1 = new_call(
            &mut exec,
            &mut peer_stream,
            "1",
            CallState::IncomingRinging,
            CallDirection::MobileTerminated,
        );
        poll_calls_until_pending(&mut exec, &mut calls);

        // Answer call
        update_call(&mut exec, &mut call_stream_1, CallState::OngoingActive);
        poll_calls_until_pending(&mut exec, &mut calls);
        assert!(calls.is_call_active());

        // Create second new call
        poll_calls_until_pending(&mut exec, &mut calls);
        let mut call_stream_2 = new_call(
            &mut exec,
            &mut peer_stream,
            "2",
            CallState::IncomingRinging,
            CallDirection::MobileTerminated,
        );
        poll_calls_until_pending(&mut exec, &mut calls);

        // Answer call from call manager and hold old call
        update_call(&mut exec, &mut call_stream_1, CallState::OngoingHeld);
        update_call(&mut exec, &mut call_stream_2, CallState::OngoingActive);
        poll_calls_until_pending(&mut exec, &mut calls);
        assert!(calls.is_call_active());
        assert!(calls.is_call_held());
        let first_active_idx = calls.oldest_by_state(CallState::OngoingActive).map(|(idx, _)| idx);
        let first_held_idx = calls.oldest_by_state(CallState::OngoingHeld).map(|(idx, _)| idx);

        // Swap held and active calls.
        // The HF sends a command to hold the current active call and make the held call active.
        calls.hold(CallHoldAction::HoldAllExceptSpecified(first_held_idx.unwrap())).expect("Hold.");
        let watch_responder_1 = watch_state_responder(&mut exec, &mut call_stream_1);
        let watch_responder_2 = watch_state_responder(&mut exec, &mut call_stream_2);

        // The calls module sends a call state change to the Call Manager
        assert_matches!(
            wait_for_call_state(&mut exec, &mut call_stream_2),
            CallRequest::RequestHold { .. }
        );

        // Simulate call namager responding when it has updated the state.
        watch_responder_1.send(CallState::OngoingActive).expect("Call 1 held");
        watch_responder_2.send(CallState::OngoingHeld).expect("Call 2 active");

        // Make sure we get an indicator for the swap.
        let held_update = assert_calls_indicators(&mut exec, &mut calls);
        assert_eq!(held_update.callheld, Some(CallHeld::HeldAndActive));

        // Make sure the calls state is both held and active.
        assert!(calls.is_call_active());
        assert!(calls.is_call_held());

        // Confirm that the calls actually swapped.
        let second_active_idx = calls.oldest_by_state(CallState::OngoingActive).map(|(idx, _)| idx);
        let second_held_idx = calls.oldest_by_state(CallState::OngoingHeld).map(|(idx, _)| idx);
        assert_eq!(first_active_idx, second_held_idx);
        assert_eq!(second_active_idx, first_held_idx);
    }

    #[fuchsia::test]
    fn calls_stream_lifecycle() {
        // Test the Stream for items when a single call is tracked, then a second call is added,
        // when the states of those calls are modified, and finally, when both calls have been
        // removed from the stream.

        let mut exec = fasync::TestExecutor::new();

        let (mut calls, mut handler_stream, mut call_1, _idx_1, _num_1) = setup_ongoing_call();

        let item = assert_calls_indicators(&mut exec, &mut calls);
        let expected = CallIndicatorsUpdates {
            callsetup: Some(types::CallSetup::Incoming),
            ..CallIndicatorsUpdates::default()
        };
        assert_eq!(item, expected);

        // Stream doesn't have an item ready.
        assert_calls_pending(&mut exec, &mut calls);

        let mut call_2 = new_call(
            &mut exec,
            &mut handler_stream,
            "2",
            CallState::OutgoingAlerting,
            CallDirection::MobileOriginated,
        );

        // There are no new calls in this test so close handler stream.
        drop(handler_stream);

        // Stream has an item ready.
        let item = assert_calls_indicators(&mut exec, &mut calls);
        // The ready item is OutgoingAlerting even though there is also an IncomingRinging call.
        // This is because the OutgoingAlerting call state was reported last.
        let expected = CallIndicatorsUpdates {
            callsetup: Some(types::CallSetup::OutgoingAlerting),
            ..CallIndicatorsUpdates::default()
        };
        assert_eq!(item, expected);

        // Stream doesn't have an item ready.
        assert_calls_pending(&mut exec, &mut calls);

        update_call(&mut exec, &mut call_1, CallState::OngoingActive);

        // Stream has an item ready.
        let item = assert_calls_indicators(&mut exec, &mut calls);
        // Only indicators that have changed are returned.
        let expected = CallIndicatorsUpdates {
            call: Some(types::Call::Some),
            ..CallIndicatorsUpdates::default()
        };
        assert_eq!(item, expected);

        drop(call_1);

        update_call(&mut exec, &mut call_2, CallState::OngoingHeld);

        // Stream has an item ready.
        let item = assert_calls_indicators(&mut exec, &mut calls);
        let expected = CallIndicatorsUpdates {
            callsetup: Some(types::CallSetup::None),
            callheld: Some(types::CallHeld::Held),
            ..CallIndicatorsUpdates::default()
        };
        assert_eq!(item, expected);

        drop(call_2);

        // Stream has an item ready.
        let item = assert_calls_indicators(&mut exec, &mut calls);
        let expected = CallIndicatorsUpdates {
            call: Some(types::Call::None),
            callheld: Some(types::CallHeld::None),
            ..CallIndicatorsUpdates::default()
        };
        assert_eq!(item, expected);

        assert!(!calls.is_terminated());

        // Stream is complete.
        let result = exec.run_until_stalled(&mut calls.next());
        assert_matches!(result, Poll::Ready(None));
        assert!(calls.is_terminated());
    }

    #[fuchsia::test]
    fn partial_call_updates_dont_report() {
        // Test the Stream for items when a single call is tracked, then a second call is added,
        // when the states of those calls are modified, and finally, when both calls have been
        // removed from the stream.

        let mut exec = fasync::TestExecutor::new();

        let (mut calls, mut handler_stream, mut call_1, idx_1, _num_1) = setup_ongoing_call();

        let item = assert_calls_indicators(&mut exec, &mut calls);
        let expected = CallIndicatorsUpdates {
            callsetup: Some(types::CallSetup::Incoming),
            ..CallIndicatorsUpdates::default()
        };
        assert_eq!(item, expected);

        // Stream doesn't have an item ready.
        assert_calls_pending(&mut exec, &mut calls);

        let mut call_2 = new_call(
            &mut exec,
            &mut handler_stream,
            "2",
            CallState::OngoingActive,
            CallDirection::MobileTerminated,
        );

        // There are no new calls in this test so close handler stream.
        drop(handler_stream);

        // Stream has an item ready.
        let item = assert_calls_indicators(&mut exec, &mut calls);
        // The ready item is OutgoingAlerting even though there is also an IncomingRinging call.
        // This is because the OutgoingAlerting call state was reported last.
        let expected = CallIndicatorsUpdates {
            call: Some(types::Call::Some),
            ..CallIndicatorsUpdates::default()
        };
        assert_eq!(item, expected);

        // Stream doesn't have an item ready.
        assert_calls_pending(&mut exec, &mut calls);

        let call_1_resp = watch_state_responder(&mut exec, &mut call_1);
        let call_2_resp = watch_state_responder(&mut exec, &mut call_2);

        // Requesting idx_1 active causes idx_2 to be placed on hold
        calls.request_active(idx_1, false).expect("valid index");
        assert_matches!(
            exec.run_singlethreaded(call_1.next()),
            Some(Ok(CallRequest::RequestActive { .. }))
        );
        assert_matches!(
            exec.run_singlethreaded(call_2.next()),
            Some(Ok(CallRequest::RequestHold { .. }))
        );

        call_1_resp.send(CallState::OngoingActive).expect("response to succeed");

        // Stream doesn't have an item ready until both call_1 and call_2 have been updated.
        assert_calls_pending(&mut exec, &mut calls);

        call_2_resp.send(CallState::OngoingHeld).expect("response to succeed");

        // Stream has an item ready now that both calls have been updated.
        let item = assert_calls_indicators(&mut exec, &mut calls);
        // Only indicators that have changed are returned.
        let expected = CallIndicatorsUpdates {
            callsetup: Some(types::CallSetup::None),
            callheld: Some(types::CallHeld::HeldAndActive),
            ..CallIndicatorsUpdates::default()
        };
        assert_eq!(item, expected);
    }

    #[fuchsia::test]
    fn bad_watch_next_call_data_returns_error() {
        // `setup_ongoing_call` requires that an executor exists.
        let mut _exec = fasync::TestExecutor::new();

        let (mut calls, ..) = setup_ongoing_call();

        // Make an call that is missing some required field.
        let next_call = new_next_call_fidl(None, "2");
        let result = calls.handle_new_call(next_call);
        assert_matches!(result, Err(Error::MissingParameter(_)));
    }

    #[fuchsia::test]
    fn send_dtmf_code_to_call() {
        let mut exec = fasync::TestExecutor::new();

        let (mut calls, _handler_stream, mut call_1, _idx_1, _num_1) = setup_ongoing_call();
        poll_calls_until_pending(&mut exec, &mut calls);

        // Send DTMF code to a call that is not in a state which accepts DTMF codes.
        {
            let send = calls.send_dtmf_code(DtmfCode::One);
            futures::pin_mut!(send);
            let result = exec.run_until_stalled(&mut send).unwrap();
            // Call is IncomingRinging so it cannot accept a DTMF Code.
            assert!(result.is_err());
        }

        // Update the call state to one which will accept DTMF codes.
        update_call(&mut exec, &mut call_1, CallState::OngoingActive);
        poll_calls_until_pending(&mut exec, &mut calls);
        let watch_state_responder = get_watch_state(&mut exec, &mut call_1);

        // Sending a DTMF code should now succeed.
        {
            let send = calls.send_dtmf_code(DtmfCode::One);
            futures::pin_mut!(send);
            assert!(exec.run_until_stalled(&mut send).is_pending());
            assert_dtmf_code(&mut exec, &mut call_1, DtmfCode::One, Ok(()));
            let result = exec.run_until_stalled(&mut send).unwrap();
            // Call is OngoingActive so the DTMF Code request returns the expected result.
            assert!(result.is_ok());
        }

        // Errors from DTMF codes are propagated back to sender.
        {
            let send = calls.send_dtmf_code(DtmfCode::One);
            futures::pin_mut!(send);
            assert!(exec.run_until_stalled(&mut send).is_pending());
            assert_dtmf_code(&mut exec, &mut call_1, DtmfCode::One, Err(0));
            let result = exec.run_until_stalled(&mut send).unwrap();
            // Call is OngoingActive so the DTMF Code request returns the expected result.
            assert!(result.is_err());
        }

        // DTMF codes are not valid when there are no calls present.
        drop(watch_state_responder);
        drop(call_1);
        poll_calls_until_pending(&mut exec, &mut calls);
        {
            let send = calls.send_dtmf_code(DtmfCode::One);
            futures::pin_mut!(send);
            let result = exec.run_until_stalled(&mut send).unwrap();
            // There are no calls present to which to direct the DTMF code.
            assert!(result.is_err());
        }
    }
}
