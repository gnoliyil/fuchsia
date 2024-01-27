// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_MAGMA_SRC_MAGMA_UTIL_ADDRESS_SPACE_H_
#define SRC_GRAPHICS_LIB_MAGMA_SRC_MAGMA_UTIL_ADDRESS_SPACE_H_

#include <platform_buffer.h>
#include <platform_bus_mapper.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

#include <magma_util/status.h>

#include "accessor.h"

namespace magma {

class AddressSpaceOwner {
 public:
  virtual ~AddressSpaceOwner() = default;
  virtual magma::PlatformBusMapper* GetBusMapper() = 0;
};

template <typename GpuMapping>
class AddressSpace {
 public:
  using GpuMappingAccessor = GpuMappingAccessor<GpuMapping>;
  using Buffer = typename GpuMappingAccessor::Buffer;
  using BufferAccessor = BufferAccessor<Buffer>;

  AddressSpace(AddressSpaceOwner* owner) : owner_(owner) {}

  virtual ~AddressSpace() = default;

  virtual uint64_t Size() const = 0;

  bool CheckIfMappingFits(uint64_t gpu_addr, uint64_t length,
                          uint32_t* guard_page_count_out = nullptr);

  // Maps the given |buffer| to a gpu address created from the |address_space| allocator.
  // The address space must support allocation.
  static std::unique_ptr<GpuMapping> MapBufferGpu(std::shared_ptr<AddressSpace> address_space,
                                                  std::shared_ptr<Buffer> buffer, uint64_t offset,
                                                  uint64_t length);

  static std::unique_ptr<GpuMapping> MapBufferGpu(std::shared_ptr<AddressSpace> address_space,
                                                  std::shared_ptr<Buffer> buffer) {
    return MapBufferGpu(address_space, buffer, 0,
                        BufferAccessor::platform_buffer(buffer.get())->size());
  }

  // Maps the given |buffer| at the given gpu address.
  static magma::Status MapBufferGpu(std::shared_ptr<AddressSpace> address_space,
                                    std::shared_ptr<Buffer> buffer, uint64_t gpu_addr,
                                    uint64_t page_offset, uint64_t page_count,
                                    std::shared_ptr<GpuMapping>* gpu_mapping_out);

  magma::Status GrowMapping(GpuMapping* mapping, uint64_t page_count);

  std::shared_ptr<GpuMapping> FindGpuMapping(uint64_t gpu_addr) const;

  // Returns a gpu mapping for the given buffer starting at the given offset if the mapping
  // length is at least the given length.
  std::shared_ptr<GpuMapping> FindGpuMapping(magma::PlatformBuffer* buffer, uint64_t offset,
                                             uint64_t length) const;

  bool AddMapping(std::shared_ptr<GpuMapping> gpu_mapping);

  bool ReleaseMapping(magma::PlatformBuffer* buffer, uint64_t gpu_addr,
                      std::shared_ptr<GpuMapping>* mapping_out);

  void ReleaseBuffer(magma::PlatformBuffer* buffer,
                     std::vector<std::shared_ptr<GpuMapping>>* released_mappings_out);

  // Returns the maximum number of guard pages that should be inserted after a mapping.
  virtual uint64_t MaxGuardPageCount() { return 0; }

  // By default the AddressSpace will perform a bus mapping first then call Insert(addr,
  // bus_mapping); however, some address spaces may require an external actor to perform the bus
  // mapping, so if this returns false then Insert(addr, buffer, page_offset, page_count) will be
  // called instead.
  virtual bool InsertWithBusMapping() { return true; }

  // Allocates space and returns an address to the start of the allocation.
  // May return false if the AddressSpace doesn't support allocation.
  bool Alloc(size_t size, uint8_t align_pow2, uint64_t* addr_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    return AllocLocked(size, align_pow2, addr_out);
  }

  // Releases the allocation at the given address.
  bool Free(uint64_t addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FreeLocked(addr);
  }

  // Inserts the pages for the given buffer into page table entries for the allocation at the
  // given address.
  bool Insert(uint64_t addr, magma::PlatformBusMapper::BusMapping* bus_mapping,
              uint32_t guard_page_count = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    return InsertLocked(addr, bus_mapping, guard_page_count);
  }

  // Inserts without assuming a bus mapping; used if InsertWithBusMapping() is false.
  bool Insert(uint64_t addr, magma::PlatformBuffer* buffer, uint64_t page_offset,
              uint64_t page_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    return InsertLocked(addr, buffer, page_offset, page_count);
  }

