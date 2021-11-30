// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    constants::*,
    test_topology::{self, expose_test_realm_protocol},
    utils,
};
use component_events::{events::*, matcher::*};
use diagnostics_data::Lifecycle;
use diagnostics_reader::{assert_data_tree, ArchiveReader, Data, Logs};
use fasync::Task;
use fidl::endpoints::ServerEnd;
use fidl_fuchsia_component as fcomponent;
use fidl_fuchsia_component::RealmMarker;
use fidl_fuchsia_component_decl::ChildRef;
use fidl_fuchsia_diagnostics::ArchiveAccessorMarker;
use fidl_fuchsia_io::DirectoryMarker;
use fidl_fuchsia_sys2::EventSourceMarker;
use fidl_fuchsia_sys_internal::{LogConnectorRequest, LogConnectorRequestStream};
use fuchsia_async as fasync;
use fuchsia_component::{client, server::ServiceFs};
use fuchsia_component_test::{mock::MockHandles, ChildProperties, RouteBuilder, RouteEndpoint};
use fuchsia_zircon as zx;
use futures::{channel::mpsc, lock::Mutex, SinkExt, StreamExt};
use std::sync::Arc;
use zx::DurationNum;

async fn serve_mocks(
    mock_handles: MockHandles,
    before_response_recv: Arc<Mutex<mpsc::UnboundedReceiver<()>>>,
    after_response_snd: mpsc::UnboundedSender<()>,
) -> Result<(), anyhow::Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |mut stream: LogConnectorRequestStream| {
        let recv_clone = before_response_recv.clone();
        let mut send_clone = after_response_snd.clone();
        Task::spawn(async move {
            while let Some(Ok(LogConnectorRequest::TakeLogConnectionListener { responder })) =
                stream.next().await
            {
                let (_client, server) = zx::Channel::create().unwrap();
                recv_clone.lock().await.next().await;
                responder.send(Some(ServerEnd::new(server))).unwrap();
                send_clone.send(()).await.unwrap();
            }
        })
        .detach()
    });
    fs.serve_connection(mock_handles.outgoing_dir.into_channel()).unwrap();
    fs.collect::<()>().await;
    Ok(())
}

const LOG_AND_EXIT_COMPONENT: &str = "log_and_exit";

#[fuchsia::test]
async fn test_logs_with_hanging_log_connector() {
    let (mut before_response_snd, before_response_recv) = mpsc::unbounded();
    let (after_response_snd, mut after_response_recv) = mpsc::unbounded();
    let recv = Arc::new(Mutex::new(before_response_recv));
    let builder = test_topology::create(test_topology::Options {
        archivist_url:
            "fuchsia-pkg://fuchsia.com/archivist-integration-tests-v2#meta/archivist_with_log_connector.cm",
    })
    .await
    .expect("create base topology");
    builder
        .add_mock_child(
            "mocks-server",
            move |mock_handles| {
                Box::pin(serve_mocks(mock_handles, recv.clone(), after_response_snd.clone()))
            },
            ChildProperties::new(),
        )
        .await
        .unwrap()
        .add_route(
            RouteBuilder::protocol("fuchsia.sys.internal.LogConnector")
                .source(RouteEndpoint::component("mocks-server"))
                .targets(vec![RouteEndpoint::component("test/archivist")]),
        )
        .await
        .unwrap();
    test_topology::add_eager_child(&builder, LOG_AND_EXIT_COMPONENT, LOG_AND_EXIT_COMPONENT_URL)
        .await
        .expect("add log_and_exit");

    let instance = builder.build().await.expect("create instance");
    let accessor =
        instance.root.connect_to_protocol_at_exposed_dir::<ArchiveAccessorMarker>().unwrap();

    let mut reader = ArchiveReader::new();
    reader
        .with_archive(accessor)
        .with_minimum_schema_count(0) // we want this to return even when no log messages
        .retry_if_empty(false);

    let (mut subscription, mut errors) =
        reader.snapshot_then_subscribe::<Logs>().unwrap().split_streams();
    let _log_errors = fasync::Task::spawn(async move {
        if let Some(error) = errors.next().await {
            panic!("{:#?}", error);
        }
    });

    let moniker = format!(
        "fuchsia_component_test_collection:{}/test/log_and_exit",
        instance.root.child_name()
    );

    reader.retry_if_empty(true);

    check_message(&moniker, subscription.next().await.unwrap());
    // Trigger a response to the TakeLogConnectionListener request that is hanging for
    // the purposes of the test and ensure the archivist received it so we don't see a PEER_CLOSED
    // in the realm builder server component.
    before_response_snd.send(()).await.unwrap();
    after_response_recv.next().await.unwrap();
}

const RETRY_DELAY_MS: i64 = 300;

