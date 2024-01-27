// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;

use anyhow::Error;
use async_utils::hanging_get::client::HangingGetStream;
use fidl::endpoints::create_endpoints;
use fuchsia_async::Task;
use fuchsia_component::client::connect_to_protocol;
use fuchsia_inspect::{LazyNode, Node, StringProperty};
use fuchsia_inspect_contrib::inspect_log;
use fuchsia_inspect_contrib::nodes::{BoundedListNode, NodeExt, TimeProperty};
use lowpan_driver_common::lowpan_fidl::{
    CountersConnectorMarker, CountersMarker, DeviceConnectorMarker, DeviceExtraConnectorMarker,
    DeviceExtraMarker, DeviceMarker, DeviceState, DeviceTestConnectorMarker, DeviceTestMarker,
    DeviceWatcherMarker, DeviceWatcherProxyInterface, Identity, TelemetryProviderConnectorMarker,
    TelemetryProviderMarker,
};
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

type IfaceId = String;

// Limit was chosen arbitrary.
const EVENTS_LIMIT: usize = 20;

pub struct LowpanServiceTree {
    // Root of the tree
    inspector: Inspector,

    // "events" subtree
    events: Mutex<BoundedListNode>,

    // "iface-<n>" subtrees, where n is the iface ID.
    ifaces_trees: Mutex<HashMap<IfaceId, Arc<IfaceTreeHolder>>>,

    // Iface devices that have been removed but whose debug infos are still kept in Inspect tree.
    dead_ifaces: Mutex<HashMap<IfaceId, Arc<IfaceTreeHolder>>>,
}

impl LowpanServiceTree {
    pub fn new(inspector: Inspector) -> Self {
        let events = inspector.root().create_child("events");
        Self {
            inspector,
            events: Mutex::new(BoundedListNode::new(events, EVENTS_LIMIT)),
            ifaces_trees: Mutex::new(HashMap::new()),
            dead_ifaces: Mutex::new(HashMap::new()),
        }
    }

    pub fn create_iface_child(&self, iface_id: IfaceId) -> Arc<IfaceTreeHolder> {
        // Check if this iface is already in |dead_ifaces|
        if let Some(prev_holder) = self.dead_ifaces.lock().remove(&iface_id) {
            self.ifaces_trees.lock().insert(iface_id, prev_holder.clone());
            prev_holder
        } else {
            let child = self.inspector.root().create_child(&format!("iface-{}", iface_id));
            let holder = Arc::new(IfaceTreeHolder::new(child));
            self.ifaces_trees.lock().insert(iface_id, holder.clone());
            holder
        }
    }

    pub fn notify_iface_removed(&self, iface_id: IfaceId) {
        let mut iface_tree_list = self.ifaces_trees.lock();
        let mut dead_iface_list = self.dead_ifaces.lock();
        if let Some(child_holder) = iface_tree_list.remove(&iface_id) {
            inspect_log!(child_holder.events.lock(), msg: "Removed");

            //Remove lazy child monitoring for these interfaces
            {
                let mut lazy_counters = child_holder.counters.lock();
                *lazy_counters = LazyNode::default();
                let mut lazy_neighbors = child_holder.neighbors.lock();
                *lazy_neighbors = LazyNode::default();
                let mut lazy_telemetry = child_holder.telemetry.lock();
                *lazy_telemetry = LazyNode::default();
            }

            dead_iface_list.insert(iface_id, child_holder.clone());
        }
    }
}

pub struct IfaceTreeHolder {
    node: Node,
    // "iface-*/events" subtree
    events: Mutex<BoundedListNode>,
    // "iface-*/status" subtree
    status: Mutex<IfaceStatusNode>,
    // "iface-*/counters" subtree
    counters: Mutex<LazyNode>,
    // "iface-*/neighbors" subtree
    neighbors: Mutex<LazyNode>,
    // "iface-*/telemetry" subtree
    telemetry: Mutex<LazyNode>,
}

impl IfaceTreeHolder {
    pub fn new(node: Node) -> Self {
        let events = node.create_child("events");
        let status = node.create_child("status");
        Self {
            node,
            events: Mutex::new(BoundedListNode::new(events, EVENTS_LIMIT)),
            status: Mutex::new(IfaceStatusNode::new(status)),
            counters: Mutex::new(LazyNode::default()),
            neighbors: Mutex::new(LazyNode::default()),
            telemetry: Mutex::new(LazyNode::default()),
        }
    }

