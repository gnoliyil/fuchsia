// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/block/drivers/zxcrypt/device-manager.h"

#include <fidl/fuchsia.hardware.block.encrypted/cpp/wire.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <threads.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <memory>

#include <ddktl/device.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/macros.h>

#include "src/devices/block/drivers/zxcrypt/device-info.h"
#include "src/devices/block/drivers/zxcrypt/device.h"
#include "src/security/lib/fcrypto/secret.h"
#include "src/security/lib/zxcrypt/ddk-volume.h"
#include "src/security/lib/zxcrypt/volume.h"

namespace zxcrypt {

zx_status_t DeviceManager::Create(void* ctx, zx_device_t* parent) {
  zx_status_t rc;
  fbl::AllocChecker ac;

  auto manager = fbl::make_unique_checked<DeviceManager>(&ac, parent);
  if (!ac.check()) {
    zxlogf(ERROR, "failed to allocate %zu bytes", sizeof(DeviceManager));
    return ZX_ERR_NO_MEMORY;
  }

  if ((rc = manager->Bind()) != ZX_OK) {
    zxlogf(ERROR, "failed to bind: %s", zx_status_get_string(rc));
    return rc;
  }

  // devmgr is now in charge of the memory for |manager|.
  [[maybe_unused]] auto* owned_by_devmgr_now = manager.release();

  return ZX_OK;
}

zx_status_t DeviceManager::Bind() {
  zx_status_t rc;
  fbl::AutoLock lock(&mtx_);

  if ((rc = DdkAdd(ddk::DeviceAddArgs("zxcrypt")
                       .set_flags(DEVICE_ADD_NON_BINDABLE)
                       .set_proto_id(ZX_PROTOCOL_ZXCRYPT)
                       .set_inspect_vmo(inspect_.DuplicateVmo()))) != ZX_OK) {
    zxlogf(ERROR, "failed to add device: %s", zx_status_get_string(rc));
    TransitionState(kRemoved);
    return rc;
  }

  TransitionState(kSealed);
  return ZX_OK;
}

void DeviceManager::DdkUnbind(ddk::UnbindTxn txn) {
  fbl::AutoLock lock(&mtx_);
  switch (state_) {
    case kSealed:
    case kUnsealed:
    case kUnsealedShredded:
    case kSealing:
      break;
    case kBinding:
    case kRemoved:
      ZX_PANIC("unexpected DdkUnbind, state=%d", state_);
  }
  TransitionState(kRemoved);
  txn.Reply();
}

void DeviceManager::DdkRelease() { delete this; }

void DeviceManager::DdkChildPreRelease(void* child_ctx) {
  fbl::AutoLock lock(&mtx_);
  switch (state_) {
    case kSealing:
      // We initiated the removal of our child.
      ZX_ASSERT(child_.has_value());
      ZX_ASSERT(child_ctx == child_.value());
      ZX_ASSERT(seal_completer_.has_value());
      seal_completer_.value().Reply(ZX_OK);
      TransitionState(kSealed);
      break;
    case kBinding:
      ZX_PANIC("impossible to release a child before a child is bound");
      break;
    case kSealed:
      ZX_PANIC("unexpected DdkChildPreRelease while sealed");
      break;
    case kUnsealed:
    case kUnsealedShredded:
      // These can be triggered if some other program explicitly unbound our
      // child out from under us.  It's not expected, but let's handle it
      // cleanly anyway and return the device to the Sealed state.
      zxlogf(ERROR, "unexpected DdkChildPreRelease, state=%d", state_);
      TransitionState(kSealed);
      break;
    case kRemoved:
      // driver_manager initiated the removal of our child.  This may occur when
      // driver_manager is tearing down if Seal() was never called.
      break;
  }
}

void DeviceManager::Format(FormatRequestView request, FormatCompleter::Sync& completer) {
  fbl::AutoLock lock(&mtx_);
  if (state_ != kSealed) {
    zxlogf(ERROR, "can't format zxcrypt, state=%d", state_);
    completer.Reply(ZX_ERR_BAD_STATE);
    return;
  }
  completer.Reply(FormatLocked(request->key.data(), request->key.count(), request->slot));
}

void DeviceManager::Unseal(UnsealRequestView request, UnsealCompleter::Sync& completer) {
  fbl::AutoLock lock(&mtx_);
  if (state_ != kSealed) {
    zxlogf(ERROR, "can't unseal zxcrypt, state=%d", state_);
    completer.Reply(ZX_ERR_BAD_STATE);
    return;
  }
  completer.Reply(UnsealLocked(request->key.data(), request->key.count(), request->slot));
}

void DeviceManager::Seal(SealCompleter::Sync& completer) {
  fbl::AutoLock lock(&mtx_);

  if (state_ != kUnsealed && state_ != kUnsealedShredded) {
    zxlogf(ERROR, "can't seal zxcrypt, state=%d", state_);
    completer.Reply(ZX_ERR_BAD_STATE);
    return;
  }

  // Stash the completer somewhere so we can signal it when device manager confirms removal
  // of the child.
  TransitionState(kSealing, child_, completer.ToAsync());
  child_.value()->DdkAsyncRemove();
}

void DeviceManager::Shred(ShredCompleter::Sync& completer) {
  fbl::AutoLock lock(&mtx_);

  if (state_ != kSealed && state_ != kUnsealed && state_ != kUnsealedShredded) {
    zxlogf(ERROR, "can't shred zxcrypt, state=%d", state_);
    completer.Reply(ZX_ERR_BAD_STATE);
    return;
  }

  // We want to shred the underlying volume, but if we have an unsealed device,
  // we don't mind letting it keep working for now.  Other parts of the system
  // would rather we shut down gracefully than immediately stop permitting reads
  // or acking writes.  So we instantiate a new DdkVolume here, quietly shred
  // it, and let child devices carry on as if nothing happened.
  std::unique_ptr<DdkVolume> volume_to_shred;
  zx_status_t rc;
  rc = DdkVolume::OpenOpaque(parent(), &volume_to_shred);
  if (rc != ZX_OK) {
    zxlogf(ERROR, "failed to open volume to shred: %s", zx_status_get_string(rc));
    completer.Reply(rc);
    return;
  }

  rc = volume_to_shred->Shred();
  if (rc != ZX_OK) {
    zxlogf(ERROR, "failed to shred volume: %s", zx_status_get_string(rc));
    completer.Reply(rc);
    return;
  }

  if (state_ == kUnsealed) {
    TransitionState(kUnsealedShredded, child_);
  }
  completer.Reply(ZX_OK);
}

zx_status_t DeviceManager::FormatLocked(const uint8_t* ikm, size_t ikm_len, key_slot_t slot) {
  zx_status_t rc;

  crypto::Secret key;
  uint8_t* buf;
  if ((rc = key.Allocate(ikm_len, &buf)) != ZX_OK) {
    zxlogf(ERROR, "failed to allocate %zu-byte key: %s", ikm_len, zx_status_get_string(rc));
    return rc;
  }
  memcpy(buf, ikm, key.len());
  std::unique_ptr<DdkVolume> volume;
  if ((rc = DdkVolume::OpenOpaque(parent(), &volume)) != ZX_OK) {
    zxlogf(ERROR, "failed to open volume: %s", zx_status_get_string(rc));
    return rc;
  }

  if ((rc = volume->Format(key, slot)) != ZX_OK) {
    zxlogf(ERROR, "failed to format: %s", zx_status_get_string(rc));
    return rc;
  }

  return ZX_OK;
}

zx_status_t DeviceManager::UnsealLocked(const uint8_t* ikm, size_t ikm_len, key_slot_t slot) {
  zx_status_t rc;

  // Unseal the zxcrypt volume.
  crypto::Secret key;
  uint8_t* buf;
  if ((rc = key.Allocate(ikm_len, &buf)) != ZX_OK) {
    zxlogf(ERROR, "failed to allocate %zu-byte key: %s", ikm_len, zx_status_get_string(rc));
    return rc;
  }
  memcpy(buf, ikm, key.len());
  std::unique_ptr<DdkVolume> volume;
  if ((rc = DdkVolume::Unlock(parent(), key, slot, &volume)) != ZX_OK) {
    zxlogf(ERROR, "failed to unseal volume: %s", zx_status_get_string(rc));
    return rc;
  }

  // Get the parent device's configuration details.
  DeviceInfo info(parent(), *volume);
  if (!info.IsValid()) {
    zxlogf(ERROR, "failed to get valid device info");
    return ZX_ERR_BAD_STATE;
  }
  // Reserve space for shadow I/O transactions
  if ((rc = info.Reserve(Volume::kBufferSize)) != ZX_OK) {
    zxlogf(ERROR, "failed to reserve buffer for I/O: %s", zx_status_get_string(rc));
    return rc;
  }

  inspect::Node inspect = inspect_.GetRoot().CreateChild(inspect_.GetRoot().UniqueName("zxcrypt"));

  // Create the unsealed device
  fbl::AllocChecker ac;
  auto device =
      fbl::make_unique_checked<zxcrypt::Device>(&ac, zxdev(), std::move(info), std::move(inspect));
  if (!ac.check()) {
    zxlogf(ERROR, "failed to allocate %zu bytes", sizeof(zxcrypt::Device));
    return ZX_ERR_NO_MEMORY;
  }
  if ((rc = device->Init(*volume)) != ZX_OK) {
    zxlogf(ERROR, "failed to initialize device: %s", zx_status_get_string(rc));
    return rc;
  }
  if ((rc = device->DdkAdd("unsealed")) != ZX_OK) {
    zxlogf(ERROR, "failed to add device: %s", zx_status_get_string(rc));
    return rc;
  }

  // device_manager is now in charge of the memory for |device|.
  TransitionState(kUnsealed, device.release());
  return ZX_OK;
}

void DeviceManager::TransitionState(State state, std::optional<zxcrypt::Device*> child,
                                    std::optional<SealCompleter::Async> seal_completer) {
  ZX_ASSERT_MSG(state_ != kRemoved, "can't transition out of kRemoved: state=%d", state);
  switch (state) {
    case kBinding:
      ZX_PANIC("can't transition to kBinding");
      break;
    case kSealed:
    case kRemoved:
      ZX_ASSERT(!child.has_value());
      ZX_ASSERT(!seal_completer.has_value());
      break;
    case kUnsealed:
    case kUnsealedShredded:
      ZX_ASSERT(child.has_value());
      ZX_ASSERT(!seal_completer.has_value());
      break;
    case kSealing:
      ZX_ASSERT(child.has_value());
      ZX_ASSERT(seal_completer.has_value());
      break;
  }
  state_ = state;
  child_ = std::move(child);
  seal_completer_ = std::move(seal_completer);
}

void DeviceManager::TransitionState(State state, std::optional<zxcrypt::Device*> child) {
  TransitionState(state, child, std::nullopt);
}

void DeviceManager::TransitionState(State state) {
  TransitionState(state, std::nullopt, std::nullopt);
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = DeviceManager::Create;
  return ops;
}();

}  // namespace zxcrypt

ZIRCON_DRIVER(zxcrypt, zxcrypt::driver_ops, "zircon", "0.1");
