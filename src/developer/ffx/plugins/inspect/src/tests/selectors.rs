// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use errors::ResultExt as _;
use ffx_inspect_common::run_command;
use ffx_inspect_test_utils::{
    inspect_bridge_data, make_inspect_with_length, make_inspects_for_lifecycle,
    setup_fake_diagnostics_bridge, setup_fake_rcs, FakeBridgeData,
};
use ffx_writer::{Format, MachineWriter, TestBuffers};
use fidl_fuchsia_developer_remotecontrol::BridgeStreamParameters;
use fidl_fuchsia_diagnostics::{
    ClientSelectorConfiguration, DataType, SelectorArgument, StreamMode,
};
use iquery::commands::SelectorsCommand;
use std::sync::Arc;

#[fuchsia::test]
async fn test_selectors_no_parameters() {
    let params = BridgeStreamParameters {
        stream_mode: Some(StreamMode::Snapshot),
        data_type: Some(DataType::Inspect),
        client_selector_configuration: Some(ClientSelectorConfiguration::SelectAll(true)),
        ..BridgeStreamParameters::EMPTY
    };
    let expected_responses = Arc::new(vec![]);
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = SelectorsCommand { manifest: None, selectors: vec![], accessor: None };
    assert!(run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![FakeBridgeData::new(
            params,
            expected_responses.clone(),
        )]),
        SelectorsCommand::from(cmd),
        &mut writer
    )
    .await
    .unwrap_err()
    .ffx_error()
    .is_some());
}

#[fuchsia::test]
async fn test_selectors_with_unknown_manifest() {
    let params = BridgeStreamParameters {
        stream_mode: Some(StreamMode::Snapshot),
        data_type: Some(DataType::Inspect),
        client_selector_configuration: Some(ClientSelectorConfiguration::SelectAll(true)),
        ..BridgeStreamParameters::EMPTY
    };
    let expected_responses = Arc::new(vec![]);
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = SelectorsCommand {
        manifest: Some(String::from("some-bad-moniker")),
        selectors: vec![],
        accessor: None,
    };
    assert!(run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![FakeBridgeData::new(
            params,
            expected_responses.clone(),
        )]),
        SelectorsCommand::from(cmd),
        &mut writer
    )
    .await
    .unwrap_err()
    .ffx_error()
    .is_some());
}

#[fuchsia::test]
async fn test_selectors_with_manifest_that_exists() {
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = SelectorsCommand {
        manifest: Some(String::from("moniker1")),
        selectors: vec![],
        accessor: None,
    };
    let lifecycle_data = inspect_bridge_data(
        ClientSelectorConfiguration::SelectAll(true),
        make_inspects_for_lifecycle(),
    );
    let inspects = vec![
        make_inspect_with_length(String::from("test/moniker1"), 1, 20),
        make_inspect_with_length(String::from("test/moniker1"), 3, 10),
        make_inspect_with_length(String::from("test/moniker1"), 6, 30),
    ];
    let inspect_data = inspect_bridge_data(
        ClientSelectorConfiguration::Selectors(vec![SelectorArgument::RawSelector(String::from(
            "test/moniker1:root",
        ))]),
        inspects,
    );
    run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![lifecycle_data, inspect_data]),
        SelectorsCommand::from(cmd),
        &mut writer,
    )
    .await
    .unwrap();

    let expected = serde_json::to_string(&vec![
        String::from("test/moniker1:name:hello_1"),
        String::from("test/moniker1:name:hello_3"),
        String::from("test/moniker1:name:hello_6"),
    ])
    .unwrap();
    let output = test_buffers.into_stdout_str();
    assert_eq!(output.trim_end(), expected);
}

#[fuchsia::test]
async fn test_selectors_with_selectors() {
    let test_buffers = TestBuffers::default();
    let mut writer = MachineWriter::new_test(Some(Format::Json), &test_buffers);
    let cmd = SelectorsCommand {
        manifest: None,
        selectors: vec![String::from("test/moniker1:name:hello_3")],
        accessor: None,
    };
    let lifecycle_data = inspect_bridge_data(
        ClientSelectorConfiguration::SelectAll(true),
        make_inspects_for_lifecycle(),
    );
    let inspects = vec![make_inspect_with_length(String::from("test/moniker1"), 3, 10)];
    let inspect_data = inspect_bridge_data(
        ClientSelectorConfiguration::Selectors(vec![SelectorArgument::RawSelector(String::from(
            "test/moniker1:name:hello_3",
        ))]),
        inspects,
    );
    run_command(
        setup_fake_rcs(),
        setup_fake_diagnostics_bridge(vec![lifecycle_data, inspect_data]),
        SelectorsCommand::from(cmd),
        &mut writer,
    )
    .await
    .unwrap();

    let expected =
        serde_json::to_string(&vec![String::from("test/moniker1:name:hello_3")]).unwrap();
    let output = test_buffers.into_stdout_str();
    assert_eq!(output.trim_end(), expected);
}
