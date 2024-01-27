// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/abr/data.h>
#include <lib/stdcompat/span.h>
#include <lib/zbi/zbi.h>
#include <lib/zircon_boot/android_boot_image.h>
#include <lib/zircon_boot/test/mock_zircon_boot_ops.h>
#include <lib/zircon_boot/zircon_boot.h>
#include <zircon/hw/gpt.h>

#include <set>
#include <vector>

#include <zxtest/zxtest.h>

#include "lib/abr/abr.h"
#include "test_data/test_images.h"

namespace {

constexpr bool operator<(const cpp20::span<const char>& lhs,
                         const cpp20::span<const char>& rhs) noexcept {
  auto l = lhs.begin();
  auto r = rhs.begin();
  for (; l != lhs.end() && r != rhs.end(); ++l, ++r) {
    if (*l < *r) {
      return true;
    }
  }

  return (lhs.size() < rhs.size());
}

constexpr bool operator==(const cpp20::span<const char>& lhs,
                          const cpp20::span<const char>& rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  auto l = lhs.begin();
  auto r = rhs.begin();
  for (; l != lhs.end() && r != rhs.end(); ++l, ++r) {
    if (*l != *r) {
      return false;
    }
  }

  return true;
}

struct NormalizedZbiItem {
  uint32_t type;
  uint32_t extra;
  cpp20::span<const char> payload;
};

constexpr bool operator<(const NormalizedZbiItem& lhs, const NormalizedZbiItem& rhs) noexcept {
  if (lhs.type != rhs.type) {
    return lhs.type < rhs.type;
  }

  if (lhs.extra != rhs.extra) {
    return lhs.extra < rhs.extra;
  }

  return lhs.payload < rhs.payload;
}

constexpr bool operator==(const NormalizedZbiItem& lhs, const NormalizedZbiItem& rhs) noexcept {
  return lhs.type == rhs.type && lhs.extra == rhs.extra && lhs.payload == rhs.payload;
}

constexpr size_t kZirconPartitionSize = 128 * 1024;
constexpr size_t kVbmetaPartitionSize = 64 * 1024;

constexpr cpp20::span<const char> kTestCmdline = "foo=bar";

void CreateMockZirconBootOps(std::unique_ptr<MockZirconBootOps>* out) {
  auto device = std::make_unique<MockZirconBootOps>();

  // durable boot
  device->AddPartition(GPT_DURABLE_BOOT_NAME, sizeof(AbrData));

  // zircon partitions
  struct PartitionAndData {
    const char* name;
    const void* data;
    size_t data_len;
  } zircon_partitions[] = {
      {GPT_ZIRCON_A_NAME, kTestZirconAImage, sizeof(kTestZirconAImage)},
      {GPT_ZIRCON_B_NAME, kTestZirconBImage, sizeof(kTestZirconBImage)},
      {GPT_ZIRCON_R_NAME, kTestZirconRImage, sizeof(kTestZirconRImage)},
      {GPT_ZIRCON_SLOTLESS_NAME, kTestZirconSlotlessImage, sizeof(kTestZirconSlotlessImage)},
  };
  for (auto& ele : zircon_partitions) {
    device->AddPartition(ele.name, kZirconPartitionSize);
    ASSERT_OK(device->WriteToPartition(ele.name, 0, ele.data_len, ele.data));
  }

  // vbmeta partitions
  PartitionAndData vbmeta_partitions[] = {
      {GPT_VBMETA_A_NAME, kTestVbmetaAImage, sizeof(kTestVbmetaAImage)},
      {GPT_VBMETA_B_NAME, kTestVbmetaBImage, sizeof(kTestVbmetaBImage)},
      {GPT_VBMETA_R_NAME, kTestVbmetaRImage, sizeof(kTestVbmetaRImage)},
      {GPT_VBMETA_SLOTLESS_NAME, kTestVbmetaSlotlessImage, sizeof(kTestVbmetaSlotlessImage)},
  };
  for (auto& ele : vbmeta_partitions) {
    device->AddPartition(ele.name, kVbmetaPartitionSize);
    ASSERT_OK(device->WriteToPartition(ele.name, 0, ele.data_len, ele.data));
  }

  device->AddPartition(GPT_DURABLE_BOOT_NAME, sizeof(AbrData));

  device->SetAddDeviceZbiItemsMethod([](zbi_header_t* image, size_t capacity,
                                        const AbrSlotIndex* slot) {
    if (slot && AppendCurrentSlotZbiItem(image, capacity, *slot) != ZBI_RESULT_OK) {
      return false;
    }

    if (zbi_create_entry_with_payload(image, capacity, ZBI_TYPE_CMDLINE, 0, 0, kTestCmdline.data(),
                                      kTestCmdline.size()) != ZBI_RESULT_OK) {
      return false;
    }
    return true;
  });

  AvbAtxPermanentAttributes permanent_attributes;
  memcpy(&permanent_attributes, kPermanentAttributes, sizeof(permanent_attributes));
  device->SetPermanentAttributes(permanent_attributes);

  for (size_t i = 0; i < AVB_MAX_NUMBER_OF_ROLLBACK_INDEX_LOCATIONS; i++) {
    device->WriteRollbackIndex(i, 0);
  }
  device->WriteRollbackIndex(AVB_ATX_PIK_VERSION_LOCATION, 0);
  device->WriteRollbackIndex(AVB_ATX_PSK_VERSION_LOCATION, 0);
  // Most tests want a load buffer that fits the partition.
  // The tests that don't set the buffer explicitly.
  device->SetKernelLoadBufferSize(kZirconPartitionSize);

  *out = std::move(device);
}

void MarkSlotActive(MockZirconBootOps* dev, AbrSlotIndex slot) {
  AbrOps abr_ops = dev->GetAbrOps();
  if (slot != kAbrSlotIndexR) {
    AbrMarkSlotActive(&abr_ops, slot);
  } else {
    AbrMarkSlotUnbootable(&abr_ops, kAbrSlotIndexA);
    AbrMarkSlotUnbootable(&abr_ops, kAbrSlotIndexB);
  }
}

zbi_result_t ExtractAndSortZbiItemsCallback(zbi_header_t* hdr, void* payload, void* cookie) {
  std::multiset<NormalizedZbiItem>* out = static_cast<std::multiset<NormalizedZbiItem>*>(cookie);
  if (hdr->type == ZBI_TYPE_KERNEL_ARM64) {
    return ZBI_RESULT_OK;
  }
  out->insert(NormalizedZbiItem{
      hdr->type, hdr->extra, {reinterpret_cast<const char*>(payload), hdr->length}});
  return ZBI_RESULT_OK;
}

void ExtractAndSortZbiItems(const uint8_t* buffer, std::multiset<NormalizedZbiItem>* out) {
  out->clear();
  ASSERT_EQ(zbi_for_each(buffer, ExtractAndSortZbiItemsCallback, out), ZBI_RESULT_OK);
}

void ValidateBootedSlot(const MockZirconBootOps* dev, std::optional<AbrSlotIndex> expected_slot) {
  auto booted_slot = dev->GetBootedSlot();
  ASSERT_EQ(booted_slot, expected_slot);
  // Use multiset so that we can catch bugs such as duplicated append.
  std::multiset<NormalizedZbiItem> zbi_items_added;
  ASSERT_NO_FAILURES(ExtractAndSortZbiItems(dev->GetBootedImage().data(), &zbi_items_added));

  std::multiset<NormalizedZbiItem> zbi_items_expected;
  std::string current_slot;
  if (expected_slot) {
    // Verify that the current slot item is appended for A/B/R boots (plus 1 for null terminator).
    current_slot = "zvb.current_slot=" + std::string(AbrGetSlotSuffix(*expected_slot));
    zbi_items_expected.insert(
        NormalizedZbiItem{ZBI_TYPE_CMDLINE, 0, {current_slot.data(), current_slot.size() + 1}});
  }

  // Verify that the additional cmdline item is appended.
  zbi_items_expected.insert({ZBI_TYPE_CMDLINE, 0, kTestCmdline});

  // Exactly the above items are appended. No more, no less.
  EXPECT_EQ(zbi_items_added, zbi_items_expected);
}

TEST(BootTests, NotEnoughBuffer) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  zircon_boot_ops.firmware_can_boot_kernel_slot = nullptr;
  dev->SetKernelLoadBufferSize(0);
  MarkSlotActive(dev.get(), kAbrSlotIndexA);
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, kZirconBootModeAbr), kBootResultErrorNoValidSlot);
}

