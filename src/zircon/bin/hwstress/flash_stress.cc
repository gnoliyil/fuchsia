// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/zircon/bin/hwstress/flash_stress.h"

#include <fcntl.h>
#include <fuchsia/device/cpp/fidl.h>
#include <fuchsia/hardware/block/cpp/fidl.h>
#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <fuchsia/hardware/block/volume/cpp/fidl.h>
#include <inttypes.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/zx/clock.h>
#include <lib/zx/fifo.h>
#include <lib/zx/time.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <zircon/assert.h>
#include <zircon/status.h>

#include <queue>
#include <string>
#include <utility>

#include <fbl/unique_fd.h>
#include <src/lib/uuid/uuid.h>

#include "src/devices/block/drivers/core/block-fifo.h"
#include "src/lib/storage/fs_management/cpp/fvm.h"
#include "src/zircon/bin/hwstress/status.h"
#include "src/zircon/bin/hwstress/util.h"

namespace hwstress {

namespace {

constexpr uint32_t kMaxInFlightRequests = 8;
constexpr uint32_t kDefaultTransferSize = 1024 * 1024;
constexpr uint32_t kMinFvmFreeSpace = 16 * 1024 * 1024;
constexpr uint32_t kMinPartitionFreeSpace = 2 * 1024 * 1024;

void WriteBlockData(zx_vaddr_t start, uint32_t block_size, uint64_t value) {
  uint64_t num_words = block_size / sizeof(value);
  uint64_t* data = reinterpret_cast<uint64_t*>(start);
  for (uint64_t i = 0; i < num_words; i++) {
    data[i] = value;
  }
}

void VerifyBlockData(zx_vaddr_t start, uint32_t block_size, uint64_t value) {
  uint64_t num_words = block_size / sizeof(value);
  uint64_t* data = reinterpret_cast<uint64_t*>(start);
  for (uint64_t i = 0; i < num_words; i++) {
    if (unlikely(data[i] != value)) {
      ZX_PANIC("Found error: expected 0x%016" PRIX64 ", got 0x%016" PRIX64 " at offset %" PRIu64,
               value, data[i], value * block_size + i * sizeof(value));
    }
  }
}

zx_status_t SendFifoRequest(const zx::fifo& fifo, const block_fifo_request_t& request) {
  zx_status_t r = fifo.write(sizeof(request), &request, 1, nullptr);
  if (r == ZX_OK || r == ZX_ERR_SHOULD_WAIT) {
    return r;
  }
  fprintf(stderr, "error: failed writing fifo: %s\n", zx_status_get_string(r));
  return r;
}

zx_status_t ReceiveFifoResponse(const zx::fifo& fifo, block_fifo_response_t* resp) {
  zx_status_t r = fifo.read(sizeof(*resp), resp, 1, nullptr);
  if (r == ZX_ERR_SHOULD_WAIT) {
    // Nothing ready yet.
    return r;
  }
  if (r != ZX_OK) {
    // Transport error.
    fprintf(stderr, "error: failed reading fifo: %s\n", zx_status_get_string(r));
    return r;
  }
  if (resp->status != ZX_OK) {
    // Block device error.
    fprintf(stderr, "error: io txn failed: %s\n", zx_status_get_string(resp->status));
    return resp->status;
  }
  return ZX_OK;
}

}  // namespace

zx_status_t FlashIo(const BlockDevice& device, size_t bytes_to_test, size_t transfer_size,
                    bool is_write_test) {
  ZX_ASSERT(bytes_to_test % device.info.block_size == 0);
  size_t bytes_to_send = bytes_to_test;
  size_t bytes_to_receive = bytes_to_test;

  size_t blksize = device.info.block_size;
  size_t vmo_byte_offset = 0;
  size_t dev_off = 0;
  uint32_t opcode = is_write_test ? BLOCK_OP_WRITE : BLOCK_OP_READ;

  std::queue<reqid_t> ready_to_send;
  block_fifo_request_t reqs[kMaxInFlightRequests];

  for (reqid_t next_reqid = 0; next_reqid < kMaxInFlightRequests; next_reqid++) {
    reqs[next_reqid] = {.opcode = opcode,
                        .reqid = next_reqid,
                        .vmoid = device.vmoid.id,
                        // |length|, |vmo_offset|, and |dev_offset| are measured in blocks.
                        .length = static_cast<uint32_t>(transfer_size / blksize),
                        .vmo_offset = vmo_byte_offset / blksize};
    ready_to_send.push(next_reqid);
    vmo_byte_offset += transfer_size;
  }

  while (bytes_to_receive > 0) {
    // Ensure we are ready to either write to or read from the FIFO.
    zx_signals_t flags = ZX_FIFO_PEER_CLOSED;
    if (!ready_to_send.empty() && bytes_to_send > 0) {
      flags |= ZX_FIFO_WRITABLE;
    }
    if (ready_to_send.size() < kMaxInFlightRequests) {
      flags |= ZX_FIFO_READABLE;
    }
    zx_signals_t pending_signals;
    device.fifo.wait_one(flags, zx::time(ZX_TIME_INFINITE), &pending_signals);

    // If we lost our connection to the block device, abort the test.
    if ((pending_signals & ZX_FIFO_PEER_CLOSED) != 0) {
      fprintf(stderr, "Error: connection to block device lost\n");
      return ZX_ERR_PEER_CLOSED;
    }

    // If the FIFO is writable send a request unless we have kMaxInFlightRequests in flight,
    // or have finished reading/writing.
    if ((pending_signals & ZX_FIFO_WRITABLE) != 0 && !ready_to_send.empty() && bytes_to_send > 0) {
      reqid_t reqid = ready_to_send.front();
      reqs[reqid].dev_offset = dev_off / blksize;
      reqs[reqid].length = static_cast<uint32_t>(std::min(transfer_size, bytes_to_send) /
                                                 static_cast<uint32_t>(blksize));
      if (is_write_test) {
        vmo_byte_offset = reqs[reqid].vmo_offset * blksize;
        for (size_t i = 0; i < reqs[reqid].length; i++) {
          uint64_t value = reqs[reqid].dev_offset + i;
          WriteBlockData(device.vmo_addr + vmo_byte_offset + blksize * i,
                         static_cast<uint32_t>(blksize), value);
        }
      }
      zx_status_t r = SendFifoRequest(device.fifo, reqs[reqid]);
      if (r != ZX_OK) {
        return r;
      }
      dev_off += transfer_size;
      ready_to_send.pop();
      bytes_to_send -= reqs[reqid].length * blksize;
      continue;
    }

    // Process response from the block device if the FIFO is readable.
    if ((pending_signals & ZX_FIFO_READABLE) != 0) {
      ZX_ASSERT(ready_to_send.size() < kMaxInFlightRequests);
      block_fifo_response_t resp;
      zx_status_t r = ReceiveFifoResponse(device.fifo, &resp);
      if (r != ZX_OK) {
        return r;
      }

      reqid_t reqid = resp.reqid;
      bytes_to_receive -= reqs[reqid].length * blksize;
      if (!is_write_test) {
        vmo_byte_offset = reqs[reqid].vmo_offset * blksize;
        for (size_t i = 0; i < reqs[reqid].length; i++) {
          uint64_t value = reqs[reqid].dev_offset + i;
          VerifyBlockData(device.vmo_addr + vmo_byte_offset + blksize * i,
                          static_cast<uint32_t>(blksize), value);
        }
      }
      if (bytes_to_send > 0) {
        ready_to_send.push(reqid);
      }
      continue;
    }
  }

  return ZX_OK;
}

zx_status_t SetupBlockFifo(const std::string& path, BlockDevice* device) {
  // Fetch a FIFO for communicating with the block device over.
  fuchsia::hardware::block::Session_GetFifo_Result fifo_result;
  if (zx_status_t status = device->device->GetFifo(&fifo_result); status != ZX_OK) {
    fprintf(stderr, "Error: cannot get FIFO for '%s': %s\n", path.c_str(),
            zx_status_get_string(status));
    return status;
  }
  if (fifo_result.is_err()) {
    fprintf(stderr, "Error: cannot get FIFO for '%s': %s\n", path.c_str(),
            zx_status_get_string(fifo_result.err()));
    return fifo_result.err();
  }

  // Setup a shared VMO with the block device.
  zx::vmo vmo;
  if (zx_status_t status = zx::vmo::create(device->vmo_size, /*options=*/0, &vmo);
      status != ZX_OK) {
    fprintf(stderr, "Error: could not allocate memory: %s\n", zx_status_get_string(status));
    return status;
  }
  zx::vmo shared_vmo;
  if (zx_status_t status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &shared_vmo); status != ZX_OK) {
    fprintf(stderr, "Error: cannot duplicate handle: %s\n", zx_status_get_string(status));
    return status;
  }
  fuchsia::hardware::block::Session_AttachVmo_Result vmo_result;
  if (zx_status_t status = device->device->AttachVmo(std::move(shared_vmo), &vmo_result);
      status != ZX_OK) {
    fprintf(stderr, "Error: cannot attach VMO for '%s': %s\n", path.c_str(),
            zx_status_get_string(status));
    return status;
  }
  if (vmo_result.is_err()) {
    fprintf(stderr, "Error: cannot attach VMO for '%s': %s\n", path.c_str(),
            zx_status_get_string(vmo_result.err()));
    return vmo_result.err();
  }

