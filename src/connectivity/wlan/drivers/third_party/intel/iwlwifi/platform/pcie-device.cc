// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/pcie-device.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <zircon/assert.h>
#include <zircon/status.h>

#include <memory>

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/pci-fidl.h"

extern "C" {
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-debug.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-drv.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/pcie/entry.h"
}

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/driver-inspector.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/rcu-manager.h"

#if !CPTCFG_IWLMVM
#error "PcieDevice requires support for MVM firmwares."
#endif  // CPTCFG_IWLMVM

namespace wlan {
namespace iwlwifi {

PcieDevice::PcieDevice(zx_device_t* parent) : WlanPhyImplDevice(parent) { pci_dev_ = {}; }

PcieDevice::~PcieDevice() { ZX_DEBUG_ASSERT(pci_dev_.drvdata == nullptr); }

zx_status_t PcieDevice::Create(zx_device_t* parent_device) {
  zx_status_t status = ZX_OK;

  std::unique_ptr<PcieDevice> device(new PcieDevice(parent_device));
  device->driver_inspector_ =
      std::make_unique<DriverInspector>(DriverInspectorOptions{.root_name = "iwlwifi"});
  if ((status = device->DdkAdd(::ddk::DeviceAddArgs("iwlwifi-wlanphyimpl")
                                   .set_proto_id(ZX_PROTOCOL_WLANPHY_IMPL)
                                   .set_inspect_vmo(device->driver_inspector_->DuplicateVmo()))) !=
      ZX_OK) {
    IWL_ERR(device->drvdata(), "%s() failed device add: %s", __func__,
            zx_status_get_string(status));
    return status;
  }

  // The lifecycle of this object is now managed by the devhost.
  device.release();
  return ZX_OK;
}

iwl_trans* PcieDevice::drvdata() { return pci_dev_.drvdata; }

const iwl_trans* PcieDevice::drvdata() const { return pci_dev_.drvdata; }

void PcieDevice::DdkInit(::ddk::InitTxn txn) {
  const zx_status_t status = [&]() {
    zx_status_t status = ZX_OK;

    task_loop_ = std::make_unique<::async::Loop>(&kAsyncLoopConfigNoAttachToCurrentThread);
    if ((status = task_loop_->StartThread("iwlwifi-task-worker", nullptr)) != ZX_OK) {
      IWL_ERR(iwl_trans, "Failed to create task async loop thread: %s\n",
              zx_status_get_string(status));
      return status;
    }
    irq_loop_ = std::make_unique<::async::Loop>(&kAsyncLoopConfigNoAttachToCurrentThread);
    if ((status = irq_loop_->StartThread("iwlwifi-irq-worker", nullptr)) != ZX_OK) {
      IWL_ERR(iwl_trans, "Failed to create irq async loop thread: %s\n",
              zx_status_get_string(status));
      return status;
    }
    rcu_manager_ = std::make_unique<RcuManager>(task_loop_->dispatcher());
    rcu_manager_->InitForThread();
    ::async::PostTask(task_loop_->dispatcher(), [this]() { rcu_manager_->InitForThread(); });
    ::async::PostTask(irq_loop_->dispatcher(), [this]() { rcu_manager_->InitForThread(); });

    // Fill in the relevant Fuchsia-specific fields in our driver interface struct.
    pci_dev_.dev.zxdev = zxdev();
    pci_dev_.dev.task_dispatcher = task_loop_->dispatcher();
    pci_dev_.dev.irq_dispatcher = irq_loop_->dispatcher();
    pci_dev_.dev.rcu_manager = static_cast<struct rcu_manager*>(rcu_manager_.get());
    pci_dev_.dev.inspector = static_cast<struct driver_inspector*>(driver_inspector_.get());

    if ((status = iwl_pci_connect_fragment_protocol(parent(), "pci", &pci_dev_.fidl)) != ZX_OK) {
      return status;
    }

    // Perform Fuchsia-specific PCI initialization.
    if ((status = iwl_pci_get_bti(pci_dev_.fidl, /*index*/ 0, &pci_dev_.dev.bti)) != ZX_OK) {
      IWL_ERR(nullptr, "Failed to get PCI BTI: %s\n", zx_status_get_string(status));
      return status;
    }
    pci_device_info_t pci_info = {};
    if ((status = iwl_pci_get_device_info(pci_dev_.fidl, &pci_info)) != ZX_OK) {
      return status;
    }
    uint16_t subsystem_device_id = 0;
    if ((status = iwl_pci_read_config16(pci_dev_.fidl, PCI_CONFIG_SUBSYSTEM_ID,
                                        &subsystem_device_id)) != ZX_OK) {
      IWL_ERR(nullptr, "Failed to read PCI subsystem device ID: %s\n",
              zx_status_get_string(status));
      return status;
    }

    IWL_INFO(nullptr, "Device ID: %04x Subsystem Device ID: %04x\n", pci_info.device_id,
             subsystem_device_id);

    if ((status = iwl_drv_init()) != ZX_OK) {
      IWL_ERR(nullptr, "Failed to init driver: %s\n", zx_status_get_string(status));
      return status;
    }

    const iwl_pci_device_id* id = nullptr;
    if ((status = iwl_pci_find_device_id(pci_info.device_id, subsystem_device_id, &id)) != ZX_OK) {
      IWL_ERR(nullptr, "Failed to find PCI config: %s  device_id=0x%04x  subsys_did=0x%04x\n",
              zx_status_get_string(status), pci_info.device_id, subsystem_device_id);
      return status;
    }

    if ((status = iwl_pci_probe(&pci_dev_, id)) != ZX_OK) {
      IWL_ERR(nullptr, "Failed to probe PCI device: %s\n", zx_status_get_string(status));
      return status;
    }

    return ZX_OK;
  }();

  txn.Reply(status);
  return;
}

void PcieDevice::DdkUnbind(::ddk::UnbindTxn txn) {
  unbind_txn_ = std::move(txn);
  iwl_pci_remove(&pci_dev_);
  zx_handle_close(pci_dev_.dev.bti);
  pci_dev_.dev.bti = ZX_HANDLE_INVALID;
  irq_loop_->Shutdown();
  task_loop_->Shutdown();
  server_dispatcher_.ShutdownAsync();
}

}  // namespace iwlwifi
}  // namespace wlan
