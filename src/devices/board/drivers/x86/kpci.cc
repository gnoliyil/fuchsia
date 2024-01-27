// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <lib/ddk/debug.h>
#include <lib/pci/pciroot.h>
#include <stdio.h>
#include <zircon/hw/pci.h>
#include <zircon/syscalls/pci.h>

#include <array>

#include <acpica/acpi.h>

#include "acpi-private.h"
#include "methods.h"
#include "src/devices/board/lib/acpi/acpi.h"
#include "src/devices/board/lib/acpi/device.h"
#include "src/devices/board/lib/acpi/resources.h"
#include "src/devices/board/lib/acpi/status.h"
#include "src/devices/lib/iommu/iommu-x86.h"

#define PCI_HID ((char*)"PNP0A03")
#define PCIE_HID ((char*)"PNP0A08")
#define xprintf(fmt...) zxlogf(TRACE, fmt)
#define PANIC_UNIMPLEMENTED __builtin_trap()

/* Helper routine for translating IRQ routing tables into usable form
 *
 * @param port_dev_id The device ID on the root bus of this root port or
 * UINT8_MAX if this call is for the root bus, not a root port
 * @param port_func_id The function ID on the root bus of this root port or
 * UINT8_MAX if this call is for the root bus, not a root port
 */
static ACPI_STATUS handle_prt(ACPI_HANDLE object, zx_pci_init_arg_t* arg, uint8_t port_dev_id,
                              uint8_t port_func_id) {
  assert((port_dev_id == UINT8_MAX && port_func_id == UINT8_MAX) ||
         (port_dev_id != UINT8_MAX && port_func_id != UINT8_MAX));

  ACPI_BUFFER buffer = {
      // Request that the ACPI subsystem allocate the buffer
      .Length = ACPI_ALLOCATE_BUFFER,
      .Pointer = NULL,
  };
  ACPI_BUFFER crs_buffer = {
      .Length = ACPI_ALLOCATE_BUFFER,
      .Pointer = NULL,
  };

  ACPI_STATUS status = AcpiGetIrqRoutingTable(object, &buffer);
  // IRQ routing tables are *required* to exist on the root hub
  if (status != AE_OK) {
    goto cleanup;
  }

  uintptr_t entry_addr;
  entry_addr = reinterpret_cast<uintptr_t>(buffer.Pointer);
  ACPI_PCI_ROUTING_TABLE* entry;
  for (entry = (ACPI_PCI_ROUTING_TABLE*)entry_addr; entry->Length != 0;
       entry_addr += entry->Length, entry = (ACPI_PCI_ROUTING_TABLE*)entry_addr) {
    if (entry_addr > (uintptr_t)buffer.Pointer + buffer.Length) {
      return AE_ERROR;
    }
    if (entry->Pin >= PCI_MAX_LEGACY_IRQ_PINS) {
      return AE_ERROR;
    }
    uint8_t dev_id = (entry->Address >> 16) & (PCI_MAX_DEVICES_PER_BUS - 1);
    // Either we're handling the root complex (port_dev_id == UINT8_MAX), or
    // we're handling a root port, and if it's a root port, dev_id should
    // be 0.
    if (port_dev_id != UINT8_MAX && dev_id != 0) {
      // this is a weird entry, skip it
      continue;
    }

    uint32_t global_irq = ZX_PCI_NO_IRQ_MAPPING;
    bool level_triggered = true;
    bool active_high = false;
    if (entry->u.Source[0]) {
      // If the Source is not just a NULL byte, then it refers to a
      // PCI Interrupt Link Device
      ACPI_HANDLE ild;
      status = AcpiGetHandle(object, entry->u.Source, &ild);
      if (status != AE_OK) {
        goto cleanup;
      }
      status = AcpiGetCurrentResources(ild, &crs_buffer);
      if (status != AE_OK) {
        goto cleanup;
      }

      uintptr_t crs_entry_addr = (uintptr_t)crs_buffer.Pointer;
      ACPI_RESOURCE* res = (ACPI_RESOURCE*)crs_entry_addr;
      while (res->Type != ACPI_RESOURCE_TYPE_END_TAG) {
        if (res->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
          ACPI_RESOURCE_EXTENDED_IRQ* irq = &res->Data.ExtendedIrq;
          if (global_irq != ZX_PCI_NO_IRQ_MAPPING) {
            // TODO: Handle finding two allocated IRQs.  Shouldn't
            // happen?
            PANIC_UNIMPLEMENTED;
          }
          if (irq->InterruptCount != 1) {
            // TODO: Handle finding two allocated IRQs.  Shouldn't
            // happen?
            PANIC_UNIMPLEMENTED;
          }
          if (irq->u.Interrupts[0] != 0) {
            active_high = (irq->Polarity == ACPI_ACTIVE_HIGH);
            level_triggered = (irq->Triggering == ACPI_LEVEL_SENSITIVE);
            global_irq = irq->u.Interrupts[0];
          }
        } else {
          // TODO: Handle non extended IRQs
          PANIC_UNIMPLEMENTED;
        }
        crs_entry_addr += res->Length;
        res = (ACPI_RESOURCE*)crs_entry_addr;
      }
      if (global_irq == ZX_PCI_NO_IRQ_MAPPING) {
        // TODO: Invoke PRS to find what is allocatable and allocate it with SRS
        PANIC_UNIMPLEMENTED;
      }
      AcpiOsFree(crs_buffer.Pointer);
      crs_buffer.Length = ACPI_ALLOCATE_BUFFER;
      crs_buffer.Pointer = NULL;
    } else {
      // Otherwise, SourceIndex refers to a global IRQ number that the pin
      // is connected to
      global_irq = entry->SourceIndex;
    }

    // Check if we've seen this IRQ already, and if so, confirm the
    // IRQ signaling is the same.
    bool found_irq = false;
    for (unsigned int i = 0; i < arg->num_irqs; ++i) {
      if (global_irq != arg->irqs[i].global_irq) {
        continue;
      }
      if (active_high != arg->irqs[i].active_high ||
          level_triggered != arg->irqs[i].level_triggered) {
        // TODO: Handle mismatch here
        PANIC_UNIMPLEMENTED;
      }
      found_irq = true;
      break;
    }
    if (!found_irq) {
      assert(arg->num_irqs < std::size(arg->irqs));
      arg->irqs[arg->num_irqs].global_irq = global_irq;
      arg->irqs[arg->num_irqs].active_high = active_high;
      arg->irqs[arg->num_irqs].level_triggered = level_triggered;
      arg->num_irqs++;
    }

    if (port_dev_id == UINT8_MAX) {
      for (unsigned int i = 0; i < PCI_MAX_FUNCTIONS_PER_DEVICE; ++i) {
        arg->dev_pin_to_global_irq[dev_id][i][entry->Pin] = global_irq;
      }
    } else {
      arg->dev_pin_to_global_irq[port_dev_id][port_func_id][entry->Pin] = global_irq;
    }
  }

cleanup:
  if (crs_buffer.Pointer) {
    AcpiOsFree(crs_buffer.Pointer);
  }
  if (buffer.Pointer) {
    AcpiOsFree(buffer.Pointer);
  }
  return status;
}