// Tests that boot logic for OS ABR works correctly.
// ABR metadata is initialized to mark |initial_active_slot| as the active slot.
// |expected_slot| specifies the resulting booted slot.
void TestOsAbrSuccessfulBoot(AbrSlotIndex initial_active_slot,
                             std::optional<AbrSlotIndex> expected_slot, ZirconBootMode boot_mode) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  zircon_boot_ops.firmware_can_boot_kernel_slot = nullptr;
  MarkSlotActive(dev.get(), initial_active_slot);
  // If we're doing a slotless boot, nothing should touch the A/B/R metadata
  // during LoadAndBoot().
  if (!expected_slot) {
    dev->RemovePartition(GPT_DURABLE_BOOT_NAME);
  }
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, boot_mode), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateBootedSlot(dev.get(), expected_slot));
}

void TestOsAbrSuccessfulBootOneShotRecovery(ZirconBootMode boot_mode) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  zircon_boot_ops.firmware_can_boot_kernel_slot = nullptr;
  AbrOps abr_ops = dev->GetAbrOps();
  AbrSetOneShotRecovery(&abr_ops, true);
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, boot_mode), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateBootedSlot(dev.get(), kAbrSlotIndexR));
}

TEST(BootTests, TestSuccessfulBootOsAbr) {
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexA, kAbrSlotIndexA, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexB, kAbrSlotIndexB, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(TestOsAbrSuccessfulBootOneShotRecovery(kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexA, kAbrSlotIndexR, kZirconBootModeForceRecovery));
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexB, kAbrSlotIndexR, kZirconBootModeForceRecovery));
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeForceRecovery));
  ASSERT_NO_FATAL_FAILURE(TestOsAbrSuccessfulBootOneShotRecovery(kZirconBootModeForceRecovery));
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexA, std::nullopt, kZirconBootModeSlotless));
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexB, std::nullopt, kZirconBootModeSlotless));
  ASSERT_NO_FATAL_FAILURE(
      TestOsAbrSuccessfulBoot(kAbrSlotIndexR, std::nullopt, kZirconBootModeSlotless));
}

TEST(BootTests, SkipAddZbiItems) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  zircon_boot_ops.firmware_can_boot_kernel_slot = nullptr;
  zircon_boot_ops.add_zbi_items = nullptr;
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, kZirconBootModeAbr), kBootResultBootReturn);
  std::multiset<NormalizedZbiItem> zbi_items_added;
  ASSERT_NO_FAILURES(ExtractAndSortZbiItems(dev->GetBootedImage().data(), &zbi_items_added));
  ASSERT_TRUE(zbi_items_added.empty());
}

// Tests that OS ABR booting logic detects ZBI header corruption and falls back to the other slots.
// |corrupt_hdr| is a function that specifies how header should be corrupted.
void TestInvalidZbiHeaderOsAbr(std::function<void(zbi_header_t* hdr)> corrupt_hdr) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  zircon_boot_ops.firmware_can_boot_kernel_slot = nullptr;
  // corrupt header
  zbi_header_t header;
  ASSERT_OK(dev->ReadFromPartition(GPT_ZIRCON_A_NAME, 0, sizeof(header), &header));
  corrupt_hdr(&header);
  ASSERT_OK(dev->WriteToPartition(GPT_ZIRCON_A_NAME, 0, sizeof(header), &header));

  // Boot to the corrupted slot A first.
  AbrOps abr_ops = dev->GetAbrOps();
  AbrMarkSlotActive(&abr_ops, kAbrSlotIndexA);

  // Slot A should fail and fall back to slot B
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, kZirconBootModeAbr), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateBootedSlot(dev.get(), kAbrSlotIndexB));
  // Slot A unbootable
  AbrData abr_data;
  ASSERT_OK(dev->ReadFromPartition(GPT_DURABLE_BOOT_NAME, 0, sizeof(abr_data), &abr_data));
  ASSERT_EQ(abr_data.slot_data[0].tries_remaining, 0);
  ASSERT_EQ(abr_data.slot_data[0].successful_boot, 0);
}

TEST(BootTests, LoadAndBootInvalidZbiHeaderType) {
  ASSERT_NO_FATAL_FAILURE(TestInvalidZbiHeaderOsAbr([](auto hdr) { hdr->type = 0; }));
}

