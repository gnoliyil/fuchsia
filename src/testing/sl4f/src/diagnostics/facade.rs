// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::diagnostics::types::SnapshotInspectArgs;
use anyhow::Error;
use diagnostics_reader::{ArchiveReader, Inspect};
use fidl_fuchsia_diagnostics::ArchiveAccessorMarker;
use fuchsia_component::client;
use serde_json::Value;

// Give components 5 minutes to respond with their Inspect data in case the system is under heavy
// load. This can happen especially when running unoptimized on emulators.
const BATCH_RETRIEVAL_TIMEOUT_SECONDS: i64 = 300;

/// Facade providing access to diagnostics interface.
#[derive(Debug)]
pub struct DiagnosticsFacade {}

impl DiagnosticsFacade {
    pub fn new() -> DiagnosticsFacade {
        DiagnosticsFacade {}
    }

    pub async fn snapshot_inspect(&self, args: SnapshotInspectArgs) -> Result<Value, Error> {
        let service_path = format!("/svc/{}", args.service_name);
        let proxy =
            client::connect_to_protocol_at_path::<ArchiveAccessorMarker>(&service_path).unwrap();
        let value = ArchiveReader::new()
            .retry_if_empty(false)
            .with_archive(proxy)
            .add_selectors(args.selectors.into_iter())
            .with_batch_retrieval_timeout_seconds(BATCH_RETRIEVAL_TIMEOUT_SECONDS)
            .snapshot_raw::<Inspect, serde_json::Value>()
            .await?;
        Ok(value)
    }
}
