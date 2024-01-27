// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <fidl/fuchsia.sysmem/cpp/fidl.h>
#include <fidl/fuchsia.sysmem2/cpp/fidl.h>
#include <fuchsia/sysmem/c/banjo.h>
#include <inttypes.h>
#include <lib/async/dispatcher.h>
#include <lib/ddk/device.h>
#include <lib/ddk/platform-defs.h>
#include <lib/fidl/cpp/wire/arena.h>
#include <lib/sync/completion.h>
#include <lib/sysmem-version/sysmem-version.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <zircon/assert.h>
#include <zircon/errors.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

#include <fbl/string_printf.h>
#include <sdk/lib/sys/cpp/service_directory.h>

#include "allocator.h"
#include "buffer_collection_token.h"
#include "contiguous_pooled_memory_allocator.h"
#include "driver.h"
#include "external_memory_allocator.h"
#include "lib/ddk/debug.h"
#include "lib/ddk/driver.h"
#include "macros.h"
#include "src/devices/sysmem/metrics/metrics.cb.h"

using sysmem_driver::MemoryAllocator;

namespace sysmem_driver {
namespace {

// These defaults only take effect if there is no SYSMEM_METADATA_TYPE, and also
// neither of these kernel cmdline parameters set:
// driver.sysmem.contiguous_memory_size
// driver.sysmem.protected_memory_size
//
// Typically these defaults are overriden.
//
// By default there is no protected memory pool.
constexpr int64_t kDefaultProtectedMemorySize = 0;
// By default we pre-reserve 5% of physical memory for contiguous memory
// allocation via sysmem.
//
// This is enough to allow tests in sysmem_tests.cc to pass, and avoids relying
// on zx::vmo::create_contiguous() after early boot (by default), since it can
// fail if physical memory has gotten too fragmented.
constexpr int64_t kDefaultContiguousMemorySize = -5;

// fbl::round_up() doesn't work on signed types.
template <typename T>
T AlignUp(T value, T divisor) {
  return (value + divisor - 1) / divisor * divisor;
}

// Helper function to build owned HeapProperties table with coherency domain support.
fuchsia_sysmem2::HeapProperties BuildHeapPropertiesWithCoherencyDomainSupport(
    bool cpu_supported, bool ram_supported, bool inaccessible_supported, bool need_clear,
    bool need_flush) {
  using fuchsia_sysmem2::CoherencyDomainSupport;
  using fuchsia_sysmem2::HeapProperties;

  CoherencyDomainSupport coherency_domain_support;
  coherency_domain_support.cpu_supported().emplace(cpu_supported);
  coherency_domain_support.ram_supported().emplace(ram_supported);
  coherency_domain_support.inaccessible_supported().emplace(inaccessible_supported);

  HeapProperties heap_properties;
  heap_properties.coherency_domain_support().emplace(std::move(coherency_domain_support));
  heap_properties.need_clear().emplace(need_clear);
  heap_properties.need_flush().emplace(need_flush);
  return heap_properties;
}

class SystemRamMemoryAllocator : public MemoryAllocator {
 public:
  explicit SystemRamMemoryAllocator(Owner* parent_device)
      : MemoryAllocator(BuildHeapPropertiesWithCoherencyDomainSupport(
            true /*cpu*/, true /*ram*/, true /*inaccessible*/,
            // Zircon guarantees created VMO are filled with 0; sysmem doesn't
            // need to clear it once again.  There's little point in flushing a
            // demand-backed VMO that's only virtually filled with 0.
            /*need_clear=*/false, /*need_flush=*/false)) {
    node_ = parent_device->heap_node()->CreateChild("SysmemRamMemoryAllocator");
    node_.CreateUint("id", id(), &properties_);
  }

  zx_status_t Allocate(uint64_t size, std::optional<std::string> name,
                       zx::vmo* parent_vmo) override {
    zx_status_t status = zx::vmo::create(size, 0, parent_vmo);
    if (status != ZX_OK) {
      return status;
    }
    constexpr const char vmo_name[] = "Sysmem-core";
    parent_vmo->set_property(ZX_PROP_NAME, vmo_name, sizeof(vmo_name));
    return status;
  }

  zx_status_t SetupChildVmo(const zx::vmo& parent_vmo, const zx::vmo& child_vmo,
                            fuchsia_sysmem2::SingleBufferSettings buffer_settings) override {
    // nothing to do here
    return ZX_OK;
  }

  void Delete(zx::vmo parent_vmo) override {
    // ~parent_vmo
  }
  // Since this allocator only allocates independent VMOs, it's fine to orphan those VMOs from the
  // allocator since the VMOs independently track what pages they're using.  So this allocator can
  // always claim is_empty() true.
  bool is_empty() override { return true; }

 private:
  inspect::Node node_;
  inspect::ValueList properties_;
};

class ContiguousSystemRamMemoryAllocator : public MemoryAllocator {
 public:
  explicit ContiguousSystemRamMemoryAllocator(Owner* parent_device)
      : MemoryAllocator(BuildHeapPropertiesWithCoherencyDomainSupport(
            /*cpu_supported=*/true, /*ram_supported=*/true,
            /*inaccessible_supported=*/true,
            // Zircon guarantees contagious VMO created are filled with 0;
            // sysmem doesn't need to clear it once again.  Unlike non-contiguous
            // VMOs which haven't backed pages yet, contiguous VMOs have backed
            // pages, and it's effective to flush the zeroes to RAM.  Some current
            // sysmem clients rely on contiguous allocations having their initial
            // zero-fill already flushed to RAM (at least for the RAM coherency
            // domain, this should probably remain true).
            /*need_clear=*/false, /*need_flush=*/true)),
        parent_device_(parent_device) {
    node_ = parent_device_->heap_node()->CreateChild("ContiguousSystemRamMemoryAllocator");
    node_.CreateUint("id", id(), &properties_);
  }

  zx_status_t Allocate(uint64_t size, std::optional<std::string> name,
                       zx::vmo* parent_vmo) override {
    zx::vmo result_parent_vmo;
    // This code is unlikely to work after running for a while and physical
    // memory is more fragmented than early during boot. The
    // ContiguousPooledMemoryAllocator handles that case by keeping
    // a separate pool of contiguous memory.
    zx_status_t status =
        zx::vmo::create_contiguous(parent_device_->bti(), size, 0, &result_parent_vmo);
    if (status != ZX_OK) {
      DRIVER_ERROR(
          "zx::vmo::create_contiguous() failed - size_bytes: %lu "
          "status: %d",
          size, status);
      zx_info_kmem_stats_t kmem_stats;
      status = zx_object_get_info(get_root_resource(), ZX_INFO_KMEM_STATS, &kmem_stats,
                                  sizeof(kmem_stats), nullptr, nullptr);
      if (status == ZX_OK) {
        DRIVER_ERROR(
            "kmem stats: total_bytes: 0x%lx free_bytes 0x%lx: wired_bytes: 0x%lx vmo_bytes: 0x%lx\n"
            "mmu_overhead_bytes: 0x%lx other_bytes: 0x%lx",
            kmem_stats.total_bytes, kmem_stats.free_bytes, kmem_stats.wired_bytes,
            kmem_stats.vmo_bytes, kmem_stats.mmu_overhead_bytes, kmem_stats.other_bytes);
      }
      // sanitize to ZX_ERR_NO_MEMORY regardless of why.
      status = ZX_ERR_NO_MEMORY;
      return status;
    }
    constexpr const char vmo_name[] = "Sysmem-contig-core";
    result_parent_vmo.set_property(ZX_PROP_NAME, vmo_name, sizeof(vmo_name));
    *parent_vmo = std::move(result_parent_vmo);
    return ZX_OK;
  }
  zx_status_t SetupChildVmo(const zx::vmo& parent_vmo, const zx::vmo& child_vmo,
                            fuchsia_sysmem2::SingleBufferSettings buffer_settings) override {
    // nothing to do here
    return ZX_OK;
  }
  void Delete(zx::vmo parent_vmo) override {
    // ~vmo
  }
  // Since this allocator only allocates independent VMOs, it's fine to orphan those VMOs from the
  // allocator since the VMOs independently track what pages they're using.  So this allocator can
  // always claim is_empty() true.
  bool is_empty() override { return true; }

