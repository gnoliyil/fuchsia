// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef ZIRCON_BOOT_CUSTOM_SYSDEPS_HEADER
#include <zircon_boot_sysdeps.h>
#else
#include <assert.h>
#include <stddef.h>
#include <string.h>
#endif

#include <lib/zbi/zbi.h>
#include <lib/zircon_boot/zircon_boot.h>
#include <zircon/hw/gpt.h>

#include "utils.h"
#include "zircon_vboot.h"

const char* GetSlotPartitionName(AbrSlotIndex slot) {
  if (slot == kAbrSlotIndexA) {
    return GPT_ZIRCON_A_NAME;
  } else if (slot == kAbrSlotIndexB) {
    return GPT_ZIRCON_B_NAME;
  } else if (slot == kAbrSlotIndexR) {
    return GPT_ZIRCON_R_NAME;
  }
  return NULL;
}

static bool ReadAbrMetaData(void* context, size_t size, uint8_t* buffer) {
  ZirconBootOps* ops = (ZirconBootOps*)context;
  size_t read_size;
  return ZIRCON_BOOT_OPS_CALL(ops, read_from_partition, GPT_DURABLE_BOOT_NAME, 0, size, buffer,
                              &read_size) &&
         read_size == size;
}

static bool WriteAbrMetaData(void* context, const uint8_t* buffer, size_t size) {
  ZirconBootOps* ops = (ZirconBootOps*)context;
  size_t write_size;
  return ZIRCON_BOOT_OPS_CALL(ops, write_to_partition, GPT_DURABLE_BOOT_NAME, 0, size, buffer,
                              &write_size) &&
         write_size == size;
}

AbrOps GetAbrOpsFromZirconBootOps(ZirconBootOps* ops) {
  AbrOps abr_ops = {
      .context = ops, .read_abr_metadata = ReadAbrMetaData, .write_abr_metadata = WriteAbrMetaData};
  return abr_ops;
}

static bool IsVerifiedBootOpsImplemented(ZirconBootOps* ops) {
  return ops->verified_boot_get_partition_size && ops->verified_boot_read_rollback_index &&
         ops->verified_boot_write_rollback_index && ops->verified_boot_read_is_device_locked &&
         ops->verified_boot_read_permanent_attributes &&
         ops->verified_boot_read_permanent_attributes_hash;
}

static ZirconBootResult VerifyKernel(ZirconBootOps* zb_ops, void* load_address,
                                     size_t load_address_size, AbrSlotIndex slot) {
  const char* ab_suffix = AbrGetSlotSuffix(slot);
  AbrOps abr_ops = GetAbrOpsFromZirconBootOps(zb_ops);
  AbrSlotInfo slot_info;
  AbrResult res = AbrGetSlotInfo(&abr_ops, slot, &slot_info);
  if (res != kAbrResultOk) {
    zircon_boot_dlog("Failed to get slot info %d\n", res);
    return kBootResultErrorSlotVerification;
  }

  if (!ZirconVBootSlotVerify(zb_ops, load_address, load_address_size, ab_suffix,
                             slot_info.is_marked_successful)) {
    zircon_boot_dlog("Slot verification failed\n");
    return kBootResultErrorSlotVerification;
  }
  return kBootResultOK;
}

static ZirconBootResult LoadKernel(AbrSlotIndex slot, ZirconBootOps* ops, void** load_address,
                                   size_t* load_address_size) {
  const char* zircon_part = GetSlotPartitionName(slot);
  if (zircon_part == NULL) {
    zircon_boot_dlog("Invalid slot idx %d\n", slot);
    return kBootResultErrorInvalidSlotIdx;
  }
  zircon_boot_dlog("ABR: loading kernel from slot %s...\n", zircon_part);

  if (!ops->get_kernel_load_buffer) {
    return kBootResultErrorImageTooLarge;
  }

  zbi_header_t zbi_hdr __attribute__((aligned(ZBI_ALIGNMENT)));
  size_t read_size;
  // This library only deals with zircon image and assume that it always starts from 0 offset.
  if (!ZIRCON_BOOT_OPS_CALL(ops, read_from_partition, zircon_part, 0, sizeof(zbi_hdr), &zbi_hdr,
                            &read_size) ||
      read_size != sizeof(zbi_hdr)) {
    zircon_boot_dlog("Failed to read header from slot\n");
    return kBootResultErrorReadHeader;
  }

  if (zbi_hdr.type != ZBI_TYPE_CONTAINER || zbi_hdr.extra != ZBI_CONTAINER_MAGIC ||
      zbi_hdr.magic != ZBI_ITEM_MAGIC) {
    zircon_boot_dlog("Fail to find ZBI header\n");
    return kBootResultErrorZbiHeaderNotFound;
  }

  size_t image_size = zbi_hdr.length + sizeof(zbi_header_t);
  *load_address_size = image_size;
  *load_address = ops->get_kernel_load_buffer(ops, load_address_size);
  if (*load_address == NULL) {
    *load_address_size = 0;
    zircon_boot_dlog("Cannot get kernel load buffer\n");
    return kBootResultErrorImageTooLarge;
  }

  if (!ZIRCON_BOOT_OPS_CALL(ops, read_from_partition, zircon_part, 0, image_size, *load_address,
                            &read_size) ||
      read_size != image_size) {
    zircon_boot_dlog("Fail to read image from slot\n");
    return kBootResultErrorReadImage;
  }

  if (IsVerifiedBootOpsImplemented(ops)) {
    ZirconBootResult res = VerifyKernel(ops, *load_address, *load_address_size, slot);
    if (res != kBootResultOK) {
      return res;
    }
  }

  zircon_boot_dlog("Successfully loaded slot: %s\n", zircon_part);
  return kBootResultOK;
}

