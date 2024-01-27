// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DRIVERS_MSD_VSI_VIP_INCLUDE_MAGMA_VSI_VIP_DEVICES_H_
#define SRC_GRAPHICS_DRIVERS_MSD_VSI_VIP_INCLUDE_MAGMA_VSI_VIP_DEVICES_H_

#include <cstdint>

#define MAGMA_VSI_VIP_AS370_CUSTOMER_ID 0x80
#define MAGMA_VSI_VIP_SHERLOCK_CUSTOMER_ID 0x88
#define MAGMA_VSI_VIP_NELSON_CUSTOMER_ID 0x99
#define MAGMA_VSI_VIP_A5_CUSTOMER_ID 0x1000000e

// Supported devices
static constexpr const uint32_t kMsdVsiVipDevice8000 = 0x8000;
static constexpr const uint32_t kMsdVsiVipDevice9000 = 0x9000;

#endif  // SRC_GRAPHICS_DRIVERS_MSD_VSI_VIP_INCLUDE_MAGMA_VSI_VIP_DEVICES_H_
