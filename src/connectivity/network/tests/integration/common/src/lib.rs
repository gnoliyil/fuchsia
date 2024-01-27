// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(missing_docs)]

//! Provides utilities for Netstack integration tests.

pub mod constants;
pub mod devices;
pub mod dhcpv4;
pub mod interfaces;
pub mod ndp;
pub mod packets;
pub mod ping;
#[macro_use]
pub mod realms;

use component_events::events::EventStream;
use fidl_fuchsia_netemul as fnetemul;
use fuchsia_async::{self as fasync, DurationExt as _};
use fuchsia_zircon as zx;

use anyhow::Context as _;
use futures::stream::{Stream, StreamExt as _, TryStreamExt as _};

use crate::realms::TestSandboxExt as _;

/// An alias for `Result<T, anyhow::Error>`.
pub type Result<T = ()> = std::result::Result<T, anyhow::Error>;

/// Extra time to use when waiting for an async event to occur.
///
/// A large timeout to help prevent flakes.
pub const ASYNC_EVENT_POSITIVE_CHECK_TIMEOUT: zx::Duration = zx::Duration::from_seconds(120);

/// Extra time to use when waiting for an async event to not occur.
///
/// Since a negative check is used to make sure an event did not happen, its okay to use a
/// smaller timeout compared to the positive case since execution stall in regards to the
/// monotonic clock will not affect the expected outcome.
pub const ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT: zx::Duration = zx::Duration::from_seconds(5);

/// The time to wait between two consecutive checks of an event.
pub const ASYNC_EVENT_CHECK_INTERVAL: zx::Duration = zx::Duration::from_seconds(1);

/// Returns `true` once the stream yields a `true`.
///
/// If the stream never yields `true` or never terminates, `try_any` may never resolve.
pub async fn try_any<S: Stream<Item = Result<bool>>>(stream: S) -> Result<bool> {
    futures::pin_mut!(stream);
    stream.try_filter(|v| futures::future::ready(*v)).next().await.unwrap_or(Ok(false))
}

/// Returns `true` if the stream only yields `true`.
///
/// If the stream never yields `false` or never terminates, `try_all` may never resolve.
pub async fn try_all<S: Stream<Item = Result<bool>>>(stream: S) -> Result<bool> {
    futures::pin_mut!(stream);
    stream.try_filter(|v| futures::future::ready(!*v)).next().await.unwrap_or(Ok(true))
}

/// Asynchronously sleeps for specified `secs` seconds.
pub async fn sleep(secs: i64) {
    fasync::Timer::new(zx::Duration::from_seconds(secs).after_now()).await;
}

/// Gets a component event stream yielding component stopped events.
pub async fn get_component_stopped_event_stream() -> Result<component_events::events::EventStream> {
    EventStream::open_at_path("/events/stopped")
        .await
        .context("failed to subscribe to `Stopped` events")
}

/// Waits for a `stopped` event to be emitted for a component in a test realm.
///
/// Optionally specifies a matcher for the expected exit status of the `stopped`
/// event.
pub async fn wait_for_component_stopped_with_stream(
    event_stream: &mut component_events::events::EventStream,
    realm: &netemul::TestRealm<'_>,
    component_moniker: &str,
    status_matcher: Option<component_events::matcher::ExitStatusMatcher>,
) -> Result<component_events::events::Stopped> {
    let matcher = get_child_component_event_matcher(realm, component_moniker)
        .await
        .context("get child component matcher")?;
    matcher.stop(status_matcher).wait::<component_events::events::Stopped>(event_stream).await
}

/// Like [`wait_for_component_stopped_with_stream`] but retrieves an event
/// stream for the caller.
///
/// Note that this function fails to observe stop events that happen in early
/// realm creation, which is especially true for eager components.
pub async fn wait_for_component_stopped(
    realm: &netemul::TestRealm<'_>,
    component_moniker: &str,
    status_matcher: Option<component_events::matcher::ExitStatusMatcher>,
) -> Result<component_events::events::Stopped> {
    let mut stream = get_component_stopped_event_stream().await?;
    wait_for_component_stopped_with_stream(&mut stream, realm, component_moniker, status_matcher)
        .await
}

/// Gets an event matcher for `component_moniker` in `realm`.
pub async fn get_child_component_event_matcher(
    realm: &netemul::TestRealm<'_>,
    component_moniker: &str,
) -> Result<component_events::matcher::EventMatcher> {
    let realm_moniker = &realm.get_moniker().await.context("calling get moniker")?;
    let moniker_for_match =
        format!("./{}/{}/{}", NETEMUL_SANDBOX_MONIKER, realm_moniker, component_moniker);
    Ok(component_events::matcher::EventMatcher::ok().moniker(moniker_for_match))
}

/// The name of the netemul sandbox component, which is the parent component of
/// managed test realms.
const NETEMUL_SANDBOX_MONIKER: &str = "sandbox";

