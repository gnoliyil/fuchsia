// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_LIB_SCSI_INCLUDE_LIB_SCSI_SCSILIB_H_
#define SRC_DEVICES_BLOCK_LIB_SCSI_INCLUDE_LIB_SCSI_SCSILIB_H_

#include <fuchsia/hardware/block/c/banjo.h>
#include <fuchsia/hardware/block/cpp/banjo.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <stdint.h>

#include <ddktl/device.h>
#include <hwreg/bitfields.h>

#include "scsilib_controller.h"

namespace scsi {

const uint8_t kCachingPageCode = 0x8;

enum class Opcode : uint8_t {
  TEST_UNIT_READY = 0x00,
  REQUEST_SENSE = 0x03,
  INQUIRY = 0x12,
  MODE_SELECT_6 = 0x15,
  MODE_SENSE_6 = 0x1A,
  START_STOP_UNIT = 0x1B,
  TOGGLE_REMOVABLE = 0x1E,
  READ_FORMAT_CAPACITIES = 0x23,
  READ_CAPACITY_10 = 0x25,
  READ_10 = 0x28,
  WRITE_10 = 0x2A,
  SYNCHRONIZE_CACHE_10 = 0x35,
  MODE_SELECT_10 = 0x55,
  MODE_SENSE_10 = 0x5A,
  READ_16 = 0x88,
  WRITE_16 = 0x8A,
  READ_CAPACITY_16 = 0x9E,
  REPORT_LUNS = 0xA0,
  READ_12 = 0xA8,
  WRITE_12 = 0xAA,
};

// SCSI command structures (CDBs)

struct TestUnitReadyCDB {
  Opcode opcode;
  uint8_t reserved[4];
  uint8_t control;
} __PACKED;

static_assert(sizeof(TestUnitReadyCDB) == 6, "TestUnitReady CDB must be 6 bytes");

struct InquiryCDB {
  Opcode opcode;
  // reserved_and_evpd(0) is 'Enable Vital Product Data'
  uint8_t reserved_and_evpd;
  uint8_t page_code;
  // allocation_length is in network-byte-order.
  uint16_t allocation_length;
  uint8_t control;
} __PACKED;

static_assert(sizeof(InquiryCDB) == 6, "Inquiry CDB must be 6 bytes");

// Standard INQUIRY Data Header.
struct InquiryData {
  // Peripheral Device Type Header and qualifier.
  uint8_t peripheral_device_type;
  // removable(7) is the 'Removable' bit.
  uint8_t removable;
  uint8_t version;
  // response_data_format_and_control(3 downto 0) is Response Data Format
  // response_data_format_and_control(4) is HiSup
  // response_data_format_and_control(5) is NormACA
  uint8_t response_data_format_and_control;
  uint8_t additional_length;
  // Various control bits, unused currently.
  uint8_t control[3];
  uint8_t t10_vendor_id[8];
  uint8_t product_id[16];
  uint8_t product_revision[4];
  uint8_t drive_serial_number[8];
} __PACKED;

static_assert(offsetof(InquiryData, t10_vendor_id) == 8, "T10 Vendor ID is at offset 8");
static_assert(offsetof(InquiryData, product_id) == 16, "Product ID is at offset 16");

struct VPDBlockLimits {
  uint8_t peripheral_qualifier_device_type;
  uint8_t page_code;
  uint8_t reserved1;
  uint8_t page_length;
  uint8_t reserved2[2];
  uint16_t optimal_xfer_granularity;
  uint32_t max_xfer_length_blocks;
  uint32_t optimal_xfer_length;
} __PACKED;

static_assert(sizeof(VPDBlockLimits) == 16, "BlockLimits Page must be 16 bytes");

struct VPDPageList {
  uint8_t peripheral_qualifier_device_type;
  uint8_t page_code;
  uint8_t reserved;
  uint8_t page_length;
  uint8_t pages[255];
};

struct RequestSenseCDB {
  Opcode opcode;
  uint8_t desc;
  uint16_t reserved;
  uint8_t allocation_length;
  uint8_t control;
} __PACKED;

static_assert(sizeof(RequestSenseCDB) == 6, "Request Sense CDB must be 6 bytes");

struct ModeSense6CDB {
  Opcode opcode;
  // If disable_block_descriptors(3) is '1', device will not return Block Descriptors.
  uint8_t disable_block_descriptors;
  // page_code(7 downto 6) is 'page control'. Should be 00h for current devices.
  uint8_t page_code;
  uint8_t subpage_code;
  uint8_t allocation_length;
  uint8_t control;
} __PACKED;

static_assert(sizeof(ModeSense6CDB) == 6, "Mode Sense 6 CDB must be 6 bytes");

struct ModeSense6ParameterHeader {
  uint8_t mode_data_length;
  // 00h is 'Direct Access Block Device'
  uint8_t medium_type;
  // For Direct Access Block Devices:
  // device_specific_parameter(7) is write-protected bit
  // device_specific_parameter(4) is disable page out/force unit access available
  uint8_t device_specific_parameter;
  uint8_t block_descriptor_length;

