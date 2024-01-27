// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_TESTS_FIDL_SERVER_SUITE_HARNESS_ORDINALS_H_
#define SRC_TESTS_FIDL_SERVER_SUITE_HARNESS_ORDINALS_H_

#include <cstdint>

namespace server_suite {

// To find all ordinals:
//
//     cat
//     out/default/fidling/gen/src/tests/fidl/server_suite/fidl/fidl.serversuite/cpp/fidl/fidl.serversuite/cpp/wire_messaging.cc
//     | grep -e 'constexpr.*k.*Target.*Ordinal'
//
// While using `jq` would be much nicer, large numbers are mishandled and the
// displayed ordinal ends up being incorrect.
//
// Ordinals are redefined here even though they may be accessible via C++
// binding definitions to ensure they are unchanged by changes in the bindings.

// Closed Target Ordinals
static const uint64_t kOrdinalOneWayNoPayload = 462698674125537694lu;
static const uint64_t kOrdinalTwoWayNoPayload = 6618634609655918175lu;
static const uint64_t kOrdinalTwoWayStructPayload = 3546419415198665872lu;
static const uint64_t kOrdinalTwoWayTablePayload = 7142567342575659946lu;
static const uint64_t kOrdinalTwoWayUnionPayload = 8633460217663942074lu;
static const uint64_t kOrdinalTwoWayResult = 806800322701855052lu;
static const uint64_t kOrdinalGetHandleRights = 1195943399487699944lu;
static const uint64_t kOrdinalGetSignalableEventRights = 475344252578913711lu;
static const uint64_t kOrdinalEchoAsTransferableSignalableEvent = 6829189580925709472lu;
static const uint64_t kOrdinalByteVectorSize = 1174084469162245669lu;
static const uint64_t kOrdinalHandleVectorSize = 5483915628125979959lu;
static const uint64_t kOrdinalCreateNByteVector = 2219580753158511713lu;
static const uint64_t kOrdinalCreateNHandleVector = 2752855654734922045lu;

// Open Target Ordinals
static const uint64_t kOrdinalStrictEvent = 538454334407181957lu;
static const uint64_t kOrdinalFlexibleEvent = 4889200613481231166lu;
static const uint64_t kOrdinalStrictOneWay = 2656433164255935131lu;
static const uint64_t kOrdinalFlexibleOneWay = 4763610705738353240lu;
static const uint64_t kOrdinalStrictTwoWay = 8071027055008411395lu;
static const uint64_t kOrdinalStrictTwoWayFields = 3163464055637704720lu;
static const uint64_t kOrdinalStrictTwoWayErr = 7997291255991962412lu;
static const uint64_t kOrdinalStrictTwoWayFieldsErr = 3502827294789008624lu;
static const uint64_t kOrdinalFlexibleTwoWay = 1871583035380534385lu;
static const uint64_t kOrdinalFlexibleTwoWayFields = 5173692443570239348lu;
static const uint64_t kOrdinalFlexibleTwoWayErr = 372287587009602464lu;
static const uint64_t kOrdinalFlexibleTwoWayFieldsErr = 1925250685993373878lu;

// Common Ordinals
static const uint64_t kOrdinalEpitaph = 0xfffffffffffffffflu;
// A made-up ordinal used when a method is needed that isn't known to the
// server.
static const uint64_t kOrdinalFakeUnknownMethod = 0x10ff10ff10ff10fflu;

}  // namespace server_suite

#endif  // SRC_TESTS_FIDL_SERVER_SUITE_HARNESS_ORDINALS_H_