 private:
  Owner* const parent_device_;
  inspect::Node node_;
  inspect::ValueList properties_;
};

}  // namespace

Device::Device(zx_device_t* parent_device, Driver* parent_driver)
    : DdkDeviceType(parent_device),
      parent_driver_(parent_driver),
      loop_(&kAsyncLoopConfigNeverAttachToThread),
      in_proc_sysmem_protocol_{.ops = &sysmem_protocol_ops_, .ctx = this} {
  ZX_DEBUG_ASSERT(parent_);
  ZX_DEBUG_ASSERT(parent_driver_);
  zx_status_t status = loop_.StartThread("sysmem", &loop_thrd_);
  ZX_ASSERT(status == ZX_OK);
  // Up until DdkAdd, all access to member variables must happen on this thread.
  loop_checker_.emplace(fit::thread_checker());
}

zx_status_t Device::OverrideSizeFromCommandLine(const char* name, int64_t* memory_size) {
  char pool_arg[32];
  auto status = device_get_variable(parent(), name, pool_arg, sizeof(pool_arg), nullptr);
  if (status != ZX_OK || strlen(pool_arg) == 0)
    return ZX_OK;
  char* end = nullptr;
  int64_t override_size = strtoll(pool_arg, &end, 10);
  // Check that entire string was used and there isn't garbage at the end.
  if (*end != '\0') {
    DRIVER_ERROR("Ignoring flag %s with invalid size \"%s\"", name, pool_arg);
    return ZX_ERR_INVALID_ARGS;
  }
  DRIVER_INFO("Flag %s overriding size to %ld", name, override_size);
  if (override_size < -99) {
    DRIVER_ERROR("Flag %s specified too-large percentage: %" PRId64, name, -override_size);
    return ZX_ERR_INVALID_ARGS;
  }
  *memory_size = override_size;
  return ZX_OK;
}

zx::result<std::string> Device::GetFromCommandLine(const char* name) {
  char arg[32];
  auto status = device_get_variable(parent(), name, arg, sizeof(arg), nullptr);
  if (status == ZX_ERR_NOT_FOUND) {
    return zx::error(status);
  }
  if (status != ZX_OK) {
    LOG(ERROR, "device_get_variable() failed - status: %d", status);
    return zx::error(status);
  }
  if (strlen(arg) == 0) {
    LOG(ERROR, "strlen(arg) == 0");
    return zx::error(ZX_ERR_INTERNAL);
  }
  return zx::ok(std::string(arg, strlen(arg)));
}

zx::result<bool> Device::GetBoolFromCommandLine(const char* name, bool default_value) {
  auto result = GetFromCommandLine(name);
  if (!result.is_ok()) {
    if (result.error_value() == ZX_ERR_NOT_FOUND) {
      return zx::ok(default_value);
    }
    return zx::error(result.error_value());
  }
  std::string str = *result;
  std::transform(str.begin(), str.end(), str.begin(), ::tolower);
  if (str == "true" || str == "1") {
    return zx::ok(true);
  }
  if (str == "false" || str == "0") {
    return zx::ok(false);
  }
  return zx::error(ZX_ERR_INVALID_ARGS);
}

zx_status_t Device::GetContiguousGuardParameters(uint64_t* guard_bytes_out,
                                                 bool* unused_pages_guarded,
                                                 zx::duration* unused_page_check_cycle_period,
                                                 bool* internal_guard_pages_out,
                                                 bool* crash_on_fail_out) {
  const uint64_t kDefaultGuardBytes = zx_system_get_page_size();
  *guard_bytes_out = kDefaultGuardBytes;
  *unused_pages_guarded = true;
  *unused_page_check_cycle_period =
      ContiguousPooledMemoryAllocator::kDefaultUnusedPageCheckCyclePeriod;
  *internal_guard_pages_out = false;
  *crash_on_fail_out = false;

  // If true, sysmem crashes on a guard page violation.
  char arg[32];
  if (ZX_OK == device_get_variable(parent(), "driver.sysmem.contiguous_guard_pages_fatal", arg,
                                   sizeof(arg), nullptr)) {
    DRIVER_INFO("Setting contiguous_guard_pages_fatal");
    *crash_on_fail_out = true;
  }

  // If true, sysmem will create guard regions around every allocation.
  if (ZX_OK == device_get_variable(parent(), "driver.sysmem.contiguous_guard_pages_internal", arg,
                                   sizeof(arg), nullptr)) {
    DRIVER_INFO("Setting contiguous_guard_pages_internal");
    *internal_guard_pages_out = true;
  }

  // If true, sysmem will _not_ treat currently-unused pages as guard pages.  We flip the sense on
  // this one because we want the default to be enabled so we discover any issues with
  // DMA-write-after-free by default, and because this mechanism doesn't cost any pages.
  if (ZX_OK == device_get_variable(parent(), "driver.sysmem.contiguous_guard_pages_unused_disabled",
                                   arg, sizeof(arg), nullptr)) {
    DRIVER_INFO("Clearing unused_pages_guarded");
    *unused_pages_guarded = false;
  }

  const char* kUnusedPageCheckCyclePeriodName =
      "driver.sysmem.contiguous_guard_pages_unused_cycle_seconds";
  char unused_page_check_cycle_period_seconds_string[32];
  auto status = device_get_variable(parent(), kUnusedPageCheckCyclePeriodName,
                                    unused_page_check_cycle_period_seconds_string,
                                    sizeof(unused_page_check_cycle_period_seconds_string), nullptr);
  if (status == ZX_OK && strlen(unused_page_check_cycle_period_seconds_string)) {
    char* end = nullptr;
    int64_t potential_cycle_period_seconds =
        strtoll(unused_page_check_cycle_period_seconds_string, &end, 10);
    if (*end != '\0') {
      DRIVER_ERROR("Flag %s has invalid value \"%s\"", kUnusedPageCheckCyclePeriodName,
                   unused_page_check_cycle_period_seconds_string);
      return ZX_ERR_INVALID_ARGS;
    }
    DRIVER_INFO("Flag %s setting unused page check period to %ld seconds",
                kUnusedPageCheckCyclePeriodName, potential_cycle_period_seconds);
    *unused_page_check_cycle_period = zx::sec(potential_cycle_period_seconds);
  }

  const char* kGuardBytesName = "driver.sysmem.contiguous_guard_page_count";
  char guard_count[32];
  status =
      device_get_variable(parent(), kGuardBytesName, guard_count, sizeof(guard_count), nullptr);
  if (status == ZX_OK && strlen(guard_count)) {
    char* end = nullptr;
    int64_t page_count = strtoll(guard_count, &end, 10);
    // Check that entire string was used and there isn't garbage at the end.
    if (*end != '\0') {
      DRIVER_ERROR("Flag %s has invalid value \"%s\"", kGuardBytesName, guard_count);
      return ZX_ERR_INVALID_ARGS;
    }
    DRIVER_INFO("Flag %s setting guard page count to %ld", kGuardBytesName, page_count);
    *guard_bytes_out = zx_system_get_page_size() * page_count;
  }

  return ZX_OK;
}

void Device::DdkUnbind(ddk::UnbindTxn txn) {
  // Try to ensure there are no outstanding VMOS before shutting down the loop.
  async::PostTask(loop_.dispatcher(), [this]() mutable {
    std::lock_guard checker(*loop_checker_);
    waiting_for_unbind_ = true;
    CheckForUnbind();
  });

  // JoinThreads waits for the Quit() in CheckForUnbind to execute and cause the thread to exit. We
  // could instead try to asynchronously do these operations on another thread, but the display unit
  // tests don't have a way to wait for the unbind to be complete before tearing down the device.
  loop_.JoinThreads();
  loop_.Shutdown();
  // After this point the FIDL servers should have been shutdown and all DDK and other protocol
  // methods will error out because posting tasks to the dispatcher fails.
  txn.Reply();
  zxlogf(INFO, "Finished unbind.");
}

void Device::CheckForUnbind() {
  std::lock_guard checker(*loop_checker_);
  if (!waiting_for_unbind_)
    return;
  if (!logical_buffer_collections().empty()) {
    zxlogf(INFO, "Not unbinding because there are logical buffer collections count %ld",
           logical_buffer_collections().size());
    return;
  }
  if (!contiguous_system_ram_allocator_->is_empty()) {
    zxlogf(INFO, "Not unbinding because contiguous system ram allocator is not empty");
    return;
  }
  for (auto& [type, allocator] : allocators_) {
    if (!allocator->is_empty()) {
      zxlogf(INFO, "Not unbinding because allocator %lx is not empty", safe_cast<uint64_t>(type));

      return;
    }
  }

  // This will cause the loop to exit and will allow DdkUnbind to continue.
  loop_.Quit();
}

SysmemMetrics& Device::metrics() { return metrics_; }

protected_ranges::ProtectedRangesCoreControl& Device::protected_ranges_core_control(
    fuchsia_sysmem2::HeapType heap_type) {
  std::lock_guard checker(*loop_checker_);
  ZX_DEBUG_ASSERT(secure_mem_controls_.find(heap_type) != secure_mem_controls_.end());
  return secure_mem_controls_[heap_type];
}

bool Device::SecureMemControl::IsDynamic() { return is_dynamic; }

uint64_t Device::SecureMemControl::GetRangeGranularity() { return range_granularity; }

uint64_t Device::SecureMemControl::MaxRangeCount() { return max_range_count; }

bool Device::SecureMemControl::HasModProtectedRange() { return has_mod_protected_range; }

void Device::SecureMemControl::AddProtectedRange(const protected_ranges::Range& range) {
  std::lock_guard checker(*parent->loop_checker_);
  ZX_DEBUG_ASSERT(parent->secure_mem_);
  fuchsia_sysmem::SecureHeapAndRange secure_heap_and_range;
  secure_heap_and_range.heap().emplace(
      safe_cast<fuchsia_sysmem::HeapType>(sysmem::fidl_underlying_cast(heap_type)));
  fuchsia_sysmem::SecureHeapRange secure_heap_range;
  secure_heap_range.physical_address().emplace(range.begin());
  secure_heap_range.size_bytes().emplace(range.length());
  secure_heap_and_range.range().emplace(std::move(secure_heap_range));
  fidl::Arena arena;
  auto wire_secure_heap_and_range = fidl::ToWire(arena, std::move(secure_heap_and_range));
  auto result =
      parent->secure_mem_->channel()->AddSecureHeapPhysicalRange(wire_secure_heap_and_range);
  // If we lose the ability to control protected memory ranges ... reboot.
  ZX_ASSERT(result.ok());
  if (result->is_error()) {
    LOG(ERROR, "AddSecureHeapPhysicalRange() failed - status: %d", result->error_value());
  }
  ZX_ASSERT(!result->is_error());
}

void Device::SecureMemControl::DelProtectedRange(const protected_ranges::Range& range) {
  std::lock_guard checker(*parent->loop_checker_);
  ZX_DEBUG_ASSERT(parent->secure_mem_);
  fuchsia_sysmem::SecureHeapAndRange secure_heap_and_range;
  secure_heap_and_range.heap().emplace(
      safe_cast<fuchsia_sysmem::HeapType>(sysmem::fidl_underlying_cast(heap_type)));
  fuchsia_sysmem::SecureHeapRange secure_heap_range;
  secure_heap_range.physical_address().emplace(range.begin());
  secure_heap_range.size_bytes().emplace(range.length());
  secure_heap_and_range.range().emplace(std::move(secure_heap_range));
  fidl::Arena arena;
  auto wire_secure_heap_and_range = fidl::ToWire(arena, std::move(secure_heap_and_range));
  auto result =
      parent->secure_mem_->channel()->DeleteSecureHeapPhysicalRange(wire_secure_heap_and_range);
  // If we lose the ability to control protected memory ranges ... reboot.
  ZX_ASSERT(result.ok());
  if (result->is_error()) {
    LOG(ERROR, "DeleteSecureHeapPhysicalRange() failed - status: %d", result->error_value());
  }
  ZX_ASSERT(!result->is_error());
}

void Device::SecureMemControl::ModProtectedRange(const protected_ranges::Range& old_range,
                                                 const protected_ranges::Range& new_range) {
  if (new_range.end() != old_range.end() && new_range.begin() != old_range.begin()) {
    LOG(INFO,
        "new_range.end(): %" PRIx64 " old_range.end(): %" PRIx64 " new_range.begin(): %" PRIx64
        " old_range.begin(): %" PRIx64,
        new_range.end(), old_range.end(), new_range.begin(), old_range.begin());
    ZX_PANIC("INVALID RANGE MODIFICATION");
  }

  std::lock_guard checker(*parent->loop_checker_);
  ZX_DEBUG_ASSERT(parent->secure_mem_);
  fuchsia_sysmem::SecureHeapAndRangeModification modification;
  modification.heap().emplace(
      safe_cast<fuchsia_sysmem::HeapType>(sysmem::fidl_underlying_cast(heap_type)));
  fuchsia_sysmem::SecureHeapRange range_old;
  range_old.physical_address().emplace(old_range.begin());
  range_old.size_bytes().emplace(old_range.length());
  fuchsia_sysmem::SecureHeapRange range_new;
  range_new.physical_address().emplace(new_range.begin());
  range_new.size_bytes().emplace(new_range.length());
  modification.old_range().emplace(std::move(range_old));
  modification.new_range().emplace(std::move(range_new));
  fidl::Arena arena;
  auto wire_modification = fidl::ToWire(arena, std::move(modification));
  auto result = parent->secure_mem_->channel()->ModifySecureHeapPhysicalRange(wire_modification);
  // If we lose the ability to control protected memory ranges ... reboot.
  ZX_ASSERT(result.ok());
  if (result->is_error()) {
    LOG(ERROR, "ModifySecureHeapPhysicalRange() failed - status: %d", result->error_value());
  }
  ZX_ASSERT(!result->is_error());
}

void Device::SecureMemControl::ZeroProtectedSubRange(bool is_covering_range_explicit,
                                                     const protected_ranges::Range& range) {
  std::lock_guard checker(*parent->loop_checker_);
  ZX_DEBUG_ASSERT(parent->secure_mem_);
  fuchsia_sysmem::SecureHeapAndRange secure_heap_and_range;
  secure_heap_and_range.heap().emplace(
      safe_cast<fuchsia_sysmem::HeapType>(sysmem::fidl_underlying_cast(heap_type)));
  fuchsia_sysmem::SecureHeapRange secure_heap_range;
  secure_heap_range.physical_address().emplace(range.begin());
  secure_heap_range.size_bytes().emplace(range.length());
  secure_heap_and_range.range().emplace(std::move(secure_heap_range));
  fidl::Arena arena;
  auto wire_secure_heap_and_range = fidl::ToWire(arena, std::move(secure_heap_and_range));
  auto result = parent->secure_mem_->channel()->ZeroSubRange(is_covering_range_explicit,
                                                             wire_secure_heap_and_range);
  // If we lose the ability to control protected memory ranges ... reboot.
  ZX_ASSERT(result.ok());
  if (result->is_error()) {
    LOG(ERROR, "ZeroSubRange() failed - status: %d", result->error_value());
  }
  ZX_ASSERT(!result->is_error());
}

zx_status_t Device::Bind() {
  std::lock_guard checker(*loop_checker_);

  // Put everything under a node called "sysmem" because there's currently there's not a simple way
  // to distinguish (using a selector) which driver inspect information is coming from.
  sysmem_root_ = inspector_.GetRoot().CreateChild("sysmem");
  heaps_ = sysmem_root_.CreateChild("heaps");
  collections_node_ = sysmem_root_.CreateChild("collections");

  auto pdev_client = DdkConnectFidlProtocol<fuchsia_hardware_platform_device::Service::Device>();
  if (pdev_client.is_error()) {
    DRIVER_ERROR(
        "Failed device_connect_fidl_protocol() for fuchsia.hardware.platform.device - status: %s",
        pdev_client.status_string());
    return pdev_client.status_value();
  }

  pdev_ = fidl::SyncClient(std::move(*pdev_client));

  int64_t protected_memory_size = kDefaultProtectedMemorySize;
  int64_t contiguous_memory_size = kDefaultContiguousMemorySize;

  sysmem_metadata_t metadata;

  size_t metadata_actual;
  zx_status_t status =
      DdkGetMetadata(SYSMEM_METADATA_TYPE, &metadata, sizeof(metadata), &metadata_actual);
  if (status == ZX_OK && metadata_actual == sizeof(metadata)) {
    pdev_device_info_vid_ = metadata.vid;
    pdev_device_info_pid_ = metadata.pid;
    protected_memory_size = metadata.protected_memory_size;
    contiguous_memory_size = metadata.contiguous_memory_size;
  }

  const char* kDisableDynamicRanges = "driver.sysmem.protected_ranges.disable_dynamic";
  zx::result<bool> protected_ranges_disable_dynamic =
      GetBoolFromCommandLine(kDisableDynamicRanges, false);
  if (protected_ranges_disable_dynamic.is_error()) {
    return protected_ranges_disable_dynamic.status_value();
  }
  cmdline_protected_ranges_disable_dynamic_ = protected_ranges_disable_dynamic.value();

  status =
      OverrideSizeFromCommandLine("driver.sysmem.protected_memory_size", &protected_memory_size);
  if (status != ZX_OK) {
    // OverrideSizeFromCommandLine() already printed an error.
    return status;
  }
  status =
      OverrideSizeFromCommandLine("driver.sysmem.contiguous_memory_size", &contiguous_memory_size);
  if (status != ZX_OK) {
    // OverrideSizeFromCommandLine() already printed an error.
    return status;
  }

  // Negative values are interpreted as a percentage of physical RAM.
  if (protected_memory_size < 0) {
    protected_memory_size = -protected_memory_size;
    ZX_DEBUG_ASSERT(protected_memory_size >= 1 && protected_memory_size <= 99);
    protected_memory_size = zx_system_get_physmem() * protected_memory_size / 100;
  }
  if (contiguous_memory_size < 0) {
    contiguous_memory_size = -contiguous_memory_size;
    ZX_DEBUG_ASSERT(contiguous_memory_size >= 1 && contiguous_memory_size <= 99);
    contiguous_memory_size = zx_system_get_physmem() * contiguous_memory_size / 100;
  }

  constexpr int64_t kMinProtectedAlignment = 64 * 1024;
  assert(kMinProtectedAlignment % zx_system_get_page_size() == 0);
  protected_memory_size = AlignUp(protected_memory_size, kMinProtectedAlignment);
  contiguous_memory_size =
      AlignUp(contiguous_memory_size, safe_cast<int64_t>(zx_system_get_page_size()));

  allocators_[fuchsia_sysmem2::HeapType::kSystemRam] =
      std::make_unique<SystemRamMemoryAllocator>(this);

  auto result = pdev_.wire()->GetBti(0);
  if (!result.ok()) {
    DRIVER_ERROR("Transport error for PDev::GetBti() - status: %s", result.status_string());
    return result.status();
  }

  if (result->is_error()) {
    DRIVER_ERROR("Failed PDev::GetBti() - status: %s", zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  bti_ = std::move(result->value()->bti);

  zx::bti bti_copy;
  status = bti_.duplicate(ZX_RIGHT_SAME_RIGHTS, &bti_copy);
  if (status != ZX_OK) {
    DRIVER_ERROR("BTI duplicate failed: %d", status);
    return status;
  }

  if (contiguous_memory_size) {
    constexpr bool kIsAlwaysCpuAccessible = true;
    constexpr bool kIsEverCpuAccessible = true;
    constexpr bool kIsReady = true;
    constexpr bool kCanBeTornDown = true;
    auto pooled_allocator = std::make_unique<ContiguousPooledMemoryAllocator>(
        this, "SysmemContiguousPool", &heaps_,
        safe_cast<uint64_t>(fuchsia_sysmem::wire::HeapType::kSystemRam), contiguous_memory_size,
        kIsAlwaysCpuAccessible, kIsEverCpuAccessible, kIsReady, kCanBeTornDown, loop_.dispatcher());
    if (pooled_allocator->Init() != ZX_OK) {
      DRIVER_ERROR("Contiguous system ram allocator initialization failed");
      return ZX_ERR_NO_MEMORY;
    }
    uint64_t guard_region_size;
    bool unused_pages_guarded;
    zx::duration unused_page_check_cycle_period;
    bool internal_guard_regions;
    bool crash_on_guard;
    if (GetContiguousGuardParameters(&guard_region_size, &unused_pages_guarded,
                                     &unused_page_check_cycle_period, &internal_guard_regions,
                                     &crash_on_guard) == ZX_OK) {
      pooled_allocator->InitGuardRegion(guard_region_size, unused_pages_guarded,
                                        unused_page_check_cycle_period, internal_guard_regions,
                                        crash_on_guard, loop_.dispatcher());
    }
    pooled_allocator->SetupUnusedPages();
    contiguous_system_ram_allocator_ = std::move(pooled_allocator);
  } else {
    contiguous_system_ram_allocator_ = std::make_unique<ContiguousSystemRamMemoryAllocator>(this);
  }

  // TODO: Separate protected memory allocator into separate driver or library
  if (pdev_device_info_vid_ == PDEV_VID_AMLOGIC && protected_memory_size > 0) {
    constexpr bool kIsAlwaysCpuAccessible = false;
    constexpr bool kIsEverCpuAccessible = true;
    constexpr bool kIsReady = false;
    // We have no way to tear down secure memory.
    constexpr bool kCanBeTornDown = false;
    auto amlogic_allocator = std::make_unique<ContiguousPooledMemoryAllocator>(
        this, "SysmemAmlogicProtectedPool", &heaps_,
        safe_cast<uint64_t>(fuchsia_sysmem::wire::HeapType::kAmlogicSecure), protected_memory_size,
        kIsAlwaysCpuAccessible, kIsEverCpuAccessible, kIsReady, kCanBeTornDown, loop_.dispatcher());
    // Request 64kB alignment because the hardware can only modify protections along 64kB
    // boundaries.
    status = amlogic_allocator->Init(16);
    if (status != ZX_OK) {
      DRIVER_ERROR("Failed to init allocator for amlogic protected memory: %d", status);
      return status;
    }
    // For !is_cpu_accessible_, we don't call amlogic_allocator->SetupUnusedPages() until the start
    // of set_ready().
    secure_allocators_[fuchsia_sysmem2::HeapType::kAmlogicSecure] = amlogic_allocator.get();
    allocators_[fuchsia_sysmem2::HeapType::kAmlogicSecure] = std::move(amlogic_allocator);
  }

  sync_completion_t completion;
  async::PostTask(loop_.dispatcher(), [this, &completion] {
    // After this point, all operations must happen on the loop thread.
    loop_checker_.emplace(fit::thread_checker());
    sync_completion_signal(&completion);
  });
  sync_completion_wait_deadline(&completion, ZX_TIME_INFINITE);

  status = DdkAdd(ddk::DeviceAddArgs("sysmem")
                      .set_flags(DEVICE_ADD_NON_BINDABLE)
                      .set_inspect_vmo(inspector_.DuplicateVmo()));
  if (status != ZX_OK) {
    DRIVER_ERROR("Failed to bind device");
    return status;
  }

  zxlogf(INFO, "sysmem finished initialization");

  return ZX_OK;
}

void Device::ConnectV1(ConnectV1RequestView request, ConnectV1Completer::Sync& completer) {
  async::PostTask(loop_.dispatcher(),
                  [this, allocator_request = std::move(request->allocator_request)]() mutable {
                    // The Allocator is channel-owned / self-owned.
                    Allocator::CreateChannelOwnedV1(allocator_request.TakeChannel(), this);
                  });
}

void Device::ConnectV2(ConnectV2RequestView request, ConnectV2Completer::Sync& completer) {
  async::PostTask(loop_.dispatcher(),
                  [this, allocator_request = std::move(request->allocator_request)]() mutable {
                    // The Allocator is channel-owned / self-owned.
                    Allocator::CreateChannelOwnedV2(allocator_request.TakeChannel(), this);
                  });
}

void Device::SetAuxServiceDirectory(SetAuxServiceDirectoryRequestView request,
                                    SetAuxServiceDirectoryCompleter::Sync& completer) {
  async::PostTask(loop_.dispatcher(), [this, aux_service_directory =
                                                 std::make_shared<sys::ServiceDirectory>(
                                                     request->service_directory.TakeChannel())] {
    // Should the need arise in future, it'd be fine to stash a shared_ptr<aux_service_directory>
    // here if we need it for anything else.  For now we only need it for metrics.
    metrics_.metrics_buffer().SetServiceDirectory(aux_service_directory);
    metrics_.LogUnusedPageCheck(sysmem_metrics::UnusedPageCheckMetricDimensionEvent_Connectivity);
  });
}

zx_status_t Device::CommonSysmemConnectV1(zx::channel allocator_request) {
  // The Allocator is channel-owned / self-owned.
  return async::PostTask(loop_.dispatcher(),
                         [this, allocator_request = std::move(allocator_request)]() mutable {
                           // The Allocator is channel-owned / self-owned.
                           Allocator::CreateChannelOwnedV1(std::move(allocator_request), this);
                         });
}

zx_status_t Device::CommonSysmemConnectV2(zx::channel allocator_request) {
  // The Allocator is channel-owned / self-owned.
  return async::PostTask(loop_.dispatcher(),
                         [this, allocator_request = std::move(allocator_request)]() mutable {
                           // The Allocator is channel-owned / self-owned.
                           Allocator::CreateChannelOwnedV2(std::move(allocator_request), this);
                         });
}

zx_status_t Device::CommonSysmemRegisterHeap(
    uint64_t heap_param, fidl::ClientEnd<fuchsia_sysmem2::Heap> heap_connection) {
  // External heaps should not have bit 63 set but bit 60 must be set.
  if ((heap_param & 0x8000000000000000) || !(heap_param & 0x1000000000000000)) {
    DRIVER_ERROR("Invalid external heap");
    return ZX_ERR_INVALID_ARGS;
  }
  auto heap = safe_cast<fuchsia_sysmem2::HeapType>(heap_param);

  class EventHandler : public fidl::WireAsyncEventHandler<fuchsia_sysmem2::Heap> {
   public:
    void OnRegister(fidl::WireEvent<fuchsia_sysmem2::Heap::OnRegister>* event) override {
      auto properties = fidl::ToNatural(event->properties);
      std::lock_guard checker(*device_->loop_checker_);
      // A heap should not be registered twice.
      ZX_DEBUG_ASSERT(heap_client_.is_valid());
      // This replaces any previously registered allocator for heap. This
      // behavior is preferred as it avoids a potential race-condition during
      // heap restart.
      auto allocator = std::make_shared<ExternalMemoryAllocator>(device_, std::move(heap_client_),
                                                                 std::move(properties));
      weak_associated_allocator_ = allocator;
      device_->allocators_[heap_] = std::move(allocator);
    }

    void on_fidl_error(fidl::UnbindInfo info) override {
      if (!info.is_peer_closed()) {
        DRIVER_ERROR("Heap failed: %s\n", info.FormatDescription().c_str());
      }
    }

    // Clean up heap allocator after |heap_client_| tears down, but only if the
    // heap allocator for this |heap_| is still associated with this handler via
    // |weak_associated_allocator_|.
    ~EventHandler() override {
      std::lock_guard checker(*device_->loop_checker_);
      auto existing = device_->allocators_.find(heap_);
      if (existing != device_->allocators_.end() &&
          existing->second == weak_associated_allocator_.lock())
        device_->allocators_.erase(heap_);
    }

    static void Bind(Device* device, fidl::ClientEnd<fuchsia_sysmem2::Heap> heap_client_end,
                     fuchsia_sysmem2::HeapType heap) {
      auto event_handler = std::unique_ptr<EventHandler>(new EventHandler(device, heap));
      event_handler->heap_client_.Bind(std::move(heap_client_end), device->dispatcher(),
                                       std::move(event_handler));
    }

   private:
    EventHandler(Device* device, fuchsia_sysmem2::HeapType heap) : device_(device), heap_(heap) {}

    Device* const device_;
    fidl::WireSharedClient<fuchsia_sysmem2::Heap> heap_client_;
    const fuchsia_sysmem2::HeapType heap_;
    std::weak_ptr<ExternalMemoryAllocator> weak_associated_allocator_;
  };

  return async::PostTask(loop_.dispatcher(),
                         [this, heap, heap_connection = std::move(heap_connection)]() mutable {
                           std::lock_guard checker(*loop_checker_);
                           EventHandler::Bind(this, std::move(heap_connection), heap);
                         });
}

zx_status_t Device::CommonSysmemRegisterSecureMem(
    fidl::ClientEnd<fuchsia_sysmem::SecureMem> secure_mem_connection) {
  LOG(DEBUG, "sysmem RegisterSecureMem begin");

  current_close_is_abort_ = std::make_shared<std::atomic_bool>(true);

  return async::PostTask(
      loop_.dispatcher(), [this, secure_mem_connection = std::move(secure_mem_connection),
                           close_is_abort = current_close_is_abort_]() mutable {
        std::lock_guard checker(*loop_checker_);
        // This code must run asynchronously for two reasons:
        // 1) It does synchronous IPCs to the secure mem device, so SysmemRegisterSecureMem must
        // have return so the call from the secure mem device is unblocked.
        // 2) It modifies member variables like |secure_mem_| and |heaps_| that should only be
        // touched on |loop_|'s thread.
        auto wait_for_close = std::make_unique<async::Wait>(
            secure_mem_connection.channel().get(), ZX_CHANNEL_PEER_CLOSED, 0,
            async::Wait::Handler([this, close_is_abort](async_dispatcher_t* dispatcher,
                                                        async::Wait* wait, zx_status_t status,
                                                        const zx_packet_signal_t* signal) {
              std::lock_guard checker(*loop_checker_);
              if (*close_is_abort && secure_mem_) {
                // The server end of this channel (the aml-securemem driver) is the driver that
                // listens for suspend(mexec) so that soft reboot can succeed.  If that driver has
                // failed, intentionally force a hard reboot here to get back to a known-good state.
                //
                // TODO(fxbug.dev/98039): When there's any more direct/immediate way to
                // intentionally trigger a hard reboot, switch to that (or just remove this TODO
                // when sysmem terminating directly leads to a hard reboot).
                ZX_PANIC(
                    "secure_mem_ connection unexpectedly lost; secure mem in unknown state; hard "
                    "reboot");
              }
            }));

        // It is safe to call Begin() here before setting up secure_mem_ because handler will either
        // run on current thread (loop_thrd_), or be run after the current task finishes while the
        // loop is shutting down.
        zx_status_t status = wait_for_close->Begin(dispatcher());
        if (status != ZX_OK) {
          DRIVER_ERROR("Device::RegisterSecureMem() failed wait_for_close->Begin()");
          return;
        }

        secure_mem_ = std::make_unique<SecureMemConnection>(std::move(secure_mem_connection),
                                                            std::move(wait_for_close));

        // Else we already ZX_PANIC()ed in wait_for_close.
        ZX_DEBUG_ASSERT(secure_mem_);

        // At this point secure_allocators_ has only the secure heaps that are configured via sysmem
        // (not those configured via the TEE), and the memory for these is not yet protected.  Get
        // the SecureMem properties for these.  At some point in the future it _may_ make sense to
        // have connections to more than one SecureMem driver, but for now we assume that a single
        // SecureMem connection is handling all the secure heaps.  We don't actually protect any
        // range(s) until later.
        for (const auto& [heap_type, allocator] : secure_allocators_) {
          uint64_t phys_base;
          uint64_t size_bytes;
          zx_status_t get_status = allocator->GetPhysicalMemoryInfo(&phys_base, &size_bytes);
          if (get_status != ZX_OK) {
            LOG(WARNING, "get_status != ZX_OK - get_status: %d", get_status);
            return;
          }
          fuchsia_sysmem::SecureHeapAndRange whole_heap;
          whole_heap.heap().emplace(
              safe_cast<fuchsia_sysmem::HeapType>(sysmem::fidl_underlying_cast(heap_type)));
          fuchsia_sysmem::SecureHeapRange range;
          range.physical_address().emplace(phys_base);
          range.size_bytes().emplace(size_bytes);
          whole_heap.range().emplace(std::move(range));
          fidl::Arena arena;
          auto wire_whole_heap = fidl::ToWire(arena, std::move(whole_heap));
          auto get_properties_result =
              secure_mem_->channel()->GetPhysicalSecureHeapProperties(wire_whole_heap);
          if (!get_properties_result.ok()) {
            ZX_ASSERT(!*close_is_abort);
            return;
          }
          if (get_properties_result->is_error()) {
            LOG(WARNING, "GetPhysicalSecureHeapProperties() failed - status: %d",
                get_properties_result->error_value());
            // Don't call set_ready() on secure_allocators_.  Eg. this can happen if securemem TA is
            // not found.
            return;
          }
          ZX_ASSERT(get_properties_result->is_ok());
          const fuchsia_sysmem::SecureHeapProperties& properties =
              fidl::ToNatural(get_properties_result->value()->properties);
          ZX_ASSERT(properties.heap().has_value());
          ZX_ASSERT(properties.heap().value() ==
                    safe_cast<fuchsia_sysmem::HeapType>(sysmem::fidl_underlying_cast(heap_type)));
          ZX_ASSERT(properties.dynamic_protection_ranges().has_value());
          ZX_ASSERT(properties.protected_range_granularity().has_value());
          ZX_ASSERT(properties.max_protected_range_count().has_value());
          ZX_ASSERT(properties.is_mod_protected_range_available().has_value());
          SecureMemControl control;
          control.heap_type = heap_type;
          control.parent = this;
          control.is_dynamic = properties.dynamic_protection_ranges().value();
          control.max_range_count = properties.max_protected_range_count().value();
          control.range_granularity = properties.protected_range_granularity().value();
          control.has_mod_protected_range = properties.is_mod_protected_range_available().value();
          secure_mem_controls_.emplace(heap_type, std::move(control));
        }

        // Now we get the secure heaps that are configured via the TEE.
        auto get_result = secure_mem_->channel()->GetPhysicalSecureHeaps();
        if (!get_result.ok()) {
          // For now this is fatal unless explicitly unregistered, since this case is very
          // unexpected, and in this case rebooting is the most plausible way to get back to a
          // working state anyway.
          ZX_ASSERT(!*close_is_abort);
          return;
        }
        if (get_result->is_error()) {
          LOG(WARNING, "GetPhysicalSecureHeaps() failed - status: %d", get_result->error_value());
          // Don't call set_ready() on secure_allocators_.  Eg. this can happen if securemem TA is
          // not found.
          return;
        }
        ZX_ASSERT(get_result->is_ok());
        const fuchsia_sysmem::SecureHeapsAndRanges& tee_configured_heaps =
            fidl::ToNatural(get_result->value()->heaps);
        ZX_ASSERT(tee_configured_heaps.heaps().has_value());
        ZX_ASSERT(tee_configured_heaps.heaps()->size() != 0);
        for (const auto& heap : *tee_configured_heaps.heaps()) {
          ZX_ASSERT(heap.heap().has_value());
          ZX_ASSERT(heap.ranges().has_value());
          // A tee-configured heap with multiple ranges can be specified by the protocol but is not
          // currently supported by sysmem.
          ZX_ASSERT(heap.ranges()->size() == 1);
          // For now we assume that all TEE-configured heaps are protected full-time, and that they
          // start fully protected.
          constexpr bool kIsAlwaysCpuAccessible = false;
          constexpr bool kIsEverCpuAccessible = false;
          constexpr bool kIsReady = false;
          constexpr bool kCanBeTornDown = true;
          const fuchsia_sysmem::SecureHeapRange& heap_range = heap.ranges()->at(0);
          auto secure_allocator = std::make_unique<ContiguousPooledMemoryAllocator>(
              this, "tee_secure", &heaps_, safe_cast<uint64_t>(heap.heap().value()),
              *heap_range.size_bytes(), kIsAlwaysCpuAccessible, kIsEverCpuAccessible, kIsReady,
              kCanBeTornDown, loop_.dispatcher());
          status = secure_allocator->InitPhysical(heap_range.physical_address().value());
          // A failing status is fatal for now.
          ZX_ASSERT(status == ZX_OK);
          LOG(DEBUG,
              "created secure allocator: heap_type: %08lx base: %016" PRIx64 " size: %016" PRIx64,
              safe_cast<uint64_t>(heap.heap().value()), heap_range.physical_address().value(),
              heap_range.size_bytes().value());
          auto heap_type =
              safe_cast<fuchsia_sysmem2::HeapType>(sysmem::fidl_underlying_cast(*heap.heap()));

          // The only usage of SecureMemControl for a TEE-configured heap is to do ZeroSubRange(),
          // so field values of this SecureMemControl are somewhat degenerate (eg. VDEC).
          SecureMemControl control;
          control.heap_type = heap_type;
          control.parent = this;
          control.is_dynamic = false;
          control.max_range_count = 0;
          control.range_granularity = 0;
          control.has_mod_protected_range = false;
          secure_mem_controls_.emplace(heap_type, std::move(control));

          ZX_ASSERT(secure_allocators_.find(heap_type) == secure_allocators_.end());
          secure_allocators_[heap_type] = secure_allocator.get();
          ZX_ASSERT(allocators_.find(heap_type) == allocators_.end());
          allocators_[heap_type] = std::move(secure_allocator);
        }

        for (const auto& [heap_type, allocator] : secure_allocators_) {
          // The secure_mem_ connection is ready to protect ranges on demand to cover this heap's
          // used ranges.  There are no used ranges yet since the heap wasn't ready until now.
          allocator->set_ready();
        }

        LOG(DEBUG, "sysmem RegisterSecureMem() done (async)");
      });
}

// This call allows us to tell the difference between expected vs. unexpected close of the tee_
// channel.
zx_status_t Device::CommonSysmemUnregisterSecureMem() {
  // By this point, the aml-securemem driver's suspend(mexec) has already prepared for mexec.
  //
  // In this path, the server end of the channel hasn't closed yet, but will be closed shortly after
  // return from UnregisterSecureMem().
  //
  // We set a flag here so that a PEER_CLOSED of the channel won't cause the wait handler to crash.
  *current_close_is_abort_ = false;
  current_close_is_abort_.reset();
  return async::PostTask(loop_.dispatcher(), [this]() {
    std::lock_guard checker(*loop_checker_);
    LOG(DEBUG, "begin UnregisterSecureMem()");
    secure_mem_.reset();
    LOG(DEBUG, "end UnregisterSecureMem()");
  });
}

// "Direct" sysmem banjo is sysmem banjo directly on the sysmem device instead of on the
// "sysmem-banjo" child.  We want clients to at least request "sysmem-banjo" to essentially tag
// places that need to move to sysmem FIDL.  Or ideally, move directly to sysmem FIDL instead of
// sysmem banjo.
//
// By default this only complains up to ~once for direct false, and up to ~once for direct true.
//
// Once all deprecated sysmem banjo is at least dead/unused code (or preferably removed entirely),
// we'll see this warning zero times, at which point we'll put a ZX_PANIC() at the start of this
// function in case we missed any, and then remove banjo handling once we're sure we have zero
// banjo-using clients.
static void WarnOfDeprecatedSysmemBanjo(bool direct) {
  // This might occasionally output ~one extra time, if more than one thread gets involved, but not
  // worth using std::atomic<> since a low number of additional log lines isn't a significant
  // downside.
  static bool warning_logged[2] = {};
  if (warning_logged[direct]) {
    return;
  }
  LOG(WARNING, "Deprecated sysmem banjo detected.  Please switch to sysmem FIDL.  direct: %u",
      direct);
  warning_logged[direct] = true;
}

zx_status_t Device::SysmemConnect(zx::channel allocator_request) {
  WarnOfDeprecatedSysmemBanjo(true);
  return CommonSysmemConnectV1(std::move(allocator_request));
}

zx_status_t Device::SysmemRegisterHeap(uint64_t heap, zx::channel heap_connection) {
  WarnOfDeprecatedSysmemBanjo(true);
  return CommonSysmemRegisterHeap(
      heap, fidl::ClientEnd<fuchsia_sysmem2::Heap>{std::move(heap_connection)});
}

zx_status_t Device::SysmemRegisterSecureMem(zx::channel tee_connection) {
  WarnOfDeprecatedSysmemBanjo(true);
  return CommonSysmemRegisterSecureMem(
      fidl::ClientEnd<fuchsia_sysmem::SecureMem>{std::move(tee_connection)});
}

zx_status_t Device::SysmemUnregisterSecureMem() {
  WarnOfDeprecatedSysmemBanjo(true);
  return CommonSysmemUnregisterSecureMem();
}

const zx::bti& Device::bti() { return bti_; }

// Only use this in cases where we really can't use zx::vmo::create_contiguous() because we must
// specify a specific physical range.
zx_status_t Device::CreatePhysicalVmo(uint64_t base, uint64_t size, zx::vmo* vmo_out) {
  zx::vmo result_vmo;
  // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
  zx::unowned_resource root_resource(get_root_resource());
  zx_status_t status = zx::vmo::create_physical(*root_resource, base, size, &result_vmo);
  if (status != ZX_OK) {
    return status;
  }
  *vmo_out = std::move(result_vmo);
  return ZX_OK;
}

uint32_t Device::pdev_device_info_vid() {
  ZX_DEBUG_ASSERT(pdev_device_info_vid_ != std::numeric_limits<uint32_t>::max());
  return pdev_device_info_vid_;
}

uint32_t Device::pdev_device_info_pid() {
  ZX_DEBUG_ASSERT(pdev_device_info_pid_ != std::numeric_limits<uint32_t>::max());
  return pdev_device_info_pid_;
}

void Device::TrackToken(BufferCollectionToken* token) {
  std::lock_guard checker(*loop_checker_);
  ZX_DEBUG_ASSERT(token->has_server_koid());
  zx_koid_t server_koid = token->server_koid();
  ZX_DEBUG_ASSERT(server_koid != ZX_KOID_INVALID);
  ZX_DEBUG_ASSERT(tokens_by_koid_.find(server_koid) == tokens_by_koid_.end());
  tokens_by_koid_.insert({server_koid, token});
}

void Device::UntrackToken(BufferCollectionToken* token) {
  std::lock_guard checker(*loop_checker_);
  if (!token->has_server_koid()) {
    // The caller is allowed to un-track a token that never saw
    // OnServerKoid().
    return;
  }
  // This is intentionally idempotent, to allow un-tracking from
  // BufferCollectionToken::CloseChannel() as well as from
  // ~BufferCollectionToken().
  tokens_by_koid_.erase(token->server_koid());
}

bool Device::TryRemoveKoidFromUnfoundTokenList(zx_koid_t token_server_koid) {
  std::lock_guard checker(*loop_checker_);
  // unfound_token_koids_ is limited to kMaxUnfoundTokenCount (and likely empty), so a loop over it
  // should be efficient enough.
  for (auto it = unfound_token_koids_.begin(); it != unfound_token_koids_.end(); ++it) {
    if (*it == token_server_koid) {
      unfound_token_koids_.erase(it);
      return true;
    }
  }
  return false;
}

BufferCollectionToken* Device::FindTokenByServerChannelKoid(zx_koid_t token_server_koid) {
  std::lock_guard checker(*loop_checker_);
  auto iter = tokens_by_koid_.find(token_server_koid);
  if (iter == tokens_by_koid_.end()) {
    unfound_token_koids_.push_back(token_server_koid);
    constexpr uint32_t kMaxUnfoundTokenCount = 8;
    while (unfound_token_koids_.size() > kMaxUnfoundTokenCount) {
      unfound_token_koids_.pop_front();
    }
    return nullptr;
  }
  return iter->second;
}

MemoryAllocator* Device::GetAllocator(const fuchsia_sysmem2::BufferMemorySettings& settings) {
  std::lock_guard checker(*loop_checker_);
  if (*settings.heap() == fuchsia_sysmem2::HeapType::kSystemRam &&
      *settings.is_physically_contiguous()) {
    return contiguous_system_ram_allocator_.get();
  }

  auto iter = allocators_.find(*settings.heap());
  if (iter == allocators_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

const fuchsia_sysmem2::HeapProperties& Device::GetHeapProperties(
    fuchsia_sysmem2::HeapType heap) const {
  std::lock_guard checker(*loop_checker_);
  ZX_DEBUG_ASSERT(allocators_.find(heap) != allocators_.end());
  return allocators_.at(heap)->heap_properties();
}

Device::SecureMemConnection::SecureMemConnection(fidl::ClientEnd<fuchsia_sysmem::SecureMem> channel,
                                                 std::unique_ptr<async::Wait> wait_for_close)
    : connection_(std::move(channel)), wait_for_close_(std::move(wait_for_close)) {
  // nothing else to do here
}

const fidl::WireSyncClient<fuchsia_sysmem::SecureMem>& Device::SecureMemConnection::channel()
    const {
  ZX_DEBUG_ASSERT(connection_);
  return connection_;
}

FidlDevice::FidlDevice(zx_device_t* parent, sysmem_driver::Device* sysmem_device,
                       async_dispatcher_t* dispatcher)
    : DdkFidlDeviceBase(parent),
      sysmem_device_(sysmem_device),
      dispatcher_(dispatcher),
      outgoing_(dispatcher) {
  ZX_DEBUG_ASSERT(parent_);
  ZX_DEBUG_ASSERT(sysmem_device_);
}

zx_status_t FidlDevice::Bind() {
  auto status = outgoing_.AddService<fuchsia_hardware_sysmem::Service>(
      fuchsia_hardware_sysmem::Service::InstanceHandler({
          .sysmem = bindings_.CreateHandler(this, dispatcher_, fidl::kIgnoreBindingClosure),
          .allocator_v1 =
              [this](fidl::ServerEnd<fuchsia_sysmem::Allocator> request) {
                zx_status_t status = sysmem_device_->CommonSysmemConnectV1(request.TakeChannel());
                if (status != ZX_OK) {
                  LOG(INFO, "Direct connect to fuchsia_sysmem::Allocator() failed");
                }
              },
          .allocator =
              [this](fidl::ServerEnd<fuchsia_sysmem2::Allocator> request) {
                zx_status_t status = sysmem_device_->CommonSysmemConnectV2(request.TakeChannel());
                if (status != ZX_OK) {
                  LOG(INFO, "Direct connect to fuchsia_sysmem2::Allocator() failed");
                }
              },
      }));

  if (status.is_error()) {
    zxlogf(ERROR, "failed to add FIDL protocol to the outgoing directory (sysmem): %s",
           status.status_string());
    return status.status_value();
  }

  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.status_value();
  }

  status = outgoing_.Serve(std::move(endpoints->server));
  std::array offers = {
      fuchsia_hardware_sysmem::Service::Name,
  };

  zx_status_t add_status =
      DdkAdd(ddk::DeviceAddArgs("sysmem-fidl")
                 .set_flags(DEVICE_ADD_ALLOW_MULTI_COMPOSITE | DEVICE_ADD_MUST_ISOLATE)
                 .set_fidl_service_offers(offers)
                 .set_outgoing_dir(endpoints->client.TakeChannel()));
  if (add_status != ZX_OK) {
    DRIVER_ERROR("Failed to bind FIDL device");
    return add_status;
  }

  return ZX_OK;
}

void FidlDevice::ConnectServer(ConnectServerRequest& request,
                               ConnectServerCompleter::Sync& completer) {
  zx_status_t status =
      sysmem_device_->CommonSysmemConnectV1(request.allocator_request().TakeChannel());
  if (status != ZX_OK) {
    completer.Close(status);
    return;
  }
}

void FidlDevice::ConnectServerV2(ConnectServerV2Request& request,
                                 ConnectServerV2Completer::Sync& completer) {
  if (!request.allocator_request().has_value()) {
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }
  zx_status_t status =
      sysmem_device_->CommonSysmemConnectV2(request.allocator_request()->TakeChannel());
  if (status != ZX_OK) {
    completer.Close(status);
    return;
  }
}

void FidlDevice::RegisterHeap(RegisterHeapRequest& request,
                              RegisterHeapCompleter::Sync& completer) {
  zx_status_t status = sysmem_device_->CommonSysmemRegisterHeap(
      request.heap(), std::move(request.heap_connection()));
  if (status != ZX_OK) {
    completer.Close(status);
    return;
  }
}

void FidlDevice::RegisterSecureMem(RegisterSecureMemRequest& request,
                                   RegisterSecureMemCompleter::Sync& completer) {
  zx_status_t status =
      sysmem_device_->CommonSysmemRegisterSecureMem(std::move(request.secure_mem_connection()));
  if (status != ZX_OK) {
    completer.Close(status);
    return;
  }
}

void FidlDevice::UnregisterSecureMem(UnregisterSecureMemCompleter::Sync& completer) {
  zx_status_t status = sysmem_device_->CommonSysmemUnregisterSecureMem();
  if (status == ZX_OK) {
    completer.Reply(fit::ok());
  } else {
    completer.Reply(fit::error(status));
  }
}

BanjoDevice::BanjoDevice(zx_device_t* parent, sysmem_driver::Device* sysmem_device)
    : DdkBanjoDeviceType(parent), sysmem_device_(sysmem_device) {
  ZX_DEBUG_ASSERT(parent_);
  ZX_DEBUG_ASSERT(sysmem_device_);
}

zx_status_t BanjoDevice::Bind() {
  return DdkAdd(ddk::DeviceAddArgs("sysmem-banjo").set_flags(DEVICE_ADD_ALLOW_MULTI_COMPOSITE));
}

zx_status_t BanjoDevice::SysmemConnect(zx::channel allocator_request) {
  WarnOfDeprecatedSysmemBanjo(false);
  return sysmem_device_->CommonSysmemConnectV1(std::move(allocator_request));
}

zx_status_t BanjoDevice::SysmemRegisterHeap(uint64_t heap, zx::channel heap_connection) {
  WarnOfDeprecatedSysmemBanjo(false);
  return sysmem_device_->CommonSysmemRegisterHeap(
      heap, fidl::ClientEnd<fuchsia_sysmem2::Heap>{std::move(heap_connection)});
}

zx_status_t BanjoDevice::SysmemRegisterSecureMem(zx::channel tee_connection) {
  WarnOfDeprecatedSysmemBanjo(false);
  return sysmem_device_->CommonSysmemRegisterSecureMem(
      fidl::ClientEnd<fuchsia_sysmem::SecureMem>{std::move(tee_connection)});
}

zx_status_t BanjoDevice::SysmemUnregisterSecureMem() {
  WarnOfDeprecatedSysmemBanjo(false);
  return sysmem_device_->CommonSysmemUnregisterSecureMem();
}

}  // namespace sysmem_driver
