// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_MARVELL_BT_HCI_MARVELL_H_
#define SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_MARVELL_BT_HCI_MARVELL_H_

#include <fuchsia/hardware/bt/hci/cpp/banjo.h>
#include <fuchsia/hardware/sdio/cpp/banjo.h>

#include <ddktl/device.h>

namespace bt_hci_marvell {

class BtHciMarvell;
using BtHciMarvellType =
    ddk::Device<BtHciMarvell, ddk::GetProtocolable, ddk::Initializable, ddk::Unbindable>;

class BtHciMarvell : public BtHciMarvellType, public ddk::BtHciProtocol<BtHciMarvell> {
 public:
  BtHciMarvell(zx_device_t* parent, const ddk::SdioProtocolClient& sdio)
      : BtHciMarvellType(parent), sdio_(sdio) {}
  virtual ~BtHciMarvell() = default;

  // Allocate a new instance of the driver and register with the driver manager
  static zx_status_t Bind(void* ctx, zx_device_t* parent);

  // ddk::Device
  void DdkInit(ddk::InitTxn txn);
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out_proto);

  // ddk::BtHciProtocol
  zx_status_t BtHciOpenCommandChannel(zx::channel channel);
  zx_status_t BtHciOpenAclDataChannel(zx::channel channel);
  zx_status_t BtHciOpenScoChannel(zx::channel channel);
  zx_status_t BtHciOpenSnoopChannel(zx::channel channel);
  void BtHciConfigureSco(sco_coding_format_t coding_format, sco_encoding_t encoding,
                         sco_sample_rate_t sample_rate, bt_hci_configure_sco_callback callback,
                         void* cookie);
  void BtHciResetSco(bt_hci_reset_sco_callback callback, void* cookie);

 private:
  const ddk::SdioProtocolClient sdio_;
};

}  // namespace bt_hci_marvell

#endif  // SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_MARVELL_BT_HCI_MARVELL_H_
