// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{bail, format_err, Error},
    fidl_fuchsia_wlan_device as fidl_wlan_dev,
    fidl_fuchsia_wlan_mlme::{self as fidl_mlme, DeviceInfo},
    fuchsia_cobalt::CobaltSender,
    fuchsia_inspect_contrib::inspect_log,
    futures::{
        channel::mpsc,
        future::{Future, FutureExt, FutureObj},
        select,
        stream::{FuturesUnordered, Stream, StreamExt, TryStreamExt},
    },
    log::{error, info},
    pin_utils::pin_mut,
    std::{marker::Unpin, sync::Arc},
    void::Void,
    wlan_inspect,
    wlan_sme::{self, clone_utils},
};

use crate::{
    device_watch, inspect,
    mlme_query_proxy::MlmeQueryProxy,
    station,
    stats_scheduler::{self, StatsScheduler},
    watchable_map::WatchableMap,
    ServiceCfg,
};

/// Iface's PHY information.
#[derive(Debug, PartialEq)]
pub struct PhyOwnership {
    // Iface's global PHY ID.
    pub phy_id: u16,
    // Local ID assigned by this iface's PHY.
    pub phy_assigned_id: u16,
}

#[derive(Debug)]
pub struct NewIface {
    // Global, unique iface ID.
    pub id: u16,
    // Information about this iface's PHY.
    pub phy_ownership: PhyOwnership,
    // A channel to communicate with the iface's underlying MLME.
    pub mlme_channel: fidl_mlme::MlmeProxy,
}

pub struct PhyDevice {
    pub proxy: fidl_wlan_dev::PhyProxy,
    pub device: wlan_dev::Device,
}

pub type ClientSmeServer = mpsc::UnboundedSender<super::station::client::Endpoint>;
pub type ApSmeServer = mpsc::UnboundedSender<super::station::ap::Endpoint>;
pub type MeshSmeServer = mpsc::UnboundedSender<super::station::mesh::Endpoint>;

pub enum SmeServer {
    Client(ClientSmeServer),
    Ap(ApSmeServer),
    Mesh(MeshSmeServer),
}

pub struct IfaceDevice {
    pub phy_ownership: PhyOwnership,
    pub sme_server: SmeServer,
    pub stats_sched: StatsScheduler,
    pub mlme_query: MlmeQueryProxy,
    pub device_info: DeviceInfo,
}

pub type PhyMap = WatchableMap<u16, PhyDevice>;
pub type IfaceMap = WatchableMap<u16, IfaceDevice>;

pub async fn serve_phys(
    phys: Arc<PhyMap>,
    isolated_devmgr: bool,
    inspect_tree: Arc<inspect::WlanstackTree>,
) -> Result<Void, Error> {
    let new_phys = if isolated_devmgr {
        device_watch::watch_phy_devices::<wlan_dev::IsolatedDeviceEnv>()?.left_stream()
    } else {
        device_watch::watch_phy_devices::<wlan_dev::RealDeviceEnv>()?.right_stream()
    };
    pin_mut!(new_phys);
    let mut active_phys = FuturesUnordered::new();
    loop {
        select! {
            // OK to fuse directly in the `select!` since we bail immediately
            // when a `None` is encountered.
            new_phy = new_phys.next().fuse() => match new_phy {
                None => bail!("new phy stream unexpectedly finished"),
                Some(Err(e)) => bail!("new phy stream returned an error: {}", e),
                Some(Ok(new_phy)) => {
                    let fut = serve_phy(&phys, new_phy, inspect_tree.clone());
                    active_phys.push(fut);
                }
            },
            () = active_phys.select_next_some() => {},
        }
    }
}

async fn serve_phy(
    phys: &PhyMap,
    new_phy: device_watch::NewPhyDevice,
    inspect_tree: Arc<inspect::WlanstackTree>,
) {
    let msg = format!("new phy #{}: {}", new_phy.id, new_phy.device.path().to_string_lossy());
    info!("{}", msg);
    inspect_log!(inspect_tree.device_events.lock(), msg: msg);
    let id = new_phy.id;
    let event_stream = new_phy.proxy.take_event_stream();
    phys.insert(id, PhyDevice { proxy: new_phy.proxy, device: new_phy.device });
    let r = event_stream.map_ok(|_| ()).try_collect::<()>().await;
    phys.remove(&id);
    if let Err(e) = r {
        let msg = format!("error reading from FIDL channel of phy #{}: {}", id, e);
        error!("{}", msg);
        inspect_log!(inspect_tree.device_events.lock(), msg: msg);
    }
    info!("phy removed: #{}", id);
    inspect_log!(inspect_tree.device_events.lock(), msg: format!("phy removed: #{}", id));
}

