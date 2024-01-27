// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_WLANIF_FULLMAC_MLME_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_WLANIF_FULLMAC_MLME_H_

#include <zircon/types.h>

#include <memory>
#include <optional>

#include "src/connectivity/wlan/lib/mlme/fullmac/c-binding/bindings.h"

namespace wlanif {

using RustFullmacMlme =
    std::unique_ptr<wlan_fullmac_mlme_handle_t, void (*)(wlan_fullmac_mlme_handle_t*)>;

class Device;

class FullmacMlme {
 public:
  explicit FullmacMlme(Device* device);
  ~FullmacMlme() = default;
  void Init();
  void StopMainLoop();
  /**
   * Provide a read-only Inspect VMO. Intended to be surfaced through the driver framework
   * so that MLME's Inspect data is available for diagnostics.
   */
  std::optional<zx_handle_t> DuplicateInspectVmo();

 private:
  Device* device_;
  RustFullmacMlme rust_mlme_;
};

}  // namespace wlanif

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_WLANIF_FULLMAC_MLME_H_
