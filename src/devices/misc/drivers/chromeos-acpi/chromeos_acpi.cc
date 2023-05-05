// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/misc/drivers/chromeos-acpi/chromeos_acpi.h"

#include <fidl/fuchsia.hardware.acpi/cpp/wire.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>

// The ChromeOS ACPI tables contain a bunch of information about the state of the device that comes
// from firmware. This information is static and generated by coreboot.

namespace chromeos_acpi {

zx_status_t ChromeosAcpi::Bind(void* ctx, zx_device_t* parent) {
  auto result = acpi::Client::Create(parent);
  if (result.is_error()) {
    zxlogf(ERROR, "Could not get ACPI client: %d", result.error_value());
    return result.error_value();
  }

  auto device = std::make_unique<ChromeosAcpi>(parent, std::move(result.value()));
  zx_status_t status = device->Bind();
  if (status == ZX_OK) {
    // The driver framework takes ownership of the device if DdkAdd() succeeds.
    [[maybe_unused]] auto unused = device.release();
  }
  return status;
}

zx_status_t ChromeosAcpi::Bind() {
  // Determine what methods are available.
  zx_status_t status = EvaluateObjectHelper(
      "MLST", [this](const facpi::Object& object) { available_methods_ = ParseMlst(object); });
  if (status != ZX_OK) {
    return status;
  }

  return DdkAdd(ddk::DeviceAddArgs("chromeos_acpi")
                    .set_inspect_vmo(inspect_.DuplicateVmo())
                    .set_proto_id(ZX_PROTOCOL_CHROMEOS_ACPI));
}

void ChromeosAcpi::DdkInit(ddk::InitTxn txn) {
  if (available_methods_.find(kHwidMethodName) != available_methods_.end()) {
    EvaluateObjectHelper(kHwidMethodName, [this](const facpi::Object& object) {
      if (!object.is_package_val() || object.package_val().value.count() != 1 ||
          !object.package_val().value[0].is_string_val()) {
        zxlogf(WARNING,
               "Bad HWID - wanted a package with a single string element, but didn't get one");
        return;
      }
      auto& hwid_view = object.package_val().value[0].string_val();
      hwid_ = std::string(hwid_view.data(), hwid_view.size());
      inspect_.GetRoot().CreateString("hwid", hwid_.value(), &inspect_);
    });
  }

  if (available_methods_.find(kRoFirmwareMethodName) != available_methods_.end()) {
    EvaluateObjectHelper(kRoFirmwareMethodName, [this](const facpi::Object& object) {
      if (!object.is_package_val() || object.package_val().value.count() != 1 ||
          !object.package_val().value[0].is_string_val()) {
        zxlogf(WARNING,
               "Bad RO FWID - wanted a package with a single string element, but didn't get one");
        return;
      }
      auto& ro_fwid_view = object.package_val().value[0].string_val();
      ro_fwid_ = std::string(ro_fwid_view.data(), ro_fwid_view.size());
      inspect_.GetRoot().CreateString("ro-fwid", ro_fwid_.value(), &inspect_);
    });
  }

  if (available_methods_.find(kRwFirmwareMethodName) != available_methods_.end()) {
    EvaluateObjectHelper(kRwFirmwareMethodName, [this](const facpi::Object& object) {
      if (!object.is_package_val() || object.package_val().value.count() != 1 ||
          !object.package_val().value[0].is_string_val()) {
        zxlogf(WARNING,
               "Bad RW FWID - wanted a package with a single string element, but didn't get one");
        return;
      }
      auto& rw_fwid_view = object.package_val().value[0].string_val();
      rw_fwid_ = std::string(rw_fwid_view.data(), rw_fwid_view.size());
      inspect_.GetRoot().CreateString("rw-fwid", rw_fwid_.value(), &inspect_);
    });
  }

  if (available_methods_.find(kNvramLocationMethodName) != available_methods_.end()) {
    EvaluateObjectHelper(kNvramLocationMethodName, [this](const facpi::Object& object) {
      if (!object.is_package_val() || object.package_val().value.count() != 2) {
        zxlogf(WARNING, "Bad nvram information - expected a package with two values.");
        return;
      }

      auto& package = object.package_val().value;
      if (!package[0].is_integer_val() || !package[1].is_integer_val()) {
        zxlogf(WARNING, "Bad nvram information - expected two integers.");
        return;
      }

      NvramInfo info = {
          .base = static_cast<uint32_t>(package[0].integer_val()),
          .size = static_cast<uint32_t>(package[1].integer_val()),
      };
      nvram_location_ = info;
      inspect_.GetRoot().CreateUint("nvram-data-base", info.base, &inspect_);
      inspect_.GetRoot().CreateUint("nvram-data-size", info.size, &inspect_);
    });
  }

  if (available_methods_.find(kFlashmapBaseMethodName) != available_methods_.end()) {
    EvaluateObjectHelper(kFlashmapBaseMethodName, [this](const facpi::Object& object) {
      if (!object.is_package_val() || object.package_val().value.count() != 1 ||
          !object.package_val().value[0].is_integer_val()) {
        zxlogf(WARNING,
               "Bad flashmap base - wanted a package with a single integer element, but didn't get "
               "one");
        return;
      }

      uintptr_t addr = object.package_val().value[0].integer_val();
      flashmap_base_ = addr;
      inspect_.GetRoot().CreateUint("flashmap-addr", addr, &inspect_);
    });
  }

  if (available_methods_.find(kVbootSharedDataMethodName) != available_methods_.end()) {
    EvaluateObjectHelper(kVbootSharedDataMethodName, [this](const facpi::Object& object) {
      if (!object.is_package_val() || object.package_val().value.count() != 1 ||
          !object.package_val().value[0].is_buffer_val()) {
        zxlogf(WARNING, "Bad vboot shared data - expected a buffer");
        return;
      }

      auto buffer = object.package_val().value[0].buffer_val();
      if (buffer.count() < VB_SHARED_DATA_HEADER_SIZE_V2) {
        zxlogf(WARNING, "vboot shared data is the wrong size");
        return;
      }

      const VbSharedDataHeader* header = reinterpret_cast<const VbSharedDataHeader*>(buffer.data());
      if (header->magic != VB_SHARED_DATA_MAGIC ||
          header->struct_version != VB_SHARED_DATA_VERSION) {
        zxlogf(WARNING, "vboot shared data is invalid (bad magic or version)");
        return;
      }
      shared_data_.emplace(*header);
    });
  }

  if (available_methods_.find(kBootInfoMethodName) != available_methods_.end()) {
    EvaluateObjectHelper(kBootInfoMethodName, [this](const facpi::Object& object) {
      if (!object.is_package_val() || object.package_val().value.count() != kBootInfoNumFields) {
        zxlogf(WARNING, "Invalid BINF: expect a package with 5 values");
        return;
      }

      std::array<uint64_t, kBootInfoNumFields> data;
      for (size_t i = 0; i < object.package_val().value.count(); i++) {
        auto& entry = object.package_val().value[i];
        if (!entry.is_integer_val()) {
          zxlogf(WARNING, "Invalid BINF: expected integer values");
          return;
        }
        data[i] = entry.integer_val();
      }

      binf_ = data;
    });
  }

  txn.Reply(ZX_OK);
}

void ChromeosAcpi::DdkRelease() { delete this; }

zx_status_t ChromeosAcpi::EvaluateObjectHelper(const char* name,
                                               std::function<void(const facpi::Object&)> callback) {
  auto result = acpi_.borrow()->EvaluateObject(fidl::StringView::FromExternal(name),
                                               facpi::EvaluateObjectMode::kPlainObject,
                                               fidl::VectorView<facpi::Object>());
  if (!result.ok()) {
    zxlogf(ERROR, "Failed to send FIDL EvaluateObject: %d", result.status());
    return result.status();
  }

  if (result->is_error()) {
    zxlogf(ERROR, "MLST failed: %d", static_cast<uint32_t>(result->error_value()));
    return ZX_ERR_NOT_SUPPORTED;
  }

  fidl::WireOptional<facpi::EncodedObject>& maybe_encoded = result->value()->result;
  if (!maybe_encoded.has_value() || !maybe_encoded->is_object()) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  callback(maybe_encoded->object());
  return ZX_OK;
}

std::unordered_set<std::string> ChromeosAcpi::ParseMlst(const facpi::Object& object) {
  std::unordered_set<std::string> methods;
  std::string inspect_value;

  if (!object.is_package_val()) {
    zxlogf(ERROR, "Expected a package, but didn't get one");
    return methods;
  }

  for (auto& method : object.package_val().value) {
    if (!method.is_string_val()) {
      zxlogf(ERROR, "Expected a string but didn't get one");
      continue;
    }
    std::string name(method.string_val().data(), method.string_val().size());
    inspect_value += name + ",";
    methods.emplace(std::move(name));
  }
  if (!inspect_value.empty()) {
    inspect_value.pop_back();
  }
  inspect_.GetRoot().CreateString("method-list", inspect_value, &inspect_);

  return methods;
}

void ChromeosAcpi::GetHardwareId(GetHardwareIdCompleter::Sync& completer) {
  if (hwid_.has_value()) {
    completer.ReplySuccess(fidl::StringView::FromExternal(hwid_.value()));
  } else {
    completer.ReplyError(ZX_ERR_NOT_FOUND);
  }
}

void ChromeosAcpi::GetRwFirmwareVersion(GetRwFirmwareVersionCompleter::Sync& completer) {
  if (rw_fwid_.has_value()) {
    completer.ReplySuccess(fidl::StringView::FromExternal(rw_fwid_.value()));
  } else {
    completer.ReplyError(ZX_ERR_NOT_FOUND);
  }
}

void ChromeosAcpi::GetRoFirmwareVersion(GetRoFirmwareVersionCompleter::Sync& completer) {
  if (ro_fwid_.has_value()) {
    completer.ReplySuccess(fidl::StringView::FromExternal(ro_fwid_.value()));
  } else {
    completer.ReplyError(ZX_ERR_NOT_FOUND);
  }
}

void ChromeosAcpi::GetNvramMetadataLocation(GetNvramMetadataLocationCompleter::Sync& completer) {
  if (nvram_location_.has_value()) {
    completer.ReplySuccess(nvram_location_->base, nvram_location_->size);
  } else {
    completer.ReplyError(ZX_ERR_NOT_FOUND);
  }
}

void ChromeosAcpi::GetFlashmapAddress(GetFlashmapAddressCompleter::Sync& completer) {
  if (flashmap_base_.has_value()) {
    completer.ReplySuccess(flashmap_base_.value());
  } else {
    completer.ReplyError(ZX_ERR_NOT_FOUND);
  }
}

void ChromeosAcpi::GetNvdataVersion(GetNvdataVersionCompleter::Sync& completer) {
  if (shared_data_.has_value()) {
    completer.ReplySuccess((shared_data_->flags & kVbootSharedDataNvdataV2) ? 2 : 1);
  } else {
    completer.ReplyError(ZX_ERR_NOT_FOUND);
  }
}

void ChromeosAcpi::GetActiveApFirmware(GetActiveApFirmwareCompleter::Sync& completer) {
  if (binf_.has_value()) {
    completer.ReplySuccess(fuchsia_acpi_chromeos::wire::BootSlot(
        static_cast<uint32_t>(binf_.value()[kBootInfoActiveApFirmware])));
  } else {
    completer.ReplyError(ZX_ERR_NOT_FOUND);
  }
}

static zx_driver_ops_t driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = ChromeosAcpi::Bind,
};

}  // namespace chromeos_acpi

ZIRCON_DRIVER(ChromeosAcpi, chromeos_acpi::driver_ops, "zircon", "0.1");