/* @brief Find the PCI config (returns the first one found)
 *
 * @param arg The structure to populate
 *
 * @return ZX_OK on success.
 */
static zx_status_t find_pcie_config(zx_pci_init_arg_t* arg) {
  ACPI_TABLE_HEADER* raw_table = NULL;
  ACPI_STATUS status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, &raw_table);
  if (status != AE_OK) {
    xprintf("could not find MCFG");
    return ZX_ERR_NOT_FOUND;
  }
  ACPI_TABLE_MCFG* mcfg = (ACPI_TABLE_MCFG*)raw_table;
  ACPI_MCFG_ALLOCATION* table_start =
      reinterpret_cast<ACPI_MCFG_ALLOCATION*>(reinterpret_cast<uintptr_t>(mcfg) + sizeof(*mcfg));
  ACPI_MCFG_ALLOCATION* table_end = reinterpret_cast<ACPI_MCFG_ALLOCATION*>(
      reinterpret_cast<uintptr_t>(mcfg) + mcfg->Header.Length);
  uintptr_t table_bytes = (uintptr_t)table_end - (uintptr_t)table_start;
  if (table_bytes % sizeof(*table_start) != 0) {
    xprintf("MCFG has unexpected size");
    return ZX_ERR_INTERNAL;
  }
  size_t num_entries = table_end - table_start;
  if (num_entries == 0) {
    xprintf("MCFG has no entries");
    return ZX_ERR_NOT_FOUND;
  }
  if (num_entries > 1) {
    xprintf("MCFG has more than one entry, just taking the first");
  }

  size_t size_per_bus =
      PCIE_EXTENDED_CONFIG_SIZE * PCI_MAX_DEVICES_PER_BUS * PCI_MAX_FUNCTIONS_PER_DEVICE;
  int num_buses = table_start->EndBusNumber - table_start->StartBusNumber + 1;

  if (table_start->PciSegment != 0) {
    xprintf("Non-zero segment found");
    return ZX_ERR_NOT_SUPPORTED;
  }

  arg->addr_windows[0].cfg_space_type = PCI_CFG_SPACE_TYPE_MMIO;
  arg->addr_windows[0].has_ecam = true;
  arg->addr_windows[0].bus_start = table_start->StartBusNumber;
  arg->addr_windows[0].bus_end = table_start->EndBusNumber;

  // We need to adjust the physical address we received to align to the proper
  // bus number.
  //
  // Citation from PCI Firmware Spec 3.0:
  // For PCI-X and PCI Express platforms utilizing the enhanced
  // configuration access method, the base address of the memory mapped
  // configuration space always corresponds to bus number 0 (regardless
  // of the start bus number decoded by the host bridge).
  arg->addr_windows[0].base = table_start->Address + size_per_bus * arg->addr_windows[0].bus_start;
  // The size of this mapping is defined in the PCI Firmware v3 spec to be
  // big enough for all of the buses in this config.
  arg->addr_windows[0].size = size_per_bus * num_buses;
  arg->addr_window_count = 1;
  return ZX_OK;
}

