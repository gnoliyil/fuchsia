/*
 * Copyright (c) 2019 The Fuchsia Authors.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim.h"

#include <zircon/status.h>
#include <zircon/time.h>

#include <memory>

#include <wifi/wifi-config.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/bus.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/chip.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/common.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/debug.h"

#define BUS_OP(bus) bus->bus_priv.sim->sim_fw
static const struct brcmf_bus_ops brcmf_sim_bus_ops = {
    .get_bus_type = []() { return BRCMF_BUS_TYPE_SIM; },
    .get_bootloader_macaddr =
        [](brcmf_bus* bus, uint8_t* mac_addr) {
          return BUS_OP(bus)->BusGetBootloaderMacAddr(mac_addr);
        },
    .get_wifi_metadata =
        [](brcmf_bus* bus, void* data, size_t exp_size, size_t* actual) {
          wifi_config_t wifi_config = {
              .oob_irq_mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
              .iovar_table =
                  {
                      {IOVAR_LIST_END_TYPE, {{0}}, 0},
                  },
              .cc_table =
                  {
                      {"US", 842},
                      {"WW", 999},
                      {"", 0},
                  },
          };
          memcpy(data, &wifi_config, sizeof(wifi_config));
          *actual = sizeof(wifi_config);
          return ZX_OK;
        },
    .preinit = [](brcmf_bus* bus) { return BUS_OP(bus)->BusPreinit(); },
    .stop = [](brcmf_bus* bus) { return BUS_OP(bus)->BusStop(); },
    .txframes =
        [](brcmf_bus* bus, cpp20::span<wlan::drivers::components::Frame> frames) {
          return BUS_OP(bus)->BusTxFrames(frames);
        },
    .txctl = [](brcmf_bus* bus, unsigned char* msg,
                uint len) { return BUS_OP(bus)->BusTxCtl(msg, len); },
    .rxctl = [](brcmf_bus* bus, unsigned char* msg, uint len,
                int* rxlen_out) { return BUS_OP(bus)->BusRxCtl(msg, len, rxlen_out); },
    .flush_txq = [](brcmf_bus* bus, int ifidx) { return BUS_OP(bus)->BusFlushTxQueue(ifidx); },
    .get_tx_depth =
        [](brcmf_bus* bus, uint16_t* tx_depth_out) {
          *tx_depth_out = 1;
          return ZX_OK;
        },
    .get_rx_depth =
        [](brcmf_bus* bus, uint16_t* rx_depth_out) {
          *rx_depth_out = 1;
          return ZX_OK;
        },
    .get_tail_length =
        [](brcmf_bus* bus, uint16_t* tail_length_out) {
          *tail_length_out = 0;
          return ZX_OK;
        },
    .recovery = [](brcmf_bus* bus) { return brcmf_sim_recovery(bus); },
    .log_stats = [](brcmf_bus* bus) { BRCMF_INFO("Simulated bus, no stats to log"); },
    .prepare_vmo = [](brcmf_bus*, uint8_t, zx_handle_t, uint8_t*, size_t) { return ZX_OK; },
    .queue_rx_space =
        [](brcmf_bus* bus, const rx_space_buffer_t* buffer_list, size_t buffer_count,
           uint8_t* vmo_addrs[]) {
          return BUS_OP(bus)->BusQueueRxSpace(buffer_list, buffer_count, vmo_addrs);
        },
    .acquire_tx_space = [](brcmf_bus* bus,
                           size_t count) { return BUS_OP(bus)->BusAcquireTxSpace(count); },
};
#undef BUS_OP

// Get device-specific information
static void brcmf_sim_probe(struct brcmf_bus* bus) {
  uint32_t chip, chiprev;

  bus->bus_priv.sim->sim_fw->GetChipInfo(&chip, &chiprev);
  bus->chip = chip;
  bus->chiprev = chiprev;

  brcmf_get_module_param(BRCMF_BUS_TYPE_SIM, chip, chiprev, bus->bus_priv.sim->settings.get());
}

zx_status_t brcmf_sim_alloc(brcmf_pub* drvr, std::unique_ptr<brcmf_bus>* out_bus,
                            ::wlan::simulation::FakeDevMgr* dev_mgr,
                            std::shared_ptr<::wlan::simulation::Environment> env) {
  auto simdev = new brcmf_simdev();
  auto bus_if = std::make_unique<brcmf_bus>();

  // Initialize inter-structure pointers
  simdev->drvr = drvr;
  simdev->env = env;
  simdev->sim_fw = std::make_unique<::wlan::brcmfmac::SimFirmware>(simdev);
  simdev->dev_mgr = dev_mgr;
  simdev->settings = std::make_unique<brcmf_mp_device>();
  bus_if->bus_priv.sim = simdev;

  bus_if->ops = &brcmf_sim_bus_ops;
  drvr->bus_if = bus_if.get();
  drvr->settings = simdev->settings.get();

  *out_bus = std::move(bus_if);
  return ZX_OK;
}

zx_status_t brcmf_sim_register(brcmf_pub* drvr) {
  BRCMF_DBG(SIM, "Registering simulator target");
  brcmf_sim_probe(drvr->bus_if);

  zx_status_t status = brcmf_attach(drvr);
  if (status != ZX_OK) {
    BRCMF_ERR("brcmf_attach failed");
    return status;
  }

  status = brcmf_proto_bcdc_attach(drvr);
  if (status != ZX_OK) {
    BRCMF_ERR("brcmf_proto_bcdc_attach failed: %s", zx_status_get_string(status));
    brcmf_detach(drvr);
    return status;
  }

  // Here is where we would likely simulate loading the firmware into the target. For now,
  // we don't try.

  status = brcmf_bus_started(drvr, false);
  if (status != ZX_OK) {
    BRCMF_ERR("brcmf_bus_started failed: %s", zx_status_get_string(status));
    brcmf_detach(drvr);
    brcmf_proto_bcdc_detach(drvr);
  }
  return status;
}

// Handle a simulator event
void brcmf_sim_rx_event(brcmf_simdev* simdev, std::shared_ptr<std::vector<uint8_t>> buffer) {
  // We use the passed in buffer as the backing storage for the event frame.
  //
  // The call to brcmf_rx_event() is all synchronous in sim (see fweh.cc), so we don't have to worry
  // about the lifetime of the storage -- the driver won't be accessing it asynchronously outside of
  // this function call.
  //
  // And unlike the data path in brcmf_sim_rx_frame(), the driver accesses the data directly. It
  // does not use region descriptors in shared memory to communicate, so there isn't really a reason
  // to use shared memory here.
  //
  // If we choose to make sim test events async in the future, we'll need to change this to either
  // copy or keep a shared_ptr pointer pointing to the buffer.
  wlan::drivers::components::Frame frame(nullptr, 0, 0, 0, buffer->data(), buffer->size(), 0);
  brcmf_rx_event(simdev->drvr, std::move(frame));
}

// Handle a simulator frame
void brcmf_sim_rx_frame(brcmf_simdev* simdev, std::shared_ptr<std::vector<uint8_t>> buffer) {
  ZX_ASSERT_MSG(buffer->size() >= ETH_HLEN, "Malformed packet");

  auto maybe_frame = simdev->sim_fw->GetRxFrame();
  ZX_ASSERT_MSG(maybe_frame.has_value(),
                "Simulator tried to receive a frame without queueing space");

  auto& frame = maybe_frame.value();
  ZX_ASSERT_MSG(frame.Size() >= buffer->size(), "Queued rx frame is too small");

  memcpy(frame.Data(), buffer->data(), buffer->size());
  frame.SetSize(buffer->size());
  brcmf_rx_frame(simdev->drvr, std::move(frame), false);
}

zx_status_t brcmf_sim_recovery(brcmf_bus* bus) {
  brcmf_simdev* simdev = bus->bus_priv.sim;

  // Go through the recovery process in SIM bus(Here we just do firmware reset
  // instead of firmware reload).
  simdev->sim_fw = std::make_unique<::wlan::brcmfmac::SimFirmware>(simdev);
  return ZX_OK;
}

void brcmf_sim_firmware_crash(brcmf_simdev* simdev) {
  brcmf_pub* drvr = simdev->drvr;
  zx_status_t err = drvr->recovery_trigger->firmware_crash_.Inc();
  if (err != ZX_OK) {
    BRCMF_ERR("Increase recovery trigger condition failed -- error: %s", zx_status_get_string(err));
  }
  // Clear the counters of all TriggerConditions here instead of inside brcmf_recovery_worker() to
  // break deadlock.
  drvr->recovery_trigger->ClearStatistics();
}

void brcmf_sim_exit(brcmf_bus* bus) {
  brcmf_detach((bus->bus_priv.sim)->drvr);
  brcmf_proto_bcdc_detach((bus->bus_priv.sim)->drvr);
  delete bus->bus_priv.sim;
  bus->bus_priv.sim = nullptr;
}