TEST(BootTests, LoadAndBootInvalidZbiHeaderExtra) {
  ASSERT_NO_FATAL_FAILURE(TestInvalidZbiHeaderOsAbr([](auto hdr) { hdr->extra = 0; }));
}

TEST(BootTests, LoadAndBootInvalidZbiHeaderMagic) {
  ASSERT_NO_FATAL_FAILURE(TestInvalidZbiHeaderOsAbr([](auto hdr) { hdr->magic = 0; }));
}

// A slotless boot error has nothing to fall back to and should just fail if the ZBI is bad.
TEST(BootTests, LoadAndBootInvalidZbiHeaderSlotless) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  zircon_boot_ops.firmware_can_boot_kernel_slot = nullptr;

  // Corrupt the ZBI header.
  zbi_header_t header;
  ASSERT_OK(dev->ReadFromPartition(GPT_ZIRCON_SLOTLESS_NAME, 0, sizeof(header), &header));
  header.type = 0;
  ASSERT_OK(dev->WriteToPartition(GPT_ZIRCON_SLOTLESS_NAME, 0, sizeof(header), &header));

  // We should not be touching A/B/R metadata.
  dev->RemovePartition(GPT_DURABLE_BOOT_NAME);

  // The boot should fail and exit out.
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, kZirconBootModeSlotless), kBootResultErrorInvalidZbi);
  ASSERT_FALSE(dev->GetBootedSlot());
  ASSERT_TRUE(dev->GetBootedImage().empty());
}

// Tests that firmware ABR logic correctly boots to the matching firmware slot.
// |current_firmware_slot| represents the slot fo currently running firmware. |initial_active_slot|
// represents the active slot by metadata.
void TestFirmareAbrMatchingSlotBootSucessful(AbrSlotIndex current_firmware_slot,
                                             AbrSlotIndex initial_active_slot,
                                             ZirconBootMode boot_mode) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  dev->SetFirmwareSlot(current_firmware_slot);
  MarkSlotActive(dev.get(), initial_active_slot);
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, boot_mode), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateBootedSlot(dev.get(), current_firmware_slot));
}

TEST(BootTests, LoadAndBootMatchingSlotBootSucessful) {
  ASSERT_NO_FATAL_FAILURE(
      TestFirmareAbrMatchingSlotBootSucessful(kAbrSlotIndexA, kAbrSlotIndexA, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(
      TestFirmareAbrMatchingSlotBootSucessful(kAbrSlotIndexB, kAbrSlotIndexB, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(
      TestFirmareAbrMatchingSlotBootSucessful(kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(TestFirmareAbrMatchingSlotBootSucessful(kAbrSlotIndexR, kAbrSlotIndexA,
                                                                  kZirconBootModeForceRecovery));
  ASSERT_NO_FATAL_FAILURE(TestFirmareAbrMatchingSlotBootSucessful(kAbrSlotIndexR, kAbrSlotIndexB,
                                                                  kZirconBootModeForceRecovery));
  ASSERT_NO_FATAL_FAILURE(TestFirmareAbrMatchingSlotBootSucessful(kAbrSlotIndexR, kAbrSlotIndexR,
                                                                  kZirconBootModeForceRecovery));
}

void TestFirmareAbrMatchingSlotBootSucessfulOneShotRecovery(AbrSlotIndex initial_active_slot) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  dev->SetFirmwareSlot(kAbrSlotIndexR);
  MarkSlotActive(dev.get(), initial_active_slot);
  AbrOps abr_ops = dev->GetAbrOps();
  AbrSetOneShotRecovery(&abr_ops, true);
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, kZirconBootModeAbr), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateBootedSlot(dev.get(), kAbrSlotIndexR));
}

TEST(BootTests, LoadAndBootMatchingSlotBootSucessfulOneShotRecovery) {
  ASSERT_NO_FATAL_FAILURE(TestFirmareAbrMatchingSlotBootSucessfulOneShotRecovery(kAbrSlotIndexA));
  ASSERT_NO_FATAL_FAILURE(TestFirmareAbrMatchingSlotBootSucessfulOneShotRecovery(kAbrSlotIndexB));
  ASSERT_NO_FATAL_FAILURE(TestFirmareAbrMatchingSlotBootSucessfulOneShotRecovery(kAbrSlotIndexR));
}

// Tests that slotless boots ignore the firmware slot.
TEST(BootTests, TestFirmwareAbrMatchingSlotless) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();

  // Firmware/kernel slot mismatch would normally cause a reboot, but should be ignored here
  // and A/B/R metadata shouldn't be touched.
  dev->SetFirmwareSlot(kAbrSlotIndexA);
  MarkSlotActive(dev.get(), kAbrSlotIndexB);
  dev->RemovePartition(GPT_DURABLE_BOOT_NAME);

  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, kZirconBootModeSlotless), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateBootedSlot(dev.get(), std::nullopt));
}

// Tests that device reboot if firmware slot doesn't matches the target slot to boot. i.e. either
// ABR metadata doesn't match firmware slot or we force R but device is not in firmware R slot.
void TestFirmwareAbrRebootIfSlotMismatched(AbrSlotIndex current_firmware_slot,
                                           AbrSlotIndex initial_active_slot,
                                           AbrSlotIndex expected_firmware_slot,
                                           ZirconBootMode boot_mode) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  dev->SetFirmwareSlot(current_firmware_slot);
  MarkSlotActive(dev.get(), initial_active_slot);
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, boot_mode), kBootResultRebootReturn);
  ASSERT_FALSE(dev->GetBootedSlot());
  ASSERT_EQ(dev->GetFirmwareSlot(), expected_firmware_slot);
}

TEST(BootTests, LoadAndBootMismatchedSlotTriggerReboot) {
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatched(
      kAbrSlotIndexA, kAbrSlotIndexA, kAbrSlotIndexR, kZirconBootModeForceRecovery));
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatched(
      kAbrSlotIndexA, kAbrSlotIndexB, kAbrSlotIndexB, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatched(
      kAbrSlotIndexA, kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatched(
      kAbrSlotIndexB, kAbrSlotIndexB, kAbrSlotIndexR, kZirconBootModeForceRecovery));
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatched(
      kAbrSlotIndexB, kAbrSlotIndexA, kAbrSlotIndexA, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatched(
      kAbrSlotIndexB, kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatched(
      kAbrSlotIndexR, kAbrSlotIndexA, kAbrSlotIndexA, kZirconBootModeAbr));
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatched(
      kAbrSlotIndexR, kAbrSlotIndexB, kAbrSlotIndexB, kZirconBootModeAbr));
}