async fn wait_for_log_sink_connected_event(reader: &mut ArchiveReader) {
    loop {
        let results = reader.snapshot::<Lifecycle>().await.unwrap();
        let result_contains_component = results.into_iter().any(|result| {
            result.metadata.lifecycle_event_type
                == diagnostics_data::LifecycleType::LogSinkConnected
                && result.moniker.contains(LOG_AND_EXIT_COMPONENT)
        });
        if result_contains_component {
            break;
        }
        fasync::Timer::new(fasync::Time::after(RETRY_DELAY_MS.millis())).await;
    }
}

#[fuchsia::test]
async fn log_sink_connected_event_test() {
    let builder = test_topology::create(test_topology::Options::default())
        .await
        .expect("create base topology");
    test_topology::add_eager_child(&builder, LOG_AND_EXIT_COMPONENT, LOG_AND_EXIT_COMPONENT_URL)
        .await
        .expect("add log_and_exit");

    let instance = builder.build().await.expect("create instance");
    let accessor =
        instance.root.connect_to_protocol_at_exposed_dir::<ArchiveAccessorMarker>().unwrap();

    let mut reader = ArchiveReader::new();
    reader
        .with_archive(accessor)
        .with_minimum_schema_count(0) // we want this to return even when no log messages
        .retry_if_empty(false);

    wait_for_log_sink_connected_event(&mut reader).await;
}

#[fuchsia::test]
async fn test_logs_lifecycle() {
    // TODO(https://fxbug.dev/89184): Remove when fixed.
    eprintln!("DEBUG0");
    let builder = test_topology::create(test_topology::Options::default())
        .await
        .expect("create base topology");
    test_topology::add_lazy_child(&builder, LOG_AND_EXIT_COMPONENT, LOG_AND_EXIT_COMPONENT_URL)
        .await
        .expect("add log_and_exit");
    eprintln!("DEBUG1");
    // Currently RealmBuilder doesn't support to expose a capability from framework, therefore we
    // manually update the decl that the builder creates.
    expose_test_realm_protocol(&builder).await;
    eprintln!("DEBUG2");
    let instance = builder.build().await.expect("create instance");
    let accessor =
        instance.root.connect_to_protocol_at_exposed_dir::<ArchiveAccessorMarker>().unwrap();
    eprintln!("DEBUG3");
    let mut reader = ArchiveReader::new();
    reader
        .with_archive(accessor)
        .with_minimum_schema_count(0) // we want this to return even when no log messages
        .retry_if_empty(false);

    let (mut subscription, mut errors) =
        reader.snapshot_then_subscribe::<Logs>().unwrap().split_streams();
    let _log_errors = fasync::Task::spawn(async move {
        if let Some(error) = errors.next().await {
            panic!("{:#?}", error);
        }
    });
    eprintln!("DEBUG4");
    let moniker = format!(
        "fuchsia_component_test_collection:{}/test/log_and_exit",
        instance.root.child_name()
    );

    let event_source =
        EventSource::from_proxy(client::connect_to_protocol::<EventSourceMarker>().unwrap());
    let mut event_stream = event_source
        .subscribe(vec![EventSubscription::new(vec![Stopped::NAME], EventMode::Async)])
        .await
        .unwrap();
    eprintln!("DEBUG5");
    let mut child_ref = ChildRef { name: LOG_AND_EXIT_COMPONENT.to_string(), collection: None };
    reader.retry_if_empty(true);
    for i in 1..100 {
        eprintln!("DEBUG6");
        // launch our child and wait for it to exit before asserting on its logs
        let (exposed_dir, server_end) = fidl::endpoints::create_proxy::<DirectoryMarker>().unwrap();
        let realm = instance.root.connect_to_protocol_at_exposed_dir::<RealmMarker>().unwrap();
        realm.open_exposed_dir(&mut child_ref, server_end).await.unwrap().unwrap();

        let _ = client::connect_to_protocol_at_dir_root::<fcomponent::BinderMarker>(&exposed_dir)
            .unwrap();

        eprintln!("DEBUG7");
        utils::wait_for_component_stopped_event(
            &instance.root.child_name(),
            LOG_AND_EXIT_COMPONENT,
            ExitStatusMatcher::Clean,
            &mut event_stream,
        )
        .await;

        eprintln!("DEBUG8");
        check_message(&moniker, subscription.next().await.unwrap());
        eprintln!("DEBUG9");
        reader.with_minimum_schema_count(i);
        let all_messages = reader.snapshot::<Logs>().await.unwrap();
        eprintln!("DEBUG10");
        for message in all_messages {
            check_message(&moniker, message);
        }
        eprintln!("DEBUG11");
    }
}

fn check_message(expected_moniker: &str, message: Data<Logs>) {
    assert_eq!(message.moniker, expected_moniker,);
    assert_eq!(message.metadata.component_url, Some(LOG_AND_EXIT_COMPONENT_URL.to_string()));

    assert_data_tree!(message.payload.unwrap(), root: {
        message: {
            value: "Hello, world!",
        }
    });
}