  // Map the VMO into memory.
  if (zx_status_t status = zx::vmar::root_self()->map(
          /*options=*/(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE),
          /*vmar_offset=*/0, vmo, /*vmo_offset=*/0, /*len=*/device->vmo_size, &device->vmo_addr);
      status != ZX_OK) {
    fprintf(stderr, "Error: VMO could not be mapped into memory: %s", zx_status_get_string(status));
    return status;
  }

  device->vmoid = vmo_result.response().vmoid;
  device->fifo = std::move(fifo_result.response().fifo);
  device->vmo = std::move(vmo);
  return ZX_OK;
}

std::unique_ptr<TemporaryFvmPartition> TemporaryFvmPartition::Create(int fvm_fd,
                                                                     uint64_t slices_requested) {
  fdio_cpp::UnownedFdioCaller caller(fvm_fd);
  fidl::UnownedClientEnd client_end =
      caller.borrow_as<fuchsia_hardware_block_volume::VolumeManager>();
  uuid::Uuid unique_guid = uuid::Uuid::Generate();

  // Create a new partition.
  zx::result partition = fs_management::FvmAllocatePartition(
      client_end, slices_requested, kTestPartGUID, unique_guid, "flash-test-fs",
      fuchsia_hardware_block_volume::wire::kAllocatePartitionFlagInactive);
  if (partition.is_error()) {
    fprintf(stderr, "Error: Could not allocate and open FVM partition: %s\n",
            partition.status_string());
    return nullptr;
  }
  fuchsia::device::ControllerSyncPtr controller;
  controller.Bind(partition->TakeChannel());
  fuchsia::device::Controller_GetTopologicalPath_Result result;
  if (zx_status_t status = controller->GetTopologicalPath(&result); status != ZX_OK) {
    fprintf(stderr, "Could not get topological path of fvm partition (fidl error): %s\n",
            zx_status_get_string(status));
    return nullptr;
  }
  if (result.is_err()) {
    fprintf(stderr, "Could not get topological path of fvm partition: %s\n",
            zx_status_get_string(result.err()));
    return nullptr;
  }

  return std::unique_ptr<TemporaryFvmPartition>(new TemporaryFvmPartition(
      std::string(result.response().path.data(), result.response().path.size()), unique_guid));
}

