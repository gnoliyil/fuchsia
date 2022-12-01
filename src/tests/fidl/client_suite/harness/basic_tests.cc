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

CLIENT_TEST(Setup) {}

CLIENT_TEST(TwoWayNoPayload) {
  runner()->CallTwoWayNoPayload({{.target = TakeClosedClient()}}).ThenExactlyOnce([&](auto result) {
    MarkCallbackRun();
    ASSERT_TRUE(result.is_ok()) << result.error_value();
    ASSERT_TRUE(result.value().success().has_value());
  });

  ASSERT_OK(server_end().wait_for_signal(ZX_CHANNEL_READABLE));

  Bytes bytes_out = {
      header(kTxidNotKnown, kOrdinalTwoWayNoPayload, fidl::MessageDynamicFlags::kStrictMethod),
  };
  zx_txid_t txid;
  ASSERT_OK(server_end().read_and_check_unknown_txid(&txid, bytes_out));
  ASSERT_NE(0u, txid);

  Bytes bytes_in = {
      header(txid, kOrdinalTwoWayNoPayload, fidl::MessageDynamicFlags::kStrictMethod),
  };
  ASSERT_OK(server_end().write(bytes_in));

  WAIT_UNTIL_CALLBACK_RUN();
}

CLIENT_TEST(TwoWayStructPayload) {
  static const fidl_clientsuite::NonEmptyPayload kPayload{{.some_field = 42}};

  runner()
      ->CallTwoWayStructPayload({{.target = TakeClosedClient()}})
      .ThenExactlyOnce(
          [&](fidl::Result<fidl_clientsuite::Runner::CallTwoWayStructPayload>& result) {
            MarkCallbackRun();
            ASSERT_TRUE(result.is_ok()) << result.error_value();
            ASSERT_TRUE(result.value().success().has_value());
            ASSERT_EQ(result.value().success().value(), kPayload);
          });

  ASSERT_OK(server_end().wait_for_signal(ZX_CHANNEL_READABLE));

  Bytes bytes_out = {
      header(kTxidNotKnown, kOrdinalTwoWayStructPayload, fidl::MessageDynamicFlags::kStrictMethod),
  };
  zx_txid_t txid;
  ASSERT_OK(server_end().read_and_check_unknown_txid(&txid, bytes_out));
  ASSERT_NE(0u, txid);

  Bytes bytes_in = {
      header(txid, kOrdinalTwoWayStructPayload, fidl::MessageDynamicFlags::kStrictMethod),
      i32(42),
      padding(4),
  };
  ASSERT_OK(server_end().write(bytes_in));

  WAIT_UNTIL_CALLBACK_RUN();
}

CLIENT_TEST(TwoWayTablePayload) {
  static const fidl_clientsuite::TablePayload kPayload{{.some_field = 42}};

  runner()
      ->CallTwoWayTablePayload({{.target = TakeClosedClient()}})
      .ThenExactlyOnce([&](fidl::Result<fidl_clientsuite::Runner::CallTwoWayTablePayload>& result) {
        MarkCallbackRun();
        ASSERT_TRUE(result.is_ok()) << result.error_value();
        ASSERT_TRUE(result.value().success().has_value());
        ASSERT_EQ(result.value().success().value(), kPayload);
      });

  ASSERT_OK(server_end().wait_for_signal(ZX_CHANNEL_READABLE));

  Bytes bytes_out = {
      header(kTxidNotKnown, kOrdinalTwoWayTablePayload, fidl::MessageDynamicFlags::kStrictMethod),
  };
  zx_txid_t txid;
  ASSERT_OK(server_end().read_and_check_unknown_txid(&txid, bytes_out));
  ASSERT_NE(0u, txid);

  Bytes bytes_in = {
      header(txid, kOrdinalTwoWayTablePayload, fidl::MessageDynamicFlags::kStrictMethod),
      encode(kPayload),
  };
  ASSERT_OK(server_end().write(bytes_in));

  WAIT_UNTIL_CALLBACK_RUN();
}