  // Clears the page table entries for the allocation at the given address.
  bool Clear(uint64_t addr, magma::PlatformBusMapper::BusMapping* bus_mapping) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ClearLocked(addr, bus_mapping);
  }

 protected:
  virtual bool AllocLocked(size_t size, uint8_t align_pow2, uint64_t* addr_out) {
    return MAGMA_DRETF(false, "AllocLocked not implemented");
  }
  virtual bool FreeLocked(uint64_t addr) {
    return MAGMA_DRETF(false, "FreeLocked not implemented");
  }

  virtual bool ClearLocked(uint64_t addr, magma::PlatformBusMapper::BusMapping* bus_mapping) = 0;

  virtual bool InsertLocked(uint64_t addr, magma::PlatformBusMapper::BusMapping* bus_mapping,
                            uint32_t guard_page_count) = 0;
  virtual bool InsertLocked(uint64_t addr, magma::PlatformBuffer* buffer, uint64_t page_offset,
                            uint64_t page_count) {
    return MAGMA_DRETF(false, "InsertLocked without bus mapping not implemented");
  }

 private:
  AddressSpaceOwner* owner_;

  using map_container_t = std::map<uint64_t, std::shared_ptr<GpuMapping>>;

  // Container of gpu mappings by address
  map_container_t mappings_;

  // Container of references to entries in mappings_ by buffer;
  // useful for cleaning up mappings when connections go away, and when
  // buffers are released.
  std::unordered_multimap<magma::PlatformBuffer*, typename map_container_t::iterator>
      mappings_by_buffer_;

  // Used to keep mutually exclusive access to Alloc, Free, Insert, Clear.
  std::mutex mutex_;
};

template <typename GpuMapping>
std::unique_ptr<GpuMapping> AddressSpace<GpuMapping>::MapBufferGpu(
    std::shared_ptr<AddressSpace> address_space, std::shared_ptr<Buffer> buffer, uint64_t offset,
    uint64_t length) {
  MAGMA_DASSERT(address_space);
  MAGMA_DASSERT(buffer);

  auto platform_buffer = BufferAccessor::platform_buffer(buffer.get());

  uint64_t aligned_size = magma::round_up(length, magma::page_size());

  if (!magma::is_page_aligned(offset))
    return MAGMA_DRETP(nullptr, "offset (0x%lx) not page aligned", offset);

  if (offset + aligned_size > platform_buffer->size())
    return MAGMA_DRETP(nullptr, "offset (0x%lx) + aligned_size (0x%lx) > buffer size (0x%lx)",
                       offset, aligned_size, platform_buffer->size());

  if (aligned_size > address_space->Size())
    return MAGMA_DRETP(nullptr, "aligned_size (0x%lx) > address space size (0x%lx)", aligned_size,
                       address_space->Size());

  uint64_t align_pow2;
  if (!magma::get_pow2(magma::page_size(), &align_pow2))
    return MAGMA_DRETP(nullptr, "page_size is not power of 2");

  // Casting to uint8_t below
  MAGMA_DASSERT((align_pow2 & ~0xFF) == 0);
  MAGMA_DASSERT(magma::is_page_aligned(aligned_size));

  uint64_t gpu_addr;
  if (!address_space->Alloc(aligned_size, static_cast<uint8_t>(align_pow2), &gpu_addr))
    return MAGMA_DRETP(nullptr, "failed to allocate gpu address");

  DLOG("MapBufferGpu offset 0x%lx aligned_size 0x%lx allocated gpu_addr 0x%lx", offset,
       aligned_size, gpu_addr);

  uint64_t page_offset = offset / magma::page_size();
  uint64_t page_count = aligned_size / magma::page_size();

  uint32_t guard_page_count = 0;
  if (!address_space->CheckIfMappingFits(gpu_addr, aligned_size, &guard_page_count))
    return MAGMA_DRETP(nullptr, "Mapping doesn't fit");

  std::unique_ptr<magma::PlatformBusMapper::BusMapping> bus_mapping;

  if (address_space->InsertWithBusMapping()) {
    bus_mapping = address_space->owner_->GetBusMapper()->MapPageRangeBus(platform_buffer,
                                                                         page_offset, page_count);
    if (!bus_mapping)
      return MAGMA_DRETP(nullptr, "failed to bus map the page range");

    if (!address_space->Insert(gpu_addr, bus_mapping.get(), guard_page_count))
      return MAGMA_DRETP(nullptr, "failed to insert into address_space");
  } else {
    if (!address_space->Insert(gpu_addr, platform_buffer, page_offset, page_count))
      return MAGMA_DRETP(nullptr, "failed to insert into address_space");
  }

  return GpuMappingAccessor::Create(address_space, buffer, offset, aligned_size, gpu_addr,
                                    std::move(bus_mapping));
}