    pub fn update_status(&self, new_state: DeviceState) {
        let new_connectivity_state = match new_state.connectivity_state {
            Some(state) => format!("{:?}", state),
            None => String::from(""),
        };

        let online_states: HashSet<String> =
            vec![String::from("Attaching"), String::from("Attached"), String::from("Isolated")]
                .into_iter()
                .collect();
        let mut status = self.status.lock();
        let mut status_change_messages = Vec::new();
        if online_states.contains(&new_connectivity_state)
            && !online_states.contains(&status.connectivity_state_value)
        {
            status._online_since = Some(status.node.create_time("online_since"));
        } else if !online_states.contains(&new_connectivity_state) {
            status._online_since = None;
        }
        if new_connectivity_state != status.connectivity_state_value {
            status_change_messages.push(format!(
                "connectivity_state:{}->{}",
                status.connectivity_state_value, new_connectivity_state
            ));
            status.connectivity_state_value = new_connectivity_state.clone();
            status._connectivity_state =
                status.node.create_string("connectivity_state", new_connectivity_state);
        }

        let new_role = match new_state.role {
            Some(role) => format!("{:?}", role),
            None => String::from(""),
        };
        if new_role != status.role_value {
            status_change_messages.push(format!("role:{}->{}\n", status.role_value, new_role));
            status.role_value = new_role.clone();
            status._role = status.node.create_string("role", new_role);
        }
        let joined_status_messages = status_change_messages.join(";");
        if !joined_status_messages.is_empty() {
            inspect_log!(self.events.lock(), msg: joined_status_messages);
        }
    }

    pub fn update_identity(&self, new_identity: Identity) {
        let mut status = self.status.lock();
        let new_net_type = new_identity.net_type.unwrap_or("".to_string());
        let mut status_change_messages = Vec::new();
        if new_net_type != status.net_type_value {
            status_change_messages
                .push(format!("net_type:{}->{}", status.net_type_value, new_net_type));
            status.net_type_value = new_net_type.clone();
            status._net_type = status.node.create_string("net_type", new_net_type);
        }

        let new_channel = match new_identity.channel {
            None => String::new(),
            Some(channel) => channel.to_string(),
        };
        if new_channel != status.channel_value {
            status_change_messages
                .push(format!("channel:{}->{}", status.channel_value, new_channel));

            status.channel_value = new_channel.clone();
            status._channel = status.node.create_string("channel", new_channel);
        }
        let joined_status_messages = status_change_messages.join(";");
        if !joined_status_messages.is_empty() {
            inspect_log!(self.events.lock(), msg: joined_status_messages);
        }
    }
}

pub struct IfaceStatusNode {
    // Iface state values.
    connectivity_state_value: String,
    role_value: String,
    channel_value: String,
    net_type_value: String,

    node: Node,
    // Properties of "iface-*/status" node.
    _connectivity_state: StringProperty,
    _online_since: Option<TimeProperty>,
    _role: StringProperty,
    _channel: StringProperty,
    _net_type: StringProperty,
}
impl IfaceStatusNode {
    pub fn new(node: Node) -> Self {
        Self {
            connectivity_state_value: String::from(""),
            role_value: String::from(""),
            channel_value: String::from(""),
            net_type_value: String::from(""),
            node,
            _connectivity_state: StringProperty::default(),
            _online_since: None,
            _role: StringProperty::default(),
            _channel: StringProperty::default(),
            _net_type: StringProperty::default(),
        }
    }
}

pub async fn watch_device_changes<
    LP: 'static
        + DeviceWatcherProxyInterface<
            WatchDevicesResponseFut = fidl::client::QueryResponseFut<(Vec<String>, Vec<String>)>,
        >,
