// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <lib/zx/vmo.h>
#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <future>

#include <fbl/string_printf.h>

#include "firmware_loader.h"
#include "logging.h"

namespace btintel {

Device::Device(zx_device_t* device, bt_hci_protocol_t* hci, bool secure)
    : DeviceType(device), hci_(hci), secure_(secure), firmware_loaded_(false) {}

zx_status_t Device::Bind() { return DdkAdd("bt_hci_intel"); }

void Device::DdkInit(ddk::InitTxn init_txn) {
  init_thread_ = std::thread(
      [this, txn = std::move(init_txn)]() mutable { LoadFirmware(std::move(txn), secure_); });
}

zx_status_t Device::LoadFirmware(ddk::InitTxn init_txn, bool secure) {
  infof("LoadFirmware(secure: %s)", (secure ? "yes" : "no"));

  // TODO(armansito): Track metrics for initialization failures.

  zx_status_t status;
  zx::channel our_cmd, their_cmd, our_acl, their_acl;
  status = zx::channel::create(0, &our_cmd, &their_cmd);
  if (status != ZX_OK) {
    return InitFailed(std::move(init_txn), status, "failed to create command channel");
  }
  status = zx::channel::create(0, &our_acl, &their_acl);
  if (status != ZX_OK) {
    return InitFailed(std::move(init_txn), status, "failed to create ACL channel");
  }

  // Get the channels
  status = BtHciOpenCommandChannel(std::move(their_cmd));
  if (status != ZX_OK) {
    return InitFailed(std::move(init_txn), status, "failed to bind command channel");
  }

  status = BtHciOpenAclDataChannel(std::move(their_acl));
  if (status != ZX_OK) {
    return InitFailed(std::move(init_txn), status, "failed to bind ACL channel");
  }

  if (secure) {
    status = LoadSecureFirmware(&our_cmd, &our_acl);
  } else {
    status = LoadLegacyFirmware(&our_cmd, &our_acl);
  }

  if (status != ZX_OK) {
    return InitFailed(std::move(init_txn), status, "failed to initialize controller");
  }

  firmware_loaded_ = true;
  init_txn.Reply(ZX_OK);  // This will make the device visible and able to be unbound.
  return ZX_OK;
}

zx_status_t Device::InitFailed(ddk::InitTxn init_txn, zx_status_t status, const char* note) {
  errorf("%s: %s", note, zx_status_get_string(status));
  init_txn.Reply(status);
  return status;
}

zx_handle_t Device::MapFirmware(const char* name, uintptr_t* fw_addr, size_t* fw_size) {
  zx_handle_t vmo = ZX_HANDLE_INVALID;
  size_t size;
  zx_status_t status = load_firmware(zxdev(), name, &vmo, &size);
  if (status != ZX_OK) {
    errorf("failed to load firmware '%s': %s", name, zx_status_get_string(status));
    return ZX_HANDLE_INVALID;
  }
  status = zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ, 0, vmo, 0, size, fw_addr);
  if (status != ZX_OK) {
    errorf("firmware map failed: %s", zx_status_get_string(status));
    return ZX_HANDLE_INVALID;
  }
  *fw_size = size;
  return vmo;
}

void Device::DdkUnbind(ddk::UnbindTxn txn) {
  tracef("unbind");
  txn.Reply();
}

void Device::DdkRelease() {
  if (init_thread_.joinable()) {
    init_thread_.join();
  }
  delete this;
}

zx_status_t Device::DdkGetProtocol(uint32_t proto_id, void* out_proto) {
  if (proto_id != ZX_PROTOCOL_BT_HCI) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  bt_hci_protocol_t* hci_proto = static_cast<bt_hci_protocol_t*>(out_proto);

  // Forward the underlying bt-transport ops.
  hci_.GetProto(hci_proto);

  return ZX_OK;
}

void Device::OpenCommandChannel(OpenCommandChannelRequestView request,
                                OpenCommandChannelCompleter::Sync& completer) {
  BtHciOpenCommandChannel(std::move(request->channel));
}
void Device::OpenAclDataChannel(OpenAclDataChannelRequestView request,
                                OpenAclDataChannelCompleter::Sync& completer) {
  BtHciOpenAclDataChannel(std::move(request->channel));
}
void Device::OpenSnoopChannel(OpenSnoopChannelRequestView request,
                              OpenSnoopChannelCompleter::Sync& completer) {
  BtHciOpenSnoopChannel(std::move(request->channel));
}

