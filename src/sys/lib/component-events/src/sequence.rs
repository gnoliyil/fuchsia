// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        descriptor::EventDescriptor,
        events::{event_name, EventStream},
        matcher::EventMatcher,
    },
    anyhow::{format_err, Error},
    std::convert::TryFrom,
};

/// Determines whether an EventGroup allows events to be verified in any order
/// or only in the order specified in the group.
#[derive(Clone)]
pub enum Ordering {
    Ordered,
    Unordered,
}

/// Determines whether an EventGroup requires all observed events to match
/// an EventMatcher in the group, or ignores events that don't match.
#[derive(Clone, PartialEq)]
pub enum Contains {
    All,
    Subset,
}

#[derive(Clone)]
pub struct EventSequence {
    groups: Vec<EventGroup>,
}

impl EventSequence {
    pub fn new() -> Self {
        Self { groups: vec![] }
    }

    pub fn then(self, matcher: EventMatcher) -> Self {
        self.all_of(vec![matcher], Ordering::Ordered)
    }

    /// Adds a group of matchers to verify all following events in the sequence match
    /// with the given ordering.
    ///
    /// The sequence will fail to match if contains events that don't match the group.
    pub fn all_of(mut self, events: Vec<EventMatcher>, ordering: Ordering) -> Self {
        self.groups.push(EventGroup::new(events, ordering, Contains::All));
        self
    }

    /// Adds a group of matchers to verify that the sequence contains a subset of
    /// events that match with the given ordering.
    ///
    /// Events in the sequence that don't match will be ignored. Subsequent matchers
    /// outside of this group will not be able to match against ignored events.
    pub fn has_subset(mut self, events: Vec<EventMatcher>, ordering: Ordering) -> Self {
        self.groups.push(EventGroup::new(events, ordering, Contains::Subset));
        self
    }

    /// Verify that the events in this sequence are received from the provided EventStream.
    pub async fn expect(self, event_stream: EventStream) -> Result<(), Error> {
        self.expect_and_giveback(event_stream).await.map(|_| ())
    }

    /// Verify that the events in this sequence are received from the provided EventStream,
    /// and gives back the event stream on success.
    pub async fn expect_and_giveback(
        mut self,
        mut event_stream: EventStream,
    ) -> Result<EventStream, Error> {
        while !self.groups.is_empty() {
            match event_stream.next().await {
                Err(e) => return Err(e.into()),
                Ok(event) => {
                    let actual_event = EventDescriptor::try_from(&event)?;
                    let _ = self.next(&actual_event)?;
                }
            }
        }
        Ok(event_stream)
    }

    pub fn is_empty(&self) -> bool {
        self.groups.is_empty()
    }

    pub fn event_names(&self) -> Result<Vec<String>, Error> {
        let mut event_names = vec![];
        for group in &self.groups {
            let mut group_event_names = group.event_names()?;
            event_names.append(&mut group_event_names);
        }
        event_names.dedup();
        Ok(event_names)
    }

    /// Tests an EventDescriptor and against the first EventGroup.
    ///
    /// If an EventGroup has been entirely consumed (no further EventMatchers
    /// to match against) then the EventGroup is dropped.
    ///
    /// Returns an error if the EventSequence have not been entirely consumed,
    /// and the incoming EventDescriptor does not match the first sequence.
    ///
    /// Returns Ok(true) if there is a positive match.
    /// Returns Ok(false) if the EventSequence is empty.
    pub fn next(&mut self, event: &EventDescriptor) -> Result<(), Error> {
        loop {
            if self.groups.is_empty() {
                return Ok(());
            }
            let group = &mut self.groups[0];
            if group.next(event)? {
                if group.is_empty() {
                    self.groups.remove(0);
                }
                return Ok(());
            }
            self.groups.remove(0);
        }
    }
}

#[derive(Clone)]
pub struct EventGroup {
    events: Vec<EventMatcher>,
    ordering: Ordering,
    contains: Contains,
}

impl EventGroup {
    pub fn new(events: Vec<EventMatcher>, ordering: Ordering, contains: Contains) -> Self {
        Self { events, ordering, contains }
    }

    pub fn is_empty(&self) -> bool {
        self.events.is_empty()
    }

    pub fn event_names(&self) -> Result<Vec<String>, Error> {
        let mut event_names = vec![];
        for event in &self.events {
            if let Some(event_type) = &event.event_type {
                event_names.push(event_name(&event_type.value()));
            } else {
                return Err(format_err!("No event name or type set for matcher {:?}", event));
            }
        }
        event_names.dedup();
        Ok(event_names)
    }

