// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "userpager.h"

#include <lib/fit/defer.h>
#include <lib/zx/vmar.h>
#include <string.h>
#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/syscalls-next.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/port.h>

#include <atomic>
#include <cstdio>
#include <memory>

#include <fbl/algorithm.h>
#include <fbl/array.h>

namespace pager_tests {

bool Vmo::CheckVmar(uint64_t offset, uint64_t len, const void* expected) {
  ZX_ASSERT((offset + len) <= (size_ / zx_system_get_page_size()));

  len *= zx_system_get_page_size();
  offset *= zx_system_get_page_size();

  for (uint64_t i = offset / sizeof(uint64_t); i < (offset + len) / sizeof(uint64_t); i++) {
    uint64_t actual_val = base_[i];
    // Make sure we deterministically read from the vmar before reading the
    // expected value, in case things get remapped.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint64_t expected_val = expected ? static_cast<const uint64_t*>(expected)[i] : key_ + i;
    if (actual_val != expected_val) {
      printf("mismatch at byte %zu: expected %zx, actual %zx\n", i * sizeof(actual_val),
             expected_val, actual_val);
      return false;
    }
  }
  return true;
}

bool Vmo::CheckVmo(uint64_t offset, uint64_t len, const void* expected) {
  len *= zx_system_get_page_size();
  offset *= zx_system_get_page_size();

  zx::vmo tmp_vmo;
  zx_vaddr_t buf = 0;

  zx_status_t status = zx::vmo::create(len, ZX_VMO_RESIZABLE, &tmp_vmo);
  if (status != ZX_OK) {
    fprintf(stderr, "vmo create failed with %s\n", zx_status_get_string(status));
    return false;
  }

  status = zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, tmp_vmo, 0, len, &buf);
  if (status != ZX_OK) {
    fprintf(stderr, "vmar map failed with %s\n", zx_status_get_string(status));
    return false;
  }

  auto unmap = fit::defer([&]() { zx_vmar_unmap(zx_vmar_root_self(), buf, len); });

  if (vmo_.read(reinterpret_cast<void*>(buf), offset, len) != ZX_OK) {
    return false;
  }

  for (uint64_t i = 0; i < len / sizeof(uint64_t); i++) {
    auto data_buf = reinterpret_cast<uint64_t*>(buf);
    auto expected_buf = static_cast<const uint64_t*>(expected);
    if (data_buf[i] != (expected ? expected_buf[i] : key_ + (offset / sizeof(uint64_t)) + i)) {
      return false;
    }
  }

  return true;
}

bool Vmo::Resize(uint64_t new_page_count) {
  const uint64_t new_size = new_page_count * zx_system_get_page_size();
  zx_status_t status = vmo_.set_size(new_size);
  if (status != ZX_OK) {
    fprintf(stderr, "vmo resize failed with %s\n", zx_status_get_string(status));
    return false;
  }

  // If we're growing the VMO, tear down the old mapping and create a new one to be able to access
  // the new range.
  if (new_size > size_) {
    if (size_ > 0) {
      status = zx::vmar::root_self()->unmap(base_addr_, size_);
      if (status != ZX_OK) {
        fprintf(stderr, "vmar unmap failed with %s\n", zx_status_get_string(status));
        return false;
      }
    }

    zx_vaddr_t addr = 0;
    if (new_size > 0) {
      status = zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo_, 0, new_size,
                                          &addr);
      if (status != ZX_OK) {
        fprintf(stderr, "vmar map failed with %s\n", zx_status_get_string(status));
        return false;
      }
    }

    base_ = reinterpret_cast<uint64_t*>(addr);
    base_addr_ = addr;
  }
  size_ = new_size;
  return true;
}

bool Vmo::OpRange(uint32_t op, uint64_t offset, uint64_t len) {
  return vmo_.op_range(op, offset * zx_system_get_page_size(), len * zx_system_get_page_size(),
                       nullptr, 0) == ZX_OK;
}

