// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use fho::FfxTool;

#[fuchsia_async::run_singlethreaded]
async fn main() {
    ffx_net_test_realm::NetTestRealmTool::execute_tool().await
}