TemporaryFvmPartition::TemporaryFvmPartition(std::string partition_path, uuid::Uuid unique_guid)
    : partition_path_(std::move(partition_path)), unique_guid_(unique_guid) {}

TemporaryFvmPartition::~TemporaryFvmPartition() {
  zx::result result = fs_management::DestroyPartition(
      {
          .type_guids = {kTestPartGUID},
          .instance_guids = {unique_guid_},
      },
      true);
  ZX_ASSERT_MSG(result.is_ok(), "%s", result.status_string());
}

std::string TemporaryFvmPartition::GetPartitionPath() { return partition_path_; }

// Start a stress test.
bool StressFlash(StatusLine* logger, const CommandLineArgs& args, zx::duration duration) {
  // Access the FVM.
  fbl::unique_fd fvm_fd(open(args.fvm_path.c_str(), O_RDONLY));
  if (!fvm_fd) {
    logger->Log("Error: Could not open FVM\n");
    return false;
  }

  // Calculate available space and number of slices needed.
  auto fvm_info_or = fs_management::FvmQuery(fvm_fd.get());
  if (fvm_info_or.is_error()) {
    logger->Log("Error: Could not get FVM info\n");
    return false;
  }

  // Default to using all available disk space.
  uint64_t slices_available = fvm_info_or->slice_count - fvm_info_or->assigned_slice_count;
  uint64_t bytes_to_test = slices_available * fvm_info_or->slice_size -
                           RoundUp(kMinFvmFreeSpace, fvm_info_or->slice_size) -
                           kMinPartitionFreeSpace;

  // If a value was specified and does not exceed the free disk space, use that.
  if (args.mem_to_test_megabytes.has_value()) {
    uint64_t bytes_requested = args.mem_to_test_megabytes.value() * 1024 * 1024;
    if (bytes_requested <= bytes_to_test) {
      bytes_to_test = bytes_requested;
    } else {
      logger->Log("Specified disk size (%ld bytes) exceeds available disk size (%ld bytes).\n",
                  bytes_requested, bytes_to_test);
      return false;
    }
  }
  uint64_t slices_requested =
      RoundUp(bytes_to_test, fvm_info_or->slice_size) / fvm_info_or->slice_size;

  std::unique_ptr<TemporaryFvmPartition> fvm_partition =
      TemporaryFvmPartition::Create(fvm_fd.get(), slices_requested);

  if (fvm_partition == nullptr) {
    logger->Log("Failed to create FVM partition");
    return false;
  }

  std::string partition_path = fvm_partition->GetPartitionPath();

  fuchsia::hardware::block::BlockSyncPtr block;
  if (zx_status_t status =
          fdio_service_connect(partition_path.c_str(), block.NewRequest().TakeChannel().release());
      status != ZX_OK) {
    return status;
  }

  // Fetch information about the underlying block device, such as block size.
  fuchsia::hardware::block::Block_GetInfo_Result result;
  if (zx_status_t status = block->GetInfo(&result); status != ZX_OK) {
    logger->Log("Error: cannot get block device info for '%s': %s\n", partition_path.c_str(),
                zx_status_get_string(status));
    return status;
  }
  switch (result.Which()) {
    case fuchsia::hardware::block::Block_GetInfo_Result::Tag::Invalid:
      logger->Log("Error: cannot get block device info for '%s': invalid tag\n",
                  partition_path.c_str());
      return ZX_ERR_INTERNAL;
    case fuchsia::hardware::block::Block_GetInfo_Result::Tag::kErr:
      logger->Log("Error: cannot get block device info for '%s': %s\n", partition_path.c_str(),
                  zx_status_get_string(result.err()));
      return result.err();
    case fuchsia::hardware::block::Block_GetInfo_Result::Tag::kResponse:
      break;
  }
  BlockDevice device;
  if (zx_status_t status = block->OpenSession(device.device.NewRequest()); status != ZX_OK) {
    return status;
  }
  device.info = result.response().info;

  size_t actual_transfer_size = RoundDown(
      std::min(kDefaultTransferSize, device.info.max_transfer_size), device.info.block_size);
  device.vmo_size = actual_transfer_size * kMaxInFlightRequests;

  if (SetupBlockFifo(partition_path, &device) != ZX_OK) {
    logger->Log("Error: Block device could not be set up");
    return false;
  }

  bool iterations_set = false;
  if (args.iterations > 0) {
    iterations_set = true;
  }

  zx::time end_time = zx::deadline_after(duration);
  uint64_t num_tests = 1;

  do {
    zx::time test_start = zx::clock::get_monotonic();
    if (FlashIo(device, bytes_to_test, actual_transfer_size, /*is_write_test=*/true) != ZX_OK) {
      logger->Log("Error writing to vmo.");
      return false;
    }
    zx::duration test_duration = zx::clock::get_monotonic() - test_start;
    logger->Log("Test %4ld: Write: %0.3fs, throughput: %0.2f MiB/s", num_tests,
                DurationToSecs(test_duration),
                static_cast<double>(bytes_to_test) / (DurationToSecs(test_duration) * 1024 * 1024));

    test_start = zx::clock::get_monotonic();
    if (FlashIo(device, bytes_to_test, actual_transfer_size, /*is_write_test=*/false) != ZX_OK) {
      logger->Log("Error reading from vmo.");
      return false;
    }
    test_duration = zx::clock::get_monotonic() - test_start;
    logger->Log("Test %4ld: Read: %0.3fs, throughput: %0.2f MiB/s", num_tests,
                DurationToSecs(test_duration),
                static_cast<double>(bytes_to_test) / (DurationToSecs(test_duration) * 1024 * 1024));

    num_tests++;
    // If 'iterations' is set the duration will be infinite
  } while (zx::clock::get_monotonic() < end_time &&
           (!iterations_set || num_tests <= args.iterations));

  return true;
}

void DestroyFlashTestPartitions(StatusLine* status) {
  uint32_t count = 0;
  // Remove any partitions from previous tests

  while (true) {
    if (zx::result result = fs_management::DestroyPartition({.type_guids = {kTestPartGUID}}, false);
        result.is_error()) {
      ZX_ASSERT_MSG(result.error_value() == ZX_ERR_NOT_FOUND, "%s", result.status_string());
      break;
    }
    count++;
  }
  status->Log("Deleted %u partitions", count);
}

}  // namespace hwstress
