// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INSTRUCTIONS_H
#define INSTRUCTIONS_H

#include <stdint.h>

#include <magma_util/instruction_writer.h>
#include <magma_util/short_macros.h>

#include "types.h"

// from intel-gfx-prm-osrc-bdw-vol02a-commandreference-instructions_2.pdf pp.870
class MiNoop {
 public:
  static constexpr uint32_t kDwordCount = 1;
  static constexpr uint32_t kCommandType = 0;

  static void write(magma::InstructionWriter* writer) { writer->Write32(kCommandType); }
};

// from intel-gfx-prm-osrc-bdw-vol02a-commandreference-instructions_2.pdf pp.793
class MiBatchBufferStart {
 public:
  static constexpr uint32_t kDwordCount = 3;
  static constexpr uint32_t kCommandType = 0x31 << 23;
  static constexpr uint32_t kAddressSpacePpgtt = 1 << 8;

  static void write(magma::InstructionWriter* writer, gpu_addr_t gpu_addr,
                    AddressSpaceType address_space_type) {
    writer->Write32(kCommandType | (kDwordCount - 2) |
                    (address_space_type == ADDRESS_SPACE_PPGTT ? kAddressSpacePpgtt : 0));
    writer->Write32(magma::lower_32_bits(gpu_addr));
    writer->Write32(magma::upper_32_bits(gpu_addr));
  }
};

// from intel-gfx-prm-osrc-skl-vol02a-commandreference-instructions.pdf p.906
class MiBatchBufferEnd {
 public:
  static constexpr uint32_t kDwordCount = 1;
  static constexpr uint32_t kCommandType = 0xA << 23;

  static void write(magma::InstructionWriter* writer) { writer->Write32(kCommandType); }
};

// from intel-gfx-prm-osrc-bdw-vol02a-commandreference-instructions_2.pdf pp.940
// https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-tgl-vol02a-commandreference-instructions_0.pdf
// p.1002
class MiLoadDataImmediate {
 public:
  static constexpr uint32_t kCommandType = 0x22 << 23;
  static constexpr uint32_t kForcePosted = 1 << 12;
  static constexpr uint32_t kAddMmioBase = 1 << 19;

  static uint32_t dword_count(uint32_t register_count) { return 2 * register_count + 1; }

  static uint32_t header(uint32_t register_count, bool force_posted) {
    uint32_t header = kCommandType | (dword_count(register_count) - 2);
    if (force_posted)
      header |= kForcePosted;
    return header;
  }

  static void write(magma::InstructionWriter* writer, uint32_t register_offset,
                    uint32_t register_count, uint32_t dword[]) {
    DASSERT((register_offset & 0x3) == 0);
    writer->Write32(header(register_count, /*force_posted=*/false));
    for (uint32_t i = 0; i < register_count; i++) {
      writer->Write32(register_offset + i * sizeof(uint32_t));
      writer->Write32(dword[i]);
    }
  }

  static void write(magma::InstructionWriter* writer, uint32_t count, uint32_t offset[],
                    uint32_t value[]) {
    writer->Write32(kCommandType | dword_count(count) - 2);
    for (uint32_t i = 0; i < count; i++) {
      DASSERT((offset[i] & 0x3) == 0);
      writer->Write32(offset[i]);
      writer->Write32(value[i]);
    }
  }
};

// intel-gfx-prm-osrc-kbl-vol02a-commandreference-instructions.pdf p.996
// "HW implicitly detects the Data size to be Qword or Dword to be
// written to memory based on the command dword length programmed"
// Note: TLB invalidations are implicit on every flush sync since Skylake
// (GFX_MODE bit 13 "Flush TLB invalidation Mode", from Broadwell spec, removed).
// This is also validated empirically (test_hw_exec.cc)
class MiFlush {
 public:
  static constexpr uint32_t kDwordCount = 5;
  static constexpr uint32_t kCommandType = 0 << 29;
  static constexpr uint32_t kCommandOpcode = 0x26 << 23;
  static constexpr uint32_t kPostSyncWriteImmediateBit = 1 << 14;
  static constexpr uint32_t kAddressSpaceGlobalGttBit = 1 << 2;

  static void write(magma::InstructionWriter* writer, uint32_t sequence_number,
                    AddressSpaceType address_space_type, uint64_t gpu_addr) {
    DASSERT((gpu_addr & 0x7) == 0);
    DASSERT((gpu_addr & (1 << 5)) == 0);  // HW bug
    writer->Write32(kCommandType | kCommandOpcode | kPostSyncWriteImmediateBit | (kDwordCount - 2));
    writer->Write32(magma::lower_32_bits(gpu_addr) |
                    (address_space_type == ADDRESS_SPACE_GGTT ? kAddressSpaceGlobalGttBit : 0));
    writer->Write32(magma::upper_32_bits(gpu_addr));
    writer->Write32(sequence_number);
    writer->Write32(0);
  }
};

// intel-gfx-prm-osrc-skl-vol02a-commandreference-instructions.pdf pp.1057
// Note: TLB invalidations are implicit on every flush sync since Skylake
// (GFX_MODE bit 13 "Flush TLB invalidation Mode", from Broadwell spec, removed).
// This is also validated empirically (test_hw_exec.cc)
class MiPipeControl {
 public:
  static constexpr uint32_t kDwordCount = 6;
  static constexpr uint32_t kCommandType = 0x3 << 29;
  static constexpr uint32_t kCommandSubType = 0x3 << 27;
  static constexpr uint32_t k3dCommandOpcode = 0x2 << 24;
  static constexpr uint32_t k3dCommandSubOpcode = 0 << 16;

  static constexpr uint32_t kDcFlushEnableBit = 1 << 5;
  static constexpr uint32_t kIndirectStatePointersDisableBit = 1 << 9;
  static constexpr uint32_t kPostSyncWriteImmediateBit = 1 << 14;
  static constexpr uint32_t kGenericMediaStateClearBit = 1 << 16;
  static constexpr uint32_t kCommandStreamerStallEnableBit = 1 << 20;
  static constexpr uint32_t kAddressSpaceGlobalGttBit = 1 << 24;
  static constexpr uint32_t kAddressSpaceGen9ClearEuBit = 1 << 27;

  static void write(magma::InstructionWriter* writer, uint32_t sequence_number, uint64_t gpu_addr,
                    uint32_t flags) {
    DASSERT((flags &
             ~(kCommandStreamerStallEnableBit | kIndirectStatePointersDisableBit |
               kGenericMediaStateClearBit | kDcFlushEnableBit | kAddressSpaceGen9ClearEuBit)) == 0);
    writer->Write32(kCommandType | kCommandSubType | k3dCommandOpcode | k3dCommandSubOpcode |
                    (kDwordCount - 2));
    writer->Write32(flags | kPostSyncWriteImmediateBit | kAddressSpaceGlobalGttBit);
    writer->Write32(magma::lower_32_bits(gpu_addr));
    writer->Write32(magma::upper_32_bits(gpu_addr));
    writer->Write32(sequence_number);
    writer->Write32(0);
  }
};

// intel-gfx-prm-osrc-skl-vol02a-commandreference-instructions.pdf pp.1010
class MiUserInterrupt {
 public:
  static constexpr uint32_t kDwordCount = 1;
  static constexpr uint32_t kCommandType = 0x2 << 23;

  static void write(magma::InstructionWriter* writer) { writer->Write32(kCommandType); }
};

#endif  // INSTRUCTIONS_H
