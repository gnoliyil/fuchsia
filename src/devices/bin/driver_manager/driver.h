// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_DRIVER_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_DRIVER_H_

#include <lib/ddk/binding.h>
#include <lib/fit/function.h>
#include <lib/zx/vmo.h>

#include <memory>
#include <variant>

#include <fbl/intrusive_double_list.h>
#include <fbl/string.h>

struct Driver : public fbl::DoublyLinkedListable<std::unique_ptr<Driver>> {
  Driver() = default;

  fbl::String name;

  uint32_t bytecode_version = 0;

  // Unlike the old bytecode format, the instructions in the new format are not
  // represented by three uint32 integers. To support both formats
  // simultaneously, zx_bind_inst_t values are used to represent the old bytecode
  // instructions while uint8_t values are used to represent the new bytecode.
  std::variant<std::unique_ptr<zx_bind_inst_t[]>, std::unique_ptr<uint8_t[]>> binding;

  // Number of bytes in the bind rules.
  uint32_t binding_size = 0;

  uint32_t flags = 0;
  zx::vmo dso_vmo;

  fbl::String libname;

  // If true, this driver never tries to match against new devices.
  bool never_autoselect = false;
};

#define DRIVER_NAME_LEN_MAX 64

using DriverLoadCallback = fit::function<void(Driver* driver, const char* version)>;

void load_driver(const char* path, DriverLoadCallback func);
zx_status_t load_driver_vmo(std::string_view libname, zx::vmo vmo, DriverLoadCallback func);
zx_status_t load_vmo(std::string_view libname, zx::vmo* out_vmo);
void find_loadable_drivers(const std::string& path, DriverLoadCallback func);

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_DRIVER_H_
