// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_INPUT_DRIVERS_HID_BUTTONS_HID_BUTTONS_H_
#define SRC_UI_INPUT_DRIVERS_HID_BUTTONS_HID_BUTTONS_H_

#include <fidl/fuchsia.buttons/cpp/wire.h>
#include <fuchsia/hardware/gpio/c/banjo.h>
#include <fuchsia/hardware/hidbus/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/wire/server.h>
#include <lib/sync/cpp/completion.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/interrupt.h>
#include <lib/zx/port.h>
#include <lib/zx/timer.h>

#include <list>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <ddk/metadata/buttons.h>
#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/array.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fbl/ref_counted.h>
#include <hid/buttons.h>

#include "lib/zx/channel.h"
#include "zircon/types.h"

namespace buttons {

// zx_port_packet::key.
constexpr uint64_t kPortKeyShutDown = 0x01;
// Start of up to kNumberOfRequiredGpios port types used for interrupts.
constexpr uint64_t kPortKeyInterruptStart = 0x10;
// Timer start
constexpr uint64_t kPortKeyTimerStart = 0x100;
// Poll timer
constexpr uint64_t kPortKeyPollTimer = 0x1000;
// Debounce threshold.
constexpr uint64_t kDebounceThresholdNs = 50'000'000;

class HidButtonsDevice;
using DeviceType = ddk::Device<HidButtonsDevice, ddk::Unbindable>;
class HidButtonsHidBusFunction;
using HidBusFunctionType = ddk::Device<HidButtonsHidBusFunction>;
class ButtonsNotifyInterface;

using Buttons = fuchsia_buttons::Buttons;
using ButtonType = fuchsia_buttons::wire::ButtonType;

class HidButtonsDevice : public DeviceType {
 public:
  struct Gpio {
    gpio_protocol_t gpio;
    zx::interrupt irq;
    buttons_gpio_config_t config;
  };

  explicit HidButtonsDevice(zx_device_t* device) : DeviceType(device) {}
  virtual ~HidButtonsDevice() = default;

  // Hidbus Protocol Functions.
  zx_status_t HidbusStart(const hidbus_ifc_protocol_t* ifc) TA_EXCL(client_lock_);
  zx_status_t HidbusQuery(uint32_t options, hid_info_t* info);
  void HidbusStop() TA_EXCL(client_lock_);
  zx_status_t HidbusGetDescriptor(hid_description_type_t desc_type, uint8_t* out_data_buffer,
                                  size_t data_size, size_t* out_data_actual);
  zx_status_t HidbusGetReport(uint8_t rpt_type, uint8_t rpt_id, uint8_t* data, size_t len,
                              size_t* out_len) TA_EXCL(client_lock_);
  zx_status_t HidbusSetReport(uint8_t rpt_type, uint8_t rpt_id, const uint8_t* data, size_t len);
  zx_status_t HidbusGetIdle(uint8_t rpt_id, uint8_t* duration);
  zx_status_t HidbusSetIdle(uint8_t rpt_id, uint8_t duration);
  zx_status_t HidbusGetProtocol(uint8_t* protocol);
  zx_status_t HidbusSetProtocol(uint8_t protocol);

  // FIDL Interface Functions.
  bool GetState(ButtonType type);
  zx_status_t RegisterNotify(uint8_t types, ButtonsNotifyInterface* notify);

  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  zx_status_t Bind(fbl::Array<Gpio> gpios, fbl::Array<buttons_button_config_t> buttons);
  virtual void ClosingChannel(ButtonsNotifyInterface* notify);
  virtual void Notify(uint32_t button_index);

 protected:
  // Protected for unit testing.
  void ShutDown() TA_EXCL(client_lock_);

  zx::port port_;

  fbl::Mutex channels_lock_;
  // A map of ButtonTypes to the interfaces that have to be notified when they are pressed.
  std::map<ButtonType, std::set<ButtonsNotifyInterface*>> registered_notifiers_
      TA_GUARDED(channels_lock_);
  // A map of ButtonType values to an index into the buttons_ array.
  std::map<ButtonType, uint32_t> button_map_;

  std::list<ButtonsNotifyInterface> interfaces_ TA_GUARDED(channels_lock_);  // owns the channels

  HidButtonsHidBusFunction* hidbus_function_;

