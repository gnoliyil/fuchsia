// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_UFS_UPIU_ATTRIBUTES_H_
#define SRC_DEVICES_BLOCK_DRIVERS_UFS_UPIU_ATTRIBUTES_H_

#include <endian.h>

#include "query_request.h"

namespace ufs {

// UFS Specification Version 3.1, section 14.3 "Attributes".
enum class Attributes {
  bBootLunEn = 0x00,
  bCurrentPowerMode = 0x02,
  bActiveICCLevel = 0x03,
  bOutOfOrderDataEn = 0x04,
  bBackgroundOpStatus = 0x05,
  bPurgeStatus = 0x06,
  bMaxDataInSize = 0x07,
  bMaxDataOutSize = 0x08,
  dDynCapNeeded = 0x09,
  bRefClkFreq = 0x0a,
  bConfigDescrLock = 0x0b,
  bMaxNumOfRTT = 0x0c,
  wExceptionEventControl = 0x0d,
  wExceptionEventStatus = 0x0e,
  dSecondsPassed = 0x0f,
  wContextConf = 0x10,
  bDeviceFFUStatus = 0x14,
  bPSAState = 0x15,
  dPSADataSize = 0x16,
  bRefClkGatingWaitTime = 0x17,
  bDeviceCaseRoughTemperaure = 0x18,
  bDeviceTooHighTempBoundary = 0x19,
  bDeviceTooLowTempBoundary = 0x1a,
  bThrottlingStatus = 0x1b,
  bWBBufferFlushStatus = 0x1c,
  bAvailableWBBufferSize = 0x1d,
  bWBBufferLifeTimeEst = 0x1e,
  dCurrentWBBufferSize = 0x1f,
  bRefreshStatus = 0x2c,
  bRefreshFreq = 0x2d,
  bRefreshUnit = 0x2e,
  bRefreshMethod = 0x2f,
};

class ReadAttributeUpiu : public QueryReadRequestUpiu {
 public:
  explicit ReadAttributeUpiu(Attributes type)
      : QueryReadRequestUpiu(QueryOpcode::kReadAttribute, static_cast<uint8_t>(type)) {}
};

class WriteAttributeUpiu : public QueryWriteRequestUpiu {
 public:
  explicit WriteAttributeUpiu(Attributes type, uint32_t value)
      : QueryWriteRequestUpiu(QueryOpcode::kWriteAttribute, static_cast<uint8_t>(type)) {
    if (value) {
      data_.value = htobe32(value);
    }
  }
};

class AttributeResponseUpiu : public QueryResponseUpiu {
 public:
  uint32_t GetAttribute() { return betoh32(data_.value); }
};

}  // namespace ufs

#endif  // SRC_DEVICES_BLOCK_DRIVERS_UFS_UPIU_ATTRIBUTES_H_
