// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_FIRMWARE_GIGABOOT_CPP_TESTS_MOCK_BOOT_SERVICE_H_
#define SRC_FIRMWARE_GIGABOOT_CPP_TESTS_MOCK_BOOT_SERVICE_H_

#include <lib/efi/testing/fake_disk_io_protocol.h>
#include <lib/efi/testing/stub_boot_services.h>
#include <lib/fit/defer.h>
#include <zircon/hw/gpt.h>

#include <numeric>
#include <vector>

#include <efi/protocol/block-io.h>
#include <efi/protocol/device-path.h>
#include <efi/protocol/disk-io.h>
#include <efi/protocol/tcg2.h>
#include <efi/types.h>
#include <fbl/no_destructor.h>
#include <gtest/gtest.h>
#include <phys/efi/protocol.h>

#include "acpi.h"
#include "utils.h"

namespace gigaboot {

// A helper class that mocks a device that exports UEFI protocols in the UEFI environment.
class Device {
 public:
  explicit Device(std::vector<std::string_view> paths) { InitDevicePathProtocol(paths); }
  virtual efi_block_io_protocol* GetBlockIoProtocol() { return nullptr; }
  virtual efi_disk_io_protocol* GetDiskIoProtocol() { return nullptr; }
  virtual efi_tcg2_protocol* GetTcg2Protocol() { return nullptr; }
  virtual efi_graphics_output_protocol* GetGraphicsOutputProtocol() { return nullptr; }

  efi_device_path_protocol* GetDevicePathProtocol() {
    return reinterpret_cast<efi_device_path_protocol*>(device_path_buffer_.data());
  }

 private:
  // A helper that creates a realistic device path protocol for this device.
  void InitDevicePathProtocol(std::vector<std::string_view> path_nodes);

  std::vector<uint8_t> device_path_buffer_;
};

// Use a fixed block size for test.
constexpr size_t kBlockSize = 512;

constexpr size_t kGptEntries = 128;
// Total header blocks = 1 block for header + blocks needed for 128 gpt entries.
constexpr size_t kGptHeaderBlocks = 1 + (kGptEntries * GPT_ENTRY_SIZE) / kBlockSize;
// First usable block come after mbr and primary GPT header/entries.
constexpr size_t kGptFirstUsableBlocks = kGptHeaderBlocks + 1;

// A class that mocks a block device backed by storage.
class BlockDevice : public Device {
 public:
  BlockDevice(std::vector<std::string_view> paths, size_t blocks);
  efi_block_io_protocol* GetBlockIoProtocol() override { return &block_io_protocol_; }
  efi_disk_io_protocol* GetDiskIoProtocol() override { return fake_disk_io_protocol_.protocol(); }
  efi::FakeDiskIoProtocol& fake_disk_io_protocol() { return fake_disk_io_protocol_; }
  efi_block_io_media& block_io_media() { return block_io_media_; }
  void InitializeGpt();
  void AddGptPartition(const gpt_entry_t& new_entry);
  size_t total_blocks() { return total_blocks_; }

 private:
  efi_block_io_media block_io_media_;
  efi_block_io_protocol block_io_protocol_;
  efi::FakeDiskIoProtocol fake_disk_io_protocol_;
  size_t total_blocks_;
};

class Tcg2Device : public Device {
 public:
  Tcg2Device();
  efi_tcg2_protocol* GetTcg2Protocol() override { return &tcg2_protocol_.protocol_; }
  const std::vector<uint8_t>& last_command() { return tcg2_protocol_.last_command_; }

 private:
  static efi_status GetCapability(struct efi_tcg2_protocol*,
                                  efi_tcg2_boot_service_capability*) EFIAPI;
  static efi_status SubmitCommand(struct efi_tcg2_protocol*, uint32_t block_size,
                                  uint8_t* block_data, uint32_t output_size,
                                  uint8_t* output_data) EFIAPI;

  struct Protocol {
    // This must be the first field so that we can convert a efi_tcg2_protocol* to
    // a Tcg2Device::Protocol* to access associated fields.
    efi_tcg2_protocol protocol_;
    std::vector<uint8_t> last_command_;
  } tcg2_protocol_;