template <typename GpuMapping>
bool AddressSpace<GpuMapping>::CheckIfMappingFits(uint64_t gpu_addr, uint64_t length,
                                                  uint32_t* guard_page_count_out) {
  if (gpu_addr >= Size())
    return MAGMA_DRETF(false, "gpu_addr 0x%lx out of range 0x%lx", gpu_addr, Size());

  uint64_t mapping_space_available = Size() - gpu_addr;

  auto iter = mappings_.upper_bound(gpu_addr);
  if (iter != mappings_.end()) {
    mapping_space_available = GpuMappingAccessor::gpu_addr(iter->second.get()) - gpu_addr;
  }

  if (mapping_space_available < length)
    return MAGMA_DRETF(false,
                       "Insufficient mapping space available 0x%lx (gpu_addr 0x%lx length 0x%lx)",
                       mapping_space_available, gpu_addr, length);

  // Find the mapping with the highest VA that's <= this.
  if (iter != mappings_.begin()) {
    --iter;
    if (GpuMappingAccessor::gpu_addr(iter->second.get()) +
            GpuMappingAccessor::length(iter->second.get()) >
        gpu_addr)
      return MAGMA_DRETF(false, "Mapping would overlap the preceding mapping");
  }

  if (guard_page_count_out) {
    *guard_page_count_out = magma::to_uint32(
        std::min(MaxGuardPageCount(), (mapping_space_available - length) / magma::page_size()));
  }

  return true;
}

template <typename GpuMapping>
magma::Status AddressSpace<GpuMapping>::MapBufferGpu(std::shared_ptr<AddressSpace> address_space,
                                                     std::shared_ptr<Buffer> buffer,
                                                     uint64_t gpu_addr, uint64_t page_offset,
                                                     uint64_t page_count,
                                                     std::shared_ptr<GpuMapping>* gpu_mapping_out) {
  MAGMA_DASSERT(address_space);
  MAGMA_DASSERT(buffer);

  auto platform_buffer = BufferAccessor::platform_buffer(buffer.get());

  if (!magma::is_page_aligned(gpu_addr))
    return MAGMA_DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "gpu_addr 0x%lx not page aligned", gpu_addr);

  if ((page_offset + page_count) * magma::page_size() > platform_buffer->size())
    return MAGMA_DRET_MSG(MAGMA_STATUS_INVALID_ARGS,
                          "page_offset (%lu) + page_count (%lu) > buffer size (0x%lx)", page_offset,
                          page_count, platform_buffer->size());

  uint32_t guard_page_count = 0;

  if (!address_space->CheckIfMappingFits(gpu_addr, page_count * magma::page_size(),
                                         &guard_page_count))
    return MAGMA_DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "Mapping doesn't fit");

  std::unique_ptr<magma::PlatformBusMapper::BusMapping> bus_mapping;

  if (address_space->InsertWithBusMapping()) {
    bus_mapping = address_space->owner_->GetBusMapper()->MapPageRangeBus(platform_buffer,
                                                                         page_offset, page_count);
    if (!bus_mapping)
      return MAGMA_DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "failed to map page range to bus");

    if (!address_space->Insert(gpu_addr, bus_mapping.get(), guard_page_count))
      return MAGMA_DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "failed to insert into address_space");
  } else {
    if (!address_space->Insert(gpu_addr, platform_buffer, page_offset, page_count))
      return MAGMA_DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "failed to insert into address_space");
  }

  *gpu_mapping_out =
      GpuMappingAccessor::Create(address_space, buffer, page_offset * magma::page_size(),
                                 page_count * magma::page_size(), gpu_addr, std::move(bus_mapping));

  return MAGMA_STATUS_OK;
}

template <typename GpuMapping>
std::shared_ptr<GpuMapping> AddressSpace<GpuMapping>::FindGpuMapping(magma::PlatformBuffer* buffer,
                                                                     uint64_t offset,
                                                                     uint64_t length) const {
  MAGMA_DASSERT(buffer);

  auto range = mappings_by_buffer_.equal_range(buffer);
  for (auto iter = range.first; iter != range.second; iter++) {
    auto& mapping = iter->second->second;
    const uint64_t aligned_size = magma::round_up(length, magma::page_size());
    if (GpuMappingAccessor::offset(mapping.get()) == offset &&
        GpuMappingAccessor::length(mapping.get()) >= aligned_size)
      return mapping;
  }

  return nullptr;
}