zx_status_t Device::LoadSecureFirmware(zx::channel* cmd, zx::channel* acl) {
  ZX_DEBUG_ASSERT(cmd);
  ZX_DEBUG_ASSERT(acl);

  VendorHci hci(cmd);

  // Bring the controller to a well-defined default state.
  // Send an initial reset. If the controller sends a "command not supported"
  // event on newer models, this likely means that the controller is in
  // bootloader mode and we can ignore the error.
  auto hci_status = hci.SendHciReset();
  if (hci_status == pw::bluetooth::emboss::StatusCode::UNKNOWN_COMMAND) {
    infof("Ignoring \"Unknown Command\" error while in bootloader mode");
  } else if (hci_status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    errorf("HCI_Reset failed (status: 0x%02hhx)", static_cast<unsigned char>(hci_status));
    return ZX_ERR_BAD_STATE;
  }

  // Newer Intel controllers that use the "secure send" method can send HCI
  // events over the bulk endpoint. Enable this before sending the initial
  // ReadVersion command.
  hci.enable_events_on_bulk(acl);

  ReadVersionReturnParams version = hci.SendReadVersion();
  if (version.status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    errorf("failed to obtain version information");
    return ZX_ERR_BAD_STATE;
  }

  // If we're already in firmware mode, we're done.
  if (version.fw_variant == kFirmwareFirmwareVariant) {
    infof("firmware loaded (variant %d, revision %d)", version.hw_variant, version.hw_revision);
    return ZX_OK;
  }

  // If we reached here then the controller must be in bootloader mode.
  if (version.fw_variant != kBootloaderFirmwareVariant) {
    errorf("unsupported firmware variant (0x%x)", version.fw_variant);
    return ZX_ERR_NOT_SUPPORTED;
  }

  ReadBootParamsReturnParams boot_params = hci.SendReadBootParams();
  if (boot_params.status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    errorf("failed to read boot parameters");
    return ZX_ERR_BAD_STATE;
  }

  // Map the firmware file into memory.
  auto fw_filename = fbl::StringPrintf("ibt-%d-%d.sfi", version.hw_variant, boot_params.dev_revid);
  zx::vmo fw_vmo;
  uintptr_t fw_addr;
  size_t fw_size;
  fw_vmo.reset(MapFirmware(fw_filename.c_str(), &fw_addr, &fw_size));
  if (!fw_vmo) {
    errorf("failed to map firmware");
    return ZX_ERR_NOT_SUPPORTED;
  }

  FirmwareLoader loader(cmd, acl);

  // The boot addr differs for different firmware.  Save it for later.
  uint32_t boot_addr = 0x00000000;
  auto status = loader.LoadSfi(reinterpret_cast<void*>(fw_addr), fw_size, &boot_addr);
  zx_vmar_unmap(zx_vmar_root_self(), fw_addr, fw_size);
  if (status == FirmwareLoader::LoadStatus::kError) {
    errorf("failed to load SFI firmware");
    return ZX_ERR_BAD_STATE;
  }

  // Switch the controller into firmware mode.
  hci.SendVendorReset(boot_addr);

  infof("firmware loaded using %s", fw_filename.c_str());
  return ZX_OK;
}

zx_status_t Device::LoadLegacyFirmware(zx::channel* cmd, zx::channel* acl) {
  ZX_DEBUG_ASSERT(cmd);
  ZX_DEBUG_ASSERT(acl);

  VendorHci hci(cmd);

  // Bring the controller to a well-defined default state.
  auto hci_status = hci.SendHciReset();
  if (hci_status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    errorf("HCI_Reset failed (status: 0x%02hhx)", static_cast<unsigned char>(hci_status));
    return ZX_ERR_BAD_STATE;
  }

  ReadVersionReturnParams version = hci.SendReadVersion();
  if (version.status != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    errorf("failed to obtain version information");
    return ZX_ERR_BAD_STATE;
  }

  if (version.fw_patch_num > 0) {
    infof("controller already patched");
    return ZX_OK;
  }

  auto fw_filename = fbl::StringPrintf(
      "ibt-hw-%x.%x.%x-fw-%x.%x.%x.%x.%x.bseq", version.hw_platform, version.hw_variant,
      version.hw_revision, version.fw_variant, version.fw_revision, version.fw_build_num,
      version.fw_build_week, version.fw_build_year);

  zx::vmo fw_vmo;
  uintptr_t fw_addr;
  size_t fw_size;
  fw_vmo.reset(MapFirmware(fw_filename.c_str(), &fw_addr, &fw_size));

  // Try the fallback patch file on initial failure.
  if (!fw_vmo) {
    // Try the fallback patch file
    fw_filename = fbl::StringPrintf("ibt-hw-%x.%x.bseq", version.hw_platform, version.hw_variant);
    fw_vmo.reset(MapFirmware(fw_filename.c_str(), &fw_addr, &fw_size));
  }

  // Abort if the fallback file failed to load too.
  if (!fw_vmo) {
    errorf("failed to map firmware");
    return ZX_ERR_NOT_SUPPORTED;
  }

  FirmwareLoader loader(cmd, acl);
  hci.EnterManufacturerMode();
  auto result = loader.LoadBseq(reinterpret_cast<void*>(fw_addr), fw_size);
  hci.ExitManufacturerMode(result == FirmwareLoader::LoadStatus::kPatched
                               ? MfgDisableMode::kPatchesEnabled
                               : MfgDisableMode::kNoPatches);
  zx_vmar_unmap(zx_vmar_root_self(), fw_addr, fw_size);

  if (result == FirmwareLoader::LoadStatus::kError) {
    errorf("failed to patch controller");
    return ZX_ERR_BAD_STATE;
  }

  infof("controller patched using %s", fw_filename.c_str());
  return ZX_OK;
}

zx_status_t Device::BtHciOpenCommandChannel(zx::channel in) {
  return hci_.OpenCommandChannel(std::move(in));
}

zx_status_t Device::BtHciOpenAclDataChannel(zx::channel in) {
  return hci_.OpenAclDataChannel(std::move(in));
}

zx_status_t Device::BtHciOpenScoChannel(zx::channel in) {
  return hci_.OpenScoChannel(std::move(in));
}

void Device::BtHciConfigureSco(sco_coding_format_t coding_format, sco_encoding_t encoding,
                               sco_sample_rate_t sample_rate,
                               bt_hci_configure_sco_callback callback, void* cookie) {
  hci_.ConfigureSco(coding_format, encoding, sample_rate, callback, cookie);
}

void Device::BtHciResetSco(bt_hci_reset_sco_callback callback, void* cookie) {
  hci_.ResetSco(callback, cookie);
}

zx_status_t Device::BtHciOpenSnoopChannel(zx::channel in) {
  return hci_.OpenSnoopChannel(std::move(in));
}

}  // namespace btintel