  static_assert(std::is_standard_layout<Protocol>::value,
                "Protocol struct must use standard layout");
};

class GraphicsOutputDevice : public Device {
 public:
  GraphicsOutputDevice();
  efi_graphics_output_protocol* GetGraphicsOutputProtocol() override { return &protocol_; }

  efi_graphics_output_mode& mode() { return mode_; }

 private:
  efi_graphics_output_protocol protocol_;
  efi_graphics_output_mode mode_;
  efi_graphics_output_mode_information info_;
};

// Check if the given guid correspond to the protocol of a efi protocol structure.
// i.e. IsProtocol<efi_disk_io_protocol>().
template <typename Protocol>
bool IsProtocol(const efi_guid& guid) {
  return memcmp(&guid, &kEfiProtocolGuid<Protocol>, sizeof(efi_guid)) == 0;
}

// A mock boot service implementation backed by `class Device` base objects.
class MockStubService : public efi::StubBootServices {
 public:
  virtual efi_status LocateProtocol(const efi_guid* protocol, void* registration,
                                    void** intf) override;
  virtual efi_status LocateHandleBuffer(efi_locate_search_type search_type,
                                        const efi_guid* protocol, void* search_key,
                                        size_t* num_handles, efi_handle** buf) override;

  virtual efi_status OpenProtocol(efi_handle handle, const efi_guid* protocol, void** intf,
                                  efi_handle agent_handle, efi_handle controller_handle,
                                  uint32_t attributes) override;

  virtual efi_status CloseProtocol(efi_handle handle, const efi_guid* protocol,
                                   efi_handle agent_handle, efi_handle controller_handle) override {
    return EFI_SUCCESS;
  }

  virtual efi_status GetMemoryMap(size_t* memory_map_size, efi_memory_descriptor* memory_map,
                                  size_t* map_key, size_t* desc_size,
                                  uint32_t* desc_version) override;

  void AddDevice(Device* device) { devices_.push_back(device); }

  void SetMemoryMap(const std::vector<efi_memory_descriptor>& memory_map) {
    memory_map_ = memory_map;
  }

 private:
  std::vector<Device*> devices_;

  std::vector<efi_memory_descriptor> memory_map_;
};

class EfiConfigTable {
 public:
  enum class SmbiosRev {
    kNone,
    kV1,
    kV3,
  };

  explicit EfiConfigTable(uint8_t acpi_revision, SmbiosRev smbios_revision);
  explicit EfiConfigTable(uint8_t acpi_revision) : EfiConfigTable(acpi_revision, SmbiosRev::kV3) {}
  explicit EfiConfigTable(SmbiosRev smbios_revision) : EfiConfigTable(2, smbios_revision) {}
  void CorruptChecksum() { rsdp_.checksum++; }
  void CorruptV2Checksum() { rsdp_.extended_checksum++; }
  void CorruptSignature() { rsdp_.signature++; }

  efi_configuration_table const* Table() const { return table_.data(); }
  size_t TableSize() const { return table_.size(); }
  acpi_rsdp_t& rsdp() { return rsdp_; }

 private:
  acpi_rsdp_t rsdp_;
  std::vector<efi_configuration_table> table_;
};

extern const fbl::NoDestructor<EfiConfigTable> kDefaultEfiConfigTable;

// The following overrides Efi global variables for test.
inline auto SetupEfiGlobalState(MockStubService& stub, Device& image,
                                const EfiConfigTable& config = *kDefaultEfiConfigTable) {
  EXPECT_FALSE(gEfiLoadedImage);
  EXPECT_FALSE(gEfiSystemTable);
  static efi_loaded_image_protocol loaded_image;
  static efi_system_table systab;
  systab = {
      .BootServices = stub.services(),
      .NumberOfTableEntries = config.TableSize(),
      .ConfigurationTable = config.Table(),
  };
  loaded_image.DeviceHandle = &image;
  gEfiLoadedImage = &loaded_image;
  gEfiSystemTable = &systab;
  return fit::defer([]() {
    gEfiLoadedImage = nullptr;
    gEfiSystemTable = nullptr;
  });
}

void SetGptEntryName(const char* name, gpt_entry_t& entry);

}  // namespace gigaboot
#endif  // SRC_FIRMWARE_GIGABOOT_CPP_TESTS_MOCK_BOOT_SERVICE_H_
