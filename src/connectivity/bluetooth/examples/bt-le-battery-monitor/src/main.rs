// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};
use async_utils::hanging_get::client::HangingGetStream;
use fidl_fuchsia_bluetooth_gatt2 as gatt;
use fidl_fuchsia_bluetooth_le::{
    CentralMarker, CentralProxy, ConnectionMarker, ConnectionOptions, ConnectionProxy, Filter,
    Peer, ScanOptions, ScanResultWatcherMarker, ScanResultWatcherProxy,
};
use fuchsia_bluetooth::types::le::Peer as btPeer;
use fuchsia_bluetooth::types::{PeerId, Uuid};
use fuchsia_component::client::connect_to_protocol;
use futures::stream::TryStreamExt;
use std::collections::HashMap;
use tracing::{info, warn};

/// The UUID of with the GATT battery service.
const BATTERY_SERVICE_UUID: Uuid = Uuid::new16(0x180f);
/// The UUID of the GATT Battery level characteristic.
const BATTERY_LEVEL_UUID: Uuid = Uuid::new16(0x2a19);

/// Represents an active LE connection to a remote peer that supports the Battery Service.
#[derive(Debug)]
struct BatteryClient {
    id: PeerId,
    /// LE connection to the peer - must be kept alive for GATT operations.
    _connection: ConnectionProxy,
    /// Connection to the peer's GATT services.
    _gatt: gatt::ClientProxy,
    /// Connection to the peer's GATT Battery service.
    _gatt_service_connection: gatt::RemoteServiceProxy,
}

fn is_battery_service(info: &gatt::ServiceInfo) -> bool {
    info.type_.map_or(false, |t| t == BATTERY_SERVICE_UUID.into())
}

fn is_battery_level(char: &gatt::Characteristic) -> bool {
    char.type_.map_or(false, |t| t == BATTERY_LEVEL_UUID.into())
}

fn is_readable(char: &gatt::Characteristic) -> bool {
    char.properties.map_or(false, |p| p.contains(gatt::CharacteristicPropertyBits::READ))
}

/// Attempts to parse the GATT Read `result` and return a battery value in the range [0, 100].
/// Returns Error if the GATT Read Result is invalidly formatted.
fn read_battery_level(result: gatt::ReadValue) -> Result<u8, Error> {
    let Some(battery_level_bytes) = result.value else { return Err(format_err!("Missing value")) };
    if battery_level_bytes.len() != 1 {
        return Err(format_err!("Invalidly formatted: {battery_level_bytes:?}"));
    }
    let battery_percent = battery_level_bytes[0].clamp(0, 100);
    Ok(battery_percent)
}

fn read_characteristics(characteristics: Vec<gatt::Characteristic>) -> Result<gatt::Handle, Error> {
    // TODO(fxbug.dev/123852): Also check NOTIFY to start monitor changes in battery level.
    let Some(chrc) = characteristics.iter().find(|c| is_battery_level(c) && is_readable(c)) else {
        return Err(format_err!("No battery level characteristic"));
    };
    // TODO(fxbug.dev/123852): `handle` check can be removed when converted to a local type.
    chrc.handle.ok_or(format_err!("characteristic missing handle"))
}

/// Parses the set of `services` and returns the Handle of the first valid Battery Service.
fn read_services(services: Vec<gatt::ServiceInfo>) -> Result<gatt::ServiceHandle, Error> {
    let Some(service) = services.into_iter().find(is_battery_service) else {
        return Err(format_err!("No compatible Battery service"));
    };
    // TODO(fxbug.dev/123852): `handle` check can be removed when converted to a local type.
    service.handle.ok_or(format_err!("service missing handle"))
}

