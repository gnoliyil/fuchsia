// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/fidl.clientsuite/cpp/common_types.h"
#include "fidl/fidl.clientsuite/cpp/natural_types.h"
#include "src/tests/fidl/channel_util/bytes.h"
#include "src/tests/fidl/channel_util/channel.h"
#include "src/tests/fidl/client_suite/harness/harness.h"
#include "src/tests/fidl/client_suite/harness/ordinals.h"

using namespace channel_util;

namespace client_suite {

CLIENT_TEST(TwoWayWrongResponseOrdinal) {
  runner()->CallTwoWayNoPayload({{.target = TakeClosedClient()}}).ThenExactlyOnce([&](auto result) {
    MarkCallbackRun();
    ASSERT_TRUE(result.is_ok()) << result.error_value();
    ASSERT_TRUE(result.value().fidl_error().has_value());
    EXPECT_EQ(fidl_clientsuite::FidlErrorKind::kDecodingError, result.value().fidl_error().value());
  });

  ASSERT_OK(server_end().wait_for_signal(ZX_CHANNEL_READABLE));

  Bytes bytes_out = {
      header(kTxidNotKnown, kOrdinalTwoWayNoPayload, fidl::MessageDynamicFlags::kStrictMethod),
  };
  zx_txid_t txid;
  ASSERT_OK(server_end().read_and_check_unknown_txid(&txid, bytes_out));
  ASSERT_NE(0u, txid);

  Bytes bytes_in = {
      // Ordinal in the reply intentionally set wrong, message otherwise valid.
      header(txid, kOrdinalTwoWayStructPayload, fidl::MessageDynamicFlags::kStrictMethod),
  };
  ASSERT_OK(server_end().write(bytes_in));

  WAIT_UNTIL_CALLBACK_RUN();
}

}  // namespace client_suite