  DEF_SUBBIT(device_specific_parameter, 7, write_protected);
} __PACKED;

static_assert(sizeof(ModeSense6ParameterHeader) == 4, "Mode Sense 6 parameters must be 4 bytes");

struct CachingModePage {
  ModeSense6ParameterHeader header;
  uint8_t page_code;
  uint8_t page_length;
  // control_bits (2) is write_cache_enabled.
  uint8_t control_bits;
  uint8_t retention_priorities;
  uint16_t prefetch_transfer_length;
  uint16_t min_prefetch;
  uint16_t max_prefetch;
  uint16_t max_prefetch_ceiling;
  uint8_t control_bits_1;
  uint8_t num_cache_segments;
  uint16_t cache_segment_size;
  uint8_t reserved;
  uint8_t obsolete[3];
} __PACKED;

static_assert(sizeof(CachingModePage) == 24, "Caching Mode Page must be 24 bytes");

struct ReadCapacity10CDB {
  Opcode opcode;
  uint8_t reserved0;
  uint32_t obsolete;
  uint16_t reserved1;
  uint8_t reserved2;
  uint8_t control;
} __PACKED;

static_assert(sizeof(ReadCapacity10CDB) == 10, "Read Capacity 10 CDB must be 10 bytes");

struct ReadCapacity10ParameterData {
  uint32_t returned_logical_block_address;
  uint32_t block_length_in_bytes;
} __PACKED;

static_assert(sizeof(ReadCapacity10ParameterData) == 8, "Read Capacity 10 Params are 8 bytes");

struct ReadCapacity16CDB {
  Opcode opcode;
  uint8_t service_action;
  uint64_t obsolete;
  uint32_t allocation_length;
  uint8_t reserved;
  uint8_t control;
} __PACKED;

static_assert(sizeof(ReadCapacity16CDB) == 16, "Read Capacity 16 CDB must be 16 bytes");

struct ReadCapacity16ParameterData {
  uint64_t returned_logical_block_address;
  uint32_t block_length_in_bytes;
  uint8_t prot_info;
  uint8_t logical_blocks_exponent_info;
  uint16_t lowest_aligned_logical_block;
  uint8_t reserved[16];
} __PACKED;

static_assert(sizeof(ReadCapacity16ParameterData) == 32, "Read Capacity 16 Params are 32 bytes");

struct ReportLunsCDB {
  Opcode opcode;
  uint8_t reserved0;
  uint8_t select_report;
  uint8_t reserved1[3];
  uint32_t allocation_length;
  uint8_t reserved2;
  uint8_t control;
} __PACKED;

static_assert(sizeof(ReportLunsCDB) == 12, "Report LUNs CDB must be 12 bytes");

struct ReportLunsParameterDataHeader {
  uint32_t lun_list_length;
  uint32_t reserved;
  uint64_t lun;  // Need space for at least one LUN.
                 // Followed by 8-byte LUN structures.
} __PACKED;

static_assert(sizeof(ReportLunsParameterDataHeader) == 16, "Report LUNs Header must be 16 bytes");

// Count the number of addressable LUNs attached to a target.
uint32_t CountLuns(Controller* controller, uint8_t target);

struct Read10CDB {
  Opcode opcode;
  // dpo_fua(4) - DPO - Disable Page Out
  // dpo_fua(3) - FUA - Force Unit Access
  uint8_t dpo_fua;
  // Network byte order
  uint32_t logical_block_address;
  uint8_t group_number;
  uint16_t transfer_length;
  uint8_t control;

  DEF_SUBBIT(dpo_fua, 4, disable_page_out);
  DEF_SUBBIT(dpo_fua, 3, force_unit_access);
} __PACKED;

static_assert(sizeof(Read10CDB) == 10, "Read 10 CDB must be 10 bytes");

struct Read12CDB {
  Opcode opcode;
  // dpo_fua(4) - DPO - Disable Page Out
  // dpo_fua(3) - FUA - Force Unit Access
  uint8_t dpo_fua;
  // Network byte order
  uint32_t logical_block_address;
  uint32_t transfer_length;
  uint8_t group_number;
  uint8_t control;

  DEF_SUBBIT(dpo_fua, 4, disable_page_out);
  DEF_SUBBIT(dpo_fua, 3, force_unit_access);
} __PACKED;

static_assert(sizeof(Read12CDB) == 12, "Read 12 CDB must be 12 bytes");

struct Read16CDB {
  Opcode opcode;
  // dpo_fua(4) - DPO - Disable Page Out
  // dpo_fua(3) - FUA - Force Unit Access
  uint8_t dpo_fua;
  // Network byte order
  uint64_t logical_block_address;
  uint32_t transfer_length;
  uint8_t group_number;
  uint8_t control;

