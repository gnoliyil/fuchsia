// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "../include/lib/virtio/backends/pci.h"

#include <assert.h>
#include <lib/ddk/debug.h>
#include <lib/device-protocol/pci.h>
#include <lib/zx/handle.h>
#include <lib/zx/interrupt.h>
#include <lib/zx/port.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/syscalls/port.h>

#include <algorithm>

#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <virtio/virtio.h>

namespace virtio {

namespace fpci = fuchsia_hardware_pci;

PciBackend::PciBackend(ddk::Pci pci, fuchsia_hardware_pci::wire::DeviceInfo info)
    : pci_(std::move(pci)), info_(info) {
  snprintf(tag_, sizeof(tag_), "pci[%02x:%02x.%1x]", info_.bus_id, info_.dev_id, info_.func_id);
}

zx_status_t PciBackend::Bind() {
  zx::interrupt interrupt;
  zx_status_t st;

  st = zx::port::create(/*options=*/ZX_PORT_BIND_TO_INTERRUPT, &wait_port_);
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s: cannot create wait port: %s", tag(), zx_status_get_string(st));
    return st;
  }

  // enable bus mastering
  if ((st = pci().SetBusMastering(true)) != ZX_OK) {
    zxlogf(ERROR, "%s: cannot enable bus master: %s", tag(), zx_status_get_string(st));
    return st;
  }

  if ((st = ConfigureInterruptMode()) != ZX_OK) {
    zxlogf(ERROR, "%s: cannot configure IRQs: %s", tag(), zx_status_get_string(st));
    return st;
  }

  return Init();
}

// Virtio supports both a legacy INTx IRQ as well as MSI-X. In the former case,
// a driver is required to read the ISR_STATUS register to determine what sort
// of event has happened. This can be an expensive operation depending on the
// hypervisor / emulation environment. For MSI-X a device 'should' support 2 or
// more vector table entries, but is not required to. Since we only have one IRQ
// worker in the backends at this time it's not that important that we allocate
// a vector per ring, so for now the ideal is roughly two vectors, one being for
// config changes and the other for rings.
zx_status_t PciBackend::ConfigureInterruptMode() {
  // This looks a lot like something ConfigureInterruptMode was designed for, but
  // since we have a specific requirement to use MSI-X if and only if we have 2
  // vectors it means rolling it by hand.
  fuchsia_hardware_pci::wire::InterruptModes modes{};
  pci().GetInterruptModes(&modes);
  fuchsia_hardware_pci::InterruptMode mode = fuchsia_hardware_pci::InterruptMode::kLegacy;
  uint32_t irq_cnt = 1;

  if (modes.msix_count >= 2) {
    mode = fuchsia_hardware_pci::InterruptMode::kMsiX;
    irq_cnt = 2;
  } else if (modes.has_legacy == 0) {
    zxlogf(ERROR, "No interrupt support found for device!");
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t st = pci().SetInterruptMode(mode, irq_cnt);
  if (st != ZX_OK) {
    zxlogf(ERROR, "Failed to configure a virtio IRQ mode: %s", zx_status_get_string(st));
    return st;
  }

  // Legacy only supports 1 IRQ, but for MSI-X we only need 2
  for (uint32_t i = 0; i < irq_cnt; i++) {
    zx::interrupt interrupt = {};
    if ((st = pci().MapInterrupt(i, &interrupt)) != ZX_OK) {
      zxlogf(ERROR, "Failed to map interrupt %u: %s", i, zx_status_get_string(st));
      return st;
    }

    // Use the interrupt index as the key so we can ack the correct interrupt after
    // a port wait.
    if ((st = interrupt.bind(wait_port_, /*key=*/i, /*options=*/0)) != ZX_OK) {
      zxlogf(ERROR, "Failed to bind interrupt %u: %s", i, zx_status_get_string(st));
      return st;
    }
    irq_handles().push_back(std::move(interrupt));
  }
  irq_mode() = mode;
  zxlogf(DEBUG, "%s: using %s IRQ mode (irq_cnt = %u)", tag(),
         (irq_mode() == fpci::InterruptMode::kMsiX ? "MSI-X" : "legacy"), irq_cnt);
  return ZX_OK;
}

zx::result<uint32_t> PciBackend::WaitForInterrupt() {
  zx_port_packet packet;
  zx_status_t st = wait_port_.wait(zx::deadline_after(zx::msec(100)), &packet);
  if (st != ZX_OK) {
    return zx::error(st);
  }

  return zx::ok(packet.key);
}

void PciBackend::InterruptAck(uint32_t key) {
  ZX_DEBUG_ASSERT(key < irq_handles().size());
  irq_handles()[key].ack();
  if (irq_mode() == fpci::InterruptMode::kLegacy) {
    pci().AckInterrupt();
  }
}

}  // namespace virtio