void Vmo::GenerateBufferContents(void* dest_buffer, uint64_t len, uint64_t paged_vmo_offset) {
  len *= zx_system_get_page_size();
  paged_vmo_offset *= zx_system_get_page_size();
  auto buf = static_cast<uint64_t*>(dest_buffer);
  for (uint64_t idx = 0; idx < len / sizeof(uint64_t); idx++) {
    buf[idx] = key_ + (paged_vmo_offset / sizeof(uint64_t)) + idx;
  }
}

std::unique_ptr<Vmo> Vmo::Clone() { return Clone(0, size_); }

std::unique_ptr<Vmo> Vmo::Clone(uint64_t offset, uint64_t size) {
  zx::vmo clone;
  zx_status_t status = vmo_.create_child(
      ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE | ZX_VMO_CHILD_RESIZABLE, offset, size, &clone);
  if (status != ZX_OK) {
    fprintf(stderr, "vmo create_child failed with %s\n", zx_status_get_string(status));
    return nullptr;
  }

  zx_vaddr_t addr;
  status = zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, clone, 0, size, &addr);
  if (status != ZX_OK) {
    fprintf(stderr, "vmar map failed with %s\n", zx_status_get_string(status));
    return nullptr;
  }

  return std::unique_ptr<Vmo>(new Vmo(std::move(clone), size, reinterpret_cast<uint64_t*>(addr),
                                      addr, key_ + (offset / sizeof(uint64_t))));
}

UserPager::UserPager()
    : pager_thread_([this]() -> bool {
        this->PageFaultHandler();
        return true;
      }) {}

UserPager::~UserPager() {
  // If a pager thread was started, gracefully shut it down.
  if (shutdown_event_) {
    shutdown_event_.signal(0, ZX_USER_SIGNAL_0);
    pager_thread_.Wait();
  }
  while (!vmos_.is_empty()) {
    auto vmo = vmos_.pop_front();
    zx::vmar::root_self()->unmap(vmo->base_addr(), vmo->size());
  }
}

bool UserPager::Init() {
  zx_status_t status = zx::pager::create(0, &pager_);
  if (status != ZX_OK) {
    fprintf(stderr, "pager create failed with %s\n", zx_status_get_string(status));
    return false;
  }
  status = zx::port::create(0, &port_);
  if (status != ZX_OK) {
    fprintf(stderr, "port create failed with %s\n", zx_status_get_string(status));
    return false;
  }
  return true;
}

bool UserPager::CreateVmo(uint64_t size, Vmo** vmo_out) {
  return CreateVmoWithOptions(size, 0, vmo_out);
}

bool UserPager::CreateVmoWithOptions(uint64_t size, uint32_t options, Vmo** vmo_out) {
  if (shutdown_event_) {
    fprintf(stderr, "creating vmo after starting pager thread\n");
    return false;
  }

  zx::vmo vmo;
  size *= zx_system_get_page_size();
  zx_status_t status = pager_.create_vmo(options, port_, next_key_, size, &vmo);
  if (status != ZX_OK) {
    fprintf(stderr, "pager create_vmo failed with %s\n", zx_status_get_string(status));
    return false;
  }

  zx_vaddr_t addr;
  status = zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0, size, &addr);
  if (status != ZX_OK) {
    fprintf(stderr, "vmar map failed with %s\n", zx_status_get_string(status));
    return false;
  }

  auto paged_vmo =
      Vmo::Create(std::move(vmo), size, reinterpret_cast<uint64_t*>(addr), addr, next_key_);

  next_key_ += (size / sizeof(uint64_t));

  *vmo_out = paged_vmo.get();
  vmos_.push_back(std::move(paged_vmo));

  return true;
}

bool UserPager::UnmapVmo(Vmo* vmo) {
  zx_status_t status = zx::vmar::root_self()->unmap(vmo->base_addr(), vmo->size());
  if (status != ZX_OK) {
    fprintf(stderr, "vmar unmap failed with %s\n", zx_status_get_string(status));
    return false;
  }
  return true;
}