/* @brief Device enumerator for platform_configure_pcie_legacy_irqs */
static acpi::status<> get_pcie_devices_irq(acpi::Acpi* acpi, ACPI_HANDLE object,
                                           zx_pci_init_arg_t* arg) {
  ACPI_STATUS status = handle_prt(object, arg, UINT8_MAX, UINT8_MAX);
  if (status != AE_OK) {
    return acpi::make_status(status);
  }

  // Enumerate root ports
  ACPI_HANDLE child = NULL;
  while (1) {
    status = AcpiGetNextObject(ACPI_TYPE_DEVICE, object, child, &child);
    if (status == AE_NOT_FOUND) {
      break;
    } else if (status != AE_OK) {
      return acpi::make_status(status);
    }

    auto status = acpi->EvaluateObject(child, "_ADR", std::nullopt);
    if (status.is_error() || status->Type != ACPI_TYPE_INTEGER) {
      continue;
    }
    UINT64 data = status->Integer.Value;
    uint8_t port_dev_id = (data >> 16) & (PCI_MAX_DEVICES_PER_BUS - 1);
    uint8_t port_func_id = data & (PCI_MAX_FUNCTIONS_PER_DEVICE - 1);
    // Ignore the return value of this, since if child is not a
    // root port, it will fail and we don't care.
    handle_prt(child, arg, port_dev_id, port_func_id);
  }
  return acpi::ok();
}

/* @brief Find the legacy IRQ swizzling for the PCIe root bus
 *
 * @param arg The structure to populate
 *
 * @return ZX_OK on success
 */
