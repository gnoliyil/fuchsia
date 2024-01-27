// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "registers.h"

#include <lib/ddk/binding_driver.h>
#include <lib/ddk/metadata.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/fidl/epitaph.h>

namespace registers {

namespace {

const std::map<fuchsia_hardware_registers::wire::Mask::Tag, uint8_t> kTagToBytes = {
    {fuchsia_hardware_registers::wire::Mask::Tag::kR8, 1},
    {fuchsia_hardware_registers::wire::Mask::Tag::kR16, 2},
    {fuchsia_hardware_registers::wire::Mask::Tag::kR32, 4},
    {fuchsia_hardware_registers::wire::Mask::Tag::kR64, 8},
};

template <typename Ty>
std::optional<Ty> GetMask(const fuchsia_hardware_registers::wire::Mask& mask) {
  if constexpr (std::is_same_v<Ty, uint8_t>) {
    // Need cast to compile
    return static_cast<Ty>(mask.r8());
  }
  if constexpr (std::is_same_v<Ty, uint16_t>) {
    // Need cast to compile
    return static_cast<Ty>(mask.r16());
  }
  if constexpr (std::is_same_v<Ty, uint32_t>) {
    // Need cast to compile
    return static_cast<Ty>(mask.r32());
  }
  if constexpr (std::is_same_v<Ty, uint64_t>) {
    // Need cast to compile
    return static_cast<Ty>(mask.r64());
  }
  return std::nullopt;
}

}  // namespace

template <typename T>
zx_status_t Register<T>::Init(const RegistersMetadataEntry& config) {
  id_ = config.bind_id();

  for (const auto& m : config.masks()) {
    auto mask = GetMask<T>(m.mask());
    if (!mask.has_value()) {
      return ZX_ERR_INTERNAL;
    }
    masks_.emplace(m.mmio_offset(), std::make_pair(mask.value(), m.count()));
  }

  return ZX_OK;
}

template <typename T>
zx::result<fidl::ClientEnd<fuchsia_io::Directory>> Register<T>::CreateAndServeOutgoingDirectory() {
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }

  fuchsia_hardware_registers::Service::InstanceHandler handler({
      .device = bindings_.CreateHandler(this, dispatcher_, fidl::kIgnoreBindingClosure),
  });
  auto result = outgoing_.AddService<fuchsia_hardware_registers::Service>(std::move(handler));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to add service to the outgoing directory");
    return result.take_error();
  }

  result = outgoing_.Serve(std::move(endpoints->server));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to add service to the outgoing directory");
    return result.take_error();
  }

  return zx::ok(std::move(endpoints->client));
}

// Returns: true if mask requested is covered by allowed mask.
//          false if mask requested is not covered by allowed mask or mask is not found.
template <typename T>
bool Register<T>::VerifyMask(T mask, const uint64_t offset) {
  auto it = masks_.upper_bound(offset);
  if ((offset % sizeof(T)) || (it == masks_.begin())) {
    return false;
  }
  it--;

  auto base_address = it->first;
  auto reg_mask = it->second.first;
  auto reg_count = it->second.second;
  return (((offset - base_address) / sizeof(T) < reg_count) &&
          // Check that mask requested is covered by allowed mask.
          ((mask | reg_mask) == reg_mask));
}

template <typename T>
zx_status_t Register<T>::ReadRegister(uint64_t offset, T mask, T* out_value) {
  if (!VerifyMask(mask, offset)) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&mmio_->locks[offset / sizeof(T)]);
  *out_value = mmio_->mmio.ReadMasked(mask, offset);
  return ZX_OK;
}

