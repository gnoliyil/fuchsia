// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/unwinder/benchmark/minidump_memory.h"

#include <stdio.h>

#include <cstdint>
#include <cstdlib>
#include <memory>

#include "lib/syslog/cpp/macros.h"
#include "src/developer/debug/unwinder/module.h"
#include "src/developer/debug/zxdb/symbols/build_id_index.h"

namespace benchmark {

std::string BuildIdToHex(const std::vector<uint8_t>& build_id) {
  FX_CHECK(!build_id.empty());

  // 2 hex characters per 1 byte, so the string size is twice the data size. Hopefully we'll be
  // overwriting the zeros we're filling with.
  std::string ret(build_id.size() * 2, '\0');
  char* pos = ret.data();

  for (const auto& byte : build_id) {
    sprintf(pos, "%02hhx", byte);
    pos += 2;
  }

  return ret;
}

namespace {

class SnapshotMemoryRegion : public MinidumpMemory::MemoryRegion {
 public:
  // Construct a memory region from a crashpad MemorySnapshot. The pointer should always be derived
  // from the minidump_ object, and will thus always share its lifetime.
  explicit SnapshotMemoryRegion(const crashpad::MemorySnapshot* snapshot)
      : MemoryRegion(snapshot->Address(), snapshot->Size()), snapshot_(snapshot) {}
  ~SnapshotMemoryRegion() override = default;

  size_t Read(uint64_t offset, size_t size, void* dst) const override {
    MinidumpReadDelegate d(offset, size, dst);

    if (!snapshot_->Read(&d)) {
      return 0;
    }

    return size;
  }

 private:
  class MinidumpReadDelegate : public crashpad::MemorySnapshot::Delegate {
   public:
    // Construct a delegate object for reading minidump memory regions.
    //
    // Minidump will always give us a pointer to the whole region and its size. We give an offset
    // and size of a portion of that region to read. Then when the MemorySnapshotDelegateRead
    // function is called, just that section will be copied out into the ptr we give here.
    explicit MinidumpReadDelegate(uint64_t offset, size_t size, void* ptr)
        : offset_(offset), size_(size), ptr_(reinterpret_cast<uint8_t*>(ptr)) {}

    bool MemorySnapshotDelegateRead(void* data, size_t size) override {
      if (offset_ + size_ > size) {
        return false;
      }

      auto data_u8 = reinterpret_cast<uint8_t*>(data);
      data_u8 += offset_;

      std::copy(data_u8, data_u8 + size_, ptr_);
      return true;
    }

   private:
    uint64_t offset_;
    size_t size_;
    uint8_t* ptr_;
  };
  const crashpad::MemorySnapshot* snapshot_;
};

class ElfMemoryRegion : public MinidumpMemory::MemoryRegion, public unwinder::Memory {
 public:
  // Construct a memory region from a file. This class is also used directly by unwinder as Memory*.
  explicit ElfMemoryRegion(const std::string& path, uint64_t start_in, size_t size_in,
                           MinidumpMemory::Statistics* stat)
      : MemoryRegion(start_in, size_in), file_(fopen(path.c_str(), "rb")), stat_(stat) {}
  ~ElfMemoryRegion() override { fclose(file_); }

  size_t Read(uint64_t offset, size_t size, void* dst) const override {
    fseek(file_, static_cast<int64_t>(offset), SEEK_SET);
    return fread(dst, 1, size, file_);
  }

  unwinder::Error ReadBytes(uint64_t addr, uint64_t size, void* dst) override {
    // If this class is used directly from the unwinder, stat_ must be updated by ourselves.
    stat_->read_count++;
    stat_->total_read_size += size;
    return Read(addr - start, size, dst) == size ? unwinder::Success()
                                                 : unwinder::Error("short read");
  }

 private:
  FILE* file_;
  MinidumpMemory::Statistics* stat_;
};

}  // namespace

MinidumpMemory::MinidumpMemory(const crashpad::ProcessSnapshotMinidump& minidump) {
  for (const auto& thread : minidump.Threads()) {
    const auto& stack = thread->Stack();
    FX_CHECK(stack);
    regions_.push_back(std::make_unique<SnapshotMemoryRegion>(stack));
  }

  std::string home = std::getenv("HOME");
  zxdb::BuildIDIndex build_id_index;
  build_id_index.AddSymbolIndexFile(home + "/.fuchsia/debug/symbol-index.json");
  build_id_index.AddBuildIdDir(home + "/.fuchsia/debug/symbol-cache");

  for (const auto& module : minidump.Modules()) {
    auto build_id = BuildIdToHex(module->BuildID());
    auto path = build_id_index.EntryForBuildID(build_id).binary;
    FX_CHECK(!path.empty()) << "Cannot find symbol file for " << build_id;
    auto elf_region =
        std::make_unique<ElfMemoryRegion>(path, module->Address(), module->Size(), &statistics_);
    unwinder_modules_.emplace_back(module->Address(), elf_region.get(),
                                   unwinder::Module::AddressMode::kFile);
    regions_.push_back(std::move(elf_region));
  }

  std::sort(regions_.begin(), regions_.end(),
            [](const auto& a, const auto& b) { return a->start < b->start; });
}

size_t MinidumpMemory::Read(uint64_t addr, void* dst, size_t size) {
  for (const auto& region : regions_) {
    if (region->start > addr) {
      return 0;
    }

    if ((region->start + region->size) <= addr) {
      continue;
    }

    size_t offset = addr - region->start;
    size_t to_read = std::min(region->size - offset, size);

    statistics_.read_count++;
    statistics_.total_read_size += size;

    return region->Read(offset, to_read, dst);
  }

  return 0;
}

unwinder::Error MinidumpMemory::ReadBytes(uint64_t addr, uint64_t size, void* dst) {
  return Read(addr, dst, size) == size ? unwinder::Success() : unwinder::Error("short read");
}

}  // namespace benchmark