    /// Returns true if `event` matches an event matcher in this group.
    ///
    /// If the group ordering is Ordered, the event must match the first matcher.
    /// If the group ordering is Unordered, the event can match any matcher in the group.
    /// The matcher is removed after a successful match.
    ///
    /// Returns an error if the event does not match a matcher and the contains
    /// policy is All, indicating that the unknown event did not match the group.
    ///
    /// Returns Ok(true) if there is a positive match.
    /// Returns Ok(false) if the EventGroup is empty.
    pub fn next(&mut self, event: &EventDescriptor) -> Result<bool, Error> {
        if self.events.is_empty() {
            return Ok(Contains::Subset == self.contains);
        }
        match self.ordering {
            Ordering::Ordered => {
                let matches = self.events.get(0).unwrap().matches(event);
                if matches.is_ok() {
                    self.events.remove(0);
                    Ok(true)
                } else {
                    // There was no matcher that matched this event.
                    // This is an error only if the group expects all events to be matched.
                    match self.contains {
                        Contains::All => Err(Error::new(matches.unwrap_err())),
                        Contains::Subset => Ok(true),
                    }
                }
            }
            Ordering::Unordered => {
                if let Some((index, _)) = self
                    .events
                    .iter()
                    .enumerate()
                    .find(|&matcher| matcher.1.matches(&event).is_ok())
                {
                    self.events.remove(index);
                    Ok(true)
                } else {
                    match self.contains {
                        Contains::All => Err(format_err!("Failed to find event: {:?}", event)),
                        Contains::Subset => Ok(true),
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::events::{Event, Started};
    use anyhow::Context;
    use fidl_fuchsia_component as fcomponent;
    use futures::StreamExt;

    async fn run_server(
        events: Vec<fcomponent::Event>,
        mut server: fcomponent::EventStreamRequestStream,
    ) {
        let (tx, mut rx) = futures::channel::mpsc::unbounded();
        for event in events {
            tx.unbounded_send(event).unwrap();
        }
        // The tests expect the event_stream to terminate once all events have been consumed.
        // This behavior does not match that of component_manager but is needed for negative
        // proof tests (such as the event_stream does NOT contain a given event).
        drop(tx);
        while let Some(Ok(request)) = server.next().await {
            match request {
                fcomponent::EventStreamRequest::GetNext { responder } => {
                    if let Some(event) = rx.next().await {
                        responder.send(vec![event]).unwrap();
                    } else {
                        return;
                    }
                }
                fcomponent::EventStreamRequest::WaitForReady { responder } => {
                    responder.send().unwrap()
                }
            }
        }
    }

    async fn make_event_stream(
        events: Vec<fcomponent::Event>,
    ) -> Result<(EventStream, fuchsia_async::Task<()>), Error> {
        let (proxy, server) = fidl::endpoints::create_proxy::<fcomponent::EventStreamMarker>()
            .context("failed to make EventStream proxy")?;
        Ok((
            EventStream::new_v2(proxy),
            fuchsia_async::Task::spawn(run_server(events, server.into_stream()?)),
        ))
    }

    // Returns a successful Started event for the given moniker.
    fn make_event<M: Into<String>>(moniker: M) -> fcomponent::Event {
        fcomponent::Event {
            header: Some(fcomponent::EventHeader {
                event_type: Some(fcomponent::EventType::Started),
                moniker: Some(moniker.into()),
                ..fcomponent::EventHeader::EMPTY
            }),
            payload: Some(fcomponent::EventPayload::Started(fcomponent::StartedPayload::EMPTY)),
            ..fcomponent::Event::EMPTY
        }
    }

    // Returns a matcher for a successful Started event for the given moniker.
    fn make_matcher<M: Into<String>>(moniker: M) -> EventMatcher {
        EventMatcher::ok().r#type(Started::TYPE).moniker(moniker)
    }

    #[fuchsia::test]
    async fn event_sequence_empty() {
        let (event_stream, _server) =
            make_event_stream(vec![]).await.expect("failed to make event stream");
        EventSequence::new()
            .expect(event_stream)
            .await
            .expect("event sequence did not match expected");
    }

    #[fuchsia::test]
    async fn event_sequence_then() {
        let moniker = "./foo:0";
        let (event_stream, _server) = make_event_stream(vec![make_event(moniker)])
            .await
            .expect("failed to make event stream");
        EventSequence::new()
            .then(make_matcher(moniker))
            .expect(event_stream)
            .await
            .expect("event sequence did not match expected");
    }

    #[fuchsia::test]
    async fn event_sequence_all_of_ordered_ok() {
        let monikers = vec!["./foo:0", "./bar:0", "./baz:0"];
        let events = monikers.iter().copied().map(make_event).collect();
        let matchers = monikers.iter().copied().map(make_matcher).collect();

        let (event_stream, _server) =
            make_event_stream(events).await.expect("failed to make event stream");
        EventSequence::new()
            .all_of(matchers, Ordering::Ordered)
            .expect(event_stream)
            .await
            .expect("event sequence did not match expected");
    }

    #[fuchsia::test]
    async fn event_sequence_all_of_ordered_fail_order() {
        let monikers = vec!["./foo:0", "./bar:0", "./baz:0"];
        // Events are in reverse order of the matchers.
        let events = monikers.iter().rev().copied().map(make_event).collect();
        let matchers = monikers.iter().copied().map(make_matcher).collect();

        let (event_stream, _server) =
            make_event_stream(events).await.expect("failed to make event stream");
        assert!(EventSequence::new()
            .all_of(matchers, Ordering::Ordered)
            .expect(event_stream)
            .await
            .is_err());
    }

    #[fuchsia::test]
    async fn event_sequence_all_of_ordered_fail_missing_event() {
        let monikers = vec!["./foo:0", "./bar:0", "./baz:0"];
        // The first event is missing.
        let events = monikers.iter().skip(1).copied().map(make_event).collect();
        let matchers = monikers.iter().copied().map(make_matcher).collect();

        let (event_stream, _server) =
            make_event_stream(events).await.expect("failed to make event stream");
        assert!(EventSequence::new()
            .all_of(matchers, Ordering::Ordered)
            .expect(event_stream)
            .await
            .is_err());
    }

    #[fuchsia::test]
    async fn event_sequence_all_of_ordered_fail_missing_matcher() {
        let monikers = vec!["./foo:0", "./bar:0", "./baz:0"];
        let events = monikers.iter().copied().map(make_event).collect();
        // The first matcher is missing, so the first event is unmatched.
        let matchers = monikers.iter().skip(1).copied().map(make_matcher).collect();

        let (event_stream, _server) =
            make_event_stream(events).await.expect("failed to make event stream");
        assert!(EventSequence::new()
            .all_of(matchers, Ordering::Ordered)
            .expect(event_stream)
            .await
            .is_err());
    }

    #[fuchsia::test]
    async fn event_sequence_all_of_unordered_ok_reversed() {
        let monikers = vec!["./foo:0", "./bar:0", "./baz:0"];
        // Events are in reverse order of the matchers.
        let events = monikers.iter().rev().copied().map(make_event).collect();
        let matchers = monikers.iter().copied().map(make_matcher).collect();

        let (event_stream, _server) =
            make_event_stream(events).await.expect("failed to make event stream");
        EventSequence::new()
            .all_of(matchers, Ordering::Unordered)
            .expect(event_stream)
            .await
            .expect("event sequence did not match expected");
    }

    #[fuchsia::test]
    async fn event_sequence_has_subset_ordered_ok() {
        let monikers = vec!["./foo:0", "./bar:0", "./baz:0"];
        let events = monikers.iter().copied().map(make_event).collect();
        // The first matcher is missing, so the first event is ignored.
        let matchers = monikers.iter().skip(1).copied().map(make_matcher).collect();

        let (event_stream, _server) =
            make_event_stream(events).await.expect("failed to make event stream");
        EventSequence::new()
            .has_subset(matchers, Ordering::Ordered)
            .expect(event_stream)
            .await
            .expect("event sequence did not match expected");
    }

    #[fuchsia::test]
    async fn event_sequence_has_subset_ordered_missing_event() {
        let monikers = vec!["./foo:0", "./bar:0", "./baz:0"];
        // The first two events are missing.
        let events = monikers.iter().skip(2).copied().map(make_event).collect();
        // The first matcher is missing, so the first event is ignored.
        let matchers = monikers.iter().skip(1).copied().map(make_matcher).collect();

        // Matching should fail because the matcher for "./bar:0" can't find the event.
        let (event_stream, _server) =
            make_event_stream(events).await.expect("failed to make event stream");
        assert!(EventSequence::new()
            .has_subset(matchers, Ordering::Ordered)
            .expect(event_stream)
            .await
            .is_err());
    }

    #[fuchsia::test]
    async fn event_sequence_has_subset_unordered_ok_reversed() {
        let monikers = vec!["./foo:0", "./bar:0", "./baz:0"];
        // Events are in reverse order of the matchers.
        let events = monikers.iter().rev().copied().map(make_event).collect();
        // The first matcher is missing, so the first event is ignored.
        let matchers = monikers.iter().skip(1).copied().map(make_matcher).collect();

        let (event_stream, _server) =
            make_event_stream(events).await.expect("failed to make event stream");
        EventSequence::new()
            .has_subset(matchers, Ordering::Unordered)
            .expect(event_stream)
            .await
            .expect("event sequence did not match expected");
    }
}
