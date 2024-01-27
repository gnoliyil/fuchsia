// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fuchsia_inspect::{Inspector, LazyNode, Node as InspectNode},
    futures::FutureExt,
    parking_lot::Mutex,
    std::sync::Arc,
    windowed_stats::{
        aggregations::create_saturating_add_fn, CombinedWindowedStats, FifteenMinutelyWindows,
        HourlyWindows, MinutelyWindows,
    },
};

pub(crate) struct TimeSeriesStats {
    pub(crate) total_duration_sec: CombinedWindowedStats<i32>,
    pub(crate) connected_duration_sec: CombinedWindowedStats<i32>,
    pub(crate) connect_attempt_count: CombinedWindowedStats<u32>,
    pub(crate) connect_successful_count: CombinedWindowedStats<u32>,
    pub(crate) disconnect_count: CombinedWindowedStats<u32>,

    // Packet counters stats, or stats calculated from packet counters
    pub(crate) rx_unicast_total_count: CombinedWindowedStats<u32>,
    pub(crate) rx_unicast_drop_count: CombinedWindowedStats<u32>,
    pub(crate) tx_total_count: CombinedWindowedStats<u32>,
    pub(crate) tx_drop_count: CombinedWindowedStats<u32>,
    pub(crate) no_rx_duration_sec: CombinedWindowedStats<i32>,
}

impl TimeSeriesStats {
    pub(crate) fn new() -> Self {
        Self {
            // `total_duration_sec` is served as the denominator for other duration stats.
            // This is really only needed for the most recent window, so we just instantiate
            // window sizes 1 to save space.
            // For preceding windows with fully elapsed time, it's already implied
            // that for example, the denominator for the minutely window would be 60 seconds.
            total_duration_sec: CombinedWindowedStats::with_n_windows(
                MinutelyWindows(1),
                FifteenMinutelyWindows(1),
                HourlyWindows(1),
                create_saturating_add_fn,
            ),
            connected_duration_sec: CombinedWindowedStats::new(create_saturating_add_fn),
            connect_attempt_count: CombinedWindowedStats::new(create_saturating_add_fn),
            connect_successful_count: CombinedWindowedStats::new(create_saturating_add_fn),
            disconnect_count: CombinedWindowedStats::new(create_saturating_add_fn),
            rx_unicast_total_count: CombinedWindowedStats::new(create_saturating_add_fn),
            rx_unicast_drop_count: CombinedWindowedStats::new(create_saturating_add_fn),
            tx_total_count: CombinedWindowedStats::new(create_saturating_add_fn),
            tx_drop_count: CombinedWindowedStats::new(create_saturating_add_fn),
            no_rx_duration_sec: CombinedWindowedStats::new(create_saturating_add_fn),
        }
    }

    pub(crate) fn slide_minute(&mut self) {
        self.total_duration_sec.slide_minute();
        self.connected_duration_sec.slide_minute();
        self.connect_attempt_count.slide_minute();
        self.connect_successful_count.slide_minute();
        self.disconnect_count.slide_minute();
        self.rx_unicast_total_count.slide_minute();
        self.rx_unicast_drop_count.slide_minute();
        self.tx_total_count.slide_minute();
        self.tx_drop_count.slide_minute();
        self.no_rx_duration_sec.slide_minute();
    }

    pub(crate) fn log_inspect(&mut self, node: &InspectNode) {
        self.total_duration_sec.log_inspect_int_array(node, "total_duration_sec");
        self.connected_duration_sec.log_inspect_int_array(node, "connected_duration_sec");
        self.connect_attempt_count.log_inspect_uint_array(node, "connect_attempt_count");
        self.connect_successful_count.log_inspect_uint_array(node, "connect_successful_count");
        self.disconnect_count.log_inspect_uint_array(node, "disconnect_count");
        self.rx_unicast_total_count.log_inspect_uint_array(node, "rx_unicast_total_count");
        self.rx_unicast_drop_count.log_inspect_uint_array(node, "rx_unicast_drop_count");
        self.tx_total_count.log_inspect_uint_array(node, "tx_total_count");
        self.tx_drop_count.log_inspect_uint_array(node, "tx_drop_count");
        self.no_rx_duration_sec.log_inspect_int_array(node, "no_rx_duration_sec");
    }
}

pub(crate) fn inspect_create_stats(
    inspect_node: &InspectNode,
    child_name: &str,
    stats: Arc<Mutex<TimeSeriesStats>>,
) -> LazyNode {
    inspect_node.create_lazy_child(child_name, move || {
        let stats = Arc::clone(&stats);
        async move {
            let inspector = Inspector::default();
            {
                stats.lock().log_inspect(inspector.root());
            }
            Ok(inspector)
        }
        .boxed()
    })
}
