// Copyright 2022 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "vm/compression.h"

#include <lib/boot-options/boot-options.h>
#include <lib/counters.h>

#include <ktl/algorithm.h>
#include <vm/physmap.h>
#include <vm/pmm.h>

namespace {

// We always add zx_ticks_t to any data that we store, so ensure that the maximum size of the
// compressed data combined with that would not require us to store more than a page.
constexpr size_t ensure_threshold(size_t threshold) {
  if (threshold + sizeof(zx_ticks_t) > PAGE_SIZE) {
    return PAGE_SIZE - sizeof(zx_ticks_t);
  }
  return threshold;
}

constexpr int bucket_for_ticks(zx_ticks_t start, zx_ticks_t end) {
  // Turn the ticks range into seconds. As we want whole seconds we are happy to tolerate the
  // rounding behavior of integer division here.
  const uint64_t seconds = end > start ? (end - start) / ticks_per_second() : 0;
  // Calculate the bucket and return it, clamping to the maximum number of buckets.
  return ktl::min(seconds == 0 ? 0 : __builtin_ctzl(seconds) + 1, VmCompression::kNumLogBuckets);
}

}  // namespace

VmCompression::CompressorGuard::~CompressorGuard() {
  // Ensure the compressor instance was not left in an in progress state before returning it.
  ASSERT(instance_.IsIdle());
}

VmCompression::CompressorGuard VmCompression::AcquireCompressor() {
  Guard<Mutex> guard_{&instance_lock_};
  return CompressorGuard(instance_, ktl::move(guard_));
}

VmCompression::VmCompression(fbl::RefPtr<VmCompressedStorage> storage,
                             fbl::RefPtr<VmCompressionStrategy> strategy,
                             size_t compression_threshold)
    : storage_(ktl::move(storage)),
      strategy_(ktl::move(strategy)),
      compression_threshold_(ensure_threshold(compression_threshold)),
      instance_(*this, kTempReferenceValue) {
  ASSERT(storage_);
  ASSERT(strategy_);
  // Ensure we can steal space to store the compression timestamp.
  ASSERT(compression_threshold_ + sizeof(zx_ticks_t) <= PAGE_SIZE);
}

VmCompression::~VmCompression() {
  if (buffer_page_) {
    pmm_free_page(buffer_page_);
  }
}

VmCompression::CompressResult VmCompression::Compress(const void* page_src, zx_ticks_t now) {
  // Take the compression lock so we can use the buffer_page_.
  Guard<Mutex> guard{&compression_lock_};

  // Ensure buffer page exists.
  if (!buffer_page_) {
    // Explicitly do not use delayed allocation since we might be under memory pressure.
    zx_status_t status = pmm_alloc_page(0, &buffer_page_);
    if (status != ZX_OK) {
      return FailTag{};
    }
    buffer_page_->set_state(vm_page_state::ZRAM);
  }

  compression_attempts_.fetch_add(1);

  // Compress into the buffer page, measuring the time taken to do so.
  const zx_duration_t start_runtime = Thread::Current::Get()->Runtime();
  void* buffer_ptr = paddr_to_physmap(buffer_page_->paddr());
  auto result = strategy_->Compress(page_src, buffer_ptr, compression_threshold_);
  const zx_duration_t end_runtime = Thread::Current::Get()->Runtime();
  if (likely(end_runtime > start_runtime)) {
    compression_time_.fetch_add(end_runtime - start_runtime);
  }

  // The result is a different type so we need to convert, and it gives us a chance to record
  // statistics.
  if (ktl::holds_alternative<FailTag>(result)) {
    compression_fail_.fetch_add(1);
    return FailTag{};
  }
  if (ktl::holds_alternative<ZeroTag>(result)) {
    compression_zero_page_.fetch_add(1);
    return ZeroTag{};
  }

  // Have actual data to store.
  DEBUG_ASSERT(ktl::holds_alternative<size_t>(result));
  const size_t compressed_size = *ktl::get_if<size_t>(&result);
  DEBUG_ASSERT(compressed_size > 0 && compressed_size <= compression_threshold_);

  // Store the current ticks for tracking how long pages remain compressed. We had previously
  // validated in the constructor that we would always have space on the page.
  const size_t storage_size = compressed_size + sizeof(zx_ticks_t);
  DEBUG_ASSERT(storage_size <= PAGE_SIZE);
  *reinterpret_cast<zx_ticks_t*>(reinterpret_cast<uintptr_t>(buffer_ptr) + compressed_size) = now;

  // Store the data, it takes ownership of the buffer_page_ and might return ownership of a page.
  auto [maybe_ref, page] = storage_->Store(buffer_page_, storage_size);
  buffer_page_ = page;

  if (auto ref = maybe_ref) {
    // Make sure the storage system never produced the temp reference.
    ASSERT(!IsTempReference(*ref));
    compression_success_.fetch_add(1);
    return *ref;
  }
  compression_fail_.fetch_add(1);
  return FailTag{};
}