template <typename T>
zx_status_t Register<T>::WriteRegister(uint64_t offset, T mask, T value) {
  if (!VerifyMask(mask, offset)) {
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::AutoLock lock(&mmio_->locks[offset / sizeof(T)]);
  mmio_->mmio.ModifyBits(value, mask, offset);
  return ZX_OK;
}

template <typename T>
zx_status_t RegistersDevice<T>::Init(zx_device_t* parent, Metadata metadata) {
  zx_status_t status = ZX_OK;

  ddk::PDevFidl pdev(parent);
  pdev_device_info_t device_info = {};
  if ((status = pdev.GetDeviceInfo(&device_info)) != ZX_OK) {
    zxlogf(ERROR, "%s: Could not get device info", __func__);
    return status;
  }
  if (metadata.mmio().count() != device_info.mmio_count) {
    zxlogf(ERROR, "%s: MMIO metadata size doesn't match MMIO count.", __func__);
    return ZX_ERR_INTERNAL;
  }

  // Get MMIOs
  std::map<uint32_t, std::vector<T>> overlap;
  for (uint32_t i = 0; i < device_info.mmio_count; i++) {
    std::optional<fdf::MmioBuffer> tmp_mmio;
    if ((status = pdev.MapMmio(i, &tmp_mmio)) != ZX_OK) {
      zxlogf(ERROR, "%s: Could not get mmio regions", __func__);
      return status;
    }

    auto register_count = tmp_mmio->get_size() / sizeof(T);
    if (tmp_mmio->get_size() % sizeof(T)) {
      zxlogf(ERROR, "%s: MMIO size does not cover full registers", __func__);
      return ZX_ERR_INTERNAL;
    }

    std::vector<fbl::Mutex> tmp_locks(register_count);
    mmios_.emplace(metadata.mmio()[i].id(), std::make_shared<MmioInfo>(MmioInfo{
                                                .mmio = *std::move(tmp_mmio),
                                                .locks = std::move(tmp_locks),
                                            }));

    overlap.emplace(metadata.mmio()[i].id(), std::vector<T>(register_count, 0));
  }

  // Check for overlapping bits.
  for (const auto& reg : metadata.registers()) {
    if (!reg.has_bind_id() && !reg.has_mmio_id() && !reg.has_masks()) {
      // Doesn't have to have all Register IDs.
      continue;
    }

    if (mmios_.find(reg.mmio_id()) == mmios_.end()) {
      zxlogf(ERROR, "%s: Invalid MMIO ID %u for Register %u.\n", __func__, reg.mmio_id(),
             reg.bind_id());
      return ZX_ERR_INTERNAL;
    }

    for (const auto& m : reg.masks()) {
      if (m.mmio_offset() / sizeof(T) >= mmios_[reg.mmio_id()]->locks.size()) {
        zxlogf(ERROR, "%s: Invalid offset.\n", __func__);
        return ZX_ERR_INTERNAL;
      }

      if (!m.overlap_check_on()) {
        continue;
      }
      auto bits = overlap[reg.mmio_id()][m.mmio_offset() / sizeof(T)];
      auto mask = GetMask<T>(m.mask());
      if (!mask.has_value()) {
        zxlogf(ERROR, "%s: Invalid mask\n", __func__);
        return ZX_ERR_INTERNAL;
      }
      auto mask_value = mask.value();
      if (bits & mask_value) {
        zxlogf(ERROR, "%s: Overlapping bits in MMIO ID %u, Register No. %lu, Bit mask 0x%lx\n",
               __func__, reg.mmio_id(), m.mmio_offset() / sizeof(T),
               static_cast<uint64_t>(bits & mask_value));
        return ZX_ERR_INTERNAL;
      }
      overlap[reg.mmio_id()][m.mmio_offset() / sizeof(T)] |= mask_value;
    }
  }

  // Create Registers
  for (auto& reg : metadata.registers()) {
    if (!reg.has_bind_id() && !reg.has_mmio_id() && !reg.has_masks()) {
      // Doesn't have to have all Register IDs.
      continue;
    }

    fbl::AllocChecker ac;
    std::unique_ptr<Register<T>> tmp_register(
        new (&ac) Register<T>(this->zxdev(), mmios_[reg.mmio_id()]));
    if (!ac.check()) {
      return ZX_ERR_NO_MEMORY;
    }
    zx_device_prop_t props[] = {
        {BIND_REGISTER_ID, 0, reg.bind_id()},
    };
    char name[20];
    snprintf(name, sizeof(name), "register-%u", reg.bind_id());

    zx::result outgoing_directory_result = tmp_register->CreateAndServeOutgoingDirectory();
    if (outgoing_directory_result.is_error()) {
      return outgoing_directory_result.error_value();
    }

    std::array offers = {
        fuchsia_hardware_registers::Service::Name,
    };

    auto status = tmp_register->DdkAdd(
        ddk::DeviceAddArgs(name)
            .set_flags(DEVICE_ADD_ALLOW_MULTI_COMPOSITE)
            .set_props(props)
            .set_fidl_service_offers(offers)
            .set_outgoing_dir(outgoing_directory_result.value().TakeChannel()));
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: DdkAdd for %s failed %d", __func__, name, status);
      return status;
    }

    auto dev = tmp_register.release();
    dev->Init(reg);
  }

  return ZX_OK;
}