bool UserPager::ReplaceVmo(Vmo* vmo, zx::vmo* old_vmo) {
  if (shutdown_event_) {
    fprintf(stderr, "creating vmo after starting pager thread\n");
    return false;
  }

  zx::vmo new_vmo;
  zx_status_t status = pager_.create_vmo(0, port_, next_key_, vmo->size(), &new_vmo);
  if (status != ZX_OK) {
    fprintf(stderr, "pager create_vmo failed with %s\n", zx_status_get_string(status));
    return false;
  }

  zx_info_vmar_t info;
  uint64_t a1, a2;
  status = zx::vmar::root_self()->get_info(ZX_INFO_VMAR, &info, sizeof(info), &a1, &a2);
  if (status != ZX_OK) {
    fprintf(stderr, "vmar get_info failed with %s\n", zx_status_get_string(status));
    return false;
  }

  zx_vaddr_t addr;
  status = zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_SPECIFIC_OVERWRITE,
                                      vmo->base_addr() - info.base, new_vmo, 0, vmo->size(), &addr);
  if (status != ZX_OK) {
    fprintf(stderr, "vmar map failed with %s\n", zx_status_get_string(status));
    return false;
  }
  std::atomic_thread_fence(std::memory_order_seq_cst);

  vmo->set_key(next_key_);
  next_key_ += (vmo->size() / sizeof(uint64_t));

  *old_vmo = std::move(vmo->vmo());
  vmo->vmo() = std::move(new_vmo);

  return true;
}

bool UserPager::DetachVmo(Vmo* vmo) {
  zx_status_t status = pager_.detach_vmo(vmo->vmo());
  if (status != ZX_OK) {
    fprintf(stderr, "pager detach_vmo failed with %s\n", zx_status_get_string(status));
    return false;
  }
  return true;
}

void UserPager::ReleaseVmo(Vmo* vmo) {
  if (shutdown_event_) {
    fprintf(stderr, "releasing vmo after starting pager thread\n");
    // Generate an assertion error as there is no return code.
    ZX_ASSERT(!shutdown_event_);
    return;
  }

  zx::vmar::root_self()->unmap(vmo->base_addr(), vmo->size());
  vmos_.erase(*vmo);
}

bool UserPager::WaitForPageRead(Vmo* vmo, uint64_t page_offset, uint64_t page_count,
                                zx_time_t deadline) {
  return WaitForPageRequest(ZX_PAGER_VMO_READ, vmo, page_offset, page_count, deadline);
}

bool UserPager::WaitForPageDirty(Vmo* vmo, uint64_t page_offset, uint64_t page_count,
                                 zx_time_t deadline) {
  return WaitForPageRequest(ZX_PAGER_VMO_DIRTY, vmo, page_offset, page_count, deadline);
}

bool UserPager::WaitForPageRequest(uint16_t command, Vmo* vmo, uint64_t page_offset,
                                   uint64_t page_count, zx_time_t deadline) {
  zx_packet_page_request_t req = {};
  req.command = command;
  req.offset = page_offset * zx_system_get_page_size();
  req.length = page_count * zx_system_get_page_size();
  return WaitForRequest(vmo->key(), req, deadline);
}

bool UserPager::WaitForPageComplete(uint64_t key, zx_time_t deadline) {
  zx_packet_page_request_t req = {};
  req.command = ZX_PAGER_VMO_COMPLETE;
  return WaitForRequest(key, req, deadline);
}

bool UserPager::WaitForRequest(uint64_t key, const zx_packet_page_request& req,
                               zx_time_t deadline) {
  zx_port_packet_t expected = {
      .key = key,
      .type = ZX_PKT_TYPE_PAGE_REQUEST,
      .status = ZX_OK,
      .page_request = req,
  };

  return WaitForRequest(
      [expected](const zx_port_packet& actual) -> bool {
        ZX_ASSERT(expected.type == ZX_PKT_TYPE_PAGE_REQUEST);
        if (expected.key != actual.key || ZX_PKT_TYPE_PAGE_REQUEST != actual.type) {
          return false;
        }
        return memcmp(&expected.page_request, &actual.page_request,
                      sizeof(zx_packet_page_request_t)) == 0;
      },
      deadline);
}

