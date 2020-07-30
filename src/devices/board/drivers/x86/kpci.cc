// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <lib/pci/pciroot.h>
#include <stdio.h>

#include <array>

#include <acpica/acpi.h>
#include <ddk/debug.h>

#include "acpi-private.h"
#include "methods.h"
#include "pci.h"
#include "resources.h"

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
    if (entry->Source[0]) {
      // If the Source is not just a NULL byte, then it refers to a
      // PCI Interrupt Link Device
      ACPI_HANDLE ild;
      status = AcpiGetHandle(object, entry->Source, &ild);
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
          if (irq->Interrupts[0] != 0) {
            active_high = (irq->Polarity == ACPI_ACTIVE_HIGH);
            level_triggered = (irq->Triggering == ACPI_LEVEL_SENSITIVE);
            global_irq = irq->Interrupts[0];
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
      assert(arg->num_irqs < countof(arg->irqs));
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
static ACPI_STATUS get_pcie_devices_irq(ACPI_HANDLE object, UINT32 nesting_level, void* context,
                                        void** ret) {
  zx_pci_init_arg_t* arg = static_cast<zx_pci_init_arg_t*>(context);
  ACPI_STATUS status = handle_prt(object, arg, UINT8_MAX, UINT8_MAX);
  if (status != AE_OK) {
    return status;
  }

  // Enumerate root ports
  ACPI_HANDLE child = NULL;
  while (1) {
    status = AcpiGetNextObject(ACPI_TYPE_DEVICE, object, child, &child);
    if (status == AE_NOT_FOUND) {
      break;
    } else if (status != AE_OK) {
      return status;
    }

    ACPI_OBJECT object = {0};
    ACPI_BUFFER buffer = {
        .Length = sizeof(object),
        .Pointer = &object,
    };
    status = AcpiEvaluateObject(child, (char*)"_ADR", NULL, &buffer);
    if (status != AE_OK || buffer.Length < sizeof(object) || object.Type != ACPI_TYPE_INTEGER) {
      continue;
    }
    UINT64 data = object.Integer.Value;
    uint8_t port_dev_id = (data >> 16) & (PCI_MAX_DEVICES_PER_BUS - 1);
    uint8_t port_func_id = data & (PCI_MAX_FUNCTIONS_PER_DEVICE - 1);
    // Ignore the return value of this, since if child is not a
    // root port, it will fail and we don't care.
    handle_prt(child, arg, port_dev_id, port_func_id);
  }
  return AE_OK;
}

/* @brief Find the legacy IRQ swizzling for the PCIe root bus
 *
 * @param arg The structure to populate
 *
 * @return ZX_OK on success
 */
static zx_status_t find_pci_legacy_irq_mapping(zx_pci_init_arg_t* arg) {
  unsigned int map_len =
      sizeof(arg->dev_pin_to_global_irq) / (sizeof(**(arg->dev_pin_to_global_irq)));
  for (unsigned int i = 0; i < map_len; ++i) {
    uint32_t* flat_map = (uint32_t*)&arg->dev_pin_to_global_irq;
    flat_map[i] = ZX_PCI_NO_IRQ_MAPPING;
  }
  arg->num_irqs = 0;

  ACPI_STATUS status = AcpiGetDevices((arg->addr_windows[0].has_ecam) ? PCIE_HID : PCI_HID,
                                      get_pcie_devices_irq, arg, NULL);
  if (status != AE_OK) {
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

static ACPI_STATUS find_pci_configs_cb(ACPI_HANDLE object, uint32_t nesting_level, void* _ctx,
                                       void** ret) {
  size_t size_per_bus =
      PCI_BASE_CONFIG_SIZE * PCI_MAX_DEVICES_PER_BUS * PCI_MAX_FUNCTIONS_PER_DEVICE;
  zx_pci_init_arg_t* arg = (zx_pci_init_arg_t*)_ctx;

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

    return AE_OK;
  }

  return AE_ERROR;
}

/* @brief Find the PCI config (returns the first one found)
 *
 * @param arg The structure to populate
 *
 * @return ZX_OK on success.
 */
static zx_status_t find_pci_config(zx_pci_init_arg_t* arg) {
  // TODO: Although this will find every PCI legacy root, we're presently
  // hardcoding to just use the first at bus 0 dev 0 func 0 segment 0.
  return AcpiGetDevices(PCI_HID, find_pci_configs_cb, arg, NULL);
}

/* @brief Compute PCIe initialization information
 *
 * The computed initialization information can be released with free()
 *
 * @param arg Pointer to store the initialization information into
 *
 * @return ZX_OK on success
 */
zx_status_t get_pci_init_arg(zx_pci_init_arg_t** arg, uint32_t* size) {
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
    status = find_pci_config(res);
    if (status != ZX_OK) {
      goto fail;
    }
  }

  status = find_pci_legacy_irq_mapping(res);
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

static ACPI_STATUS pci_report_current_resources_device_cb(ACPI_HANDLE object,
                                                          uint32_t nesting_level, void* _ctx,
                                                          void** ret) {
  acpi::UniquePtr<ACPI_DEVICE_INFO> info;
  if (auto res = acpi::GetObjectInfo(object); res.is_error()) {
    return res.error_value();
  } else {
    info = std::move(res.value());
  }

  auto* ctx = static_cast<report_current_resources_ctx*>(_ctx);
  ctx->device_is_root_bridge = (info->Flags & ACPI_PCI_ROOT_BRIDGE) != 0;

  ACPI_STATUS status =
      AcpiWalkResources(object, (char*)"_CRS", report_current_resources_resource_cb, ctx);
  if (status == AE_NOT_FOUND || status == AE_OK) {
    return AE_OK;
  }
  return status;
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
zx_status_t pci_report_current_resources(zx_handle_t root_resource_handle) {
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
  ACPI_STATUS status = AcpiGetDevices(NULL, pci_report_current_resources_device_cb, &ctx, NULL);
  if (status != AE_OK) {
    return ZX_ERR_INTERNAL;
  }

  // Removes resources we believe are in use by other parts of the platform
  ctx = (struct report_current_resources_ctx){
      .pci_handle = root_resource_handle,
      .device_is_root_bridge = false,
      .add_pass = false,
  };
  status = AcpiGetDevices(NULL, pci_report_current_resources_device_cb, &ctx, NULL);
  if (status != AE_OK) {
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
zx_status_t pci_init(zx_device_t* sysdev, ACPI_HANDLE object, ACPI_DEVICE_INFO* info,
                     AcpiWalker* ctx) {
  if (!ctx->found_pci()) {
    // Report current resources to kernel PCI driver
    // Please do not use get_root_resource() in new code. See ZX-1467.
    zx_status_t status = pci_report_current_resources(get_root_resource());
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi: WARNING: ACPI failed to report all current resources!");
    }

    // Initialize kernel PCI driver
    zx_pci_init_arg_t* arg;
    uint32_t arg_size;
    status = get_pci_init_arg(&arg, &arg_size);
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi: erorr %d in get_pci_init_arg", status);
      return AE_ERROR;
    }

    // Please do not use get_root_resource() in new code. See ZX-1467.
    status = zx_pci_init(get_root_resource(), arg, arg_size);
    if (status != ZX_OK) {
      zxlogf(ERROR, "acpi: error %d in zx_pci_init", status);
      return AE_ERROR;
    }

    free(arg);

    // Publish PCI root as /dev/sys/ level.
    // Only publish one PCI root device for all PCI roots
    // TODO: store context for PCI root protocol
    std::array<zx_device_prop_t, 4> props;
    auto args = get_device_add_args("pci", info, &props);
    auto device = std::make_unique<AcpiDevice>(sysdev, object, ctx->platform_bus());

    args.version = DEVICE_ADD_ARGS_VERSION;
    args.ctx = device.get();
    args.ops = &acpi_device_proto;
    // The AcpiDevice implements both the Acpi protocol and the Pciroot protocol. We may find a
    // better way to do this in the future, but kernel PCI will eventually be removed anyway, at
    // which point we'll need to refactor it all anyway.
    args.proto_id = ZX_PROTOCOL_PCIROOT;
    args.proto_ops = get_pciroot_ops();

    if (zx_status_t status = device_add(sysdev, &args, device->mutable_zxdev()); status != ZX_OK) {
      zxlogf(ERROR, "acpi: error %d in device_add, parent=%s(%p)", status, device_get_name(sysdev),
             sysdev);
    } else {
      zxlogf(ERROR, "acpi: published device %s(%p), parent=%s(%p), handle=%p", args.name,
             device.get(), device_get_name(sysdev), sysdev, device->acpi_handle());
      ctx->set_found_pci(true);
      // device_add takes ownership of args.ctx, but only on success.
      device.release();
    }
  }
  // Get the PCI base bus number
  zx_status_t status = acpi_bbn_call(object, ctx->mutable_last_pci());
  if (status != ZX_OK) {
    zxlogf(ERROR,
           "acpi: failed to get PCI base bus number for device '%s' "
           "(status %u)",
           (const char*)&info->Name, status);
  }

  zxlogf(DEBUG, "acpi: found pci root #%u", ctx->last_pci());
  return ZX_OK;
}
