// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "harness.h"

#include <lib/component/incoming/cpp/service_client.h>

namespace server_suite {

void ClosedEventReporter::ReceivedOneWayNoPayload() { received_one_way_no_payload_ = true; }

void OpenEventReporter::ReceivedStrictOneWay() { received_strict_one_way_ = true; }

void OpenEventReporter::ReceivedFlexibleOneWay() { received_flexible_one_way_ = true; }

void LargeMessageEventReporter::ReceivedOneWay(
    fidl::Event<fidl_serversuite::LargeMessageTargetController::ReceivedOneWay>& request) {
  received_one_way_ = request.method();
}

}  // namespace server_suite