  DEF_SUBBIT(dpo_fua, 4, disable_page_out);
  DEF_SUBBIT(dpo_fua, 3, force_unit_access);
} __PACKED;

static_assert(sizeof(Read16CDB) == 16, "Read 16 CDB must be 16 bytes");

struct Write10CDB {
  Opcode opcode;
  // dpo_fua(4) - DPO - Disable Page Out
  // dpo_fua(3) - FUA - Write to medium
  // dpo_fua(1) - FUA_NV - If NV_SUP is 1, prefer write to nonvolatile cache.
  uint8_t dpo_fua;
  // Network byte order
  uint32_t logical_block_address;
  uint8_t group_number;
  uint16_t transfer_length;
  uint8_t control;

  DEF_SUBBIT(dpo_fua, 4, disable_page_out);
  DEF_SUBBIT(dpo_fua, 3, force_unit_access);
} __PACKED;

static_assert(sizeof(Write10CDB) == 10, "Write 10 CDB must be 10 bytes");

struct Write12CDB {
  Opcode opcode;
  // dpo_fua(4) - DPO - Disable Page Out
  // dpo_fua(3) - FUA - Write to medium
  // dpo_fua(1) - FUA_NV - If NV_SUP is 1, prefer write to nonvolatile cache.
  uint8_t dpo_fua;
  // Network byte order
  uint32_t logical_block_address;
  uint32_t transfer_length;
  uint8_t group_number;
  uint8_t control;

  DEF_SUBBIT(dpo_fua, 4, disable_page_out);
  DEF_SUBBIT(dpo_fua, 3, force_unit_access);
} __PACKED;

static_assert(sizeof(Write12CDB) == 12, "Write 12 CDB must be 12 bytes");

struct Write16CDB {
  Opcode opcode;
  // dpo_fua(4) - DPO - Disable Page Out
  // dpo_fua(3) - FUA - Write to medium
  // dpo_fua(1) - FUA_NV - If NV_SUP is 1, prefer write to nonvolatile cache.
  uint8_t dpo_fua;
  // Network byte order
  uint64_t logical_block_address;
  uint32_t transfer_length;
  uint8_t group_number;
  uint8_t control;

  DEF_SUBBIT(dpo_fua, 4, disable_page_out);
  DEF_SUBBIT(dpo_fua, 3, force_unit_access);
} __PACKED;

static_assert(sizeof(Write16CDB) == 16, "Write 16 CDB must be 16 bytes");

struct SynchronizeCache10CDB {
  Opcode opcode;
  // syncnv_immed(2) - SYNC_NV - If SYNC_NV is 1 prefer write to nonvolatile cache.
  // syncnv_immed(1) - IMMED - If IMMED is 1 return after CDB has been
  //                           validated.
  uint8_t syncnv_immed;
  uint32_t logical_block_address;
  uint8_t group_number;
  uint16_t num_blocks;
  uint8_t control;
} __PACKED;

static_assert(sizeof(SynchronizeCache10CDB) == 10, "Synchronize Cache 10 CDB must be 10 bytes");

class Disk;
using DeviceType = ddk::Device<Disk>;

// |Disk| represents a single SCSI direct access block device.
// |Disk| bridges between the Zircon block protocol and SCSI commands/responses.
class Disk : public DeviceType, public ddk::BlockImplProtocol<Disk, ddk::base_protocol> {
 public:
  // Public so that we can use make_unique.
  // Clients should use Disk::Create().
  Disk(Controller* controller, zx_device_t* parent, uint8_t target, uint16_t lun);

  // Create a Disk at a specific target/lun.
  // |controller| is a pointer to the scsi::Controller this disk is attached to.
  // |controller| must outlast Disk.
  // This disk does not take ownership of or any references on |controller|.
  static zx_status_t Create(Controller* controller, zx_device_t* parent, uint8_t target,
                            uint16_t lun, uint32_t max_xfer_size);

  const char* tag() const { return tag_; }

  // DeviceType functions.
  void DdkRelease() { delete this; }

  // ddk::BlockImplProtocol functions.
  void BlockImplQuery(block_info_t* info_out, size_t* block_op_size_out);
  void BlockImplQueue(block_op_t* operation, block_impl_queue_callback completion_cb, void* cookie);

  Disk(const Disk&) = delete;
  Disk& operator=(const Disk&) = delete;

 private:
  zx_status_t Bind();

  Controller* const controller_;
  char tag_[24];
  const uint8_t target_;
  const uint16_t lun_;

  bool removable_;
  bool write_cache_enabled_;
  uint64_t blocks_;
  uint32_t block_size_;
  uint32_t max_xfer_size_;  // In block_size_ units.
};

}  // namespace scsi

#endif  // SRC_DEVICES_BLOCK_LIB_SCSI_INCLUDE_LIB_SCSI_SCSILIB_H_