static bool ReadAbrMetaDataCache(void* context, size_t size, uint8_t* buffer) {
  assert(size <= sizeof(AbrData));
  memcpy(buffer, context, size);
  return true;
}

static bool WriteAbrMetaDataCache(void* context, const uint8_t* buffer, size_t size) {
  assert(size <= sizeof(AbrData));
  memcpy(context, buffer, size);
  return true;
}

// TODO(b/258467776): We can't use AbrGetBootSlot(.. update_metadata = false, ..) to get the slot to
// boot because it doesn't consider one shot recovery.
AbrSlotIndex AbrPeekBootSlot(const AbrOps* abr_ops) {
  // Load abr metadata into memory and simulate storage.
  uint8_t cache[sizeof(AbrData)];
  assert(abr_ops->read_abr_metadata);
  if (!abr_ops->read_abr_metadata(abr_ops->context, sizeof(cache), cache)) {
    return kAbrSlotIndexR;
  }

  AbrOps cache_ops;
  memset(&cache_ops, 0, sizeof(AbrOps));
  cache_ops.context = cache;
  cache_ops.read_abr_metadata = ReadAbrMetaDataCache;
  cache_ops.write_abr_metadata = WriteAbrMetaDataCache;
  AbrSlotIndex ret = AbrGetBootSlot(&cache_ops, true, NULL);
  return ret;
}

static ZirconBootResult LoadImage(ZirconBootOps* ops, ForceRecovery force_recovery,
                                  AbrSlotIndex* out_slot, void** load_address,
                                  size_t* load_address_size) {
  ZirconBootResult ret;
  AbrSlotIndex cur_slot;
  AbrOps abr_ops = GetAbrOpsFromZirconBootOps(ops);
  do {
    if (ops->firmware_can_boot_kernel_slot) {
      // Get the target slot about to be booted
      AbrSlotIndex target_slot =
          force_recovery == kForceRecoveryOn ? kAbrSlotIndexR : AbrPeekBootSlot(&abr_ops);
      bool supported = false;
      if (!ZIRCON_BOOT_OPS_CALL(ops, firmware_can_boot_kernel_slot, target_slot, &supported)) {
        zircon_boot_dlog("Fail to check slot supported\n");
        return kBootResultErrorIsSlotSupprotedByFirmware;
      }

      if (!supported) {
        zircon_boot_dlog(
            "Target kernel slot %s is not supported by current firmware. Rebooting...\n",
            AbrGetSlotSuffix(target_slot));
        ZIRCON_BOOT_OPS_CALL(ops, reboot, force_recovery == kForceRecoveryOn);
        zircon_boot_dlog("Should not reach here. Reboot handoff failed\n");
        return kBootResultRebootReturn;
      }
    }

    cur_slot =
        force_recovery == kForceRecoveryOn ? kAbrSlotIndexR : AbrGetBootSlot(&abr_ops, true, NULL);
    ret = LoadKernel(cur_slot, ops, load_address, load_address_size);
    if (ret != kBootResultOK) {
      zircon_boot_dlog("ABR: failed to load slot %d\n", cur_slot);
      if (cur_slot != kAbrSlotIndexR && AbrMarkSlotUnbootable(&abr_ops, cur_slot) != kAbrResultOk) {
        return kBootResultErrorMarkUnbootable;
      }
    }

  } while ((ret != kBootResultOK) && (cur_slot != kAbrSlotIndexR));

  if (ret != kBootResultOK) {
    zircon_boot_dlog("Fail to boot: no valid slots\n");
    return kBootResultErrorNoValidSlot;
  }
  *out_slot = cur_slot;
  return kBootResultOK;
}

ZirconBootResult LoadAndBoot(ZirconBootOps* ops, ForceRecovery force_recovery) {
  void* load_address;
  size_t load_address_size;

  AbrSlotIndex slot;
  ZirconBootResult res = LoadImage(ops, force_recovery, &slot, &load_address, &load_address_size);
  if (res != kBootResultOK) {
    return res;
  }

  // Adds device-specific ZBI items provided by device through |add_zbi_items| method
  if (ops->add_zbi_items && !ops->add_zbi_items(ops, load_address, load_address_size, slot)) {
    zircon_boot_dlog("Failed to add ZBI items\n");
    return kBootResultErrorAppendZbiItems;
  }

  ZIRCON_BOOT_OPS_CALL(ops, boot, load_address, load_address_size, slot);
  zircon_boot_dlog("Should not reach here. Boot handoff failed\n");
  return kBootResultBootReturn;
}

AbrSlotIndex GetActiveBootSlot(ZirconBootOps* ops) {
  AbrOps abr_ops = GetAbrOpsFromZirconBootOps(ops);
  return AbrPeekBootSlot(&abr_ops);
}