CLIENT_TEST(TwoWayUnionPayload) {
  static const fidl_clientsuite::UnionPayload kPayload =
      fidl_clientsuite::UnionPayload::WithSomeVariant(320494);

  runner()
      ->CallTwoWayUnionPayload({{.target = TakeClosedClient()}})
      .ThenExactlyOnce([&](fidl::Result<fidl_clientsuite::Runner::CallTwoWayUnionPayload>& result) {
        MarkCallbackRun();
        ASSERT_TRUE(result.is_ok()) << result.error_value();
        ASSERT_TRUE(result.value().success().has_value());
        ASSERT_EQ(result.value().success().value(), kPayload);
      });

  ASSERT_OK(server_end().wait_for_signal(ZX_CHANNEL_READABLE));

  Bytes bytes_out = {
      header(kTxidNotKnown, kOrdinalTwoWayUnionPayload, fidl::MessageDynamicFlags::kStrictMethod),
  };
  zx_txid_t txid;
  ASSERT_OK(server_end().read_and_check_unknown_txid(&txid, bytes_out));
  ASSERT_NE(0u, txid);

  Bytes bytes_in = {
      header(txid, kOrdinalTwoWayUnionPayload, fidl::MessageDynamicFlags::kStrictMethod),
      encode(kPayload),
  };
  ASSERT_OK(server_end().write(bytes_in));

  WAIT_UNTIL_CALLBACK_RUN();
}

CLIENT_TEST(TwoWayResultWithPayload) {
  static const fidl_clientsuite::NonEmptyPayload kPayload{{.some_field = 390023}};

  runner()
      ->CallTwoWayStructPayloadErr({{.target = TakeClosedClient()}})
      .ThenExactlyOnce(
          [&](fidl::Result<fidl_clientsuite::Runner::CallTwoWayStructPayloadErr>& result) {
            MarkCallbackRun();
            ASSERT_TRUE(result.is_ok()) << result.error_value();
            ASSERT_TRUE(result.value().success().has_value());
            ASSERT_EQ(result.value().success().value(), kPayload);
          });

  ASSERT_OK(server_end().wait_for_signal(ZX_CHANNEL_READABLE));

  Bytes bytes_out = {
      header(kTxidNotKnown, kOrdinalTwoWayStructPayloadErr,
             fidl::MessageDynamicFlags::kStrictMethod),
  };
  zx_txid_t txid;
  ASSERT_OK(server_end().read_and_check_unknown_txid(&txid, bytes_out));
  ASSERT_NE(0u, txid);

  Bytes bytes_in = {
      header(txid, kOrdinalTwoWayStructPayloadErr, fidl::MessageDynamicFlags::kStrictMethod),
      union_ordinal(kResultUnionSuccess),
      inline_envelope({i32(kPayload.some_field())}, false),
  };
  ASSERT_OK(server_end().write(bytes_in));

  WAIT_UNTIL_CALLBACK_RUN();
}

CLIENT_TEST(TwoWayResultWithError) {
  static const int32_t kError = 90240;

  runner()
      ->CallTwoWayStructPayloadErr({{.target = TakeClosedClient()}})
      .ThenExactlyOnce(
          [&](fidl::Result<fidl_clientsuite::Runner::CallTwoWayStructPayloadErr>& result) {
            MarkCallbackRun();
            ASSERT_TRUE(result.is_ok()) << result.error_value();
            ASSERT_TRUE(result.value().application_error().has_value());
            ASSERT_EQ(result.value().application_error().value(), kError);
          });

  ASSERT_OK(server_end().wait_for_signal(ZX_CHANNEL_READABLE));

  Bytes bytes_out = {
      header(kTxidNotKnown, kOrdinalTwoWayStructPayloadErr,
             fidl::MessageDynamicFlags::kStrictMethod),
  };
  zx_txid_t txid;
  ASSERT_OK(server_end().read_and_check_unknown_txid(&txid, bytes_out));
  ASSERT_NE(0u, txid);

  Bytes bytes_in = {
      header(txid, kOrdinalTwoWayStructPayloadErr, fidl::MessageDynamicFlags::kStrictMethod),
      union_ordinal(kResultUnionError),
      inline_envelope({i32(kError)}, false),
  };
  ASSERT_OK(server_end().write(bytes_in));

  WAIT_UNTIL_CALLBACK_RUN();
}

CLIENT_TEST(GracefulFailureDuringCallAfterPeerClose) {
  server_end().get().reset();

  runner()->CallTwoWayNoPayload({{.target = TakeClosedClient()}}).ThenExactlyOnce([&](auto result) {
    MarkCallbackRun();
    ASSERT_TRUE(result.is_ok()) << result.error_value();
    ASSERT_TRUE(result.value().fidl_error().has_value());
    ASSERT_EQ(fidl_clientsuite::FidlErrorKind::kChannelPeerClosed,
              result.value().fidl_error().value());
  });

  WAIT_UNTIL_CALLBACK_RUN();
}

}  // namespace client_suite