pub async fn query_and_serve_iface(
    cfg: ServiceCfg,
    id: u16,
    phy_ownership: PhyOwnership,
    mlme_proxy: fidl_mlme::MlmeProxy,
    ifaces: Arc<IfaceMap>,
    inspect_tree: Arc<inspect::WlanstackTree>,
    iface_tree_holder: Arc<wlan_inspect::iface_mgr::IfaceTreeHolder>,
    cobalt_sender: CobaltSender,
) -> Result<(), failure::Error> {
    let event_stream = mlme_proxy.take_event_stream();
    let (stats_sched, stats_reqs) = stats_scheduler::create_scheduler();

    let device_info = mlme_proxy
        .query_device_info()
        .await
        .map_err(|e| format_err!("failed querying iface: {}", e))?;
    let (sme, sme_fut) = create_sme(
        cfg,
        mlme_proxy.clone(),
        event_stream,
        &device_info,
        stats_reqs,
        cobalt_sender,
        iface_tree_holder,
    )
    .map_err(|e| format_err!("failed to creating SME: {}", e))?;

    info!("new iface #{} with role '{:?}'", id, device_info.role);
    inspect_log!(inspect_tree.device_events.lock(), {
        msg: format!("new iface #{} with role '{:?}'", id, device_info.role)
    });
    let mlme_query = MlmeQueryProxy::new(mlme_proxy);
    ifaces.insert(
        id,
        IfaceDevice { phy_ownership, sme_server: sme, stats_sched, mlme_query, device_info },
    );

    let result = sme_fut.await.map_err(|e| format_err!("error while serving SME: {}", e));
    info!("iface removed: #{}", id);
    inspect_log!(inspect_tree.device_events.lock(), msg: format!("iface removed: #{}", id));
    ifaces.remove(&id);
    result
}