/// Gets the moniker of a component in a test realm, relative to the root of the
/// dynamic collection in which it is running.
pub async fn get_component_moniker<'a>(
    realm: &netemul::TestRealm<'a>,
    component: &str,
) -> Result<String> {
    let realm_moniker = realm.get_moniker().await.context("calling get moniker")?;
    Ok([NETEMUL_SANDBOX_MONIKER, &realm_moniker, component].join("/"))
}

/// Gets inspect data in realm.
///
/// Returns the resulting inspect data for `component`, filtered by
/// `tree_selector` and with inspect file starting with `file_prefix`.
pub async fn get_inspect_data(
    realm: &netemul::TestRealm<'_>,
    component_moniker: impl Into<String>,
    tree_selector: impl Into<String>,
    file_prefix: &str,
) -> Result<diagnostics_hierarchy::DiagnosticsHierarchy> {
    let moniker = realm.get_moniker().await.context("calling get moniker")?;
    let realm_moniker = selectors::sanitize_string_for_selectors(&moniker);
    let mut archive_reader = diagnostics_reader::ArchiveReader::new();
    let _archive_reader_ref = archive_reader
        .add_selector(
            diagnostics_reader::ComponentSelector::new(vec![
                NETEMUL_SANDBOX_MONIKER.into(),
                realm_moniker.into_owned(),
                component_moniker.into(),
            ])
            .with_tree_selector(tree_selector.into()),
        )
        // Enable `retry_if_empty` to prevent races in test realm bringup where
        // we may end up reaching `ArchiveReader` before it has observed
        // the component starting.
        //
        // Eventually there will be support for lifecycle streams, with which it
        // will be possible to wait on the event of Archivist obtaining a handle
        // to the component's diagnostics, and then request the snapshot of
        // inspect data once that event is received.
        .retry_if_empty(true);

    // Loop to wait for the component to begin publishing inspect data after it
    // starts.
    loop {
        let mut data = archive_reader
            .snapshot::<diagnostics_reader::Inspect>()
            .await
            .context("snapshot did not return any inspect data")?
            .into_iter()
            .filter_map(
                |diagnostics_data::InspectData {
                     data_source: _,
                     metadata,
                     moniker: _,
                     payload,
                     version: _,
                 }| {
                    if metadata.filename.starts_with(file_prefix) {
                        Some(payload.ok_or_else(|| {
                            anyhow::anyhow!(
                                "empty inspect payload, metadata errors: {:?}",
                                metadata.errors
                            )
                        }))
                    } else {
                        None
                    }
                },
            );
        match data.next() {
            Some(datum) => {
                let data: Vec<_> = data.collect();
                assert!(
                    data.is_empty(),
                    "expected a single inspect entry; got {:?} and also {:?}",
                    datum,
                    data
                );
                return datum;
            }
            None => {
                fasync::Timer::new(zx::Duration::from_millis(100).after_now()).await;
            }
        }
    }
}

/// Sets up a realm with a network with no required services.
pub async fn setup_network<'a, N: realms::Netstack>(
    sandbox: &'a netemul::TestSandbox,
    name: &'a str,
    metric: Option<u32>,
) -> Result<(
    netemul::TestNetwork<'a>,
    netemul::TestRealm<'a>,
    netemul::TestInterface<'a>,
    netemul::TestFakeEndpoint<'a>,
)> {
    setup_network_with::<N, _>(sandbox, name, metric, std::iter::empty::<fnetemul::ChildDef>())
        .await
}

/// Sets up a realm with required services and a network used for tests
/// requiring manual packet inspection and transmission.
///
/// Returns the network, realm, netstack client, interface (added to the
/// netstack and up) and a fake endpoint used to read and write raw ethernet
/// packets.
pub async fn setup_network_with<'a, N: realms::Netstack, I>(
    sandbox: &'a netemul::TestSandbox,
    name: &'a str,
    metric: Option<u32>,
    children: I,
) -> Result<(
    netemul::TestNetwork<'a>,
    netemul::TestRealm<'a>,
    netemul::TestInterface<'a>,
    netemul::TestFakeEndpoint<'a>,
)>
where
    I: IntoIterator,
    I::Item: Into<fnetemul::ChildDef>,
{
    let network = sandbox.create_network(name).await.context("failed to create network")?;
    let realm = sandbox
        .create_netstack_realm_with::<N, _, _>(name, children)
        .context("failed to create netstack realm")?;
    // It is important that we create the fake endpoint before we join the
    // network so no frames transmitted by Netstack are lost.
    let fake_ep = network.create_fake_endpoint()?;

    let iface = realm
        .join_network_with_if_config(
            &network,
            name,
            netemul::InterfaceConfig { name: Some(name.into()), metric },
        )
        .await
        .context("failed to configure networking")?;

    Ok((network, realm, iface, fake_ep))
}

/// Pauses the fake clock in the given realm.
pub async fn pause_fake_clock(realm: &netemul::TestRealm<'_>) -> Result<()> {
    let fake_clock_control = realm
        .connect_to_protocol::<fidl_fuchsia_testing::FakeClockControlMarker>()
        .context("failed to connect to FakeClockControl")?;
    let () = fake_clock_control.pause().await.context("failed to pause time")?;
    Ok(())
}