template <typename T>
zx_status_t RegistersDevice<T>::Create(zx_device_t* parent, Metadata metadata) {
  fbl::AllocChecker ac;
  std::unique_ptr<RegistersDevice<T>> device(new (&ac) RegistersDevice(parent));
  if (!ac.check()) {
    zxlogf(ERROR, "%s: device object alloc failed", __func__);
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status = ZX_OK;
  if ((status = device->DdkAdd("registers-device", DEVICE_ADD_NON_BINDABLE)) != ZX_OK) {
    zxlogf(ERROR, "%s: DdkAdd failed", __func__);
    return status;
  }

  auto* device_ptr = device.release();

  if ((status = device_ptr->Init(parent, metadata)) != ZX_OK) {
    device_ptr->DdkAsyncRemove();
    zxlogf(ERROR, "%s: Init failed", __func__);
    return status;
  }

  return ZX_OK;
}

zx_status_t Bind(void* ctx, zx_device_t* parent) {
  zx_status_t status = ZX_OK;
  // Get metadata
  auto decoded = ddk::GetEncodedMetadata<Metadata>(parent, DEVICE_METADATA_REGISTERS);
  if (!decoded.is_ok()) {
    return decoded.error_value();
  }

  const Metadata& metadata = *decoded.value();

  // Validate
  if (!metadata.has_mmio() || !metadata.has_registers()) {
    zxlogf(ERROR, "Metadata incomplete");
    return ZX_ERR_INTERNAL;
  }
  for (const auto& mmio : metadata.mmio()) {
    if (!mmio.has_id()) {
      zxlogf(ERROR, "Metadata incomplete");
      return ZX_ERR_INTERNAL;
    }
  }
  bool begin = true;
  fuchsia_hardware_registers::wire::Mask::Tag tag;
  for (const auto& reg : metadata.registers()) {
    if (!reg.has_bind_id() && !reg.has_mmio_id() && !reg.has_masks()) {
      // Doesn't have to have all Register IDs.
      continue;
    }

    if (!reg.has_bind_id() || !reg.has_mmio_id() || !reg.has_masks()) {
      zxlogf(ERROR, "Metadata incomplete");
      return ZX_ERR_INTERNAL;
    }

    if (begin) {
      tag = reg.masks().begin()->mask().Which();
      begin = false;
    }

    for (const auto& mask : reg.masks()) {
      if (!mask.has_mask() || !mask.has_mmio_offset() || !mask.has_count()) {
        zxlogf(ERROR, "Metadata incomplete");
        return ZX_ERR_INTERNAL;
      }

      if (mask.mask().Which() != tag) {
        zxlogf(ERROR, "Width of registers don't match up.");
        return ZX_ERR_INTERNAL;
      }

      if (mask.mmio_offset() % kTagToBytes.at(tag)) {
        zxlogf(ERROR, "%s: Mask with offset 0x%08lx is not aligned", __func__, mask.mmio_offset());
        return ZX_ERR_INTERNAL;
      }
    }
  }

  // Create devices
  switch (tag) {
    case fuchsia_hardware_registers::wire::Mask::Tag::kR8:
      status = RegistersDevice<uint8_t>::Create(parent, metadata);
      break;
    case fuchsia_hardware_registers::wire::Mask::Tag::kR16:
      status = RegistersDevice<uint16_t>::Create(parent, metadata);
      break;
    case fuchsia_hardware_registers::wire::Mask::Tag::kR32:
      status = RegistersDevice<uint32_t>::Create(parent, metadata);
      break;
    case fuchsia_hardware_registers::wire::Mask::Tag::kR64:
      status = RegistersDevice<uint64_t>::Create(parent, metadata);
      break;
  }

  return status;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = Bind;
  return ops;
}();

template class Register<uint8_t>;
template class Register<uint16_t>;
template class Register<uint32_t>;
template class Register<uint64_t>;

}  // namespace registers

// clang-format off
ZIRCON_DRIVER(registers, registers::driver_ops, "zircon", "0.1");
// clang-format on
