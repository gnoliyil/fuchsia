// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <fuchsia/sysmem/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/platform-defs.h>

#include "vim3.h"

namespace vim3 {
namespace fpbus = fuchsia_hardware_platform_bus;
static const std::vector<fpbus::Bti> sysmem_btis{
    {{
        .iommu_index = 0,
        .bti_id = BTI_SYSMEM,
    }},
};
static const sysmem_metadata_t sysmem_metadata = {
    .vid = PDEV_VID_AMLOGIC,
    .pid = PDEV_PID_AMLOGIC_A311D,
    .protected_memory_size = 0,

    // The AMlogic display engine needs contiguous physical memory for each
    // frame buffer, because it does not have a page table walker. We reserve
    // enough memory to hold  5 framebuffers (2 for virtcon, 3 for scenic) at
    // the maximum supported resolution of 3840 x 2160 with 4 bytes per pixel
    // (for the RGBA/BGRA 8888 pixel format).
    //
    // The maximum supported resolution is documented below.
    // * "A311D Quick Reference Manual" revision 01, pages 2-3
    // * "A311D Datasheet" revision 08, section 2.2 "Features", pages 4-5
    //
    // TODO(fxbug.dev/121456): Reserving this much memory seems wasteful. The
    // quantity below is 10% of RAM on a VIM3 Basic and 5% of RAM on a VIM3 Pro.
    .contiguous_memory_size = int64_t{200} * 1024 * 1024,
};

static const std::vector<fpbus::Metadata> sysmem_metadata_list{
    {{
        .type = SYSMEM_METADATA_TYPE,
        .data = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(&sysmem_metadata),
            reinterpret_cast<const uint8_t*>(&sysmem_metadata) + sizeof(sysmem_metadata)),
    }},
};

static const fpbus::Node sysmem_dev = []() {
  fpbus::Node dev = {};
  dev.name() = "sysmem";
  dev.vid() = PDEV_VID_GENERIC;
  dev.pid() = PDEV_PID_GENERIC;
  dev.did() = PDEV_DID_SYSMEM;
  dev.bti() = sysmem_btis;
  dev.metadata() = sysmem_metadata_list;
  return dev;
}();

zx_status_t Vim3::SysmemInit() {
  fidl::Arena<> fidl_arena;
  fdf::Arena arena('SYSM');
  auto result = pbus_.buffer(arena)->NodeAdd(fidl::ToWire(fidl_arena, sysmem_dev));
  if (!result.ok()) {
    zxlogf(ERROR, "%s: NodeAdd Sysmem(sysmem_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: NodeAdd Sysmem(sysmem_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }
  return ZX_OK;
}
}  // namespace vim3
