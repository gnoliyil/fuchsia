// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDIO_FUNCTION_DEVICE_H_
#define SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDIO_FUNCTION_DEVICE_H_

#include <fidl/fuchsia.hardware.sdio/cpp/wire.h>
#include <fuchsia/hardware/sdio/cpp/banjo.h>
#include <lib/driver/compat/cpp/compat.h>
#include <lib/driver/component/cpp/driver_base.h>

#include <atomic>
#include <memory>

namespace sdmmc {

using fuchsia_hardware_sdio::wire::SdioRwTxn;

class SdioControllerDevice;

class SdioFunctionDevice : public fidl::WireServer<fuchsia_hardware_sdio::Device>,
                           public ddk::SdioProtocol<SdioFunctionDevice> {
 public:
  SdioFunctionDevice(SdioControllerDevice* sdio_parent, uint32_t func);

  static zx_status_t Create(SdioControllerDevice* sdio_parent, uint32_t func,
                            std::unique_ptr<SdioFunctionDevice>* out_dev);

  zx_status_t AddDevice(const sdio_func_hw_info_t& hw_info);

  zx_status_t SdioGetDevHwInfo(sdio_hw_info_t* out_hw_info);
  zx_status_t SdioEnableFn();
  zx_status_t SdioDisableFn();
  zx_status_t SdioEnableFnIntr();
  zx_status_t SdioDisableFnIntr();
  zx_status_t SdioUpdateBlockSize(uint16_t blk_sz, bool deflt);
  zx_status_t SdioGetBlockSize(uint16_t* out_cur_blk_size);
  zx_status_t SdioDoRwByte(bool write, uint32_t addr, uint8_t write_byte, uint8_t* out_read_byte);
  zx_status_t SdioGetInBandIntr(zx::interrupt* out_irq);
  void SdioAckInBandIntr();
  zx_status_t SdioIoAbort();
  zx_status_t SdioIntrPending(bool* out_pending);
  zx_status_t SdioDoVendorControlRwByte(bool write, uint8_t addr, uint8_t write_byte,
                                        uint8_t* out_read_byte);
  zx_status_t SdioRegisterVmo(uint32_t vmo_id, zx::vmo vmo, uint64_t offset, uint64_t size,
                              uint32_t vmo_rights);
  zx_status_t SdioUnregisterVmo(uint32_t vmo_id, zx::vmo* out_vmo);
  zx_status_t SdioDoRwTxn(const sdio_rw_txn_t* txn);
  zx_status_t SdioRequestCardReset();
  zx_status_t SdioPerformTuning();

  // FIDL methods
  void GetDevHwInfo(GetDevHwInfoCompleter::Sync& completer) override;
  void EnableFn(EnableFnCompleter::Sync& completer) override;
  void DisableFn(DisableFnCompleter::Sync& completer) override;
  void EnableFnIntr(EnableFnIntrCompleter::Sync& completer) override;
  void DisableFnIntr(DisableFnIntrCompleter::Sync& completer) override;
  void UpdateBlockSize(UpdateBlockSizeRequestView request,
                       UpdateBlockSizeCompleter::Sync& completer) override;
  void GetBlockSize(GetBlockSizeCompleter::Sync& completer) override;
  void DoRwTxn(DoRwTxnRequestView request, DoRwTxnCompleter::Sync& completer) override;
  void DoRwByte(DoRwByteRequestView request, DoRwByteCompleter::Sync& completer) override;
  void GetInBandIntr(GetInBandIntrCompleter::Sync& completer) override;
  void IoAbort(IoAbortCompleter::Sync& completer) override;
  void IntrPending(IntrPendingCompleter::Sync& completer) override;
  void DoVendorControlRwByte(DoVendorControlRwByteRequestView request,
                             DoVendorControlRwByteCompleter::Sync& completer) override;
  void AckInBandIntr(AckInBandIntrCompleter::Sync& completer) override;
  void RegisterVmo(RegisterVmoRequestView request, RegisterVmoCompleter::Sync& completer) override;
  void UnregisterVmo(UnregisterVmoRequestView request,
                     UnregisterVmoCompleter::Sync& completer) override;
  void RequestCardReset(RequestCardResetCompleter::Sync& completer) override;
  void PerformTuning(PerformTuningCompleter::Sync& completer) override;

  fdf::Logger& logger();

 private:
  void Serve(fidl::ServerEnd<fuchsia_hardware_sdio::Device> request);

  uint8_t function_ = SDIO_MAX_FUNCS;
  SdioControllerDevice* const sdio_parent_;

  std::string sdio_function_name_;
  fidl::WireSyncClient<fuchsia_driver_framework::NodeController> controller_;

  compat::BanjoServer sdio_server_{ZX_PROTOCOL_SDIO, this, &sdio_protocol_ops_};
  std::optional<compat::DeviceServer> compat_server_;
};

}  // namespace sdmmc

#endif  // SRC_DEVICES_BLOCK_DRIVERS_SDMMC_SDIO_FUNCTION_DEVICE_H_