template <typename GpuMapping>
std::shared_ptr<GpuMapping> AddressSpace<GpuMapping>::FindGpuMapping(uint64_t gpu_addr) const {
  auto iter = mappings_.find(gpu_addr);
  return (iter != mappings_.end()) ? iter->second : nullptr;
}

template <typename GpuMapping>
bool AddressSpace<GpuMapping>::AddMapping(std::shared_ptr<GpuMapping> gpu_mapping) {
  uint64_t gpu_addr = GpuMappingAccessor::gpu_addr(gpu_mapping.get());

  // We should already have determined that the mapping fits.
  if (magma::kDebug) {
    if (!CheckIfMappingFits(gpu_addr, GpuMappingAccessor::length(gpu_mapping.get())))
      return MAGMA_DRETF(false, "Mapping overlaps existing mapping");
  }

  std::pair<typename map_container_t::iterator, bool> result =
      mappings_.insert({gpu_addr, gpu_mapping});
  MAGMA_DASSERT(result.second);

  mappings_by_buffer_.insert(
      {BufferAccessor::platform_buffer(gpu_mapping->buffer()), result.first});

  return true;
}

template <typename GpuMapping>
bool AddressSpace<GpuMapping>::ReleaseMapping(magma::PlatformBuffer* buffer, uint64_t gpu_addr,
                                              std::shared_ptr<GpuMapping>* mapping_out) {
  // TODO(fxbug.dev/111304) - restore guard pages for adjacent mapping?
  auto range = mappings_by_buffer_.equal_range(buffer);
  for (auto iter = range.first; iter != range.second; iter++) {
    std::shared_ptr<GpuMapping> gpu_mapping = iter->second->second;
    if (GpuMappingAccessor::gpu_addr(gpu_mapping.get()) == gpu_addr) {
      mappings_.erase(iter->second);
      mappings_by_buffer_.erase(iter);
      *mapping_out = std::move(gpu_mapping);
      return true;
    }
  }
  return MAGMA_DRETF(false, "failed to remove mapping");
}

template <typename GpuMapping>
void AddressSpace<GpuMapping>::ReleaseBuffer(
    magma::PlatformBuffer* buffer,
    std::vector<std::shared_ptr<GpuMapping>>* released_mappings_out) {
  released_mappings_out->clear();

  // TODO(fxbug.dev/111304) - restore guard pages for adjacent mapping?
  auto range = mappings_by_buffer_.equal_range(buffer);
  for (auto iter = range.first; iter != range.second;) {
    released_mappings_out->emplace_back(std::move(iter->second->second));
    mappings_.erase(iter->second);
    iter = mappings_by_buffer_.erase(iter);
  }
}

template <typename GpuMapping>
magma::Status AddressSpace<GpuMapping>::GrowMapping(GpuMapping* mapping, uint64_t page_increment) {
  const uint64_t length = GpuMappingAccessor::length(mapping) + page_increment * magma::page_size();

  uint64_t gpu_addr = GpuMappingAccessor::gpu_addr(mapping);
  uint32_t guard_page_count = 0;

  if (!CheckIfMappingFits(gpu_addr, length, &guard_page_count))
    return MAGMA_DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "Mapping doesn't fit");

  auto platform_buffer = BufferAccessor::platform_buffer(GpuMappingAccessor::buffer(mapping));

  uint64_t offset = GpuMappingAccessor::offset(mapping);
  if (offset + length > platform_buffer->size())
    return MAGMA_DRET_MSG(MAGMA_STATUS_INVALID_ARGS,
                          "offset (%lu) + length (%lu) > buffer size (0x%lx)", offset, length,
                          platform_buffer->size());

  MAGMA_DASSERT(InsertWithBusMapping());

  std::unique_ptr<magma::PlatformBusMapper::BusMapping> bus_mapping =
      owner_->GetBusMapper()->MapPageRangeBus(
          platform_buffer, (offset + length) / magma::page_size(), page_increment);
  if (!bus_mapping)
    return MAGMA_DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "failed to map page range to bus");

  if (!Insert(gpu_addr + GpuMappingAccessor::length(mapping), bus_mapping.get(), guard_page_count))
    return MAGMA_DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "failed to insert into address_space");

  mapping->Grow(std::move(bus_mapping));

  return MAGMA_STATUS_OK;
}

}  // namespace magma

#endif  // SRC_GRAPHICS_LIB_MAGMA_SRC_MAGMA_UTIL_ADDRESS_SPACE_H_
