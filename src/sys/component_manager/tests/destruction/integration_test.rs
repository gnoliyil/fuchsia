// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    component_events::{
        events::*,
        matcher::EventMatcher,
        sequence::{EventSequence, Ordering},
    },
    fidl_fuchsia_component as fcomponent, fuchsia_async as fasync,
    fuchsia_component_test::{Capability, ChildOptions, RealmBuilder, Ref, Route},
};

#[fasync::run_singlethreaded(test)]
async fn destroy() {
    let builder = RealmBuilder::new().await.unwrap();
    let collection_realm = builder
        .add_child("collection_realm", "#meta/collection_realm.cm", ChildOptions::new().eager())
        .await
        .unwrap();
    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                .from(Ref::parent())
                .to(&collection_realm),
        )
        .await
        .unwrap();
    let instance =
        builder.build_in_nested_component_manager("#meta/component_manager.cm").await.unwrap();
    let proxy = instance
        .root
        .connect_to_protocol_at_exposed_dir::<fcomponent::EventStreamMarker>()
        .unwrap();
    proxy.wait_for_ready().await.unwrap();

    let event_stream = EventStream::new(proxy);

    let expectation = EventSequence::new()
        .has_subset(
            vec![
                EventMatcher::ok()
                    .r#type(Stopped::TYPE)
                    .moniker("./collection_realm/coll:parent/trigger_a"),
                EventMatcher::ok()
                    .r#type(Stopped::TYPE)
                    .moniker("./collection_realm/coll:parent/trigger_b"),
                EventMatcher::ok().r#type(Stopped::TYPE).moniker("./collection_realm/coll:parent"),
                EventMatcher::ok()
                    .r#type(Destroyed::TYPE)
                    .moniker("./collection_realm/coll:parent/trigger_a"),
                EventMatcher::ok()
                    .r#type(Destroyed::TYPE)
                    .moniker("./collection_realm/coll:parent/trigger_b"),
                EventMatcher::ok()
                    .r#type(Destroyed::TYPE)
                    .moniker("./collection_realm/coll:parent"),
            ],
            Ordering::Unordered,
        )
        .expect(event_stream);
    instance.start_component_tree().await.unwrap();

    // Assert the expected lifecycle events. The leaves can be stopped/destroyed in either order.
    expectation.await.unwrap();
}

#[fasync::run_singlethreaded(test)]
async fn destroy_and_recreate() {
    let builder = RealmBuilder::new().await.unwrap();
    let destroy_and_recreate = builder
        .add_child(
            "destroy_and_recreate",
            "#meta/destroy_and_recreate.cm",
            ChildOptions::new().eager(),
        )
        .await
        .unwrap();
    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                .from(Ref::parent())
                .to(&destroy_and_recreate),
        )
        .await
        .unwrap();
    let instance =
        builder.build_in_nested_component_manager("#meta/component_manager.cm").await.unwrap();
    let proxy = instance
        .root
        .connect_to_protocol_at_exposed_dir::<fcomponent::EventStreamMarker>()
        .unwrap();
    proxy.wait_for_ready().await.unwrap();

    let event_stream = EventStream::new(proxy);

    instance.start_component_tree().await.unwrap();

    EventSequence::new()
        .has_subset(
            vec![
                EventMatcher::ok()
                    .r#type(Started::TYPE)
                    .moniker("./destroy_and_recreate/coll:trigger"),
                EventMatcher::ok()
                    .r#type(Destroyed::TYPE)
                    .moniker("./destroy_and_recreate/coll:trigger"),
            ],
            Ordering::Ordered,
        )
        .has_subset(
            vec![
                EventMatcher::ok()
                    .r#type(Started::TYPE)
                    .moniker("./destroy_and_recreate/coll:trigger"),
                EventMatcher::ok()
                    .r#type(Destroyed::TYPE)
                    .moniker("./destroy_and_recreate/coll:trigger"),
            ],
            // The previous instance can be purged before/after the new instance is started.
            // That is why this sequence is unordered.
            Ordering::Unordered,
        )
        .expect(event_stream)
        .await
        .unwrap();
}