bool UserPager::GetPageReadRequest(Vmo* vmo, zx_time_t deadline, uint64_t* offset,
                                   uint64_t* length) {
  return GetPageRequest(vmo, ZX_PAGER_VMO_READ, deadline, offset, length);
}

bool UserPager::GetPageDirtyRequest(Vmo* vmo, zx_time_t deadline, uint64_t* offset,
                                    uint64_t* length) {
  return GetPageRequest(vmo, ZX_PAGER_VMO_DIRTY, deadline, offset, length);
}

bool UserPager::GetPageRequest(Vmo* vmo, uint16_t command, zx_time_t deadline, uint64_t* offset,
                               uint64_t* length) {
  return WaitForRequest(
      [vmo, command, offset, length](const zx_port_packet& packet) -> bool {
        if (packet.key == vmo->key() && packet.type == ZX_PKT_TYPE_PAGE_REQUEST &&
            packet.page_request.command == command) {
          *offset = packet.page_request.offset / zx_system_get_page_size();
          *length = packet.page_request.length / zx_system_get_page_size();
          return true;
        }
        return false;
      },
      deadline);
}

bool UserPager::WaitForRequest(fit::function<bool(const zx_port_packet_t& packet)> cmp_fn,
                               zx_time_t deadline) {
  for (auto& iter : requests_) {
    if (cmp_fn(iter.req)) {
      requests_.erase(iter);
      return true;
    }
  }

  zx_time_t now = zx_clock_get_monotonic();
  if (deadline < now) {
    deadline = now;
  }
  while (now <= deadline) {
    zx_port_packet_t actual_packet;
    // TODO: this can block forever if the thread that's
    // supposed to generate the request unexpectedly dies.
    zx_status_t status = port_.wait(zx::time(deadline), &actual_packet);
    if (status == ZX_OK) {
      if (cmp_fn(actual_packet)) {
        return true;
      }

      auto req = std::make_unique<request>();
      req->req = actual_packet;
      requests_.push_front(std::move(req));
    } else {
      // Don't advance now on success, to make sure we read any pending requests
      now = zx_clock_get_monotonic();
    }
  }
  return false;
}

bool UserPager::SupplyPages(Vmo* paged_vmo, uint64_t dest_offset, uint64_t length,
                            uint64_t src_offset) {
  zx::vmo vmo;
  zx_status_t status = zx::vmo::create((length + src_offset) * zx_system_get_page_size(), 0, &vmo);
  if (status != ZX_OK) {
    fprintf(stderr, "vmo create failed with %s\n", zx_status_get_string(status));
    return false;
  }

  uint64_t cur = 0;
  while (cur < length) {
    uint8_t data[zx_system_get_page_size()];
    paged_vmo->GenerateBufferContents(data, 1, dest_offset + cur);

    status =
        vmo.write(data, (src_offset + cur) * zx_system_get_page_size(), zx_system_get_page_size());
    if (status != ZX_OK) {
      fprintf(stderr, "vmo write failed with %s\n", zx_status_get_string(status));
      return false;
    }

    cur++;
  }

  return SupplyPages(paged_vmo, dest_offset, length, std::move(vmo), src_offset);
}

bool UserPager::SupplyPages(Vmo* paged_vmo, uint64_t dest_offset, uint64_t length, zx::vmo src,
                            uint64_t src_offset) {
  zx_status_t status = pager_.supply_pages(
      paged_vmo->vmo(), dest_offset * zx_system_get_page_size(), length * zx_system_get_page_size(),
      src, src_offset * zx_system_get_page_size());
  if (status != ZX_OK) {
    fprintf(stderr, "pager supply_pages failed with %s\n", zx_status_get_string(status));
    return false;
  }
  return true;
}

bool UserPager::FailPages(Vmo* paged_vmo, uint64_t page_offset, uint64_t page_count,
                          zx_status_t error_status) {
  zx_status_t status =
      pager_.op_range(ZX_PAGER_OP_FAIL, paged_vmo->vmo(), page_offset * zx_system_get_page_size(),
                      page_count * zx_system_get_page_size(), error_status);
  if (status != ZX_OK) {
    fprintf(stderr, "pager op_range FAIL failed with %s\n", zx_status_get_string(status));
    return false;
  }
  return true;
}

