// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[fuchsia::test]
async fn client_integration_test() {
    sc_client_integration_support::run_test_case(
        "cpp_driver_shim/*/driver_test_realm/*/pkg-drivers*:root",
    )
    .await;
}