// Tests that in the case of one shot recovery, device reboots if firmware slot doesn't match R.
void TestFirmwareAbrRebootIfSlotMismatchedOneShotRecovery(AbrSlotIndex current_firmware_slot) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps zircon_boot_ops = dev->GetZirconBootOps();
  dev->SetFirmwareSlot(current_firmware_slot);
  AbrOps abr_ops = dev->GetAbrOps();
  AbrSetOneShotRecovery(&abr_ops, true);
  ASSERT_EQ(LoadAndBoot(&zircon_boot_ops, kZirconBootModeAbr), kBootResultRebootReturn);
  ASSERT_FALSE(dev->GetBootedSlot());
  ASSERT_EQ(dev->GetFirmwareSlot(), kAbrSlotIndexR);
}

TEST(BootTests, LoadAndBootMismatchedSlotTriggerRebootOneShotRecovery) {
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatchedOneShotRecovery(kAbrSlotIndexA));
  ASSERT_NO_FATAL_FAILURE(TestFirmwareAbrRebootIfSlotMismatchedOneShotRecovery(kAbrSlotIndexB));
}

// Validate that a target slot is booted after successful kernel verification.
// Nullopt slot means we expect a slotless boot.
void ValidateVerifiedBootedSlot(const MockZirconBootOps* dev,
                                std::optional<AbrSlotIndex> expected_slot) {
  auto booted_slot = dev->GetBootedSlot();
  ASSERT_EQ(booted_slot, expected_slot);

  std::multiset<NormalizedZbiItem> zbi_items_added;
  ASSERT_NO_FAILURES(ExtractAndSortZbiItems(dev->GetBootedImage().data(), &zbi_items_added));

  // Expected suffix is set in test images via generate_test_data.py.
  std::string slot_suffix = expected_slot ? AbrGetSlotSuffix(*expected_slot) : "_slotless";

  std::vector<std::string> expected_cmdlines{
      // cmdline "vb_arg_1=foo_{slot}" from vbmeta property. See "generate_test_data.py"
      "vb_arg_1=foo" + slot_suffix,
      // cmdline "vb_arg_2=bar_{slot}" from vbmeta property. See "generate_test_data.py"
      "vb_arg_2=bar" + slot_suffix,
  };
  if (expected_slot) {
    // Current slot item is added by MockZirconBootOps::AddDeviceZbiItems for A/B/R boots.
    expected_cmdlines.push_back("zvb.current_slot=" + slot_suffix);
  }
  expected_cmdlines.emplace_back(kTestCmdline.begin(), kTestCmdline.end() - 1);

  std::multiset<NormalizedZbiItem> zbi_items_expected;
  for (auto& str : expected_cmdlines) {
    zbi_items_expected.insert(NormalizedZbiItem{ZBI_TYPE_CMDLINE, 0, {str.data(), str.size() + 1}});
  }
  // Exactly the above items are appended. No more no less.
  EXPECT_EQ(zbi_items_added, zbi_items_expected);
}

// Test OS ABR logic pass verified boot logic and boot to expected slot.
struct VerifiedBootOsAbrTestCase {
  AbrSlotIndex initial_active_slot;
  std::optional<AbrSlotIndex> expected_slot;
  ZirconBootMode boot_mode;
  MockZirconBootOps::LockStatus lock_status;

  std::string Name() const {
    static char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s_to_%s_mode%d_lock%d",
             AbrGetSlotSuffix(initial_active_slot),
             expected_slot ? AbrGetSlotSuffix(*expected_slot) : "null", boot_mode, lock_status);
    return std::string(buffer);
  }
};

using VerifiedBootOsAbrTest = zxtest::TestWithParam<VerifiedBootOsAbrTestCase>;

TEST_P(VerifiedBootOsAbrTest, TestSuccessfulVerifiedBootOsAbrParameterized) {
  const VerifiedBootOsAbrTestCase& test_case = GetParam();
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  dev->SetDeviceLockStatus(test_case.lock_status);
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;
  MarkSlotActive(dev.get(), test_case.initial_active_slot);
  // If we're doing a slotless boot, nothing should touch the A/B/R metadata
  // during LoadAndBoot().
  if (!test_case.expected_slot) {
    dev->RemovePartition(GPT_DURABLE_BOOT_NAME);
  }
  ASSERT_EQ(LoadAndBoot(&ops, test_case.boot_mode), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateVerifiedBootedSlot(dev.get(), test_case.expected_slot));
}

INSTANTIATE_TEST_SUITE_P(
    VerifiedBootOsAbrTests, VerifiedBootOsAbrTest,
    zxtest::Values<VerifiedBootOsAbrTestCase>(
        VerifiedBootOsAbrTestCase{kAbrSlotIndexA, kAbrSlotIndexA, kZirconBootModeAbr,
                                  MockZirconBootOps::LockStatus::kLocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexB, kAbrSlotIndexB, kZirconBootModeAbr,
                                  MockZirconBootOps::LockStatus::kLocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeAbr,
                                  MockZirconBootOps::LockStatus::kLocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexA, kAbrSlotIndexR, kZirconBootModeForceRecovery,
                                  MockZirconBootOps::LockStatus::kLocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexB, kAbrSlotIndexR, kZirconBootModeForceRecovery,
                                  MockZirconBootOps::LockStatus::kLocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeForceRecovery,
                                  MockZirconBootOps::LockStatus::kLocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexA, std::nullopt, kZirconBootModeSlotless,
                                  MockZirconBootOps::LockStatus::kLocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexB, std::nullopt, kZirconBootModeSlotless,
                                  MockZirconBootOps::LockStatus::kLocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexR, std::nullopt, kZirconBootModeSlotless,
                                  MockZirconBootOps::LockStatus::kLocked},

        // Test the same cases but with unlocked state
        VerifiedBootOsAbrTestCase{kAbrSlotIndexA, kAbrSlotIndexA, kZirconBootModeAbr,
                                  MockZirconBootOps::LockStatus::kUnlocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexB, kAbrSlotIndexB, kZirconBootModeAbr,
                                  MockZirconBootOps::LockStatus::kUnlocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeAbr,
                                  MockZirconBootOps::LockStatus::kUnlocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexA, kAbrSlotIndexR, kZirconBootModeForceRecovery,
                                  MockZirconBootOps::LockStatus::kUnlocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexB, kAbrSlotIndexR, kZirconBootModeForceRecovery,
                                  MockZirconBootOps::LockStatus::kUnlocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexR, kAbrSlotIndexR, kZirconBootModeForceRecovery,
                                  MockZirconBootOps::LockStatus::kUnlocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexA, std::nullopt, kZirconBootModeSlotless,
                                  MockZirconBootOps::LockStatus::kUnlocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexB, std::nullopt, kZirconBootModeSlotless,
                                  MockZirconBootOps::LockStatus::kUnlocked},
        VerifiedBootOsAbrTestCase{kAbrSlotIndexR, std::nullopt, kZirconBootModeSlotless,
                                  MockZirconBootOps::LockStatus::kUnlocked}),
    [](const auto& info) { return info.param.Name(); });

