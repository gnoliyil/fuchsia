// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::run_command;
use crate::tests::utils::{
    make_inspects_for_lifecycle, setup_fake_diagnostics_bridge, setup_fake_rcs,
    FakeArchiveIteratorResponse, FakeBridgeData,
};
use errors::ResultExt as _;
use ffx_writer::{Format, MachineWriter, TestBuffers};
use fidl_fuchsia_developer_remotecontrol::{ArchiveIteratorError, BridgeStreamParameters};
use fidl_fuchsia_diagnostics::{ClientSelectorConfiguration, DataType, StreamMode};
use iquery::commands::ListCommand;
use std::sync::Arc;

#[fuchsia::test]
async fn test_list_empty() {
    let params = BridgeStreamParameters {
        stream_mode: Some(StreamMode::Snapshot),
        data_type: Some(DataType::Inspect),
        client_selector_configuration: Some(ClientSelectorConfiguration::SelectAll(true)),
        ..BridgeStreamParameters::EMPTY
    };
    let expected_responses = Arc::new(vec![]);
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = ListCommand { manifest: None, with_url: false, accessor: None };
    run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![FakeBridgeData::new(
            params,
            expected_responses.clone(),
        )]),
        ListCommand::from(cmd),
        &mut writer,
    )
    .await
    .unwrap();

    let output = test_buffers.into_stdout_str();
    assert_eq!(output.trim_end(), String::from("[]"));
}

#[fuchsia::test]
async fn test_list_fidl_error() {
    let params = BridgeStreamParameters {
        stream_mode: Some(StreamMode::Snapshot),
        data_type: Some(DataType::Inspect),
        client_selector_configuration: Some(ClientSelectorConfiguration::SelectAll(true)),
        ..BridgeStreamParameters::EMPTY
    };
    let expected_responses = Arc::new(vec![FakeArchiveIteratorResponse::new_with_fidl_error()]);
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = ListCommand { manifest: None, with_url: false, accessor: None };

    assert!(run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![FakeBridgeData::new(
            params,
            expected_responses.clone()
        )]),
        ListCommand::from(cmd),
        &mut writer
    )
    .await
    .unwrap_err()
    .ffx_error()
    .is_some());
}

#[fuchsia::test]
async fn test_list_iterator_error() {
    let params = BridgeStreamParameters {
        stream_mode: Some(StreamMode::Snapshot),
        data_type: Some(DataType::Inspect),
        client_selector_configuration: Some(ClientSelectorConfiguration::SelectAll(true)),
        ..BridgeStreamParameters::EMPTY
    };
    let expected_responses = Arc::new(vec![FakeArchiveIteratorResponse::new_with_iterator_error(
        ArchiveIteratorError::GenericError,
    )]);
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = ListCommand { manifest: None, with_url: false, accessor: None };

    assert!(run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![FakeBridgeData::new(
            params,
            expected_responses.clone()
        )]),
        ListCommand::from(cmd),
        &mut writer
    )
    .await
    .unwrap_err()
    .ffx_error()
    .is_some());
}

#[fuchsia::test]
async fn test_list_with_data() {
    let params = BridgeStreamParameters {
        stream_mode: Some(StreamMode::Snapshot),
        data_type: Some(DataType::Inspect),
        client_selector_configuration: Some(ClientSelectorConfiguration::SelectAll(true)),
        ..BridgeStreamParameters::EMPTY
    };
    let lifecycles = make_inspects_for_lifecycle();
    let value = serde_json::to_string(&lifecycles).unwrap();
    let expected_responses = Arc::new(vec![FakeArchiveIteratorResponse::new_with_value(value)]);
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = ListCommand { manifest: None, with_url: false, accessor: None };
    run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![FakeBridgeData::new(
            params,
            expected_responses.clone(),
        )]),
        ListCommand::from(cmd),
        &mut writer,
    )
    .await
    .unwrap();

    let expected =
        serde_json::to_string(&vec![String::from("test/moniker1"), String::from("test/moniker3")])
            .unwrap();
    let output = test_buffers.into_stdout_str();
    assert_eq!(output.trim_end(), expected);
}

#[fuchsia::test]
async fn test_list_with_data_with_url() {
    let params = BridgeStreamParameters {
        stream_mode: Some(StreamMode::Snapshot),
        data_type: Some(DataType::Inspect),
        client_selector_configuration: Some(ClientSelectorConfiguration::SelectAll(true)),
        ..BridgeStreamParameters::EMPTY
    };
    let lifecycles = make_inspects_for_lifecycle();
    let value = serde_json::to_string(&lifecycles).unwrap();
    let expected_responses = Arc::new(vec![FakeArchiveIteratorResponse::new_with_value(value)]);
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = ListCommand { manifest: None, with_url: true, accessor: None };
    run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![FakeBridgeData::new(
            params,
            expected_responses.clone(),
        )]),
        ListCommand::from(cmd),
        &mut writer,
    )
    .await
    .unwrap();

    let expected = serde_json::to_string(&vec![
        iquery::commands::MonikerWithUrl {
            moniker: String::from("test/moniker1"),
            component_url: String::from("fake-url://test/moniker1"),
        },
        iquery::commands::MonikerWithUrl {
            moniker: String::from("test/moniker3"),
            component_url: String::from("fake-url://test/moniker3"),
        },
    ])
    .unwrap();
    let output = test_buffers.into_stdout_str();
    assert_eq!(output.trim_end(), expected);
}

#[fuchsia::test]
async fn test_list_with_data_with_manifest_and_archive() {
    let accessor = selectors::parse_selector::<selectors::FastError>(
        "./test/component:expose:fuchsia.diagnostics.ArchiveAccessor",
    )
    .unwrap();
    let params = BridgeStreamParameters {
        stream_mode: Some(StreamMode::Snapshot),
        data_type: Some(DataType::Inspect),
        client_selector_configuration: Some(ClientSelectorConfiguration::SelectAll(true)),
        accessor: Some(accessor.clone()),
        ..BridgeStreamParameters::EMPTY
    };
    let lifecycles = make_inspects_for_lifecycle();
    let value = serde_json::to_string(&lifecycles).unwrap();
    let expected_responses = Arc::new(vec![FakeArchiveIteratorResponse::new_with_value(value)]);
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = ListCommand {
        manifest: Some(String::from("moniker1")),
        with_url: true,
        accessor: Some("./test/component:expose:fuchsia.diagnostics.ArchiveAccessor".to_owned()),
    };
    run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![FakeBridgeData::new(
            params,
            expected_responses.clone(),
        )]),
        ListCommand::from(cmd),
        &mut writer,
    )
    .await
    .unwrap();

    let expected = serde_json::to_string(&vec![iquery::commands::MonikerWithUrl {
        moniker: String::from("test/moniker1"),
        component_url: String::from("fake-url://test/moniker1"),
    }])
    .unwrap();
    let output = test_buffers.into_stdout_str();
    assert_eq!(output.trim_end(), expected);
}