bool UserPager::DirtyPages(Vmo* paged_vmo, uint64_t page_offset, uint64_t page_count) {
  zx_status_t status =
      pager_.op_range(ZX_PAGER_OP_DIRTY, paged_vmo->vmo(), page_offset * zx_system_get_page_size(),
                      page_count * zx_system_get_page_size(), 0);
  if (status != ZX_OK) {
    fprintf(stderr, "pager op_range DIRTY failed with %s\n", zx_status_get_string(status));
    return false;
  }
  return true;
}

bool UserPager::WritebackBeginPages(Vmo* paged_vmo, uint64_t page_offset, uint64_t page_count) {
  zx_status_t status = pager_.op_range(ZX_PAGER_OP_WRITEBACK_BEGIN, paged_vmo->vmo(),
                                       page_offset * zx_system_get_page_size(),
                                       page_count * zx_system_get_page_size(), 0);
  if (status != ZX_OK) {
    fprintf(stderr, "pager op_range WRITEBACK_BEGIN failed with %s\n",
            zx_status_get_string(status));
    return false;
  }
  return true;
}

bool UserPager::WritebackBeginZeroPages(Vmo* paged_vmo, uint64_t page_offset, uint64_t page_count) {
  zx_status_t status = pager_.op_range(
      ZX_PAGER_OP_WRITEBACK_BEGIN, paged_vmo->vmo(), page_offset * zx_system_get_page_size(),
      page_count * zx_system_get_page_size(), ZX_VMO_DIRTY_RANGE_IS_ZERO);
  if (status != ZX_OK) {
    fprintf(stderr, "pager op_range WRITEBACK_BEGIN (zero_range) failed with %s\n",
            zx_status_get_string(status));
    return false;
  }
  return true;
}

bool UserPager::WritebackEndPages(Vmo* paged_vmo, uint64_t page_offset, uint64_t page_count) {
  zx_status_t status = pager_.op_range(ZX_PAGER_OP_WRITEBACK_END, paged_vmo->vmo(),
                                       page_offset * zx_system_get_page_size(),
                                       page_count * zx_system_get_page_size(), 0);
  if (status != ZX_OK) {
    fprintf(stderr, "pager op_range WRITEBACK_END failed with %s\n", zx_status_get_string(status));
    return false;
  }
  return true;
}

bool UserPager::VerifyModified(Vmo* paged_vmo) {
  zx_pager_vmo_stats_t stats;
  zx_status_t status = zx_pager_query_vmo_stats(pager_.get(), paged_vmo->vmo().get(),
                                                ZX_PAGER_RESET_VMO_STATS, &stats, sizeof(stats));
  if (status != ZX_OK) {
    fprintf(stderr, "failed to query pager vmo stats with %s\n", zx_status_get_string(status));
    return false;
  }
  return stats.modified == ZX_PAGER_VMO_STATS_MODIFIED;
}

