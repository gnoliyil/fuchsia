// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bus/drivers/pci/allocation.h"

#include <err.h>
#include <lib/zx/resource.h>
#include <lib/zx/result.h>
#include <lib/zx/vmo.h>
#include <zircon/rights.h>
#include <zircon/status.h>

#include <cassert>
#include <cinttypes>
#include <cstring>
#include <memory>

#include <fbl/algorithm.h>

namespace pci {

zx::result<zx::vmo> PciAllocation::CreateVmo() const {
  zx::vmo vmo;
  zx_status_t status = zx::vmo::create_physical(resource(), base(), size(), &vmo);
  if (status != ZX_OK) {
    return zx::error(status);
  }

  return zx::ok(std::move(vmo));
}

zx::result<zx::resource> PciAllocation::CreateResource() const {
  zx::resource resource;
  // A BAR allocation will already be sized to the BAR, so we can simply
  // duplicate the resource rather than creating a sub-resource.
  zx_status_t status = resource_.duplicate(ZX_RIGHT_SAME_RIGHTS, &resource);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(std::move(resource));
}

zx::result<std::unique_ptr<PciAllocation>> PciRootAllocator::Allocate(
    std::optional<zx_paddr_t> base, size_t size) {
  zx_paddr_t in_base = (base) ? *base : 0;
  zx_paddr_t out_base = {};
  zx::resource res = {};
  zx::eventpair ep = {};
  zx_status_t status = pciroot_.GetAddressSpace(in_base, size, type(), low_, &out_base, &res, &ep);
  if (status != ZX_OK) {
    bool mmio = type() == PCI_ADDRESS_SPACE_MEMORY;
    // This error may not be fatal, the Device probe/allocation methods will know for sure.
    zxlogf(DEBUG, "failed to allocate %s %s [%#8lx, %#8lx) from root: %s", (mmio) ? "mmio" : "io",
           (mmio) ? ((low_) ? "<4GB" : ">4GB") : "", in_base, in_base + size,
           zx_status_get_string(status));
    return zx::error(status);
  }

  auto allocation = std::unique_ptr<PciAllocation>(
      new PciRootAllocation(pciroot_, type(), std::move(res), std::move(ep), out_base, size));
  return zx::ok(std::move(allocation));
}

zx::result<std::unique_ptr<PciAllocation>> PciRegionAllocator::Allocate(
    std::optional<zx_paddr_t> base, size_t size) {
  if (!parent_alloc_) {
    return zx::error(ZX_ERR_NO_MEMORY);
  }

  RegionAllocator::Region::UPtr region_uptr;
  zx_status_t status = ZX_OK;
  // Only use base if it is non-zero. RegionAllocator's interface is overloaded so we have
  // to call it differently.
  if (base) {
    ralloc_region_t request = {
        .base = (base) ? *base : 0,
        .size = size,
    };
    status = allocator_.GetRegion(request, region_uptr);
  } else {
    status = allocator_.GetRegion(size, zx_system_get_page_size(), region_uptr);
  }

  if (status != ZX_OK) {
    return zx::error(status);
  }

  zx::resource out_resource = {};
  // TODO(fxbug.dev/32978): When the resource subset CL lands, make this a smaller resource.
  status = parent_alloc_->resource().duplicate(ZX_DEFAULT_RESOURCE_RIGHTS, &out_resource);
  if (status != ZX_OK) {
    return zx::error(status);
  }

  zxlogf(TRACE, "bridge: assigned [%#lx, %#lx) downstream", region_uptr->base,
         region_uptr->base + size);

  auto allocation = std::unique_ptr<PciAllocation>(
      new PciRegionAllocation(type(), std::move(out_resource), std::move(region_uptr)));
  return zx::ok(std::move(allocation));
}

zx_status_t PciRegionAllocator::SetParentAllocation(std::unique_ptr<PciAllocation> alloc) {
  ZX_DEBUG_ASSERT(!parent_alloc_);

  parent_alloc_ = std::move(alloc);
  auto base = parent_alloc_->base();
  auto size = parent_alloc_->size();
  return allocator_.AddRegion({.base = base, .size = size});
}

}  // namespace pci