/// Attempts to connect to the peer and read the battery level.
/// Returns OK if the battery level characteristic was successfully read, Error otherwise.
async fn try_connect(id: PeerId, central: &CentralProxy) -> Result<BatteryClient, Error> {
    info!(%id, "Trying to connect");
    // Try to connect and establish a GATT connection.
    let (le_client, le_server) = fidl::endpoints::create_proxy::<ConnectionMarker>()?;
    central.connect(&mut id.into(), ConnectionOptions::EMPTY, le_server)?;
    let (gatt_client, gatt_server) = fidl::endpoints::create_proxy::<gatt::ClientMarker>()?;
    le_client.request_gatt_client(gatt_server)?;

    // Read the GATT services offered by the peer.
    let mut uuids = vec![BATTERY_SERVICE_UUID.into()];
    let (added, _) = gatt_client.watch_services(&mut uuids.iter_mut()).await?;
    let mut service_handle = read_services(added)?;
    let (remote_client, remote_server) =
        fidl::endpoints::create_proxy::<gatt::RemoteServiceMarker>()?;
    gatt_client.connect_to_service(&mut service_handle, remote_server)?;

    // Discover the characteristics provided by the service.
    let characteristics = remote_client.discover_characteristics().await?;
    let mut battery_level_handle = read_characteristics(characteristics)?;

    let read_response = remote_client
        .read_characteristic(
            &mut battery_level_handle,
            &mut gatt::ReadOptions::ShortRead(gatt::ShortReadOptions),
        )
        .await?
        .map_err(|e| format_err!("{e:?}"))?;
    match read_battery_level(read_response) {
        Ok(level) => info!(%id, "Battery level: {level}"),
        Err(e) => info!(%id, "Failed to read battery level: {e:?}"),
    };
    // TODO(fxbug.dev/123852): If Notifications are supported, start a battery watching task.
    Ok(BatteryClient {
        id,
        _connection: le_client,
        _gatt: gatt_client,
        _gatt_service_connection: remote_client,
    })
}

/// Processes the scan `result` and returns an LE connection to a peer with a compatible battery
/// service.
async fn handle_scan_result(
    central: &CentralProxy,
    result: Vec<Peer>,
) -> Result<BatteryClient, Error> {
    // Try each peer until one succeeds.
    for peer in result {
        let peer = btPeer::try_from(peer)?;
        match try_connect(peer.id, central).await {
            Ok(battery_client) => {
                info!(id = %peer.id, "Successfully initiated LE connection");
                return Ok(battery_client);
            }
            Err(e) => info!(id = %peer.id, "Couldn't connect: {e:?}"),
        }
    }

    Err(format_err!("No compatible peers"))
}

async fn watch_scan_results(
    central: CentralProxy,
    watcher: ScanResultWatcherProxy,
) -> Result<(), Error> {
    let mut scan_result_stream =
        HangingGetStream::new(watcher.clone(), ScanResultWatcherProxy::watch);
    // Saves active connections to peers.
    let mut peers = HashMap::new();

    while let Some(scan_result) = scan_result_stream.try_next().await? {
        match handle_scan_result(&central, scan_result).await {
            Ok(peer) => {
                let _ = peers.insert(peer.id, peer);
            }
            Err(e) => warn!("Error processing scan result: {e:?}"),
        }
    }
    info!("Scan result watcher finished");
    Ok(())
}