void VmCompression::Decompress(CompressedRef ref, void* page_dest, zx_ticks_t now) {
  if (unlikely(IsTempReference(ref))) {
    DecompressTempReference(ref, page_dest);
    return;
  }

  decompressions_.fetch_add(1);

  // Lookup the data so we can decompress out.
  auto [src, len] = storage_->CompressedData(ref);

  // pull out the timestamp and determine how long this was compressed for.
  DEBUG_ASSERT(len >= sizeof(zx_ticks_t));
  const zx_ticks_t compressed_ticks =
      *reinterpret_cast<zx_ticks_t*>(reinterpret_cast<uintptr_t>(src) + len - sizeof(zx_ticks_t));
  const int bucket = bucket_for_ticks(compressed_ticks, now);
  decompressions_within_log_seconds_[bucket].fetch_add(1);

  // Decompress the data, excluding our timestamp, and measure how long decompression takes.
  const zx_duration_t start_runtime = Thread::Current::Get()->Runtime();
  strategy_->Decompress(src, len - sizeof(zx_ticks_t), page_dest);
  const zx_duration_t end_runtime = Thread::Current::Get()->Runtime();
  if (end_runtime > start_runtime) {
    decompression_time_.fetch_add(end_runtime - start_runtime);
  }

  // Now that decompression is finished, free the backing memory.
  storage_->Free(ref);
}

void VmCompression::Free(CompressedRef ref) {
  if (unlikely(IsTempReference(ref))) {
    FreeTempReference(ref);
    return;
  }
  storage_->Free(ref);
  decompression_skipped_.fetch_add(1);
}

VmCompression::Stats VmCompression::GetStats() const {
  VmCompression::Stats stats =
      VmCompression::Stats{.memory_usage = storage_->GetMemoryUsage(),
                           .compression_time = compression_time_,
                           .decompression_time = decompression_time_,
                           .total_page_compression_attempts = compression_attempts_,
                           .failed_page_compression_attempts = compression_fail_,
                           .total_page_decompressions = decompressions_,
                           .compressed_page_evictions = decompression_skipped_};
  for (int i = 0; i < kNumLogBuckets; i++) {
    stats.pages_decompressed_within_log_seconds[i] = decompressions_within_log_seconds_[i];
  }
  return stats;
}

void VmCompression::Dump() const {
  printf("[zram]: Compression / decompression time %" PRIi64 "/%" PRIi64 " ns\n",
         compression_time_.load(), decompression_time_.load());
  printf("[zram]: Compression attempts: %zu success: %zu zero page: %zu failed: %zu\n",
         compression_attempts_.load(), compression_success_.load(), compression_zero_page_.load(),
         compression_fail_.load());
  printf(
      "[zram]: Total decompressions: %zu skipped: %zu within log seconds counts: %zu, %zu, %zu, %zu, %zu, %zu, %zu, %zu\n",
      decompressions_.load(), decompression_skipped_.load(),
      decompressions_within_log_seconds_[0].load(), decompressions_within_log_seconds_[1].load(),
      decompressions_within_log_seconds_[2].load(), decompressions_within_log_seconds_[3].load(),
      decompressions_within_log_seconds_[4].load(), decompressions_within_log_seconds_[5].load(),
      decompressions_within_log_seconds_[6].load(), decompressions_within_log_seconds_[7].load());
  strategy_->Dump();
  storage_->Dump();
}

// static
fbl::RefPtr<VmCompression> VmCompression::CreateDefault() {
  // TODO(fxbug.dev/60238)
  return nullptr;
}

// These need to disable analysis as there is a requirement on the caller that the lock for the VMO
// who created this temporary reference is held, however at this point here we have no way to refer
// to that lock.
ktl::optional<vm_page_t*> VmCompression::MoveTempReference(CompressedRef ref)
    TA_NO_THREAD_SAFETY_ANALYSIS {
  DEBUG_ASSERT(IsTempReference(ref));
  // The owner of the temp ref is the owner as page. So if we are seeing the temporary reference
  // then we know page cannot progress (i.e. FinalizeState can be called), so we can safely
  // perform the copy.
  ASSERT(instance_.using_temp_reference_);
  ASSERT(instance_.page_);
  ASSERT(instance_.spare_page_);
  void* addr = paddr_to_physmap(instance_.spare_page_->paddr());
  ASSERT(addr);
  Decompress(ref, addr);
  vm_page_t* ret = instance_.spare_page_;
  instance_.spare_page_ = nullptr;
  return ret;
}

void VmCompression::FreeTempReference(CompressedRef ref) TA_NO_THREAD_SAFETY_ANALYSIS {
  DEBUG_ASSERT(IsTempReference(ref));
  ASSERT(instance_.using_temp_reference_);
  ASSERT(instance_.page_);
  instance_.using_temp_reference_ = false;
}

void VmCompression::DecompressTempReference(CompressedRef ref,
                                            void* page_dest) TA_NO_THREAD_SAFETY_ANALYSIS {
  DEBUG_ASSERT(IsTempReference(ref));
  ASSERT(instance_.using_temp_reference_);
  ASSERT(instance_.page_);
  void* addr = paddr_to_physmap(instance_.page_->paddr());
  ASSERT(addr);
  memcpy(page_dest, addr, PAGE_SIZE);
  FreeTempReference(ref);
}