fn create_sme<S>(
    cfg: ServiceCfg,
    proxy: fidl_mlme::MlmeProxy,
    event_stream: fidl_mlme::MlmeEventStream,
    device_info: &DeviceInfo,
    stats_requests: S,
    cobalt_sender: CobaltSender,
    iface_tree_holder: Arc<wlan_inspect::iface_mgr::IfaceTreeHolder>,
) -> Result<(SmeServer, impl Future<Output = Result<(), Error>>), Error>
where
    S: Stream<Item = stats_scheduler::StatsRequest> + Send + Unpin + 'static,
{
    let role = device_info.role;
    let device_info = wlan_sme::DeviceInfo {
        addr: device_info.mac_addr,
        bands: clone_utils::clone_bands(&device_info.bands),
        driver_features: device_info.driver_features.clone(),
    };

    match role {
        fidl_mlme::MacRole::Client => {
            let (sender, receiver) = mpsc::unbounded();
            let fut = station::client::serve(
                cfg.into(),
                proxy,
                device_info,
                event_stream,
                receiver,
                stats_requests,
                cobalt_sender,
                iface_tree_holder,
            );
            Ok((SmeServer::Client(sender), FutureObj::new(Box::new(fut))))
        }
        fidl_mlme::MacRole::Ap => {
            let (sender, receiver) = mpsc::unbounded();
            let fut =
                station::ap::serve(proxy, device_info, event_stream, receiver, stats_requests);
            Ok((SmeServer::Ap(sender), FutureObj::new(Box::new(fut))))
        }
        fidl_mlme::MacRole::Mesh => {
            let (sender, receiver) = mpsc::unbounded();
            let fut =
                station::mesh::serve(proxy, device_info, event_stream, receiver, stats_requests);
            Ok((SmeServer::Mesh(sender), FutureObj::new(Box::new(fut))))
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl::endpoints::create_proxy,
        fidl_fuchsia_wlan_common::DriverFeature,
        fidl_fuchsia_wlan_mlme::MlmeMarker,
        fuchsia_async as fasync,
        fuchsia_cobalt::{self, CobaltSender},
        fuchsia_inspect::{assert_inspect_tree, Inspector},
        futures::channel::mpsc,
        futures::sink::SinkExt,
        futures::task::Poll,
        pin_utils::pin_mut,
        wlan_common::assert_variant,
    };

    fn fake_device_info() -> DeviceInfo {
        fidl_mlme::DeviceInfo {
            role: fidl_mlme::MacRole::Client,
            bands: vec![],
            mac_addr: [0xAC; 6],
            driver_features: vec![DriverFeature::TempDirectSmeChannel],
        }
    }

    #[test]
    fn query_serve_with_sme_channel() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let (mlme_proxy, mlme_server) =
            create_proxy::<MlmeMarker>().expect("failed to create MlmeProxy");
        let mut mlme_stream = mlme_server.into_stream().expect("failed to create stream");
        let (iface_map, _iface_map_events) = IfaceMap::new();
        let iface_map = Arc::new(iface_map);
        let inspect_tree = Arc::new(inspect::WlanstackTree::new(Inspector::new()));
        let iface_tree_holder = inspect_tree.create_iface_child(1);
        let (sender, _receiver) = mpsc::channel(1);
        let cobalt_sender = CobaltSender::new(sender);

        let serve_fut = query_and_serve_iface(
            ServiceCfg::default(),
            5,
            PhyOwnership { phy_id: 1, phy_assigned_id: 2 },
            mlme_proxy,
            iface_map.clone(),
            inspect_tree.clone(),
            iface_tree_holder,
            cobalt_sender,
        );
        pin_mut!(serve_fut);
        // Progress to cause query request.
        let fut_result = exec.run_until_stalled(&mut serve_fut);
        assert_variant!(fut_result, Poll::Pending, "expected pending iface creation");

        // The call above should trigger a Query message to the iface.
        assert_variant!(exec.run_until_stalled(&mut mlme_stream.next()),
            Poll::Ready(Some(Ok(fidl_mlme::MlmeRequest::QueryDeviceInfo { responder }))) => {
                // Respond with query message.
                responder.send(&mut fake_device_info()).expect("failed to send QueryResponse");
            }
        );

        // Progress to cause SME creation and serving.
        assert!(iface_map.get(&5).is_none());
        let fut_result = exec.run_until_stalled(&mut serve_fut);
        assert_variant!(fut_result, Poll::Pending, "expected pending SME serving");

        // Retrieve SME instance and close SME (iface must be acquired).
        let mut iface = iface_map.get(&5).expect("expected iface");
        iface_map.remove(&5);
        let mut_iface = Arc::get_mut(&mut iface).expect("error yielding iface");
        let sme = assert_variant!(
            mut_iface.sme_server,
            SmeServer::Client(ref mut sme) => sme,
            "expected Client SME to be spawned"
        );
        let close_fut = sme.close();
        pin_mut!(close_fut);
        let fut_result = exec.run_until_stalled(&mut close_fut);
        assert_variant!(fut_result, Poll::Ready(_), "expected closing SME to succeed");

        // Insert iface back into map.
        let (mlme_proxy, _) = create_proxy::<MlmeMarker>().expect("failed to create MlmeProxy");
        let (sender, _) = mpsc::unbounded();
        let (stats_sched, _) = stats_scheduler::create_scheduler();
        iface_map.insert(
            5,
            IfaceDevice {
                phy_ownership: PhyOwnership { phy_id: 0, phy_assigned_id: 0 },
                sme_server: SmeServer::Client(sender),
                stats_sched,
                mlme_query: MlmeQueryProxy::new(mlme_proxy),
                device_info: fake_device_info(),
            },
        );
        iface_map.get(&5).expect("expected iface");

        // Progress SME serving to completion and verify iface was deleted
        let fut_result = exec.run_until_stalled(&mut serve_fut);
        assert_variant!(fut_result, Poll::Ready(_), "expected SME serving to be terminated");
        assert!(iface_map.get(&5).is_none());

        assert_inspect_tree!(inspect_tree.inspector, root: contains {
            device_events: {
                "0": contains { msg: "new iface #5 with role 'Client'" },
                "1": contains { msg: "iface removed: #5" },
            },
        });
    }
}
