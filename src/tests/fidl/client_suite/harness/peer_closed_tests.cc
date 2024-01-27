// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/fidl.clientsuite/cpp/common_types.h"
#include "fidl/fidl.clientsuite/cpp/natural_types.h"
#include "src/tests/fidl/channel_util/channel.h"
#include "src/tests/fidl/client_suite/harness/harness.h"

using namespace channel_util;

namespace client_suite {

CLIENT_TEST(OneWayCallDoNotReportPeerClosed) {
  server_end().get().reset();

  runner()->CallOneWayNoRequest({{.target = TakeClosedClient()}}).ThenExactlyOnce([&](auto result) {
    MarkCallbackRun();
    ASSERT_TRUE(result.is_ok()) << result.error_value();
    ASSERT_TRUE(result.value().success().has_value());
  });

  WAIT_UNTIL_CALLBACK_RUN();
}

}  // namespace client_suite