#[fuchsia::main(logging_tags = ["bt-le-battery-monitor"])]
async fn main() -> Result<(), Error> {
    info!("Starting LE Battery Monitor");
    let central = connect_to_protocol::<CentralMarker>()?;
    let (scan_client, scan_server) = fidl::endpoints::create_proxy::<ScanResultWatcherMarker>()?;
    // Only scan for devices with the Battery Service.
    let options = ScanOptions {
        filters: Some(vec![Filter {
            service_uuid: Some(BATTERY_SERVICE_UUID.into()),
            connectable: Some(true),
            ..Filter::EMPTY
        }]),
        ..ScanOptions::EMPTY
    };
    // The lifetime of the scan will be determined by `watch_scan_results` so this Future can be
    // ignored.
    let _scan_fut = central.scan(options, scan_server).check()?;
    let scan_result = watch_scan_results(central, scan_client).await;
    info!("LE Battery Monitor finished: {scan_result:?}");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    use assert_matches::assert_matches;
    use async_test_helpers::run_while;
    use async_utils::PollExt;
    use fidl_fuchsia_bluetooth_le::CentralRequest;
    use fuchsia_async as fasync;
    use futures::{pin_mut, StreamExt};

    #[fuchsia::test]
    fn watch_scan_results_lifetime() {
        let mut exec = fasync::TestExecutor::new();
        let (central, _central_server) =
            fidl::endpoints::create_proxy_and_stream::<CentralMarker>().unwrap();
        let (watch_client, mut watch_server) =
            fidl::endpoints::create_proxy_and_stream::<ScanResultWatcherMarker>().unwrap();

        let watch_fut = watch_scan_results(central.clone(), watch_client);
        let server_fut = watch_server.next();
        pin_mut!(watch_fut, server_fut);
        // Should receive initial request to watch.
        let (result, mut watch_fut) = run_while(&mut exec, watch_fut, server_fut);
        let responder = result.unwrap().expect("fidl request").into_watch().unwrap();

        let peers: Vec<Peer> = vec![];
        let _ = responder.send(&mut peers.into_iter());
        // If the ScanResultWatcher server terminates, then the watcher future should terminate.
        drop(watch_server);

        // After running the watcher, it will register a new call to `watch` and will fail because
        // the server is gone.
        let result = exec.run_until_stalled(&mut watch_fut).expect("finished");
        assert_matches!(result, Err(_));
    }

    #[fuchsia::test]
    fn watch_scan_results_empty_reply_is_ok() {
        let mut exec = fasync::TestExecutor::new();
        let (central, _central_server) =
            fidl::endpoints::create_proxy_and_stream::<CentralMarker>().unwrap();
        let (watch_client, mut watch_server) =
            fidl::endpoints::create_proxy_and_stream::<ScanResultWatcherMarker>().unwrap();

        let watch_fut = watch_scan_results(central.clone(), watch_client);
        let server_fut = watch_server.next();
        pin_mut!(watch_fut, server_fut);
        // Should receive initial request to watch.
        let (result, mut watch_fut) = run_while(&mut exec, watch_fut, server_fut);
        let responder = result.unwrap().expect("fidl request").into_watch().unwrap();
        // Replying with no found peers is OK. Watcher should still be active.
        let peers: Vec<Peer> = vec![];
        let _ = responder.send(&mut peers.into_iter());
        let _ = exec.run_until_stalled(&mut watch_fut).expect_pending("still active");
    }

    #[fuchsia::test]
    fn read_battery_level_success() {
        let value = example_battery_level();
        let parsed_battery_level = read_battery_level(value).expect("valid read result");
        assert_eq!(parsed_battery_level, 50);

        // The value should be clamped between [0, 100].
        let value = gatt::ReadValue {
            handle: Some(gatt::Handle { value: 10 }),
            value: Some(vec![u8::MAX]),
            maybe_truncated: Some(false),
            ..gatt::ReadValue::EMPTY
        };
        let truncated_value = read_battery_level(value).expect("valid read result");
        assert_eq!(truncated_value, 100);
    }

    #[fuchsia::test]
    fn read_battery_level_error() {
        // Missing all fields is an Error.
        assert_matches!(read_battery_level(gatt::ReadValue::EMPTY), Err(_));

        // Missing the read result value is an Error.
        let missing_value = gatt::ReadValue {
            handle: Some(gatt::Handle { value: 10 }),
            maybe_truncated: Some(false),
            ..gatt::ReadValue::EMPTY
        };
        assert_matches!(read_battery_level(missing_value), Err(_));

        // A non 1-byte battery value is an Error.
        let invalid_value = gatt::ReadValue {
            handle: Some(gatt::Handle { value: 10 }),
            value: Some(vec![0, 1, 2]),
            maybe_truncated: Some(false),
            ..gatt::ReadValue::EMPTY
        };
        assert_matches!(read_battery_level(invalid_value), Err(_));
    }

    fn example_battery_service() -> gatt::ServiceInfo {
        gatt::ServiceInfo {
            handle: Some(gatt::ServiceHandle { value: 5 }),
            kind: Some(gatt::ServiceKind::Primary),
            type_: Some(BATTERY_SERVICE_UUID.into()),
            ..gatt::ServiceInfo::EMPTY
        }
    }

    fn example_battery_level_characteristic() -> gatt::Characteristic {
        gatt::Characteristic {
            handle: Some(gatt::Handle { value: 15 }),
            type_: Some(BATTERY_LEVEL_UUID.into()),
            properties: Some(
                gatt::CharacteristicPropertyBits::READ | gatt::CharacteristicPropertyBits::NOTIFY,
            ),
            ..gatt::Characteristic::EMPTY
        }
    }

    fn example_battery_level() -> gatt::ReadValue {
        gatt::ReadValue {
            handle: Some(gatt::Handle { value: 10 }),
            value: Some(vec![50]),
            maybe_truncated: Some(false),
            ..gatt::ReadValue::EMPTY
        }
    }

    #[fuchsia::test]
    fn try_connect_success() {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(123);
        let (central_client, mut central_server) =
            fidl::endpoints::create_proxy_and_stream::<CentralMarker>().unwrap();
        let connect_fut = try_connect(id, &central_client);
        pin_mut!(connect_fut);

        exec.run_until_stalled(&mut connect_fut).expect_pending("waiting for result");

        // Expect a LE connect request.
        let central_fut = central_server.select_next_some();
        let (central_result, connect_fut) = run_while(&mut exec, connect_fut, central_fut);
        let mut gatt_connection_server = match central_result {
            Ok(CentralRequest::Connect { id: received_id, handle, .. }) => {
                assert_eq!(received_id, id.into());
                handle.into_stream().expect("valid FIDL server")
            }
            x => panic!("Expected Connect got: {x:?}"),
        };

        // Expect a request to connect GATT.
        let gatt_connect_fut = gatt_connection_server.select_next_some();
        let (gatt_connect_result, connect_fut) =
            run_while(&mut exec, connect_fut, gatt_connect_fut);
        let (gatt_server, _) =
            gatt_connect_result.unwrap().into_request_gatt_client().expect("only request");
        let mut gatt_server = gatt_server.into_stream().unwrap();

        // Expect a request to watch GATT services - send back the example service.
        let gatt_fut = gatt_server.select_next_some();
        let (gatt_result, connect_fut) = run_while(&mut exec, connect_fut, gatt_fut);
        let (_, responder) = gatt_result.unwrap().into_watch_services().unwrap();
        let _ = responder
            .send(&mut vec![example_battery_service()].into_iter(), &mut vec![].into_iter());

        // Expect a request to connect to the service and discover characteristics.
        let gatt_fut = gatt_server.select_next_some();
        let (gatt_result, connect_fut) = run_while(&mut exec, connect_fut, gatt_fut);
        let (_, remote_service_server, _) = gatt_result.unwrap().into_connect_to_service().unwrap();
        let mut remote_service_server = remote_service_server.into_stream().unwrap();
        let discover_fut = remote_service_server.select_next_some();
        let (discover_result, connect_fut) = run_while(&mut exec, connect_fut, discover_fut);
        let responder = discover_result.unwrap().into_discover_characteristics().unwrap();
        // Send back an example battery level characteristic.
        let _ = responder.send(&mut vec![example_battery_level_characteristic()].into_iter());

        // Expect a request to read the battery level - send back an example battery level.
        let read_fut = remote_service_server.select_next_some();
        let (read_result, mut connect_fut) = run_while(&mut exec, connect_fut, read_fut);
        let (_, _, responder) = read_result.unwrap().into_read_characteristic().unwrap();
        let _ = responder.send(&mut Ok(example_battery_level()));

        let result = exec.run_until_stalled(&mut connect_fut).expect("connect success");
        assert_matches!(result, Ok(_));
    }
}
