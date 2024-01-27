// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use diagnostics_reader::ArchiveReader;
use diagnostics_reader::Logs;
use fuchsia_async::Task;
use futures_util::StreamExt;

#[fuchsia::main]
async fn main() {
    let reader = ArchiveReader::new();
    let (mut logs, mut errors) = reader.snapshot_then_subscribe::<Logs>().unwrap().split_streams();
    let _errors = Task::spawn(async move {
        if let Some(e) = errors.next().await {
            panic!("error in subscription: {}", e);
        }
    });
    while let Some(log_entry) = logs.next().await {
        if log_entry.msg().unwrap().contains("This is a test error")
            && log_entry.tags().unwrap().contains(&"structured_log".to_string())
        {
            break;
        }
    }
}
