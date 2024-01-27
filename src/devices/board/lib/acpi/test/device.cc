// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/board/lib/acpi/test/device.h"

#include <lib/ddk/debug.h>

#include <cstdint>
#include <string>

#include "src/devices/board/lib/acpi/status.h"
#include "src/devices/board/lib/acpi/util.h"
#include "src/devices/lib/acpi/util.h"

namespace acpi::test {

Device* Device::FindByPath(std::string_view path) {
  if (path.empty()) {
    return nullptr;
  }
  if (path[0] == '\\') {
    Device* root = this;
    while (root->parent_) {
      root = root->parent_;
    }
    return root->FindByPathInternal(path.substr(1));
  }
  if (path[0] == '^') {
    if (parent_) {
      return parent_->FindByPathInternal(path.substr(1));
    }
    return nullptr;
  }
  return FindByPathInternal(path);
}

Device* Device::FindByPathInternal(std::string_view path) {
  if (path.empty()) {
    return this;
  }
  std::string_view segment;
  std::string leftover;
  auto pos = path.find('.');
  if (pos == std::string::npos) {
    segment = path;
    leftover = "";
  } else {
    segment = std::string_view(path.data(), pos);
    leftover = path.substr(pos + 1);
  }

  if (Device* child = LookupChild(segment)) {
    return child->FindByPathInternal(leftover);
  }

  if (leftover.empty() && methods_.find(std::string(segment)) != methods_.end()) {
    return this;
  }

  return nullptr;
}

Device* Device::LookupChild(std::string_view name) {
  for (auto& child : children_) {
    if (child->name_ == name) {
      return child.get();
    }
  }
  return nullptr;
}

std::string Device::GetAbsolutePath() {
  std::string ret = name_;
  Device* cur = parent_;
  while (cur) {
    if (cur->parent_) {
      // If we have a parent, then separate names by '.'.
      ret = cur->name_ + "." + ret;
    } else {
      // The root node is called '\' and doesn't need a separator.
      ret = cur->name_ + ret;
    }
    cur = cur->parent_;
  }
  return ret;
}

acpi::status<acpi::UniquePtr<ACPI_OBJECT>> Device::EvaluateObject(
    std::optional<std::string> pathname, std::optional<std::vector<ACPI_OBJECT>> args) {
  if (!pathname.has_value()) {
    return methods_.find(pathname)->second(std::move(args));
  }

  auto pos = pathname->find('.');
  if (pos != std::string::npos) {
    Device* d = LookupChild(std::string_view(pathname->data(), pos));
    if (d == nullptr) {
      return acpi::error(AE_NOT_FOUND);
    }
    return d->EvaluateObject(pathname->substr(pos + 1).c_str(), std::move(args));
  }

  if (methods_.find(pathname) != methods_.end()) {
    return methods_.find(pathname)->second(std::move(args));
  }

  if (pathname == "_DSD") {
    // Number of objects we need to create: one for each UUID, one for each set of values.
    size_t object_count = dsd_.size() * 2;
    // One for the top-level "package".
    object_count += 1;

    ACPI_OBJECT* array =
        static_cast<ACPI_OBJECT*>(AcpiOsAllocate(sizeof(ACPI_OBJECT) * object_count));
    acpi::UniquePtr<ACPI_OBJECT> objects(array);

    array[0] = ACPI_OBJECT{.Package = {
                               .Type = ACPI_TYPE_PACKAGE,
                               .Count = static_cast<uint32_t>(object_count - 1),
                               .Elements = &array[1],
                           }};
    size_t i = 1;
    for (auto& pair : dsd_) {
      array[i] = ACPI_OBJECT{.Buffer = {
                                 .Type = ACPI_TYPE_BUFFER,
                                 .Length = acpi::kUuidBytes,
                                 .Pointer = const_cast<uint8_t*>(pair.first.bytes),
                             }};
      i++;

      array[i] = ACPI_OBJECT{.Package = {
                                 .Type = ACPI_TYPE_PACKAGE,
                                 .Count = static_cast<uint32_t>(pair.second.size()),
                                 .Elements = pair.second.data(),
                             }};
      i++;
    }

    return acpi::ok(std::move(objects));
  }
  if (pathname == "_STA" && sta_.has_value()) {
    ACPI_OBJECT* value = static_cast<ACPI_OBJECT*>(AcpiOsAllocate(sizeof(*value)));
    value->Integer.Type = ACPI_TYPE_INTEGER;
    value->Integer.Value = sta_.value();
    return acpi::ok(acpi::UniquePtr<ACPI_OBJECT>(value));
  }
  if (pathname == "_GLK" && glk_.has_value()) {
    ACPI_OBJECT* value = static_cast<ACPI_OBJECT*>(AcpiOsAllocate(sizeof(*value)));
    value->Integer.Type = ACPI_TYPE_INTEGER;
    value->Integer.Value = glk_.value();
    return acpi::ok(acpi::UniquePtr<ACPI_OBJECT>(value));
  }
  return acpi::error(AE_NOT_FOUND);
}

acpi::status<> Device::InstallNotifyHandler(Acpi::NotifyHandlerCallable callback, void* context,
                                            uint32_t raw_mode) {
  if (notify_handler_ != nullptr) {
    return acpi::error(AE_ALREADY_EXISTS);
  }

  notify_handler_mode_ = fuchsia_hardware_acpi::wire::NotificationMode::TryFrom(raw_mode);
  if (notify_handler_mode_ == std::nullopt) {
    return acpi::error(AE_BAD_PARAMETER);
  }
  notify_handler_ = callback;
  notify_handler_ctx_ = context;
  return acpi::ok();
}

acpi::status<> Device::RemoveNotifyHandler(Acpi::NotifyHandlerCallable callback,
                                           uint32_t raw_mode) {
  if (raw_mode != static_cast<uint32_t>(notify_handler_mode_.value())) {
    return acpi::error(AE_BAD_PARAMETER);
  }
  if (callback != notify_handler_) {
    return acpi::error(AE_NOT_FOUND);
  }
  notify_handler_ = nullptr;
  return acpi::ok();
}

acpi::status<> Device::AddAddressSpaceHandler(ACPI_ADR_SPACE_TYPE type,
                                              Acpi::AddressSpaceHandler handler, void* context) {
  if (address_space_handlers_.find(type) != address_space_handlers_.end()) {
    return acpi::error(AE_ALREADY_EXISTS);
  }

  address_space_handlers_.emplace(type, std::pair(handler, context));
  return acpi::ok();
}

acpi::status<> Device::RemoveAddressSpaceHandler(ACPI_ADR_SPACE_TYPE type,
                                                 Acpi::AddressSpaceHandler handler) {
  auto result = address_space_handlers_.find(type);
  if (result == address_space_handlers_.end()) {
    zxlogf(ERROR, "No handler!!");
    return acpi::error(AE_NOT_FOUND);
  }

  if (result->second.first != handler) {
    zxlogf(ERROR, "Wrong handler!! %p", handler);
    return acpi::error(AE_NOT_FOUND);
  }

  address_space_handlers_.erase(result);
  return acpi::ok();
}

acpi::status<> Device::AddressSpaceOp(ACPI_ADR_SPACE_TYPE space, uint32_t function,
                                      ACPI_PHYSICAL_ADDRESS address, uint32_t bit_width,
                                      UINT64* value) {
  auto result = address_space_handlers_.find(space);
  if (result == address_space_handlers_.end()) {
    return acpi::error(AE_NOT_FOUND);
  }

  return acpi::make_status(
      result->second.first(function, address, bit_width, value, result->second.second, nullptr));
}

void Device::SetPowerResourceMethods(uint8_t system_level, uint16_t resource_order) {
  SetSta(0);
  AddMethodCallback(
      std::nullopt, [system_level, resource_order](const std::optional<std::vector<ACPI_OBJECT>>&) {
        ACPI_OBJECT* retval = static_cast<ACPI_OBJECT*>(AcpiOsAllocate(sizeof(*retval)));
        retval->PowerResource.Type = ACPI_TYPE_POWER;
        retval->PowerResource.SystemLevel = system_level;
        retval->PowerResource.ResourceOrder = resource_order;
        return acpi::ok(acpi::UniquePtr<ACPI_OBJECT>(retval));
      });

  AddMethodCallback("_ON", [this](const std::optional<std::vector<ACPI_OBJECT>>&) {
    SetSta(1);
    return acpi::ok(acpi::UniquePtr<ACPI_OBJECT>());
  });

  AddMethodCallback("_OFF", [this](const std::optional<std::vector<ACPI_OBJECT>>&) {
    SetSta(0);
    return acpi::ok(acpi::UniquePtr<ACPI_OBJECT>());
  });
}

}  // namespace acpi::test