>(
    inspect_tree: Arc<LowpanServiceTree>,
    lookup: Arc<LP>,
) {
    let mut device_table: HashMap<String, Arc<Task<()>>> = HashMap::new();
    let lookup_clone = lookup.clone();
    let mut lookup_stream = HangingGetStream::new(lookup_clone, |lookup| lookup.watch_devices());
    loop {
        match lookup_stream.next().await {
            None => {
                fx_log_debug!("LoWPAN device lookup stream finished");
                break;
            }
            Some(Ok(devices)) => {
                for available_device in devices.0.iter() {
                    inspect_log!(
                        inspect_tree.events.lock(),
                        msg: format!("{}:available", available_device)
                    );
                    let future = monitor_device(
                        available_device.clone().to_string(),
                        inspect_tree.create_iface_child(available_device.clone()),
                    )
                    .map(|x| match x {
                        Ok(()) => {}
                        Err(err) => {
                            fx_log_warn!("Failed to monitor LoWPAN device: {:?}", err);
                        }
                    });

                    device_table
                        .insert(available_device.to_string(), Arc::new(Task::spawn(future)));
                }
                for unavailable_device in devices.1.iter() {
                    inspect_log!(
                        inspect_tree.events.lock(),
                        msg: format!("{}:unavailable", unavailable_device)
                    );
                    device_table.remove(unavailable_device);
                    inspect_tree.notify_iface_removed(unavailable_device.to_string());
                }
            }
            Some(Err(e)) => {
                fx_log_warn!("LoWPAN device lookup stream returned err: {} ", e);
                break;
            }
        }
    }
}

pub async fn start_inspect_process(inspect_tree: Arc<LowpanServiceTree>) -> Result<(), Error> {
    let lookup = connect_to_protocol::<DeviceWatcherMarker>()?;
    watch_device_changes(inspect_tree, Arc::new(lookup)).await;
    Ok::<(), Error>(())
}

