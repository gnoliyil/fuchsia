// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_AML_CANVAS_AML_CANVAS_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_AML_CANVAS_AML_CANVAS_H_

#include <fuchsia/hardware/amlogiccanvas/cpp/banjo.h>
#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <lib/ddk/platform-defs.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/mmio/mmio.h>

#include <array>

#include <ddktl/device.h>
#include <fbl/mutex.h>

#define IS_ALIGNED(a, b) (!(((uintptr_t)(a)) & (((uintptr_t)(b)) - 1)))

#define CANVAS_ERROR(fmt, ...) zxlogf(ERROR, "[%s %d]" fmt, __func__, __LINE__, ##__VA_ARGS__)
#define CANVAS_INFO(fmt, ...) zxlogf(INFO, "[%s %d]" fmt, __func__, __LINE__, ##__VA_ARGS__)

namespace aml_canvas {

constexpr size_t kNumCanvasEntries = 256;

class AmlCanvas;
using DeviceType = ddk::Device<AmlCanvas>;

struct CanvasEntry {
  CanvasEntry() = default;

  CanvasEntry(CanvasEntry&&) = default;
  CanvasEntry& operator=(CanvasEntry&& right) {
    if (this == &right)
      return *this;
    if (pmt)
      pmt.unpin();
    pmt = std::move(right.pmt);
    vmo = std::move(right.vmo);
    node = std::move(right.node);
    return *this;
  }

  ~CanvasEntry() {
    if (pmt)
      pmt.unpin();
  }

  zx::pmt pmt;
  // Hold a handle to the VMO so the memory diagnostic tools can realize that it's in use by this
  // process.  See fxbug.dev/75877.
  zx::vmo vmo;
  inspect::Node node;
};

class AmlCanvas : public DeviceType,
                  public ddk::AmlogicCanvasProtocol<AmlCanvas, ddk::base_protocol> {
 public:
  // Factory method called by the device manager binding code.
  static zx_status_t Create(zx_device_t* parent);

  // `mmio` is the region documented as DMC in Section 8.1 "Memory Map" of the
  // AMLogic A311D datasheet.
  AmlCanvas(zx_device_t* parent, fdf::MmioBuffer mmio, zx::bti bti);

  AmlCanvas(const AmlCanvas&) = delete;
  AmlCanvas(AmlCanvas&&) = delete;
  AmlCanvas& operator=(const AmlCanvas&) = delete;
  AmlCanvas& operator=(AmlCanvas&&) = delete;

  ~AmlCanvas();

  // Required by ddk::AmlogicCanvasProtocol
  zx_status_t AmlogicCanvasConfig(zx::vmo vmo, size_t offset, const canvas_info_t* info,
                                  uint8_t* canvas_idx);
  zx_status_t AmlogicCanvasFree(uint8_t canvas_idx);

  // Required by ddk::Device
  void DdkRelease();

 private:
  inspect::Inspector inspector_;
  inspect::Node inspect_root_;
  fbl::Mutex lock_;
  fdf::MmioBuffer dmc_regs_ __TA_GUARDED(lock_);
  zx::bti bti_ __TA_GUARDED(lock_);
  std::array<CanvasEntry, kNumCanvasEntries> entries_ __TA_GUARDED(lock_);

};  // class AmlCanvas

}  // namespace aml_canvas

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_AML_CANVAS_AML_CANVAS_H_