 private:
  friend class HidButtonsDeviceTest;

  int Thread();
  uint8_t ReconfigurePolarity(uint32_t idx, uint64_t int_port);
  zx_status_t ConfigureInterrupt(uint32_t idx, uint64_t int_port);
  bool MatrixScan(uint32_t row, uint32_t col, zx_duration_t delay);

  thrd_t thread_;
  libsync::Completion thread_started_;
  fbl::Mutex client_lock_;
  ddk::HidbusIfcProtocolClient client_ TA_GUARDED(client_lock_);
  fbl::Array<buttons_button_config_t> buttons_;
  fbl::Array<Gpio> gpios_;

  struct debounce_state {
    bool enqueued;
    zx::timer timer;
    bool value;
  };
  fbl::Array<debounce_state> debounce_states_;
  // last_report_ saved to de-duplicate reports
  buttons_input_rpt_t last_report_;

  zx::duration poll_period_{zx::duration::infinite()};
  zx::timer poll_timer_;
};

class HidButtonsHidBusFunction
    : public HidBusFunctionType,
      public ddk::HidbusProtocol<HidButtonsHidBusFunction, ddk::base_protocol>,
      public fbl::RefCounted<HidButtonsHidBusFunction> {
 public:
  explicit HidButtonsHidBusFunction(zx_device_t* device, HidButtonsDevice* peripheral)
      : HidBusFunctionType(device), device_(peripheral) {}
  virtual ~HidButtonsHidBusFunction() = default;

  void DdkRelease() { delete this; }

  // Methods required by the ddk mixins.
  zx_status_t HidbusStart(const hidbus_ifc_protocol_t* ifc) { return device_->HidbusStart(ifc); }
  zx_status_t HidbusQuery(uint32_t options, hid_info_t* info) {
    return device_->HidbusQuery(options, info);
  }
  void HidbusStop() { device_->HidbusStop(); }
  zx_status_t HidbusGetDescriptor(hid_description_type_t desc_type, uint8_t* out_data_buffer,
                                  size_t data_size, size_t* out_data_actual) {
    return device_->HidbusGetDescriptor(desc_type, out_data_buffer, data_size, out_data_actual);
  }
  zx_status_t HidbusGetReport(uint8_t rpt_type, uint8_t rpt_id, uint8_t* data, size_t len,
                              size_t* out_len) {
    return device_->HidbusGetReport(rpt_type, rpt_id, data, len, out_len);
  }
  zx_status_t HidbusSetReport(uint8_t rpt_type, uint8_t rpt_id, const uint8_t* data, size_t len) {
    return device_->HidbusSetReport(rpt_type, rpt_id, data, len);
  }
  zx_status_t HidbusGetIdle(uint8_t rpt_id, uint8_t* duration) {
    return device_->HidbusGetIdle(rpt_id, duration);
  }
  zx_status_t HidbusSetIdle(uint8_t rpt_id, uint8_t duration) {
    return device_->HidbusSetIdle(rpt_id, duration);
  }
  zx_status_t HidbusGetProtocol(uint8_t* protocol) { return device_->HidbusGetProtocol(protocol); }
  zx_status_t HidbusSetProtocol(uint8_t protocol) { return device_->HidbusSetProtocol(protocol); }

 private:
  HidButtonsDevice* device_;
};

class ButtonsNotifyInterface : public fidl::WireServer<Buttons> {
 public:
  explicit ButtonsNotifyInterface(HidButtonsDevice* peripheral) : device_(peripheral) {}
  ~ButtonsNotifyInterface() override = default;

  const fidl::ServerBindingRef<Buttons>& binding() { return *binding_; }

  // Methods required by the FIDL interface
  void GetState(GetStateRequestView request, GetStateCompleter::Sync& completer) override {
    completer.Reply(device_->GetState(request->type));
  }
  void RegisterNotify(RegisterNotifyRequestView request,
                      RegisterNotifyCompleter::Sync& completer) override {
    completer.Reply(zx::make_result(device_->RegisterNotify(request->types, this)));
  }

 private:
  HidButtonsDevice* device_;
  std::optional<fidl::ServerBindingRef<Buttons>> binding_;
};

}  // namespace buttons

#endif  // SRC_UI_INPUT_DRIVERS_HID_BUTTONS_HID_BUTTONS_H_
