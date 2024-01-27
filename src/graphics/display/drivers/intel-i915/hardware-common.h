// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_HARDWARE_COMMON_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_HARDWARE_COMMON_H_

#include <lib/stdcompat/span.h>

#include <array>

namespace tgl_registers {

enum class Platform {
  kSkylake,
  kKabyLake,
  kTigerLake,
  kTestDevice,
};

}  // namespace tgl_registers

namespace i915_tgl {

enum DdiId {
  DDI_A = 0,
  DDI_B,
  DDI_C,
  DDI_D,
  DDI_E,

  DDI_TC_1 = DDI_D,
  DDI_TC_2,
  DDI_TC_3,
  DDI_TC_4,
  DDI_TC_5,
  DDI_TC_6,
};

namespace internal {

constexpr std::array kDdisKabyLake = {
    DDI_A, DDI_B, DDI_C, DDI_D, DDI_E,
};

constexpr std::array kDdisTigerLake = {
    DDI_A, DDI_B, DDI_C, DDI_TC_1, DDI_TC_2, DDI_TC_3, DDI_TC_4, DDI_TC_5, DDI_TC_6,
};

}  // namespace internal

template <tgl_registers::Platform P>
constexpr cpp20::span<const DdiId> DdiIds() {
  switch (P) {
    case tgl_registers::Platform::kKabyLake:
    case tgl_registers::Platform::kSkylake:
    case tgl_registers::Platform::kTestDevice:
      return internal::kDdisKabyLake;
    case tgl_registers::Platform::kTigerLake:
      return internal::kDdisTigerLake;
  }
}

// TODO(fxbug.dev/109278): Support Transcoder D on Tiger Lake.
enum TranscoderId {
  TRANSCODER_A = 0,
  TRANSCODER_B,
  TRANSCODER_C,
  TRANSCODER_EDP,
};

namespace internal {

constexpr std::array kTranscodersKabyLake = {
    TRANSCODER_A,
    TRANSCODER_B,
    TRANSCODER_C,
    TRANSCODER_EDP,
};

constexpr std::array kTranscodersTigerLake = {
    TRANSCODER_A,
    TRANSCODER_B,
    TRANSCODER_C,
};

}  // namespace internal

template <tgl_registers::Platform P>
constexpr cpp20::span<const TranscoderId> TranscoderIds() {
  switch (P) {
    case tgl_registers::Platform::kKabyLake:
    case tgl_registers::Platform::kSkylake:
    case tgl_registers::Platform::kTestDevice:
      return internal::kTranscodersKabyLake;
    case tgl_registers::Platform::kTigerLake:
      return internal::kTranscodersTigerLake;
  }
}

// TODO(fxbug.dev/109278): Support Pipe D on Tiger Lake.
enum PipeId {
  PIPE_A = 0,
  PIPE_B,
  PIPE_C,
  PIPE_INVALID,
};

namespace internal {

constexpr std::array kPipeIdsKabyLake = {
    PIPE_A,
    PIPE_B,
    PIPE_C,
};

constexpr std::array kPipeIdsTigerLake = {
    PIPE_A,
    PIPE_B,
    PIPE_C,
};

}  // namespace internal

template <tgl_registers::Platform P>
constexpr cpp20::span<const PipeId> PipeIds() {
  switch (P) {
    case tgl_registers::Platform::kKabyLake:
    case tgl_registers::Platform::kSkylake:
    case tgl_registers::Platform::kTestDevice:
      return internal::kPipeIdsKabyLake;
    case tgl_registers::Platform::kTigerLake:
      return internal::kPipeIdsTigerLake;
  }
}

enum PllId {
  DPLL_INVALID = -1,
  DPLL_0 = 0,
  DPLL_1,
  DPLL_2,
  DPLL_3,

  DPLL_TC_1,
  DPLL_TC_2,
  DPLL_TC_3,
  DPLL_TC_4,
  DPLL_TC_5,
  DPLL_TC_6,
};

namespace internal {

constexpr std::array kPllIdsKabyLake = {
    DPLL_0,
    DPLL_1,
    DPLL_2,
    DPLL_3,
};

// TODO(fxbug.dev/110351): Add support for DPLL4.
constexpr std::array kPllIdsTigerLake = {
    DPLL_0, DPLL_1, DPLL_2, DPLL_TC_1, DPLL_TC_2, DPLL_TC_3, DPLL_TC_4, DPLL_TC_5, DPLL_TC_6,
};

}  // namespace internal

template <tgl_registers::Platform P>
constexpr cpp20::span<const PllId> PllIds() {
  switch (P) {
    case tgl_registers::Platform::kSkylake:
    case tgl_registers::Platform::kKabyLake:
    case tgl_registers::Platform::kTestDevice:
      return internal::kPllIdsKabyLake;
    case tgl_registers::Platform::kTigerLake:
      return internal::kPllIdsTigerLake;
  }
}

}  // namespace i915_tgl

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_INTEL_I915_HARDWARE_COMMON_H_