async fn monitor_device(name: String, iface_tree: Arc<IfaceTreeHolder>) -> Result<(), Error> {
    let (device_client, device_server) = create_endpoints::<DeviceMarker>()?;
    let (device_extra_client, device_extra_server) = create_endpoints::<DeviceExtraMarker>()?;
    let (device_test_client, device_test_server) = create_endpoints::<DeviceTestMarker>()?;
    let (counters_client, counters_server) = create_endpoints::<CountersMarker>()?;
    let (telemetry_client, telemetry_server) = create_endpoints::<TelemetryProviderMarker>()?;

    connect_to_protocol::<DeviceConnectorMarker>()?.connect(&name, device_server)?;
    connect_to_protocol::<DeviceExtraConnectorMarker>()?.connect(&name, device_extra_server)?;
    connect_to_protocol::<DeviceTestConnectorMarker>()?.connect(&name, device_test_server)?;
    connect_to_protocol::<CountersConnectorMarker>()?.connect(&name, counters_server)?;
    connect_to_protocol::<TelemetryProviderConnectorMarker>()?.connect(&name, telemetry_server)?;

    let device = device_client.into_proxy().context("into_proxy() failed")?;
    let device_extra = device_extra_client.into_proxy().context("into_proxy() failed")?;
    let device_test = device_test_client.into_proxy().context("into_proxy() failed")?;
    let counters = counters_client.into_proxy().context("into_proxy() failed")?;
    let telemetry = telemetry_client.into_proxy().context("into_proxy() failed")?;

    {
        // "iface-*/counters" node
        let mut lazy_counters = iface_tree.counters.lock();
        *lazy_counters = iface_tree.node.create_lazy_child("counters", move || {
            let counters_clone = counters.clone();
            async move {
                let inspector = Inspector::new();
                match counters_clone.get().await {
                    Ok(all_counters) => {
                        let mac_counters =
                            [("tx", all_counters.mac_tx), ("rx", all_counters.mac_rx)];

                        for (mac_counter_for_str, mac_counter_option) in mac_counters {
                            if let Some(mac_counter) = mac_counter_option {
                                inspector.root().record_int(
                                    format!("{}_frames", mac_counter_for_str),
                                    mac_counter.total.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_unicast", mac_counter_for_str),
                                    mac_counter.unicast.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_broadcast", mac_counter_for_str),
                                    mac_counter.broadcast.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_ack_requested", mac_counter_for_str),
                                    mac_counter.ack_requested.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_acked", mac_counter_for_str),
                                    mac_counter.acked.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_no_ack_requested", mac_counter_for_str),
                                    mac_counter.no_ack_requested.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_data", mac_counter_for_str),
                                    mac_counter.data.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_data_poll", mac_counter_for_str),
                                    mac_counter.data_poll.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_beacon", mac_counter_for_str),
                                    mac_counter.beacon.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_beacon_request", mac_counter_for_str),
                                    mac_counter.beacon_request.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_other", mac_counter_for_str),
                                    mac_counter.other.unwrap_or(0).into(),
                                );
                                inspector.root().record_int(
                                    format!("{}_address_filtered", mac_counter_for_str),
                                    mac_counter.address_filtered.unwrap_or(0).into(),
                                );

                                if mac_counter_for_str == "tx" {
                                    inspector.root().record_int(
                                        format!("{}_retries", mac_counter_for_str),
                                        mac_counter.retries.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_direct_max_retry_expiry", mac_counter_for_str),
                                        mac_counter.direct_max_retry_expiry.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!(
                                            "{}_indirect_max_retry_expiry",
                                            mac_counter_for_str
                                        ),
                                        mac_counter.indirect_max_retry_expiry.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_err_cca", mac_counter_for_str),
                                        mac_counter.err_cca.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_err_abort", mac_counter_for_str),
                                        mac_counter.err_abort.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_err_busy_channel", mac_counter_for_str),
                                        mac_counter.err_busy_channel.unwrap_or(0).into(),
                                    );
                                } else {
                                    inspector.root().record_int(
                                        format!("{}_dest_addr_filtered", mac_counter_for_str),
                                        mac_counter.dest_addr_filtered.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_duplicated", mac_counter_for_str),
                                        mac_counter.duplicated.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_err_no_frame", mac_counter_for_str),
                                        mac_counter.err_no_frame.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_err_unknown_neighbor", mac_counter_for_str),
                                        mac_counter.err_unknown_neighbor.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_err_invalid_src_addr", mac_counter_for_str),
                                        mac_counter.err_invalid_src_addr.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_err_sec", mac_counter_for_str),
                                        mac_counter.err_sec.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        format!("{}_err_fcs", mac_counter_for_str),
                                        mac_counter.err_fcs.unwrap_or(0).into(),
                                    );
                                }

                                inspector.root().record_int(
                                    format!("{}_err_other", mac_counter_for_str),
                                    mac_counter.err_other.unwrap_or(0).into(),
                                );
                            }
                        }

                        // Log coex counters
                        let coex_counters =
                            [("tx", all_counters.coex_tx), ("rx", all_counters.coex_rx)];
                        inspector.root().record_child("coex_counters", |coex_counters_child| {
                            for (coex_counter_for_str, coex_counter_option) in coex_counters {
                                if let Some(coex_counter) = coex_counter_option {
                                    if let Some(val) = coex_counter.requests {
                                        coex_counters_child.record_uint(
                                            format!("{}_requests", coex_counter_for_str),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = coex_counter.grant_immediate {
                                        coex_counters_child.record_uint(
                                            format!("{}_grant_immediate", coex_counter_for_str),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = coex_counter.grant_wait {
                                        coex_counters_child.record_uint(
                                            format!("{}_grant_wait", coex_counter_for_str),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = coex_counter.grant_wait_activated {
                                        coex_counters_child.record_uint(
                                            format!(
                                                "{}_grant_wait_activated",
                                                coex_counter_for_str
                                            ),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = coex_counter.grant_wait_timeout {
                                        coex_counters_child.record_uint(
                                            format!("{}_grant_wait_timeout", coex_counter_for_str),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = coex_counter.grant_deactivated_during_request
                                    {
                                        coex_counters_child.record_uint(
                                            format!(
                                                "{}_grant_deactivated_during_request",
                                                coex_counter_for_str
                                            ),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = coex_counter.delayed_grant {
                                        coex_counters_child.record_uint(
                                            format!("{}_delayed_grant", coex_counter_for_str),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = coex_counter.avg_delay_request_to_grant_usec
                                    {
                                        coex_counters_child.record_uint(
                                            format!(
                                                "{}_avg_delay_request_to_grant_usec",
                                                coex_counter_for_str
                                            ),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = coex_counter.grant_none {
                                        coex_counters_child.record_uint(
                                            format!("{}_grant_none", coex_counter_for_str),
                                            val.into(),
                                        );
                                    }
                                }
                            }
                        });
                        if let Some(val) = all_counters.coex_saturated {
                            inspector.root().record_bool("coex_saturated", val.into());
                        }
                        // Log ip counters
                        let ip_counters = [("tx", all_counters.ip_tx), ("rx", all_counters.ip_rx)];
                        inspector.root().record_child("ip_counters", |ip_counters_child| {
                            for (ip_counter_for_str, ip_counter_option) in ip_counters {
                                if let Some(ip_counter) = ip_counter_option {
                                    if let Some(val) = ip_counter.success {
                                        ip_counters_child.record_uint(
                                            format!("{}_success", ip_counter_for_str),
                                            val.into(),
                                        );
                                    }
                                    if let Some(val) = ip_counter.failure {
                                        ip_counters_child.record_uint(
                                            format!("{}_failure", ip_counter_for_str),
                                            val.into(),
                                        );
                                    }
                                }
                            }
                        });
                    }
                    Err(e) => {
                        fx_log_warn!("Error in logging counters. Error: {}", e);
                    }
                };
                Ok(inspector)
            }
            .boxed()
        });

        // "iface-*/neighbors" node
        let mut lazy_neighbors = iface_tree.neighbors.lock();
        *lazy_neighbors = iface_tree.node.create_lazy_child("neighbors", move || {
            let device_test_clone = device_test.clone();
            async move {
                let inspector = Inspector::new();
                match device_test_clone.get_neighbor_table().await {
                    Ok(neighbor_table) => {
                        let mut index = -1;
                        for neighbor_info in neighbor_table {
                            let neighbor_info_c = neighbor_info.clone();
                            index += 1;
                            inspector.root().record_lazy_child(format!("{}", index), move || {
                                let neighbor_info_clone = neighbor_info_c.clone();
                                async move {
                                    let inspector = Inspector::new();
                                    inspector.root().record_int(
                                        "short_address",
                                        neighbor_info_clone.short_address.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_uint(
                                        "age",
                                        neighbor_info_clone
                                            .age
                                            .unwrap_or(0)
                                            .try_into()
                                            .unwrap_or(0),
                                    );
                                    inspector.root().record_bool(
                                        "is_child",
                                        neighbor_info_clone.is_child.unwrap_or(false).into(),
                                    );
                                    inspector.root().record_uint(
                                        "link_frame_count",
                                        neighbor_info_clone.link_frame_count.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_uint(
                                        "mgmt_frame_count",
                                        neighbor_info_clone.mgmt_frame_count.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        "rssi",
                                        neighbor_info_clone.last_rssi_in.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_int(
                                        "avg_rssi_in",
                                        neighbor_info_clone.avg_rssi_in.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_uint(
                                        "lqi_in",
                                        neighbor_info_clone.lqi_in.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_uint(
                                        "thread_mode",
                                        neighbor_info_clone.thread_mode.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_uint(
                                        "frame_error_rate",
                                        neighbor_info_clone.frame_error_rate.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_uint(
                                        "ipv6_error_rate",
                                        neighbor_info_clone.ipv6_error_rate.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_bool(
                                        "child_is_csl_synced",
                                        neighbor_info_clone
                                            .child_is_csl_synced
                                            .unwrap_or(false)
                                            .into(),
                                    );
                                    inspector.root().record_bool(
                                        "child_is_state_restoring",
                                        neighbor_info_clone
                                            .child_is_state_restoring
                                            .unwrap_or(false)
                                            .into(),
                                    );
                                    inspector.root().record_uint(
                                        "net_data_version",
                                        neighbor_info_clone.net_data_version.unwrap_or(0).into(),
                                    );
                                    inspector.root().record_uint(
                                        "queued_messages",
                                        neighbor_info_clone.queued_messages.unwrap_or(0).into(),
                                    );
                                    Ok(inspector)
                                }
                                .boxed()
                            });
                        }
                    }
                    Err(e) => {
                        fx_log_warn!("Error in logging neighbors. Error: {}", e);
                    }
                };
                Ok(inspector)
            }
            .boxed()
        });

        // "iface-*/telemetry" node
        let mut lazy_telemetry = iface_tree.telemetry.lock();
        *lazy_telemetry = iface_tree.node.create_lazy_child("telemetry", move || {
            let telemetry_clone = telemetry.clone();
            async move {
                let inspector = Inspector::new();
                match telemetry_clone.get_telemetry().await {
                    Ok(telemetry_data) => {
                        if let Some(x) = telemetry_data.thread_router_id {
                            inspector.root().record_uint("thread_router_id", x.into());
                        }
                        if let Some(x) = telemetry_data.thread_rloc {
                            inspector.root().record_uint("thread_rloc", x.into());
                        }
                        if let Some(x) = telemetry_data.partition_id {
                            inspector.root().record_uint("partition_id", x.into());
                        }
                        if let Some(x) = telemetry_data.tx_power {
                            inspector.root().record_int("tx_power", x.into());
                        }
                        if let Some(x) = telemetry_data.thread_link_mode {
                            inspector.root().record_uint("thread_link_mode", x.into());
                        }
                        if let Some(x) = telemetry_data.thread_network_data_version {
                            inspector.root().record_uint("thread_network_data_version", x.into());
                        }
                        if let Some(x) = telemetry_data.thread_stable_network_data_version {
                            inspector
                                .root()
                                .record_uint("thread_stable_network_data_version", x.into());
                        }
                        if let Some(x) = telemetry_data.thread_network_data {
                            inspector.root().record_bytes("thread_network_data", x);
                        }
                        if let Some(x) = telemetry_data.thread_stable_network_data {
                            inspector.root().record_bytes("thread_stable_network_data", x);
                        }
                        if let Some(x) = telemetry_data.stack_version {
                            inspector.root().record_string("stack_version", x);
                        }
                        if let Some(x) = telemetry_data.rcp_version {
                            inspector.root().record_string("rcp_version", x);
                        }
                        if let Some(x) = telemetry_data.rssi {
                            inspector.root().record_int("rssi", x.into());
                        }
                        if let Some(x) = telemetry_data.dnssd_counters {
                            inspector.root().record_child(
                                "dnssd_counters",
                                |dnssd_counters_child| {
                                    if let Some(y) = x.success_response {
                                        dnssd_counters_child
                                            .record_uint("success_response", y.into());
                                    }
                                    if let Some(y) = x.server_failure_response {
                                        dnssd_counters_child
                                            .record_uint("server_failure_response", y.into());
                                    }
                                    if let Some(y) = x.format_error_response {
                                        dnssd_counters_child
                                            .record_uint("format_error_response", y.into());
                                    }
                                    if let Some(y) = x.name_error_response {
                                        dnssd_counters_child
                                            .record_uint("name_error_response", y.into());
                                    }
                                    if let Some(y) = x.not_implemented_response {
                                        dnssd_counters_child
                                            .record_uint("not_implemented_response", y.into());
                                    }
                                    if let Some(y) = x.other_response {
                                        dnssd_counters_child
                                            .record_uint("other_response", y.into());
                                    }
                                    if let Some(y) = x.resolved_by_srp {
                                        dnssd_counters_child
                                            .record_uint("resolved_by_srp", y.into());
                                    }
                                },
                            );
                        }
                        if let Some(x) = telemetry_data.thread_border_routing_counters {
                            inspector.root().record_child(
                                "border_routing_counters",
                                |border_routing_counters_child| {
                                    border_routing_counters_child.record_child(
                                        "inbound_unicast",
                                        |counter_node| {
                                            if let Some(y) = x.inbound_unicast_packets {
                                                counter_node.record_uint("packets", y);
                                            }
                                            if let Some(y) = x.inbound_unicast_bytes {
                                                counter_node.record_uint("bytes", y);
                                            }
                                        },
                                    );
                                    border_routing_counters_child.record_child(
                                        "inbound_multicast",
                                        |counter_node| {
                                            if let Some(y) = x.inbound_multicast_packets {
                                                counter_node.record_uint("packets", y);
                                            }
                                            if let Some(y) = x.inbound_multicast_bytes {
                                                counter_node.record_uint("bytes", y);
                                            }
                                        },
                                    );
                                    border_routing_counters_child.record_child(
                                        "outbound_unicast",
                                        |counter_node| {
                                            if let Some(y) = x.outbound_unicast_packets {
                                                counter_node.record_uint("packets", y);
                                            }
                                            if let Some(y) = x.outbound_unicast_bytes {
                                                counter_node.record_uint("bytes", y);
                                            }
                                        },
                                    );
                                    border_routing_counters_child.record_child(
                                        "outbound_multicast",
                                        |counter_node| {
                                            if let Some(y) = x.outbound_multicast_packets {
                                                counter_node.record_uint("packets", y);
                                            }
                                            if let Some(y) = x.outbound_multicast_bytes {
                                                counter_node.record_uint("bytes", y);
                                            }
                                        },
                                    );
                                    if let Some(y) = x.ra_rx {
                                        border_routing_counters_child
                                            .record_uint("ra_rx", y.into());
                                    }
                                    if let Some(y) = x.ra_tx_success {
                                        border_routing_counters_child
                                            .record_uint("ra_tx_success", y.into());
                                    }
                                    if let Some(y) = x.ra_tx_failure {
                                        border_routing_counters_child
                                            .record_uint("ra_tx_failure", y.into());
                                    }
                                    if let Some(y) = x.rs_rx {
                                        border_routing_counters_child
                                            .record_uint("rs_rx", y.into());
                                    }
                                    if let Some(y) = x.rs_tx_success {
                                        border_routing_counters_child
                                            .record_uint("rs_tx_success", y.into());
                                    }
                                    if let Some(y) = x.rs_tx_failure {
                                        border_routing_counters_child
                                            .record_uint("rs_tx_failure", y.into());
                                    }
                                },
                            );
                        }
                        if let Some(x) = telemetry_data.srp_server_info {
                            inspector.root().record_child("srp_server", |srp_server_info_child| {
                                if let Some(y) = x.state {
                                    srp_server_info_child
                                        .record_string("state", format!("{:?}", y));
                                }
                                if let Some(y) = x.port {
                                    srp_server_info_child.record_uint("port", y.into());
                                }
                                if let Some(y) = x.address_mode {
                                    srp_server_info_child
                                        .record_string("address_mode", format!("{:?}", y));
                                }
                            });
                        }
                        if let Some(x) = telemetry_data.leader_data {
                            inspector.root().record_child("leader_data", |leader_data_child| {
                                if let Some(y) = x.partition_id {
                                    leader_data_child.record_uint("partition_id", y.into());
                                }
                                if let Some(y) = x.weight {
                                    leader_data_child.record_uint("weight", y.into());
                                    inspector.root().record_uint("thread_leader_weight", y.into());
                                }
                                if let Some(y) = x.network_data_version {
                                    leader_data_child.record_uint("network_data_version", y.into());
                                }
                                if let Some(y) = x.stable_network_data_version {
                                    leader_data_child
                                        .record_uint("stable_network_data_version", y.into());
                                }
                                if let Some(y) = x.router_id {
                                    leader_data_child.record_uint("router_id", y.into());
                                    inspector
                                        .root()
                                        .record_uint("thread_leader_router_id", y.into());
                                }
                            });
                        }
                    }
                    Err(e) => {
                        fx_log_warn!("Error in logging telemetry. Error: {}", e);
                    }
                };
                Ok(inspector)
            }
            .boxed()
        });
    }

    let mut device_stream_handler =
        HangingGetStream::new(device, |device| device.watch_device_state())
            .map_ok(|state| {
                iface_tree.update_status(state);
            })
            .try_collect::<()>();
    let mut device_extra_stream_handler =
        HangingGetStream::new(device_extra, |device| device.watch_identity())
            .map_ok(|identity| {
                iface_tree.update_identity(identity);
            })
            .try_collect::<()>();

    (futures::select! {
        ret = device_stream_handler => ret,
        ret = device_extra_stream_handler => ret,
    })?;

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use fuchsia_async as fasync;
    use fuchsia_async::Time;
    use fuchsia_async::TimeoutExt;
    use fuchsia_component_test::ScopedInstanceFactory;
    use fuchsia_inspect::assert_data_tree;
    use fuchsia_inspect::testing::AnyProperty;

    #[fasync::run(4, test)]
    async fn test_watch_device_changes() {
        let lookup = connect_to_protocol::<DeviceWatcherMarker>().unwrap();

        let inspector = fuchsia_inspect::Inspector::new();
        let inspector_clone = inspector.clone();
        let inspect_tree = Arc::new(LowpanServiceTree::new(inspector_clone));
        let look_up = Arc::new(lookup);

        assert_data_tree!(inspector, root: {
            "events":{},
        });

        // Start a lowpan dummy driver
        let dummy_driver = ScopedInstanceFactory::new("drivers")
            .new_instance("#meta/lowpan-dummy-driver.cm")
            .await
            .unwrap();
        dummy_driver.connect_to_binder().unwrap();

        let _res = watch_device_changes(inspect_tree.clone(), look_up.clone())
            .on_timeout(Time::after(fuchsia_zircon::Duration::from_seconds(5)), || {
                fx_log_info!("test_watch_device_changes: watch_device_changes timed out");
            })
            .await;

        assert_data_tree!(inspector, root: {
            "events":{
                "0":{
                    "@time": AnyProperty,
                    "msg": "lowpan0:available"
                }
            },
            "iface-lowpan0":{
                "events": {
                    "0": {
                        "@time": AnyProperty,
                        "msg": "connectivity_state:->Ready"
                    }
                },
                "status": {
                    "connectivity_state": "Ready",
                },
                "counters": contains {
                    "rx_ack_requested": AnyProperty,
                    "rx_acked": AnyProperty,
                    "rx_address_filtered": AnyProperty,
                    "rx_beacon": AnyProperty,
                    "rx_beacon_request": AnyProperty,
                    "rx_broadcast": AnyProperty,
                    "rx_data": AnyProperty,
                    "rx_data_poll": AnyProperty,
                    "rx_dest_addr_filtered": AnyProperty,
                    "rx_duplicated": AnyProperty,
                    "rx_err_fcs": AnyProperty,
                    "rx_err_invalid_src_addr": AnyProperty,
                    "rx_err_no_frame": AnyProperty,
                    "rx_err_other": AnyProperty,
                    "rx_err_sec": AnyProperty,
                    "rx_err_unknown_neighbor": AnyProperty,
                    "rx_frames": AnyProperty,
                    "rx_no_ack_requested": AnyProperty,
                    "rx_other": AnyProperty,
                    "rx_unicast": AnyProperty,
                    "tx_ack_requested": AnyProperty,
                    "tx_acked": AnyProperty,
                    "tx_address_filtered": AnyProperty,
                    "tx_beacon": AnyProperty,
                    "tx_beacon_request": AnyProperty,
                    "tx_broadcast": AnyProperty,
                    "tx_data": AnyProperty,
                    "tx_data_poll": AnyProperty,
                    "tx_direct_max_retry_expiry": AnyProperty,
                    "tx_err_abort": AnyProperty,
                    "tx_err_busy_channel": AnyProperty,
                    "tx_err_cca": AnyProperty,
                    "tx_err_other": AnyProperty,
                    "tx_frames": AnyProperty,
                    "tx_indirect_max_retry_expiry": AnyProperty,
                    "tx_no_ack_requested": AnyProperty,
                    "tx_other": AnyProperty,
                    "tx_retries": AnyProperty,
                    "tx_unicast": AnyProperty,
                },
                "neighbors": {
                    "0": {
                        "age": AnyProperty,
                        "avg_rssi_in": AnyProperty,
                        "child_is_csl_synced": AnyProperty,
                        "child_is_state_restoring": AnyProperty,
                        "frame_error_rate": AnyProperty,
                        "ipv6_error_rate": AnyProperty,
                        "is_child": AnyProperty,
                        "link_frame_count": AnyProperty,
                        "lqi_in": AnyProperty,
                        "mgmt_frame_count": AnyProperty,
                        "net_data_version": AnyProperty,
                        "queued_messages": AnyProperty,
                        "rssi": AnyProperty,
                        "short_address": AnyProperty,
                        "thread_mode": AnyProperty,
                    }
                },
                "telemetry": contains {},
            }
        });
        assert_data_tree!(inspector, root: contains {
            "iface-lowpan0": contains {
                "counters": contains {
                    "coex_counters": {
                        "tx_requests": AnyProperty,
                        "tx_grant_immediate": AnyProperty,
                        "tx_grant_wait": AnyProperty,
                        "tx_grant_wait_activated": AnyProperty,
                        "tx_grant_wait_timeout": AnyProperty,
                        "tx_grant_deactivated_during_request": AnyProperty,
                        "tx_delayed_grant": AnyProperty,
                        "tx_avg_delay_request_to_grant_usec": AnyProperty,
                        "rx_requests": AnyProperty,
                        "rx_grant_immediate": AnyProperty,
                        "rx_grant_wait": AnyProperty,
                        "rx_grant_wait_activated": AnyProperty,
                        "rx_grant_wait_timeout": AnyProperty,
                        "rx_grant_deactivated_during_request": AnyProperty,
                        "rx_delayed_grant": AnyProperty,
                        "rx_avg_delay_request_to_grant_usec": AnyProperty,
                        "rx_grant_none": AnyProperty,
                    },
                    "coex_saturated": false,
                }
            }
        });
    }
}
