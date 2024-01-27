// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_AMLOGIC_DISPLAY_COMMON_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_AMLOGIC_DISPLAY_COMMON_H_

#include <lib/ddk/debug.h>

#include <hwreg/mmio.h>

#include "src/graphics/display/lib/api-types-cpp/display-id.h"

#define DISPLAY_MASK(start, count) (((1 << (count)) - 1) << (start))
#define DISPLAY_SET_MASK(mask, start, count, value) \
  ((mask & ~DISPLAY_MASK(start, count)) | (((value) << (start)) & DISPLAY_MASK(start, count)))

#define SET_BIT32(x, dest, value, start, count)                                    \
  WRITE32_##x##_REG(dest, (READ32_##x##_REG(dest) & ~DISPLAY_MASK(start, count)) | \
                              (((value) << (start)) & DISPLAY_MASK(start, count)))

#define GET_BIT32(x, dest, start, count) \
  ((READ32_##x##_REG(dest) >> (start)) & ((1 << (count)) - 1))

#define SET_MASK32(x, dest, mask) WRITE32_##x##_REG(dest, (READ32_##x##_REG(dest) | mask))

#define CLEAR_MASK32(x, dest, mask) WRITE32_##x##_REG(dest, (READ32_##x##_REG(dest) & ~(mask)))

#define WRITE32_REG(x, a, v) WRITE32_##x##_REG(a, v)
#define READ32_REG(x, a) READ32_##x##_REG(a)

#define DISP_ERROR(fmt, ...) zxlogf(ERROR, "[%s %d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define DISP_INFO(fmt, ...) zxlogf(INFO, "[%s %d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define DISP_TRACE(fmt, ...) zxlogf(TRACE, "[%s %d] " fmt, __func__, __LINE__, ##__VA_ARGS__)

// Should match display_mmios table in board driver
enum {
  MMIO_VPU,
  MMIO_MPI_DSI,
  MMIO_DSI_PHY,
  MMIO_HHI,
  MMIO_AOBUS,
  MMIO_CBUS,
};

// Should match display_gpios table in board driver
enum {
  GPIO_BL,
  GPIO_LCD,
  GPIO_HW_ID0,
  GPIO_HW_ID1,
  GPIO_HW_ID2,
  GPIO_COUNT,
};

// Should match display_irqs table in board driver
enum {
  IRQ_VSYNC,
  IRQ_RDMA,
  IRQ_VD1_WR,
};

enum CaptureState {
  CAPTURE_RESET = 0,
  CAPTURE_IDLE = 1,
  CAPTURE_ACTIVE = 2,
  CAPTURE_ERROR = 3,
};

constexpr display::DisplayId kPanelDisplayId(1);

constexpr bool kBootloaderDisplayEnabled = true;

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_AMLOGIC_DISPLAY_COMMON_H_
