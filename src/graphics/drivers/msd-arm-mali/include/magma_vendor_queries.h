// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_INCLUDE_MAGMA_VENDOR_QUERIES_H_
#define SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_INCLUDE_MAGMA_VENDOR_QUERIES_H_

#include <lib/magma/magma_common_defs.h>

#include "magma_arm_mali_vendor_id.h"

// All queries return a simple result except where indicated.
enum MsdArmVendorQuery {
  kMsdArmVendorQueryL2Present = MAGMA_QUERY_VENDOR_PARAM_0,
  kMsdArmVendorQueryMaxThreads = MAGMA_QUERY_VENDOR_PARAM_0 + 1,
  kMsdArmVendorQueryThreadMaxBarrierSize = MAGMA_QUERY_VENDOR_PARAM_0 + 2,
  kMsdArmVendorQueryThreadMaxWorkgroupSize = MAGMA_QUERY_VENDOR_PARAM_0 + 3,
  kMsdArmVendorQueryShaderPresent = MAGMA_QUERY_VENDOR_PARAM_0 + 4,
  kMsdArmVendorQueryTilerFeatures = MAGMA_QUERY_VENDOR_PARAM_0 + 5,
  kMsdArmVendorQueryThreadFeatures = MAGMA_QUERY_VENDOR_PARAM_0 + 6,
  kMsdArmVendorQueryL2Features = MAGMA_QUERY_VENDOR_PARAM_0 + 7,
  kMsdArmVendorQueryMemoryFeatures = MAGMA_QUERY_VENDOR_PARAM_0 + 8,
  kMsdArmVendorQueryMmuFeatures = MAGMA_QUERY_VENDOR_PARAM_0 + 9,
  kMsdArmVendorQueryCoherencyEnabled = MAGMA_QUERY_VENDOR_PARAM_0 + 10,
  kMsdArmVendorQueryThreadTlsAlloc = MAGMA_QUERY_VENDOR_PARAM_0 + 11,
  kMsdArmVendorQuerySupportsProtectedMode = MAGMA_QUERY_VENDOR_PARAM_0 + 12,
  // Returns a buffer result.
  kMsdArmVendorQueryDeviceTimestamp = MAGMA_QUERY_VENDOR_PARAM_0 + 13,
  // Returns a buffer result.
  kMsdArmVendorQueryDeviceProperties = MAGMA_QUERY_VENDOR_PARAM_0 + 14,
};

#endif  // SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_INCLUDE_MAGMA_VENDOR_QUERIES_H_