// Corrupts the kernel image in the given slot so that verification should fail.
// Nullopt slot means corrupt the slotless image.
void CorruptSlot(MockZirconBootOps* dev, std::optional<AbrSlotIndex> slot) {
  std::vector<uint8_t> buffer(kZirconPartitionSize);
  const char* part = GetSlotPartitionName(slot ? &slot.value() : nullptr);
  ASSERT_OK(dev->ReadFromPartition(part, 0, buffer.size(), buffer.data()));
  buffer[2 * sizeof(zbi_header_t)]++;
  ASSERT_OK(dev->WriteToPartition(part, 0, buffer.size(), buffer.data()));
}

// Wrapper to call CorruptSlot() on multiple slots.
void CorruptSlots(MockZirconBootOps* dev, const std::vector<AbrSlotIndex>& corrupted_slots) {
  for (auto slot : corrupted_slots) {
    CorruptSlot(dev, slot);
  }
}

void CorruptVbmetaHash(MockZirconBootOps& dev, std::optional<AbrSlotIndex> slot) {
  AvbVBMetaImageHeader header;
  std::array<uint8_t, sizeof(header)> buffer;
  std::string part = std::string("vbmeta").append(slot ? AbrGetSlotSuffix(*slot) : "");
  ASSERT_OK(dev.ReadFromPartition(part.c_str(), 0, buffer.size(), buffer.data()));

  avb_vbmeta_image_header_to_host_byte_order(reinterpret_cast<AvbVBMetaImageHeader*>(buffer.data()),
                                             &header);

  size_t hash_offset = sizeof(header) + header.hash_offset;
  ASSERT_OK(dev.ReadFromPartition(part.c_str(), hash_offset, 1, buffer.data()));

  buffer[0]++;
  ASSERT_OK(dev.WriteToPartition(part.c_str(), hash_offset, 1, buffer.data()));
}

void CorruptAllVbmetaHashes(MockZirconBootOps& dev) {
  constexpr std::optional<AbrSlotIndex> slots[] = {
      kAbrSlotIndexA,
      kAbrSlotIndexB,
      kAbrSlotIndexR,
      std::nullopt,
  };
  for (auto slot : slots) {
    CorruptVbmetaHash(dev, slot);
  }
}

TEST(VerifiedBootOsVbmetaTest, TestUnlockedCorruptedVbmetaAllowed) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  dev->SetDeviceLockStatus(MockZirconBootOps::LockStatus::kUnlocked);
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;
  MarkSlotActive(dev.get(), kAbrSlotIndexA);

  CorruptAllVbmetaHashes(*dev);

  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultBootReturn);
  std::multiset<NormalizedZbiItem> zbi_items_added;
  ASSERT_NO_FAILURES(ExtractAndSortZbiItems(dev->GetBootedImage().data(), &zbi_items_added));

  std::multiset<NormalizedZbiItem> expected_items = {
      {ZBI_TYPE_CMDLINE, 0, "vb_arg_1=foo_a"},
      {ZBI_TYPE_CMDLINE, 0, "vb_arg_2=bar_a"},
      {ZBI_TYPE_CMDLINE, 0, "zvb.current_slot=_a"},
      {ZBI_TYPE_CMDLINE, 0, kTestCmdline},
  };

  ASSERT_EQ(zbi_items_added, expected_items);
}

TEST(VerifiedBootOsVbmetaTest, TestLockedCorruptedVbmetaUnallowed) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  dev->SetDeviceLockStatus(MockZirconBootOps::LockStatus::kLocked);
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;
  MarkSlotActive(dev.get(), kAbrSlotIndexA);

  CorruptAllVbmetaHashes(*dev);

  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultErrorNoValidSlot);
}

void VerifySlotMetadataUnbootable(MockZirconBootOps* dev, const std::vector<AbrSlotIndex>& slots) {
  AbrData abr_data;
  ASSERT_OK(dev->ReadFromPartition(GPT_DURABLE_BOOT_NAME, 0, sizeof(abr_data), &abr_data));
  for (auto slot : slots) {
    ASSERT_NE(slot, kAbrSlotIndexR);
    auto slot_data = slot == kAbrSlotIndexA ? abr_data.slot_data[0] : abr_data.slot_data[1];
    ASSERT_EQ(slot_data.tries_remaining, 0);
    ASSERT_EQ(slot_data.successful_boot, 0);
  }
}

// Test that OS ABR logic fall back to other slots when certain slots fail verification.
void TestVerifiedBootFailureOsAbr(const std::vector<AbrSlotIndex>& corrupted_slots,
                                  AbrSlotIndex initial_active_slot, AbrSlotIndex expected_slot) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;
  ASSERT_NO_FATAL_FAILURE(CorruptSlots(dev.get(), corrupted_slots));
  MarkSlotActive(dev.get(), initial_active_slot);
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateVerifiedBootedSlot(dev.get(), expected_slot));
  ASSERT_NO_FATAL_FAILURE(VerifySlotMetadataUnbootable(dev.get(), corrupted_slots));
}

TEST(BootTests, LoadAndBootImageVerificationErrorFallBackOsAbr) {
  // Slot A fails, fall back to slot B
  ASSERT_NO_FATAL_FAILURE(
      TestVerifiedBootFailureOsAbr({kAbrSlotIndexA}, kAbrSlotIndexA, kAbrSlotIndexB));
  // Slot B fails, fall back to slot A.
  ASSERT_NO_FATAL_FAILURE(
      TestVerifiedBootFailureOsAbr({kAbrSlotIndexB}, kAbrSlotIndexB, kAbrSlotIndexA));
  // Slot A, B fail, slot A active, fall back to slot R.
  ASSERT_NO_FATAL_FAILURE(TestVerifiedBootFailureOsAbr({kAbrSlotIndexA, kAbrSlotIndexB},
                                                       kAbrSlotIndexA, kAbrSlotIndexR));
  // Slot A, B fail, slot B active, fall back to slot R.
  ASSERT_NO_FATAL_FAILURE(TestVerifiedBootFailureOsAbr({kAbrSlotIndexA, kAbrSlotIndexB},
                                                       kAbrSlotIndexB, kAbrSlotIndexR));
}