bool UserPager::VerifyDirtyRanges(Vmo* paged_vmo, zx_vmo_dirty_range_t* dirty_ranges_to_verify,
                                  size_t num_dirty_ranges_to_verify) {
  if (num_dirty_ranges_to_verify > 0 && dirty_ranges_to_verify == nullptr) {
    return false;
  }

  // We will query ranges twice, using both populated and upopulated user mode buffers. Unpopulated
  // buffers will verify that faults are being captured and resolved correctly in the query syscall.
  //
  // Initialize a set of buffers for the first query. Keep the number of entries > 1 but small
  // enough so that we end up making more than one syscall in the average case.
  constexpr size_t kMaxRanges = 2;
  zx_vmo_dirty_range_t ranges_no_fault[kMaxRanges];
  for (auto& range : ranges_no_fault) {
    range = {0, 0, 0};
  }
  uint64_t num_ranges_no_fault = 0;

  if (!VerifyDirtyRangesHelper(paged_vmo, dirty_ranges_to_verify, num_dirty_ranges_to_verify,
                               ranges_no_fault, kMaxRanges * sizeof(zx_vmo_dirty_range_t),
                               &num_ranges_no_fault)) {
    fprintf(stderr, "failed to query dirty ranges\n");
    return false;
  }

  // Use a mapped VMO with no committed pages as the buffer for the second query.
  zx::vmo tmp_vmo;
  zx_vaddr_t buf = 0;

  // Use separate pages for the ranges buffer and the count, so they can trigger page faults
  // separately. Also set up the buffer such that its elements straddle a page boundary - map an
  // extra page and start the first element just before a page boundary, with the rest of the
  // elements on the following page; this ensures more than a single page fault for the buffer.
  const size_t kVmoSize =
      fbl::round_up(kMaxRanges * sizeof(zx_vmo_dirty_range_t), zx_system_get_page_size()) +
      2 * zx_system_get_page_size();

  zx_status_t status = zx::vmo::create(kVmoSize, 0, &tmp_vmo);
  if (status != ZX_OK) {
    fprintf(stderr, "vmo create failed with %s\n", zx_status_get_string(status));
    return false;
  }

  status =
      zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, tmp_vmo, 0, kVmoSize, &buf);
  if (status != ZX_OK) {
    fprintf(stderr, "vmar map failed with %s\n", zx_status_get_string(status));
    return false;
  }

  auto unmap = fit::defer([&]() { zx_vmar_unmap(zx_vmar_root_self(), buf, kVmoSize); });

  auto ranges = reinterpret_cast<zx_vmo_dirty_range_t*>(buf + zx_system_get_page_size() -
                                                        sizeof(zx_vmo_dirty_range_t));
  auto num_ranges = reinterpret_cast<uint64_t*>(buf + kVmoSize - zx_system_get_page_size());

  return VerifyDirtyRangesHelper(paged_vmo, dirty_ranges_to_verify, num_dirty_ranges_to_verify,
                                 ranges, kMaxRanges * sizeof(zx_vmo_dirty_range_t), num_ranges);
}

