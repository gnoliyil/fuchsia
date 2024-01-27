// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_STA_IFC_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_STA_IFC_H_

#include <fuchsia/wlan/common/c/banjo.h>
#include <net/ethernet.h>

#include <wlan/common/macaddr.h>

#include "sim-frame.h"

namespace wlan::simulation {

class SimFrame;
class SimManagementFrame;
struct WlanRxInfo;

class StationIfc {
 public:
  // Handler for different frames.
  virtual void Rx(std::shared_ptr<const SimFrame> frame,
                  std::shared_ptr<const WlanRxInfo> info) = 0;

  // Change station's receiver sensitivity
  void setRxSensitivity(double rxSensitivity) { rx_sensitivity_ = rxSensitivity; }

  // Get station's receiver sensitivity
  double getRxSensitivity() { return rx_sensitivity_; }

  // Default rx sensitivity of dBm
  static constexpr double kDefaultRxSensitivity = -101;

  // rx sensitivity of this stations receiver
  double rx_sensitivity_ = kDefaultRxSensitivity;
};

}  // namespace wlan::simulation

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_TESTING_LIB_SIM_ENV_SIM_STA_IFC_H_