void TestVerifiedBootAllSlotsFailureOsAbr(AbrSlotIndex initial_active_slot) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CorruptSlots(dev.get(), {kAbrSlotIndexA, kAbrSlotIndexB, kAbrSlotIndexR}));
  MarkSlotActive(dev.get(), initial_active_slot);
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultErrorNoValidSlot);
  ASSERT_FALSE(dev->GetBootedSlot().has_value());
  ASSERT_TRUE(dev->GetBootedImage().empty());
  ASSERT_NO_FATAL_FAILURE(
      VerifySlotMetadataUnbootable(dev.get(), {kAbrSlotIndexA, kAbrSlotIndexB}));
}

TEST(BootTests, LoadAndBootImageAllSlotVerificationError) {
  ASSERT_NO_FATAL_FAILURE(TestVerifiedBootAllSlotsFailureOsAbr(kAbrSlotIndexA));
  ASSERT_NO_FATAL_FAILURE(TestVerifiedBootAllSlotsFailureOsAbr(kAbrSlotIndexB));
  ASSERT_NO_FATAL_FAILURE(TestVerifiedBootAllSlotsFailureOsAbr(kAbrSlotIndexR));
}

// Verification errors during slotless boot shouldn't fall back to any other slot.
TEST(BootTests, LoadAndBootSlotlessVerificationError) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;

  ASSERT_NO_FATAL_FAILURE(CorruptSlot(dev.get(), std::nullopt));
  dev->RemovePartition(GPT_DURABLE_BOOT_NAME);

  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeSlotless), kBootResultErrorSlotVerification);
  ASSERT_FALSE(dev->GetBootedSlot().has_value());
  ASSERT_TRUE(dev->GetBootedImage().empty());
}

// Tests that firmware ABR logic reboots if slot verification fails, except R slot.
void TestVerifiedBootFailureFirmwareAbr(AbrSlotIndex initial_active_slot,
                                        AbrSlotIndex expected_firmware_slot) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();

  // corrupt the image
  ASSERT_NO_FATAL_FAILURE(CorruptSlots(dev.get(), {initial_active_slot}));
  MarkSlotActive(dev.get(), initial_active_slot);
  dev->SetFirmwareSlot(initial_active_slot);
  // Slot failure, should trigger reboot into other slot.
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultRebootReturn);
  ASSERT_FALSE(dev->GetBootedSlot());
  ASSERT_EQ(dev->GetFirmwareSlot(), expected_firmware_slot);

  AbrData abr_data;
  ASSERT_OK(dev->ReadFromPartition(GPT_DURABLE_BOOT_NAME, 0, sizeof(abr_data), &abr_data));
  // Failed slot should be marked unbootable.
  ASSERT_NO_FATAL_FAILURE(VerifySlotMetadataUnbootable(dev.get(), {initial_active_slot}));
}

TEST(BootTests, LoadAndBootFirmwareAbrVerificationError) {
  ASSERT_NO_FATAL_FAILURE(TestVerifiedBootFailureFirmwareAbr(kAbrSlotIndexA, kAbrSlotIndexB));
  ASSERT_NO_FATAL_FAILURE(TestVerifiedBootFailureFirmwareAbr(kAbrSlotIndexB, kAbrSlotIndexA));
}

TEST(BootTests, LoadAndBootFirmwareAbrRSlotVerificationError) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();

  // corrupt the image
  ASSERT_NO_FATAL_FAILURE(CorruptSlots(dev.get(), {kAbrSlotIndexR}));
  MarkSlotActive(dev.get(), kAbrSlotIndexR);
  dev->SetFirmwareSlot(kAbrSlotIndexR);
  // R Slot failure, should just return error without reboot.
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultErrorNoValidSlot);
  ASSERT_FALSE(dev->GetBootedSlot());
}

TEST(BootTests, VerificationResultNotCheckedWhenUnlocked) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;
  // Set device unlocked.
  dev->SetDeviceLockStatus(MockZirconBootOps::LockStatus::kUnlocked);
  // Corrupt slot A
  ASSERT_NO_FATAL_FAILURE(CorruptSlots(dev.get(), {kAbrSlotIndexA}));
  // Boot to slot A.
  constexpr AbrSlotIndex active_slot = kAbrSlotIndexA;
  MarkSlotActive(dev.get(), kAbrSlotIndexA);
  // Boot should succeed.
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateVerifiedBootedSlot(dev.get(), active_slot));
}

TEST(BootTests, VerificationResultNotCheckedWhenUnlockedSlotless) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;
  // Set device unlocked.
  dev->SetDeviceLockStatus(MockZirconBootOps::LockStatus::kUnlocked);
  // Corrupt the slotless image.
  ASSERT_NO_FATAL_FAILURE(CorruptSlot(dev.get(), std::nullopt));
  // Boot should succeed.
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeSlotless), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateVerifiedBootedSlot(dev.get(), std::nullopt));
}

TEST(BootTests, RollbackIndexUpdatedOnSuccessfulSlot) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;

  // Mark slot A successful.
  constexpr AbrSlotIndex active_slot = kAbrSlotIndexA;
  AbrOps abr_ops = dev->GetAbrOps();
  ASSERT_EQ(AbrMarkSlotSuccessful(&abr_ops, kAbrSlotIndexA), kAbrResultOk);
  ASSERT_EQ(AbrMarkSlotUnbootable(&abr_ops, kAbrSlotIndexB), kAbrResultOk);

  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateVerifiedBootedSlot(dev.get(), active_slot));
  // Test vbmeta image A a has a rollback index of 5 at location 0. See generate_test_data.py.
  auto res = dev->ReadRollbackIndex(0);
  ASSERT_TRUE(res.is_ok());
  ASSERT_EQ(res.value(), 5ULL);
}

// Slotless boots should still provide anti-rollback support.
TEST(BootTests, RollbackIndexUpdatedOnSuccessfulSlotless) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;

  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeSlotless), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateVerifiedBootedSlot(dev.get(), std::nullopt));
  // Test slotless vbmeta image a has a rollback index of 2 at location 0. See
  // generate_test_data.py.
  auto res = dev->ReadRollbackIndex(0);
  ASSERT_TRUE(res.is_ok());
  ASSERT_EQ(res.value(), 2);
}

