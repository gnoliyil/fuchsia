// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    component_events::{
        events::{EventMode, EventSource, EventSubscription},
        matcher::EventMatcher,
        sequence::*,
    },
    fidl_fidl_test_components as ftest, fuchsia_async as fasync,
    fuchsia_component::client::connect_to_service,
};

#[fasync::run_singlethreaded]
async fn main() {
    fuchsia_syslog::init().unwrap();

    // Track all the starting components.
    let event_source = EventSource::new().unwrap();
    let event_stream = event_source
        .subscribe(vec![EventSubscription::new(vec!["started_nested"], EventMode::Async)])
        .await
        .unwrap();

    event_source.start_component_tree().await;

    // Connect to the parent offered Trigger. The parent will start the lazy child components and
    // this component should know about their started events given that it was offered those
    // events.
    let trigger =
        connect_to_service::<ftest::TriggerMarker>().expect("error connecting to trigger");
    trigger.run().await.expect("start trigger failed");

    EventSequence::new()
        .all_of(
            vec![
                EventMatcher::ok().moniker("./child_a:0"),
                EventMatcher::ok().moniker("./child_b:0"),
                EventMatcher::ok().moniker("./child_c:0"),
            ],
            Ordering::Unordered,
        )
        .expect(event_stream)
        .await
        .unwrap();
}
