// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use diagnostics_reader::{assert_data_tree, AnyProperty, ArchiveReader, Inspect};
use fidl_fuchsia_component::BinderMarker;
use fidl_fuchsia_metrics_test::MetricEventLoggerQuerierMarker;
use fidl_fuchsia_mockrebootcontroller::MockRebootControllerMarker;
use fidl_fuchsia_samplertestcontroller::SamplerTestControllerMarker;
use fuchsia_async as fasync;
use fuchsia_zircon as zx;

mod mocks;
mod test_topology;
mod utils;

/// Runs the Sampler and a test component that can have its inspect properties
/// manipulated by the test via fidl, and uses cobalt mock and log querier to
/// verify that the sampler observers changes as expected, and logs them to
/// cobalt as expected.
#[fuchsia::test]
async fn event_count_sampler_test() {
    let instance = test_topology::create().await.expect("initialized topology");
    let test_app_controller =
        instance.root.connect_to_protocol_at_exposed_dir::<SamplerTestControllerMarker>().unwrap();
    let reboot_controller =
        instance.root.connect_to_protocol_at_exposed_dir::<MockRebootControllerMarker>().unwrap();
    let logger_querier = instance
        .root
        .connect_to_protocol_at_exposed_dir::<MetricEventLoggerQuerierMarker>()
        .unwrap();
    let _sampler_binder = instance
        .root
        .connect_to_named_protocol_at_exposed_dir::<BinderMarker>("fuchsia.component.SamplerBinder")
        .unwrap();

    test_app_controller.increment_int(1).await.unwrap();
    let events = utils::gather_sample_group(
        utils::LogQuerierConfig { project_id: 5, expected_batch_size: 3 },
        &logger_querier,
    )
    .await;

    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 101, value: 1 }
    ));
    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 102, value: 10 }
    ));
    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 103, value: 20 }
    ));

    // We want to guarantee a sample takes place before we increment the value again.
    // This is to verify that despite two samples taking place, the count type isn't uploaded with no diff
    // and the metric that is upload_once isn't sampled again.
    test_app_controller.wait_for_sample().await.unwrap().unwrap();

    let events = utils::gather_sample_group(
        utils::LogQuerierConfig { project_id: 5, expected_batch_size: 1 },
        &logger_querier,
    )
    .await;

    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 102, value: 10 }
    ));
    test_app_controller.increment_int(1).await.unwrap();

    test_app_controller.wait_for_sample().await.unwrap().unwrap();

    let events = utils::gather_sample_group(
        utils::LogQuerierConfig { project_id: 5, expected_batch_size: 2 },
        &logger_querier,
    )
    .await;

    // Even though we incremented metric-1 its value stays at 1 since it's being cached.
    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 101, value: 1 }
    ));
    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 102, value: 10 }
    ));

    // trigger_reboot calls the on_reboot callback that drives sampler shutdown. this
    // should await until sampler has finished its cleanup, which means we should have some events
    // present when we're done, and the sampler task should be finished.
    reboot_controller.trigger_reboot().await.unwrap().unwrap();

    let events = utils::gather_sample_group(
        utils::LogQuerierConfig { project_id: 5, expected_batch_size: 2 },
        &logger_querier,
    )
    .await;

    // The metric configured to run every 3000 seconds gets polled, and gets an undiffed
    // report of its values.
    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 104, value: 2 }
    ));
    // The integer metric which is always getting undiffed sampling is sampled one last time.
    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 102, value: 10 }
    ));
}