// Verification error should not update the rollback index.
TEST(BootTests, RollbackIndexNotUpdatedOnFailureSlotless) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;

  ASSERT_NO_FATAL_FAILURE(CorruptSlot(dev.get(), std::nullopt));
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeSlotless), kBootResultErrorSlotVerification);

  auto res = dev->ReadRollbackIndex(0);
  ASSERT_TRUE(res.is_ok());
  ASSERT_EQ(res.value(), 0);
}

TEST(BootTests, TestRollbackProtection) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;

  MarkSlotActive(dev.get(), kAbrSlotIndexA);
  // Slot A test vbmeta has rollback index 5 at location 0. Slot B test vbmeta has rollback index
  // 10. (See generate_test_data.py). With a rollback index of 7, slot A should fail, slot B should
  // succeed.
  dev->WriteRollbackIndex(0, 7);
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeAbr), kBootResultBootReturn);
  ASSERT_NO_FATAL_FAILURE(ValidateVerifiedBootedSlot(dev.get(), kAbrSlotIndexB));
}

TEST(BootTests, TestRollbackProtectionSlotless) {
  std::unique_ptr<MockZirconBootOps> dev;
  ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&dev));
  ZirconBootOps ops = dev->GetZirconBootOpsWithAvb();
  ops.firmware_can_boot_kernel_slot = nullptr;

  // Test slotless vbmeta image a has a rollback index of 2 at location 0,
  // if the current rollback is higher it should refuse to boot.
  dev->WriteRollbackIndex(0, 3);
  ASSERT_EQ(LoadAndBoot(&ops, kZirconBootModeSlotless), kBootResultErrorSlotVerification);

  // Nothing should have been booted and rollback should be unchanged.
  ASSERT_FALSE(dev->GetBootedSlot().has_value());
  ASSERT_TRUE(dev->GetBootedImage().empty());
  auto res = dev->ReadRollbackIndex(0);
  ASSERT_TRUE(res.is_ok());
  ASSERT_EQ(res.value(), 3);
}

const cpp20::span<const uint8_t> kRambootZbiOnly(kTestZirconSlotlessImage);
const cpp20::span<const uint8_t> kRambootVbmeta(kTestVbmetaSlotlessImage);

std::vector<uint8_t> RambootZbiAndVbmeta() {
  std::vector<uint8_t> image(kRambootZbiOnly.begin(), kRambootZbiOnly.end());
  image.insert(image.end(), kRambootVbmeta.begin(), kRambootVbmeta.end());
  return image;
}

// Wraps the given image in an Android boot image.
std::vector<uint8_t> AndroidBootImage(uint32_t version, cpp20::span<const uint8_t> image) {
  // Header versions 3+ fix the page size at 4096.
  constexpr size_t kAndroidBootImageFixedPageSize = 4096;

  std::vector<uint8_t> result;
  uint32_t image_size = static_cast<uint32_t>(image.size());

  switch (version) {
    case 0: {
      // Pick a value other than kAndroidBootImageFixedPageSize for page size so
      // we get coverage for different values.
      zircon_boot_android_image_hdr_v0 header = {.kernel_size = image_size, .page_size = 8192};
      result.resize(8192);
      memcpy(result.data(), &header, sizeof(header));
      break;
    }
    case 1: {
      zircon_boot_android_image_hdr_v1 header = {.kernel_size = image_size, .page_size = 8192};
      result.resize(8192);
      memcpy(result.data(), &header, sizeof(header));
      break;
    }
    case 2: {
      zircon_boot_android_image_hdr_v2 header = {.kernel_size = image_size, .page_size = 8192};
      result.resize(8192);
      memcpy(result.data(), &header, sizeof(header));
      break;
    }
    case 3: {
      zircon_boot_android_image_hdr_v3 header = {.kernel_size = image_size};
      result.resize(kAndroidBootImageFixedPageSize);
      memcpy(result.data(), &header, sizeof(header));
      break;
    }
    case 4: {
      zircon_boot_android_image_hdr_v4 header = {.kernel_size = image_size};
      result.resize(kAndroidBootImageFixedPageSize);
      memcpy(result.data(), &header, sizeof(header));
      break;
    }
    default:
      ADD_FAILURE("Unsupported Android image version: %u", version);
      return {};
  }

  // Fill in the common fields here.
  zircon_boot_android_image_hdr_v0* header =
      reinterpret_cast<zircon_boot_android_image_hdr_v0*>(result.data());
  memcpy(&header->magic, ZIRCON_BOOT_ANDROID_IMAGE_MAGIC, ZIRCON_BOOT_ANDROID_IMAGE_MAGIC_SIZE);
  memcpy(&header->header_version, &version, sizeof(version));

  // Now append the image.
  result.insert(result.end(), image.begin(), image.end());

  return result;
}