bool UserPager::VerifyDirtyRangesHelper(Vmo* paged_vmo,
                                        zx_vmo_dirty_range_t* dirty_ranges_to_verify,
                                        size_t num_dirty_ranges_to_verify,
                                        zx_vmo_dirty_range_t* ranges_buf, size_t ranges_buf_size,
                                        uint64_t* num_ranges_buf) {
  size_t verify_index = 0;
  uint64_t start = 0;
  uint64_t queried_ranges = 0;
  const uint64_t kMaxRanges = ranges_buf_size / sizeof(zx_vmo_dirty_range_t);
  bool found_extra_ranges = false;

  // Make an initial call to verify the total number of dirty ranges, before we get into the loop
  // and advance start per iteration.
  uint64_t avail = 0;
  zx_status_t status = zx_pager_query_dirty_ranges(pager_.get(), paged_vmo->vmo().get(), 0,
                                                   paged_vmo->size(), nullptr, 0, nullptr, &avail);
  if (status != ZX_OK) {
    fprintf(stderr, "query dirty ranges failed with %s\n", zx_status_get_string(status));
    return false;
  }
  if (avail != num_dirty_ranges_to_verify) {
    fprintf(stderr, "available ranges %zu not as expected %zu\n", avail,
            num_dirty_ranges_to_verify);
    return false;
  }

  do {
    status = zx_pager_query_dirty_ranges(pager_.get(), paged_vmo->vmo().get(), start,
                                         paged_vmo->size() - start, (void*)ranges_buf,
                                         ranges_buf_size, num_ranges_buf, &avail);
    if (status != ZX_OK) {
      fprintf(stderr, "query dirty ranges failed with %s\n", zx_status_get_string(status));
      return false;
    }
    queried_ranges = *num_ranges_buf;
    // Something went wrong if the available ranges is less than the number copied out.
    if (avail < queried_ranges) {
      fprintf(stderr, "available ranges %zu smaller than actual %zu\n", avail, queried_ranges);
      return false;
    }

    // No dirty ranges found.
    if (queried_ranges == 0) {
      break;
    }

    // If there are more ranges available, we should be using up all the space available in buffer.
    if (queried_ranges < avail) {
      if (queried_ranges != kMaxRanges) {
        fprintf(stderr, "queried ranges do not fully occupy buffer\n");
        return false;
      }
    }

    // Verify the dirty ranges returned.
    for (size_t i = 0; i < queried_ranges; i++) {
      if (verify_index == num_dirty_ranges_to_verify) {
        found_extra_ranges = true;
        break;
      }
      if (ranges_buf[i].offset !=
              dirty_ranges_to_verify[verify_index].offset * zx_system_get_page_size() ||
          ranges_buf[i].length !=
              dirty_ranges_to_verify[verify_index].length * zx_system_get_page_size() ||
          ranges_buf[i].options != dirty_ranges_to_verify[verify_index].options) {
        fprintf(stderr,
                "mismatch in queried range. expected {%zu, %zu, %zu} actual {%zu, %zu, %zu}\n",
                dirty_ranges_to_verify[verify_index].offset * zx_system_get_page_size(),
                dirty_ranges_to_verify[verify_index].length * zx_system_get_page_size(),
                dirty_ranges_to_verify[verify_index].options, ranges_buf[i].offset,
                ranges_buf[i].length, ranges_buf[i].options);
        return false;
      }
      verify_index++;
    }
    // No more ranges to verify.
    if (verify_index == num_dirty_ranges_to_verify) {
      break;
    }
    // Adjust the start and length for the next query.
    // We're constrained by the size of |ranges|. We might need another iteration.
    start = ranges_buf[queried_ranges - 1].offset + ranges_buf[queried_ranges - 1].length;
  } while (queried_ranges < avail);

  return verify_index == num_dirty_ranges_to_verify && !found_extra_ranges;
}

void UserPager::PageFaultHandler() {
  zx::vmo aux_vmo;
  zx_status_t status = zx::vmo::create(zx_system_get_page_size(), 0, &aux_vmo);
  ZX_ASSERT(status == ZX_OK);
  while (1) {
    zx_port_packet_t actual_packet;
    status = port_.wait(zx::time::infinite(), &actual_packet);
    if (status != ZX_OK) {
      fprintf(stderr, "Unexpected err %s waiting on port\n", zx_status_get_string(status));
      return;
    }
    if (actual_packet.key == kShutdownKey) {
      ZX_ASSERT(actual_packet.type == ZX_PKT_TYPE_SIGNAL_ONE);
      return;
    }
    ZX_ASSERT(actual_packet.type == ZX_PKT_TYPE_PAGE_REQUEST);
    if (actual_packet.page_request.command == ZX_PAGER_VMO_READ) {
      // Just brute force find matching VMO keys, no need for efficiency.
      for (auto& vmo : vmos_) {
        if (vmo.key() == actual_packet.key) {
          // Supply the requested range.
          SupplyPages(&vmo, actual_packet.page_request.offset / zx_system_get_page_size(),
                      actual_packet.page_request.length / zx_system_get_page_size());
        }
      }
    }
  }
}

bool UserPager::StartTaggedPageFaultHandler() {
  if (shutdown_event_) {
    fprintf(stderr, "Page fault handler already created\n");
    return false;
  }
  zx_status_t status;
  status = zx::event::create(0, &shutdown_event_);
  if (status != ZX_OK) {
    fprintf(stderr, "Failed to create event for shutdown sycnronization\n");
    return false;
  }

  status = shutdown_event_.wait_async(port_, kShutdownKey, ZX_USER_SIGNAL_0, 0);
  if (status != ZX_OK) {
    fprintf(stderr, "Failed to associate shutdown event with port\n");
    return false;
  }

  if (!pager_thread_.Start()) {
    fprintf(stderr, "Failed to start page fault handling thread\n");
    return false;
  }
  return true;
}

}  // namespace pager_tests