/// Runs the Sampler and a test component that can have its inspect properties
/// manipulated by the test via fidl, and uses mock services to determine that when
/// the reboot server goes down, sampler continues to run as expected.
#[fuchsia::test]
async fn reboot_server_crashed_test() {
    let instance = test_topology::create().await.expect("initialized topology");
    let test_app_controller =
        instance.root.connect_to_protocol_at_exposed_dir::<SamplerTestControllerMarker>().unwrap();
    let reboot_controller =
        instance.root.connect_to_protocol_at_exposed_dir::<MockRebootControllerMarker>().unwrap();
    let logger_querier = instance
        .root
        .connect_to_protocol_at_exposed_dir::<MetricEventLoggerQuerierMarker>()
        .unwrap();
    let _sampler_binder = instance
        .root
        .connect_to_named_protocol_at_exposed_dir::<BinderMarker>("fuchsia.component.SamplerBinder")
        .unwrap();

    // Crash the reboot server to verify that sampler continues to sample.
    reboot_controller.crash_reboot_channel().await.unwrap().unwrap();

    test_app_controller.increment_int(1).await.unwrap();

    let events = utils::gather_sample_group(
        utils::LogQuerierConfig { project_id: 5, expected_batch_size: 3 },
        &logger_querier,
    )
    .await;

    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 101, value: 1 }
    ));
    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 102, value: 10 }
    ));
    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 103, value: 20 }
    ));

    // We want to guarantee a sample takes place before we increment the value again.
    // This is to verify that despite two samples taking place, the count type isn't uploaded with
    // no diff and the metric that is upload_once isn't sampled again.
    test_app_controller.wait_for_sample().await.unwrap().unwrap();

    let events = utils::gather_sample_group(
        utils::LogQuerierConfig { project_id: 5, expected_batch_size: 1 },
        &logger_querier,
    )
    .await;

    assert!(utils::verify_event_present_once(
        &events,
        utils::ExpectedEvent { metric_id: 102, value: 10 }
    ));
}

/// Runs the Sampler and a test component that can have its inspect properties
/// manipulated by the test via fidl, and uses mock services to determine that when
/// the reboot server goes down, sampler continues to run as expected.
#[fuchsia::test]
async fn sampler_inspect_test() {
    let instance = test_topology::create().await.expect("initialized topology");
    let _sampler_binder = instance
        .root
        .connect_to_named_protocol_at_exposed_dir::<BinderMarker>("fuchsia.component.SamplerBinder")
        .unwrap();

    let hierarchy = loop {
        // Observe verification shows up in inspect.
        let mut data = ArchiveReader::new()
            .add_selector(format!(
                "realm_builder\\:{}/wrapper/sampler:root",
                instance.root.child_name()
            ))
            .snapshot::<Inspect>()
            .await
            .expect("got inspect data");

        let hierarchy = data.pop().expect("one result").payload.expect("payload is not none");
        if hierarchy.get_child("sampler_executor_stats").is_none()
            || hierarchy.get_child("metrics_sent").is_none()
        {
            fasync::Timer::new(fasync::Time::after(zx::Duration::from_millis(100))).await;
            continue;
        }
        break hierarchy;
    };
    assert_data_tree!(
        hierarchy,
        root: {
            config: {
                minimum_sample_rate_sec: 1 as u64,
                configs_path: "/pkg/data/config",
            },
            sampler_executor_stats: {
                healthily_exited_samplers: 0 as u64,
                errorfully_exited_samplers: 0 as u64,
                reboot_exited_samplers: 0 as u64,
                total_project_samplers_configured: 4 as u64,
                project_5: {
                    project_sampler_count: 2 as u64,
                    metrics_configured: 4 as u64,
                    cobalt_logs_sent: AnyProperty,
                },
                project_13: {
                    project_sampler_count: 2 as u64,
                    metrics_configured: 6 as u64,
                    cobalt_logs_sent: AnyProperty,
                },
            },
            metrics_sent: {
                "fire_1.json5":
                {
                    "0": {
                        selector: "single_counter:root/samples:integer_1",
                        upload_count: 0 as u64
                    },
                    "1": {
                        selector: "single_counter:root/samples:integer_1",
                        upload_count: 0 as u64
                    },
                    "2": {
                        selector: "single_counter:root/samples:integer_2",
                        upload_count: 0 as u64
                    },
                    "3": {
                        selector: "single_counter:root/samples:integer_2",
                        upload_count: 0 as u64
                    }
                },
                "fire_2.json5": {
                    "0": {
                        selector: "single_counter:root/samples:integer_1",
                        upload_count: 0 as u64
                    },
                    "1": {
                        selector: "single_counter:root/samples:integer_2",
                        upload_count: 0 as u64
                    }
                },
                "reboot_required_config.json": {
                    "0": {
                        selector: "single_counter:root/samples:counter",
                        upload_count: 0 as u64
                    }
                },
                "test_config.json": {
                    "0": {
                        selector: "single_counter:root/samples:counter",
                        upload_count: 0 as u64
                        },
                    "1": {
                        selector: "single_counter:root/samples:integer_1",
                        upload_count: 0 as u64
                        },
                    "2": {
                        selector: "single_counter:root/samples:integer_2",
                        upload_count: 0 as u64
                    }
                },
            },
            "fuchsia.inspect.Health": {
                start_timestamp_nanos: AnyProperty,
                status: AnyProperty
            }
        }
    );
}
