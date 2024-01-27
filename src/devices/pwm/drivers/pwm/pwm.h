// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_PWM_DRIVERS_PWM_PWM_H_
#define SRC_DEVICES_PWM_DRIVERS_PWM_PWM_H_

#include <fidl/fuchsia.hardware.pwm/cpp/wire.h>
#include <fuchsia/hardware/pwm/cpp/banjo.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/ddk/platform-defs.h>

#include <mutex>

#include <ddk/metadata/pwm.h>
#include <ddktl/device.h>

namespace pwm {

class PwmDevice;
using PwmDeviceType =
    ddk::Device<PwmDevice, ddk::Initializable, ddk::Messageable<fuchsia_hardware_pwm::Pwm>::Mixin>;

class PwmDevice : public PwmDeviceType, public ddk::PwmProtocol<PwmDevice, ddk::base_protocol> {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  void DdkInit(ddk::InitTxn txn);
  void DdkRelease() { delete this; }

  // Ddk Mixins.
  zx_status_t PwmGetConfig(pwm_config_t* out_config);
  zx_status_t PwmSetConfig(const pwm_config_t* config);
  zx_status_t PwmEnable();
  zx_status_t PwmDisable();

  void GetConfig(GetConfigCompleter::Sync& completer) override;
  void SetConfig(SetConfigRequestView request, SetConfigCompleter::Sync& completer) override;
  void Enable(EnableCompleter::Sync& completer) override;
  void Disable(DisableCompleter::Sync& completer) override;

 private:
  explicit PwmDevice(zx_device_t* parent, pwm_impl_protocol_t* pwm, pwm_id_t id)
      : PwmDeviceType(parent), pwm_(pwm), id_(id) {}

  ddk::PwmImplProtocolClient pwm_ __TA_GUARDED(lock_);
  pwm_id_t id_;

  // Protect against concurrent access from both the FIDL and Banjo interfaces.
  std::mutex lock_;

  std::optional<component::OutgoingDirectory> outgoing_;
  fidl::ServerEnd<fuchsia_io::Directory> outgoing_server_end_;
  fidl::ServerBindingGroup<fuchsia_hardware_pwm::Pwm> bindings_;
};

}  // namespace pwm

#endif  // SRC_DEVICES_PWM_DRIVERS_PWM_PWM_H_
