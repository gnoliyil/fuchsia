// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BOARD_DRIVERS_X86_X86_H_
#define SRC_DEVICES_BOARD_DRIVERS_X86_X86_H_

#include <fidl/fuchsia.hardware.acpi/cpp/wire.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/fidl/cpp/channel.h>
#include <threads.h>

#include <memory>

#include <ddktl/device.h>
#include <fbl/macros.h>
#include <fbl/vector.h>

#include "src/devices/board/lib/acpi/acpi.h"
#include "src/devices/board/lib/acpi/manager.h"
#include "src/devices/lib/iommu/iommu-x86.h"

namespace x86 {

class SysSuspender : public fdf::WireServer<fuchsia_hardware_platform_bus::SysSuspend> {
 public:
  void Callback(CallbackRequestView request, fdf::Arena& arena, CallbackCompleter::Sync& completer);
};

class X86;
using DeviceType =
    ddk::Device<X86, ddk::Messageable<fuchsia_hardware_acpi::Acpi>::Mixin, ddk::Initializable>;

// This is the main class for the X86 platform bus driver.
class X86 : public DeviceType {
 public:
  explicit X86(zx_device_t* parent, fdf::ClientEnd<fuchsia_hardware_platform_bus::PlatformBus> pbus,
               std::unique_ptr<acpi::Acpi> acpi)
      : DeviceType(parent), pbus_(std::move(pbus)), acpi_(std::move(acpi)) {}
  ~X86();

  static zx_status_t Create(void* ctx, zx_device_t* parent, std::unique_ptr<X86>* out);
  static zx_status_t CreateAndBind(void* ctx, zx_device_t* parent);
  static bool RunUnitTests(void* ctx, zx_device_t* parent, zx_handle_t channel);

  // Device protocol implementation.
  void DdkRelease();
  void DdkInit(ddk::InitTxn txn);

  // ACPI protocol FIDL interface implementation.
  void ListTableEntries(ListTableEntriesCompleter::Sync& completer) override;
  void ReadNamedTable(ReadNamedTableRequestView request,
                      ReadNamedTableCompleter::Sync& completer) override;

  // Performs ACPICA initialization.
  zx_status_t EarlyAcpiInit();

  zx_status_t EarlyInit();

  // Add the list of ACPI entries present in the system to |entries|.
  //
  // Requires that ACPI has been initialised.
  zx_status_t GetAcpiTableEntries(fbl::Vector<fuchsia_hardware_acpi::wire::TableInfo>* entries);

  acpi::Acpi* acpi() { return acpi_.get(); }

 private:
  X86(const X86&) = delete;
  X86(X86&&) = delete;
  X86& operator=(const X86&) = delete;
  X86& operator=(X86&&) = delete;

  zx_status_t SysmemInit();

  zx_status_t GoldfishControlInit();

  // Register this instance with devmgr.
  zx_status_t Bind();
  // Asynchronously complete initialisation.
  zx_status_t DoInit();

  IommuManager iommu_manager_{
      [](fx_log_severity_t severity, const char* file, int line, const char* msg, va_list args) {
        zxlogvf_etc(severity, nullptr, file, line, msg, args);
      }};

  // TODO(fxbug.dev/108070): migrate to fdf::SyncClient when it is available.
  fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus_;

  std::unique_ptr<acpi::Manager> acpi_manager_;
  std::unique_ptr<acpi::Acpi> acpi_;
  // Whether the global ACPICA initialization has been performed or not
  bool acpica_initialized_ = false;
  SysSuspender suspender_;
};

}  // namespace x86

#endif  // SRC_DEVICES_BOARD_DRIVERS_X86_X86_H_
