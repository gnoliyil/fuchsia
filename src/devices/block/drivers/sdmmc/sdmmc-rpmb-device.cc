// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sdmmc-rpmb-device.h"

#include <lib/fdf/dispatcher.h>

#include "sdmmc-block-device.h"
#include "sdmmc-types.h"

namespace sdmmc {

zx_status_t RpmbDevice::Create(zx_device_t* parent, SdmmcBlockDevice* sdmmc,
                               const std::array<uint8_t, SDMMC_CID_SIZE>& cid,
                               const std::array<uint8_t, MMC_EXT_CSD_SIZE>& ext_csd) {
  auto device = std::make_unique<RpmbDevice>(parent, sdmmc, cid, ext_csd);

  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.status_value();
  }

  std::array offers = {
      fuchsia_hardware_rpmb::Service::Name,
  };

  device->outgoing_server_end_ = std::move(endpoints->server);
  auto status = device->DdkAdd(ddk::DeviceAddArgs("rpmb")
                                   .set_flags(DEVICE_ADD_MUST_ISOLATE)
                                   .set_fidl_service_offers(offers)
                                   .set_outgoing_dir(endpoints->client.TakeChannel()));
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to add RPMB partition device: %d", status);
    return status;
  }

  [[maybe_unused]] auto* placeholder1 = device.release();
  return ZX_OK;
}

void RpmbDevice::DdkInit(ddk::InitTxn txn) {
  fdf_dispatcher_t* fdf_dispatcher = fdf_dispatcher_get_current_dispatcher();
  ZX_ASSERT(fdf_dispatcher);
  async_dispatcher_t* dispatcher = fdf_dispatcher_get_async_dispatcher(fdf_dispatcher);
  outgoing_ = component::OutgoingDirectory(dispatcher);

  fuchsia_hardware_rpmb::Service::InstanceHandler handler({
      .device = bindings_.CreateHandler(this, dispatcher, fidl::kIgnoreBindingClosure),
  });
  auto result = outgoing_->AddService<fuchsia_hardware_rpmb::Service>(std::move(handler));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to add service to the outgoing directory");
    txn.Reply(result.status_value());
    return;
  }

  result = outgoing_->Serve(std::move(outgoing_server_end_));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to serve the outgoing directory");
    txn.Reply(result.status_value());
    return;
  }

  txn.Reply(ZX_OK);
}

void RpmbDevice::DdkRelease() { delete this; }

void RpmbDevice::DdkUnbind(ddk::UnbindTxn txn) {
  auto result = outgoing_->RemoveService<fuchsia_hardware_rpmb::Service>();
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to remove service from the outgoing directory");
  }
  txn.Reply();
}

void RpmbDevice::GetDeviceInfo(GetDeviceInfoCompleter::Sync& completer) {
  using DeviceInfo = fuchsia_hardware_rpmb::wire::DeviceInfo;
  using EmmcDeviceInfo = fuchsia_hardware_rpmb::wire::EmmcDeviceInfo;

  EmmcDeviceInfo emmc_info = {};
  memcpy(emmc_info.cid.data(), cid_.data(), cid_.size() * sizeof(cid_[0]));
  emmc_info.rpmb_size = rpmb_size_;
  emmc_info.reliable_write_sector_count = reliable_write_sector_count_;

  auto emmc_info_ptr = fidl::ObjectView<EmmcDeviceInfo>::FromExternal(&emmc_info);

  completer.ToAsync().Reply(DeviceInfo::WithEmmcInfo(emmc_info_ptr));
}

void RpmbDevice::Request(RequestRequestView request, RequestCompleter::Sync& completer) {
  RpmbRequestInfo info = {
      .tx_frames = std::move(request->request.tx_frames),
      .completer = completer.ToAsync(),
  };

  if (request->request.rx_frames) {
    info.rx_frames = {
        .vmo = std::move(request->request.rx_frames->vmo),
        .offset = request->request.rx_frames->offset,
        .size = request->request.rx_frames->size,
    };
  }

  sdmmc_parent_->RpmbQueue(std::move(info));
}

}  // namespace sdmmc