static zx_status_t find_pci_legacy_irq_mapping(acpi::Acpi* acpi, zx_pci_init_arg_t* arg) {
  unsigned int map_len =
      sizeof(arg->dev_pin_to_global_irq) / (sizeof(**(arg->dev_pin_to_global_irq)));
  for (unsigned int i = 0; i < map_len; ++i) {
    uint32_t* flat_map = (uint32_t*)&arg->dev_pin_to_global_irq;
    flat_map[i] = ZX_PCI_NO_IRQ_MAPPING;
  }
  arg->num_irqs = 0;

  acpi::status<> status = acpi->GetDevices(
      (arg->addr_windows[0].has_ecam) ? PCIE_HID : PCI_HID,
      [acpi, arg](ACPI_HANDLE hnd, uint32_t) { return get_pcie_devices_irq(acpi, hnd, arg); });
  if (status.is_error()) {
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

static acpi::status<> find_pci_configs_cb(ACPI_HANDLE object, zx_pci_init_arg_t* arg) {
  size_t size_per_bus =
      PCI_BASE_CONFIG_SIZE * PCI_MAX_DEVICES_PER_BUS * PCI_MAX_FUNCTIONS_PER_DEVICE;

  // TODO(cja): This is essentially a hacky solution to deal with
  // legacy PCI on Virtualbox and GCE. When the ACPI bus driver
  // is enabled we'll be using proper binding and not need this
  // anymore.
  if (auto res = acpi::GetObjectInfo(object); res.is_ok()) {
    arg->addr_windows[0].cfg_space_type = PCI_CFG_SPACE_TYPE_PIO;
    arg->addr_windows[0].has_ecam = false;
    arg->addr_windows[0].base = 0;
    arg->addr_windows[0].bus_start = 0;
    arg->addr_windows[0].bus_end = 0;
    arg->addr_windows[0].size = size_per_bus;
    arg->addr_window_count = 1;

    return acpi::ok();
  }

  return acpi::error(AE_ERROR);
}

/* @brief Find the PCI config (returns the first one found)
 *
 * @param arg The structure to populate
 *
 * @return ZX_OK on success.
 */
static zx_status_t find_pci_config(acpi::Acpi* acpi, zx_pci_init_arg_t* arg) {
  // TODO: Although this will find every PCI legacy root, we're presently
  // hardcoding to just use the first at bus 0 dev 0 func 0 segment 0.
  return acpi->GetDevices(PCI_HID, [arg](ACPI_HANDLE device,
                                         uint32_t) { return find_pci_configs_cb(device, arg); })
                 .is_ok()
             ? ZX_OK
             : ZX_ERR_INTERNAL;
}

/* @brief Compute PCIe initialization information
 *
 * The computed initialization information can be released with free()
 *
 * @param arg Pointer to store the initialization information into
 *
 * @return ZX_OK on success
 */
zx_status_t get_pci_init_arg(acpi::Acpi* acpi, zx_pci_init_arg_t** arg, uint32_t* size) {
  zx_pci_init_arg_t* res = NULL;

  // TODO(teisenbe): We assume only one ECAM window right now...
  size_t obj_size = sizeof(*res) + sizeof(res->addr_windows[0]) * 1;
  res = static_cast<zx_pci_init_arg_t*>(calloc(1, obj_size));
  if (!res) {
    return ZX_ERR_NO_MEMORY;
  }

  // First look for a PCIe root complex. If none is found, try legacy PCI.
  // This presently only cares about the first root found, multiple roots
  // will be handled when the PCI bus driver binds to roots via ACPI.
  zx_status_t status = find_pcie_config(res);
  if (status != ZX_OK) {
    status = find_pci_config(acpi, res);
    if (status != ZX_OK) {
      goto fail;
    }
  }

  status = find_pci_legacy_irq_mapping(acpi, res);
  if (status != ZX_OK) {
    goto fail;
  }

  *arg = res;
  *size =
      static_cast<uint32_t>(sizeof(*res) + sizeof(res->addr_windows[0]) * res->addr_window_count);
  return ZX_OK;
fail:
  free(res);
  return status;
}

struct report_current_resources_ctx {
  zx_handle_t pci_handle;
  bool device_is_root_bridge;
  bool add_pass;
};

static ACPI_STATUS report_current_resources_resource_cb(ACPI_RESOURCE* res, void* _ctx) {
  auto* ctx = static_cast<report_current_resources_ctx*>(_ctx);

  bool is_mmio = false;
  uint64_t base = 0;
  uint64_t len = 0;
  bool add_range = false;

  if (resource_is_memory(res)) {
    resource_memory_t mem;
    zx_status_t status = resource_parse_memory(res, &mem);
    if (status != ZX_OK || mem.minimum != mem.maximum) {
      return AE_ERROR;
    }

    is_mmio = true;
    base = mem.minimum;
    len = mem.address_length;
  } else if (resource_is_address(res)) {
    resource_address_t addr;
    zx_status_t status = resource_parse_address(res, &addr);
    if (status != ZX_OK) {
      return AE_ERROR;
    }

    if (addr.resource_type == RESOURCE_ADDRESS_MEMORY) {
      is_mmio = true;
    } else if (addr.resource_type == RESOURCE_ADDRESS_IO) {
      is_mmio = false;
    } else {
      return AE_OK;
    }

    if (!addr.min_address_fixed || !addr.max_address_fixed || addr.maximum < addr.minimum) {
      printf("WARNING: ACPI found bad _CRS address entry\n");
      return AE_OK;
    }

    // We compute len from maximum rather than address_length, since some
    // implementations don't set address_length...
    base = addr.minimum;
    len = addr.maximum - base + 1;

    // PCI root bridges report downstream resources via _CRS.  Since we're
    // gathering data on acceptable ranges for PCI to use for MMIO, consider
    // non-consume-only address resources to be valid for PCI MMIO.
    if (ctx->device_is_root_bridge && !addr.consumed_only) {
      add_range = true;
    }
  } else if (resource_is_io(res)) {
    resource_io_t io;
    zx_status_t status = resource_parse_io(res, &io);
    if (status != ZX_OK) {
      return AE_ERROR;
    }

    if (io.minimum != io.maximum) {
      printf("WARNING: ACPI found bad _CRS IO entry\n");
      return AE_OK;
    }

    is_mmio = false;
    base = io.minimum;
    len = io.address_length;
  } else {
    return AE_OK;
  }

  // Ignore empty regions that are reported, and skip any resources that
  // aren't for the pass we're doing.
  if (len == 0 || add_range != ctx->add_pass) {
    return AE_OK;
  }

  if (add_range && is_mmio && base < 1024 * 1024) {
    // The PC platform defines many legacy regions below 1MB that we do not
    // want PCIe to try to map onto.
    xprintf("Skipping adding MMIO range, due to being below 1MB");
    return AE_OK;
  }

  xprintf("ACPI range modification: %sing %s %016lx %016lx", add_range ? "add" : "subtract",
          is_mmio ? "MMIO" : "PIO", base, len);

  zx_status_t status = zx_pci_add_subtract_io_range(ctx->pci_handle, is_mmio, base, len, add_range);
  if (status != ZX_OK) {
    if (add_range) {
      xprintf("Failed to add range: %d", status);
    } else {
      // If we are subtracting a range and fail, abort.  This is bad.
      return AE_ERROR;
    }
  }
  return AE_OK;
}

static acpi::status<> pci_report_current_resources_device_cb(ACPI_HANDLE object, acpi::Acpi* acpi,
                                                             report_current_resources_ctx* ctx) {
  acpi::UniquePtr<ACPI_DEVICE_INFO> info;
  if (auto res = acpi::GetObjectInfo(object); res.is_error()) {
    return res.take_error();
  } else {
    info = std::move(res.value());
  }

  ctx->device_is_root_bridge = (info->Flags & ACPI_PCI_ROOT_BRIDGE) != 0;

  ACPI_STATUS status =
      AcpiWalkResources(object, (char*)"_CRS", report_current_resources_resource_cb, ctx);
  if (status == AE_NOT_FOUND || status == AE_OK) {
    return acpi::ok();
  }
  return acpi::make_status(status);
}

/* @brief Report current resources to the kernel PCI driver
 *
 * Walks the ACPI namespace and use the reported current resources to inform
   the kernel PCI interface about what memory it shouldn't use.
 *
 * @param root_resource_handle The handle to pass to the kernel when talking
 * to the PCI driver.
 *
 * @return ZX_OK on success
 */
zx_status_t pci_report_current_resources(acpi::Acpi* acpi, zx_handle_t root_resource_handle) {
  // First we search for resources to add, then we subtract out things that
  // are being consumed elsewhere.  This forces an ordering on the
  // operations so that it should be consistent, and should protect against
  // inconistencies in the _CRS methods.

  // Walk the device tree and add to the PCIe IO ranges any resources
  // "produced" by the PCI root in the ACPI namespace.
  struct report_current_resources_ctx ctx = {
      .pci_handle = root_resource_handle,
      .device_is_root_bridge = false,
      .add_pass = true,
  };
  acpi::status<> status =
      acpi->GetDevices(nullptr, [ctx = &ctx, acpi](ACPI_HANDLE hnd, uint32_t) -> acpi::status<> {
        return pci_report_current_resources_device_cb(hnd, acpi, ctx);
      });
  if (status.is_error()) {
    return ZX_ERR_INTERNAL;
  }

  // Removes resources we believe are in use by other parts of the platform
  ctx = (struct report_current_resources_ctx){
      .pci_handle = root_resource_handle,
      .device_is_root_bridge = false,
      .add_pass = false,
  };
  status =
      acpi->GetDevices(nullptr, [ctx = &ctx, acpi](ACPI_HANDLE hnd, uint32_t) -> acpi::status<> {
        return pci_report_current_resources_device_cb(hnd, acpi, ctx);
      });
  if (status.is_error()) {
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_protocol_device_t acpi_device_proto = [] {
  zx_protocol_device_t ops = {};
  ops.version = DEVICE_OPS_VERSION;
  ops.release = free;
  return ops;
}();

// This pci_init initializes the kernel pci driver and is not compiled in at the same time as the
// userspace pci driver under development.
zx_status_t pci_init(zx_device_t* platform_bus, ACPI_HANDLE object,
                     acpi::UniquePtr<ACPI_DEVICE_INFO> info, acpi::Manager* manager,
                     std::vector<pci_bdf_t> acpi_bdfs) {
  // Report current resources to kernel PCI driver
  // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
  zx_status_t status = pci_report_current_resources(manager->acpi(), get_root_resource());
  if (status != ZX_OK) {
    zxlogf(ERROR, "acpi: WARNING: ACPI failed to report all current resources!");
  }

  // Initialize kernel PCI driver
  zx_pci_init_arg_t* arg;
  uint32_t arg_size;
  status = get_pci_init_arg(manager->acpi(), &arg, &arg_size);
  if (status != ZX_OK) {
    zxlogf(ERROR, "acpi: erorr %d in get_pci_init_arg", status);
    return AE_ERROR;
  }

  // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
  status = zx_pci_init(get_root_resource(), arg, arg_size);
  if (status != ZX_OK) {
    zxlogf(ERROR, "acpi: error %d in zx_pci_init", status);
    return AE_ERROR;
  }

  free(arg);

  // Publish PCI root as /dev/sys/platform/ level.
  // Only publish one PCI root device for all PCI roots
  // TODO: store context for PCI root protocol
  device_add_args_t args{
      .name = "pci",
  };
  auto device = std::make_unique<acpi::Device>(
      acpi::DeviceArgs(platform_bus, manager, fdf::Dispatcher::GetCurrent()->async_dispatcher(),
                       object)
          .SetPciMetadata(std::move(acpi_bdfs)));

  args.version = DEVICE_ADD_ARGS_VERSION;
  args.ctx = device.get();
  args.ops = &acpi_device_proto;
  // The acpi::Device implements both the Acpi protocol and the Pciroot protocol. We may find a
  // better way to do this in the future, but kernel PCI will eventually be removed anyway, at
  // which point we'll need to refactor it all anyway.
  args.proto_id = ZX_PROTOCOL_PCIROOT;
  args.proto_ops = get_pciroot_ops();

  if (zx_status_t status = device_add(platform_bus, &args, device->mutable_zxdev());
      status != ZX_OK) {
    zxlogf(ERROR, "acpi: error %d in device_add, parent=(%p)", status, platform_bus);
    return status;
  } else {
    zxlogf(INFO, "acpi: published device %s(%p), parent=(%p), handle=%p", args.name, device.get(),
           platform_bus, device->acpi_handle());
    // device_add takes ownership of args.ctx, but only on success.
    device.release();
  }

  return ZX_OK;
}

static zx_status_t pciroot_op_get_bti(void* /*context*/, uint32_t bdf, uint32_t index,
                                      zx_handle_t* bti) {
  // The x86 IOMMU world uses PCI BDFs as the hardware identifiers, so there
  // will only be one BTI per device.
  if (index != 0) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  // For dummy IOMMUs, the bti_id just needs to be unique.  For Intel IOMMUs,
  // the bti_ids correspond to PCI BDFs.
  zx_handle_t iommu_handle;
  zx_status_t status = iommu_manager_iommu_for_bdf(bdf, &iommu_handle);
  if (status != ZX_OK) {
    return status;
  }

  status = zx_bti_create(iommu_handle, 0, bdf, bti);
  if (status == ZX_OK) {
    char name[ZX_MAX_NAME_LEN]{};
    snprintf(name, std::size(name) - 1, "kpci bti %02x:%02x.%1x", (bdf >> 8) & 0xFF,
             (bdf >> 3) & 0x1F, bdf & 0x7);
    const zx_status_t name_status =
        zx_object_set_property(*bti, ZX_PROP_NAME, name, std::size(name));
    if (name_status != ZX_OK) {
      zxlogf(WARNING, "Couldn't set name for BTI '%s': %s", name,
             zx_status_get_string(name_status));
    }
  }

  return status;
}

static zx_status_t pciroot_op_get_pci_platform_info(void* ctx, pci_platform_info_t* info) {
  acpi::Device* device = static_cast<acpi::Device*>(ctx);
  memset(info, 0, sizeof(*info));
  info->acpi_bdfs_count = device->pci_bdfs().size();
  info->acpi_bdfs_list = device->pci_bdfs().data();
  return ZX_OK;
}

static bool pciroot_op_driver_should_proxy_config(void* /*ctx*/) { return false; }

static zx_status_t pciroot_op_read_config8(void*, const pci_bdf_t*, uint16_t, uint8_t*) {
  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t pciroot_op_read_config16(void*, const pci_bdf_t*, uint16_t, uint16_t*) {
  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t pciroot_op_read_config32(void*, const pci_bdf_t*, uint16_t, uint32_t*) {
  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t pciroot_op_write_config8(void*, const pci_bdf_t*, uint16_t, uint8_t) {
  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t pciroot_op_write_config16(void*, const pci_bdf_t*, uint16_t, uint16_t) {
  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t pciroot_op_write_config32(void*, const pci_bdf_t*, uint16_t, uint32_t) {
  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t pciroot_op_allocate_msi(void*, uint32_t, bool, zx_handle_t*) {
  return ZX_ERR_NOT_SUPPORTED;
}

static zx_status_t pciroot_op_get_address_space(void*, size_t, zx_paddr_t, pci_address_space_t,
                                                bool, zx_paddr_t*, zx_handle_t*, zx_handle_t*) {
  return ZX_ERR_NOT_SUPPORTED;
}

static pciroot_protocol_ops_t pciroot_proto = {
    .get_bti = pciroot_op_get_bti,
    .get_pci_platform_info = pciroot_op_get_pci_platform_info,
    .driver_should_proxy_config = pciroot_op_driver_should_proxy_config,
    .read_config8 = pciroot_op_read_config8,
    .read_config16 = pciroot_op_read_config16,
    .read_config32 = pciroot_op_read_config32,
    .write_config8 = pciroot_op_write_config8,
    .write_config16 = pciroot_op_write_config16,
    .write_config32 = pciroot_op_write_config32,
    .get_address_space = pciroot_op_get_address_space,
    .allocate_msi = pciroot_op_allocate_msi,
};

pciroot_protocol_ops_t* get_pciroot_ops(void) { return &pciroot_proto; }
