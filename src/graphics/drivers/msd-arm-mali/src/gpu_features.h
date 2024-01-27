// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_GPU_FEATURES_H_
#define SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_GPU_FEATURES_H_

#include <lib/inspect/cpp/inspect.h>

#include "magma_util/register_io.h"
#include "src/graphics/drivers/msd-arm-mali/src/mali_register_io.h"
#include "src/graphics/drivers/msd-arm-mali/src/registers.h"
#include "string_printf.h"

struct GpuFeatures {
  static constexpr uint32_t kSuspendSizeOffset = 0x8;
  static constexpr uint32_t kAsPresentOffset = 0x18;
  static constexpr uint32_t kJsPresentOffset = 0x1c;
  static constexpr uint32_t kThreadMaxThreadsOffset = 0xa0;
  static constexpr uint32_t kThreadMaxWorkgroupSizeOffset = 0xa4;
  static constexpr uint32_t kThreadMaxBarrierSizeOffset = 0xa8;
  static constexpr uint32_t kThreadTlsAllocOffset = 0x310;
  static constexpr uint32_t kJsFeaturesOffset = 0xc0;
  static constexpr uint32_t kTextureFeaturesOffset = 0xb0;

  // Shader core present bitmap.
  static constexpr uint32_t kShaderPresentLowOffset = 0x100;
  static constexpr uint32_t kShaderPresentHighOffset = 0x104;

  // Tiler present bitmap.
  static constexpr uint32_t kTilerPresentLowOffset = 0x110;
  static constexpr uint32_t kTilerPresentHighOffset = 0x114;

  // L2 cache present bitmap.
  static constexpr uint32_t kL2PresentLowOffset = 0x120;
  static constexpr uint32_t kL2PresentHighOffset = 0x124;

  // Core stack present bitmap.
  static constexpr uint32_t kStackPresentLowOffset = 0xe00;
  static constexpr uint32_t kStackPresentHighOffset = 0xe04;

  static constexpr uint32_t kMaxJobSlots = 16;
  static constexpr uint32_t kNumTextureFeaturesRegisters = 3;

  registers::GpuId gpu_id;
  registers::L2Features l2_features;
  uint32_t suspend_size;
  registers::TilerFeatures tiler_features;
  registers::MemoryFeatures mem_features;
  registers::MmuFeatures mmu_features;
  uint32_t address_space_present;
  uint32_t job_slot_present;
  registers::ThreadFeatures thread_features;
  uint32_t thread_tls_alloc;
  uint32_t thread_max_threads;
  uint32_t thread_max_workgroup_size;
  uint32_t thread_max_barrier_size;
  registers::CoherencyFeatures coherency_features;

  uint32_t job_slot_features[kMaxJobSlots];
  uint32_t texture_features[kNumTextureFeaturesRegisters];

  uint64_t shader_present;
  uint64_t tiler_present;
  uint64_t l2_present;
  uint64_t stack_present;

  uint32_t job_slot_count;
  uint32_t address_space_count;

  void ReadFrom(mali::RegisterIo* io) {
    gpu_id = registers::GpuId::Get().ReadFrom(io);
    l2_features = registers::L2Features::Get().ReadFrom(io);
    tiler_features = registers::TilerFeatures::Get().ReadFrom(io);
    suspend_size = io->Read<uint32_t>(kSuspendSizeOffset);
    mem_features = registers::MemoryFeatures::Get().ReadFrom(io);
    mmu_features = registers::MmuFeatures::Get().ReadFrom(io);
    address_space_present = io->Read<uint32_t>(kAsPresentOffset);
    job_slot_present = io->Read<uint32_t>(kJsPresentOffset);
    // Defaults to 0 on older GPUs.
    thread_tls_alloc = io->Read<uint32_t>(kThreadTlsAllocOffset);
    thread_max_threads = io->Read<uint32_t>(kThreadMaxThreadsOffset);
    thread_max_workgroup_size = io->Read<uint32_t>(kThreadMaxWorkgroupSizeOffset);
    thread_max_barrier_size = io->Read<uint32_t>(kThreadMaxBarrierSizeOffset);
    thread_features = registers::ThreadFeatures::Get().ReadFrom(io);
    coherency_features = registers::CoherencyFeatures::GetPresent().ReadFrom(io);

    for (uint32_t i = 0; i < kMaxJobSlots; i++)
      job_slot_features[i] = io->Read<uint32_t>(kJsFeaturesOffset + i * 4);

    for (uint32_t i = 0; i < kNumTextureFeaturesRegisters; i++)
      texture_features[i] = io->Read<uint32_t>(kTextureFeaturesOffset + i * 4);

    shader_present = ReadPair(io, kShaderPresentLowOffset);
    tiler_present = ReadPair(io, kTilerPresentLowOffset);
    l2_present = ReadPair(io, kL2PresentLowOffset);
    stack_present = ReadPair(io, kStackPresentLowOffset);

    job_slot_count = __builtin_popcount(job_slot_present);
    address_space_count = __builtin_popcount(address_space_present);
    DASSERT((1 << job_slot_count) - 1 == job_slot_present);
    DASSERT((1 << address_space_count) - 1 == address_space_present);
  }

  void InitializeInspect(inspect::Node* parent) {
    node = parent->CreateChild("features");
#define INIT_INT_PROPERTY(name) node.CreateUint(#name, (name), &properties)
#define INIT_REG_PROPERTY(name) node.CreateUint(#name, (name).reg_value(), &properties)
    INIT_REG_PROPERTY(gpu_id);
    INIT_REG_PROPERTY(l2_features);
    INIT_REG_PROPERTY(tiler_features);
    INIT_INT_PROPERTY(suspend_size);
    INIT_REG_PROPERTY(mem_features);
    INIT_REG_PROPERTY(mmu_features);
    INIT_INT_PROPERTY(address_space_present);
    INIT_INT_PROPERTY(job_slot_present);
    INIT_INT_PROPERTY(thread_tls_alloc);
    INIT_INT_PROPERTY(thread_max_threads);
    INIT_INT_PROPERTY(thread_max_workgroup_size);
    INIT_INT_PROPERTY(thread_max_barrier_size);
    INIT_REG_PROPERTY(thread_features);
    INIT_REG_PROPERTY(coherency_features);

    for (uint32_t i = 0; i < kMaxJobSlots; i++) {
      node.CreateUint(StringPrintf("job_slot_features_%d", i).c_str(), job_slot_features[i],
                      &properties);
    }
    for (uint32_t i = 0; i < kNumTextureFeaturesRegisters; i++) {
      node.CreateUint(StringPrintf("texture_features_%d", i).c_str(), texture_features[i],
                      &properties);
    }

    INIT_INT_PROPERTY(shader_present);
    INIT_INT_PROPERTY(tiler_present);
    INIT_INT_PROPERTY(l2_present);
    INIT_INT_PROPERTY(stack_present);

#undef INIT_INT_PROPERTY
#undef INIT_REG_PROPERTY
  }

 private:
  uint64_t ReadPair(mali::RegisterIo* io, uint32_t low_offset) {
    uint64_t low_word = io->Read<uint32_t>(low_offset);
    uint64_t high_word = io->Read<uint32_t>(low_offset + 4);
    return (high_word << 32) | low_word;
  }

  inspect::Node node;
  inspect::ValueList properties;
};

#endif  // SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_GPU_FEATURES_H_
