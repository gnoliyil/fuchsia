// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_REGISTERS_DRIVERS_REGISTERS_REGISTERS_H_
#define SRC_DEVICES_REGISTERS_DRIVERS_REGISTERS_REGISTERS_H_

#include <fidl/fuchsia.hardware.registers/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/ddk/platform-defs.h>
#include <lib/mmio/mmio.h>
#include <zircon/types.h>

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_counted.h>

namespace registers {

template <typename T>
class Register;
template <typename T>
using RegisterType = ddk::Device<Register<T>, ddk::Unbindable>;

template <typename T>
class RegistersDevice;
template <typename T>
using RegistersDeviceType = ddk::Device<RegistersDevice<T>>;

struct MmioInfo {
  fdf::MmioBuffer mmio;
  std::vector<fbl::Mutex> locks;
};

using fuchsia_hardware_registers::wire::Metadata;
using fuchsia_hardware_registers::wire::RegistersMetadataEntry;

template <typename T>
class Register : public RegisterType<T>,
                 public fidl::WireServer<fuchsia_hardware_registers::Device>,
                 public fbl::RefCounted<Register<T>> {
  using Device = fidl::WireServer<fuchsia_hardware_registers::Device>;

 public:
  explicit Register(zx_device_t* device, std::shared_ptr<MmioInfo> mmio)
      : RegisterType<T>(device),
        mmio_(std::move(mmio)),
        dispatcher_(fdf::Dispatcher::GetCurrent()->async_dispatcher()),
        outgoing_(dispatcher_) {}

  zx_status_t Init(const RegistersMetadataEntry& config);

  void DdkUnbind(ddk::UnbindTxn txn) { txn.Reply(); }
  void DdkRelease() { delete this; }

  zx::result<fidl::ClientEnd<fuchsia_io::Directory>> CreateAndServeOutgoingDirectory();

  void ReadRegister8(Device::ReadRegister8RequestView request,
                     Device::ReadRegister8Completer::Sync& completer) override {
    ReadRegister(request->offset, request->mask, completer);
  }
  void ReadRegister16(Device::ReadRegister16RequestView request,
                      Device::ReadRegister16Completer::Sync& completer) override {
    ReadRegister(request->offset, request->mask, completer);
  }
  void ReadRegister32(Device::ReadRegister32RequestView request,
                      Device::ReadRegister32Completer::Sync& completer) override {
    ReadRegister(request->offset, request->mask, completer);
  }
  void ReadRegister64(Device::ReadRegister64RequestView request,
                      Device::ReadRegister64Completer::Sync& completer) override {
    ReadRegister(request->offset, request->mask, completer);
  }
  void WriteRegister8(Device::WriteRegister8RequestView request,
                      Device::WriteRegister8Completer::Sync& completer) override {
    WriteRegister(request->offset, request->mask, request->value, completer);
  }
  void WriteRegister16(Device::WriteRegister16RequestView request,
                       Device::WriteRegister16Completer::Sync& completer) override {
    WriteRegister(request->offset, request->mask, request->value, completer);
  }
  void WriteRegister32(Device::WriteRegister32RequestView request,
                       Device::WriteRegister32Completer::Sync& completer) override {
    WriteRegister(request->offset, request->mask, request->value, completer);
  }
  void WriteRegister64(Device::WriteRegister64RequestView request,
                       Device::WriteRegister64Completer::Sync& completer) override {
    WriteRegister(request->offset, request->mask, request->value, completer);
  }

 private:
  template <typename Ty, typename Completer>
  void ReadRegister(uint64_t offset, Ty mask, Completer& completer) {
    if constexpr (!std::is_same_v<T, Ty>) {
      completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
      return;
    }
    T val;
    // Need cast to compile
    auto status = ReadRegister(offset, static_cast<T>(mask), &val);
    if (status == ZX_OK) {
      // Need cast to compile
      completer.ReplySuccess(static_cast<Ty>(val));
    } else {
      completer.ReplyError(status);
    }
  }

  template <typename Ty, typename Completer>
  void WriteRegister(uint64_t offset, Ty mask, Ty value, Completer& completer) {
    if constexpr (!std::is_same_v<T, Ty>) {
      completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
      return;
    }
    // Need cast to compile
    auto status = WriteRegister(offset, static_cast<T>(mask), static_cast<T>(value));
    if (status == ZX_OK) {
      completer.ReplySuccess();
    } else {
      completer.ReplyError(status);
    }
  }

  zx_status_t ReadRegister(uint64_t offset, T mask, T* out_value);
  zx_status_t WriteRegister(uint64_t offset, T mask, T value);

  bool VerifyMask(T mask, uint64_t register_offset);

  std::shared_ptr<MmioInfo> mmio_;
  uint64_t id_;
  std::map<uint64_t, std::pair<T, uint32_t>> masks_;  // base_address to (mask, reg_count)

  async_dispatcher_t* dispatcher_;
  component::OutgoingDirectory outgoing_;
  fidl::ServerBindingGroup<fuchsia_hardware_registers::Device> bindings_;
};

template <typename T>
class RegistersDevice : public RegistersDeviceType<T> {
 public:
  static zx_status_t Create(zx_device_t* parent, Metadata metadata);

  void DdkRelease() { delete this; }

 private:
  template <typename U>
  friend class FakeRegistersDevice;

  explicit RegistersDevice(zx_device_t* parent) : RegistersDeviceType<T>(parent) {}

  zx_status_t Init(zx_device_t* parent, Metadata metadata);
  // For unit tests
  zx_status_t Init(std::map<uint32_t, std::shared_ptr<MmioInfo>> mmios) {
    mmios_ = std::move(mmios);
    return ZX_OK;
  }

  std::map<uint32_t, std::shared_ptr<MmioInfo>> mmios_;  // MMIO ID to MmioInfo
};

extern template class Register<uint8_t>;
extern template class Register<uint16_t>;
extern template class Register<uint32_t>;
extern template class Register<uint64_t>;

}  // namespace registers

#endif  // SRC_DEVICES_REGISTERS_DRIVERS_REGISTERS_REGISTERS_H_
