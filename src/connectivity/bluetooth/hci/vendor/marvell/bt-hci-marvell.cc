// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/hci/vendor/marvell/bt-hci-marvell.h"

#include <lib/ddk/driver.h>

#include <fbl/alloc_checker.h>

#include "src/connectivity/bluetooth/hci/vendor/marvell/bt-hci-marvell-bind.h"

namespace bt_hci_marvell {

zx_status_t BtHciMarvell::Bind(void* ctx, zx_device_t* parent) {
  zx_status_t status;
  ddk::SdioProtocolClient sdio(parent);

  if (!sdio.is_valid()) {
    zxlogf(ERROR, "Failed to get SDIO protocol");
    return ZX_ERR_NO_RESOURCES;
  }

  // Allocate our driver instance
  fbl::AllocChecker ac;
  std::unique_ptr<BtHciMarvell> device(new (&ac) BtHciMarvell(parent, sdio));
  if (!ac.check()) {
    zxlogf(ERROR, "BtHciMarvell alloc failed");
    return ZX_ERR_NO_MEMORY;
  }

  // Add our device to the device tree
  ddk::DeviceAddArgs args("bt-hci-marvell");
  args.set_proto_id(ZX_PROTOCOL_BT_HCI);
  if ((status = device->DdkAdd(args)) != ZX_OK) {
    zxlogf(ERROR, "DdkAdd failed: %s", zx_status_get_string(status));
    return status;
  }

  // Driver Manager now owns the device - memory will be explicitly freed in DdkRelease()
  device.release();

  return ZX_OK;
}

void BtHciMarvell::DdkInit(ddk::InitTxn txn) { txn.Reply(ZX_OK); }

void BtHciMarvell::DdkUnbind(ddk::UnbindTxn txn) { txn.Reply(); }

void BtHciMarvell::DdkRelease() { delete this; }

zx_status_t BtHciMarvell::DdkGetProtocol(uint32_t proto_id, void* out_proto) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t BtHciMarvell::BtHciOpenCommandChannel(zx::channel channel) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t BtHciMarvell::BtHciOpenAclDataChannel(zx::channel channel) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t BtHciMarvell::BtHciOpenScoChannel(zx::channel channel) { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t BtHciMarvell::BtHciOpenSnoopChannel(zx::channel channel) {
  return ZX_ERR_NOT_SUPPORTED;
}

void BtHciMarvell::BtHciConfigureSco(sco_coding_format_t coding_format, sco_encoding_t encoding,
                                     sco_sample_rate_t sample_rate,
                                     bt_hci_configure_sco_callback callback, void* cookie) {}

void BtHciMarvell::BtHciResetSco(bt_hci_reset_sco_callback callback, void* cookie) {}

}  // namespace bt_hci_marvell

static constexpr zx_driver_ops_t bt_hci_marvell_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = bt_hci_marvell::BtHciMarvell::Bind;
  return ops;
}();

ZIRCON_DRIVER(bt_hci_marvell, bt_hci_marvell_driver_ops, "zircon", "0.1");