// Parameterized test fixture to run RAM-boot tests against different image
// formats:
//   std::nullopt = raw image
//   0-4 = Android boot image format
//
// Usage:
//   1. Configure mock_ops_ as needed for the test.
//   2. Call TestLoadFromRam() to exercise LoadFromRam().
//   3. Call TestBoot() to exercise booting the RAM-loaded image.
class RambootTest : public zxtest::TestWithParam<std::optional<uint32_t>> {
 public:
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(CreateMockZirconBootOps(&mock_ops_)); }

  // Whether to test using mock ops with libavb support or without.
  enum class OpsMode {
    kWithAvb,
    kWithoutAvb,
  };

  // Tests LoadFromRam() and sets up internal data for TestBoot().
  //
  // Args:
  //   ops_mode: what kind of boot ops to provide.
  //   image: the RAM image to boot; will be wrapped in the correct format
  //          according to the test parameter.
  //
  // Returns the result of LoadFromRam().
  ZirconBootResult TestLoadFromRam(OpsMode ops_mode, cpp20::span<const uint8_t> image) {
    // RAM-boot shouldn't touch any disk data, wipe the partitions to trigger
    // an error if we try to read/write.
    mock_ops_->RemoveAllPartitions();

    // Wrap the image in the desired format.
    auto wrapped = WrapImage(image);

    // Generate the C boot ops struct.
    if (ops_mode == OpsMode::kWithAvb) {
      ops_ = mock_ops_->GetZirconBootOpsWithAvb();
    } else {
      ops_ = mock_ops_->GetZirconBootOps();
    }

    return LoadFromRam(&ops_, wrapped.data(), wrapped.size(), &kernel_buffer_, &kernel_capacity_);
  }

  // Whether the resulting boot image should have ZBI items from the vbmeta image or not.
  enum class ZbiMode {
    kWithVbmeta,
    kWithoutVbmeta,
  };

  // Tests booting an image previously set up via TestLoadFromRam().
  //
  // Args:
  //   zbi_mode: whether we expect the booted ZBI to have vbmeta items or not.
  void TestBoot(ZbiMode zbi_mode) {
    // The buffer returned from LoadFromRam() should be the kernel load buffer.
    ASSERT_EQ(reinterpret_cast<uint8_t*>(kernel_buffer_), mock_ops_->GetKernelLoadBuffer().data());
    ASSERT_EQ(kernel_capacity_, mock_ops_->GetKernelLoadBuffer().size());

    // Finish the boot and make sure everything worked as expected.
    ops_.boot(&ops_, kernel_buffer_, kernel_capacity_);

    // Check that all the expected ZBI items were added.
    if (zbi_mode == ZbiMode::kWithVbmeta) {
      ASSERT_NO_FATAL_FAILURE(ValidateVerifiedBootedSlot(mock_ops_.get(), std::nullopt));
    } else {
      ASSERT_NO_FATAL_FAILURE(ValidateBootedSlot(mock_ops_.get(), std::nullopt));
    }

    // RAM-boot should never update antirollbacks.
    auto res = mock_ops_->ReadRollbackIndex(0);
    ASSERT_TRUE(res.is_ok());
    ASSERT_EQ(res.value(), 0);
  }

 protected:
  // Returns the image in the format given by the test parameter.
  std::vector<uint8_t> WrapImage(cpp20::span<const uint8_t> raw) const {
    std::optional<uint32_t> version = GetParam();
    if (!version) {
      return std::vector<uint8_t>(raw.begin(), raw.end());
    }
    return AndroidBootImage(*version, raw);
  }

  // Mock boot ops used in TestLoadFromRam() and TestBoot().
  std::unique_ptr<MockZirconBootOps> mock_ops_;

  // Filled by TestLoadFromRam().
  ZirconBootOps ops_ = {};
  zbi_header_t* kernel_buffer_ = nullptr;
  size_t kernel_capacity_ = 0;
};

TEST_P(RambootTest, TestRamBootUnverifiedZbiOnly) {
  ASSERT_EQ(kBootResultOK, TestLoadFromRam(OpsMode::kWithoutAvb, kRambootZbiOnly));
  ASSERT_NO_FATAL_FAILURE(TestBoot(ZbiMode::kWithoutVbmeta));
}

// If the boot ops don't support avb, we should just ignore any vbmeta.
TEST_P(RambootTest, TestRamBootUnverifiedZbiAndVbmeta) {
  ASSERT_EQ(kBootResultOK, TestLoadFromRam(OpsMode::kWithoutAvb, RambootZbiAndVbmeta()));
  ASSERT_NO_FATAL_FAILURE(TestBoot(ZbiMode::kWithoutVbmeta));
}

// ZBI-only RAM-boot with libavb should fail verification.
TEST_P(RambootTest, TestRamBootVerifiedZbiOnly) {
  ASSERT_EQ(kBootResultErrorSlotVerification, TestLoadFromRam(OpsMode::kWithAvb, kRambootZbiOnly));
}

TEST_P(RambootTest, TestRamBootVerifiedZbiAndVbmeta) {
  ASSERT_EQ(kBootResultOK, TestLoadFromRam(OpsMode::kWithAvb, RambootZbiAndVbmeta()));
  ASSERT_NO_FATAL_FAILURE(TestBoot(ZbiMode::kWithVbmeta));
}

TEST_P(RambootTest, TestRamBootUnlockedZbiOnly) {
  mock_ops_->SetDeviceLockStatus(MockZirconBootOps::LockStatus::kUnlocked);
  ASSERT_EQ(kBootResultOK, TestLoadFromRam(OpsMode::kWithAvb, kRambootZbiOnly));
  ASSERT_NO_FATAL_FAILURE(TestBoot(ZbiMode::kWithoutVbmeta));
}

TEST_P(RambootTest, TestRamBootUnlockedZbiAndVbmeta) {
  mock_ops_->SetDeviceLockStatus(MockZirconBootOps::LockStatus::kUnlocked);
  ASSERT_EQ(kBootResultOK, TestLoadFromRam(OpsMode::kWithAvb, RambootZbiAndVbmeta()));
  ASSERT_NO_FATAL_FAILURE(TestBoot(ZbiMode::kWithVbmeta));
}

// No ZBI should give us an "invalid ZBI" error.
TEST_P(RambootTest, TestRamBootNoZbi) {
  ASSERT_EQ(kBootResultErrorInvalidZbi, TestLoadFromRam(OpsMode::kWithoutAvb, kRambootVbmeta));
}

// Modify a ZBI header so it claims to be bigger than the actual data size.
TEST_P(RambootTest, TestRamBootZbiOverflow) {
  std::vector<uint8_t> image(kRambootZbiOnly.begin(), kRambootZbiOnly.end());
  zbi_header_t header;
  memcpy(&header, image.data(), sizeof(header));
  header.length = static_cast<uint32_t>(image.size() + 1);
  memcpy(image.data(), &header, sizeof(header));

  ASSERT_EQ(kBootResultErrorInvalidZbi, TestLoadFromRam(OpsMode::kWithoutAvb, image));
}

// Antirollback protection must still be enforced for RAM-boot.
TEST_P(RambootTest, TestRamBootRollbackEnforcement) {
  // RAM-boot image has RBI[0]=2, it shouldn't be able to boot if device RBI[0]=3.
  mock_ops_->WriteRollbackIndex(0, 3);

  ASSERT_EQ(kBootResultErrorSlotVerification,
            TestLoadFromRam(OpsMode::kWithAvb, RambootZbiAndVbmeta()));
}

INSTANTIATE_TEST_SUITE_P(AllImageFormats, RambootTest,
                         zxtest::Values<std::optional<uint32_t>>(std::nullopt, 0, 1, 2, 3, 4),
                         // Give the tests a descriptive tag to make failures easier to identify.
                         [](const auto& info) -> std::string {
                           if (!info.param) {
                             return "Raw";
                           }
                           return "AndroidV" + std::to_string(*info.param);
                         });

}  // namespace
