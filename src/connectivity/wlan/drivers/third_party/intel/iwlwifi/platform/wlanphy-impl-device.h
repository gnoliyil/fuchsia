// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_WLANPHY_IMPL_DEVICE_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_WLANPHY_IMPL_DEVICE_H_

#include <fidl/fuchsia.wlan.phyimpl/cpp/driver/wire.h>
#include <lib/ddk/device.h>
#include <lib/fdf/cpp/arena.h>
#include <lib/fdf/cpp/channel.h>
#include <lib/fdf/cpp/channel_read.h>
#include <lib/fdf/cpp/dispatcher.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <lib/fidl/cpp/wire/vector_view.h>

#include <ddktl/device.h>

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/banjo/common.h"

struct iwl_trans;

namespace wlan::iwlwifi {

class WlanPhyImplDevice : public ::ddk::Device<WlanPhyImplDevice, ::ddk::Initializable,
                                               ::ddk::Unbindable, ddk::ServiceConnectable>,
                          public fdf::WireServer<fuchsia_wlan_phyimpl::WlanPhyImpl> {
 public:
  WlanPhyImplDevice(const WlanPhyImplDevice& device) = delete;
  WlanPhyImplDevice& operator=(const WlanPhyImplDevice& other) = delete;
  virtual ~WlanPhyImplDevice();

  // ::ddk::Device functions implemented by this class.
  void DdkRelease();

  // ::ddk::Device functions for initialization and unbinding, to be implemented by derived classes.
  virtual void DdkInit(::ddk::InitTxn txn) = 0;
  virtual void DdkUnbind(::ddk::UnbindTxn txn) = 0;
  zx_status_t DdkServiceConnect(const char* service_name, fdf::Channel channel);

  // State accessors.
  virtual iwl_trans* drvdata() = 0;
  virtual const iwl_trans* drvdata() const = 0;

  void GetSupportedMacRoles(fdf::Arena& arena,
                            GetSupportedMacRolesCompleter::Sync& completer) override;
  void CreateIface(CreateIfaceRequestView request, fdf::Arena& arena,
                   CreateIfaceCompleter::Sync& completer) override;
  void DestroyIface(DestroyIfaceRequestView request, fdf::Arena& arena,
                    DestroyIfaceCompleter::Sync& completer) override;
  void SetCountry(SetCountryRequestView request, fdf::Arena& arena,
                  SetCountryCompleter::Sync& completer) override;
  void ClearCountry(fdf::Arena& arena, ClearCountryCompleter::Sync& completer) override;
  void GetCountry(fdf::Arena& arena, GetCountryCompleter::Sync& completer) override;
  void SetPowerSaveMode(SetPowerSaveModeRequestView request, fdf::Arena& arena,
                        SetPowerSaveModeCompleter::Sync& completer) override;
  void GetPowerSaveMode(fdf::Arena& arena, GetPowerSaveModeCompleter::Sync& completer) override;

 protected:
  // Only derived classes are allowed to create this object.
  explicit WlanPhyImplDevice(zx_device_t* parent);
};

}  // namespace wlan::iwlwifi

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_PLATFORM_WLANPHY_IMPL_DEVICE_H_
