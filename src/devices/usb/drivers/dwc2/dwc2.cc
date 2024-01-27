// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/usb/drivers/dwc2/dwc2.h"

#include <lib/ddk/binding_driver.h>
#include <lib/ddk/hw/arch_ops.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/sync/completion.h>
#include <lib/zx/clock.h>
#include <lib/zx/profile.h>
#include <lib/zx/time.h>
#include <threads.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/threads.h>

#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <usb/usb-request.h>
#include <usb/usb.h>

#include "src/devices/usb/drivers/dwc2/usb_dwc_regs.h"

namespace dwc2 {

using Request = usb::BorrowedRequest<void>;

// Handler for usbreset interrupt.
void Dwc2::HandleReset() {
  auto* mmio = get_mmio();

  zxlogf(SERIAL, "\nRESET");

  ep0_state_ = Ep0State::DISCONNECTED;
  configured_ = false;

  // Clear remote wakeup signalling
  DCTL::Get().ReadFrom(mmio).set_rmtwkupsig(0).WriteTo(mmio);

  for (uint32_t i = 0; i < MAX_EPS_CHANNELS; i++) {
    auto diepctl = DEPCTL::Get(i).ReadFrom(mmio);

    // Disable IN endpoints
    if (diepctl.epena()) {
      diepctl.set_snak(1);
      diepctl.set_epdis(1);
      diepctl.WriteTo(mmio);
    }

    // Clear snak on OUT endpoints
    DEPCTL::Get(i + DWC_EP_OUT_SHIFT).ReadFrom(mmio).set_snak(1).WriteTo(mmio);
  }

  // Flush endpoint zero TX FIFO
  FlushTxFifo(0);

  // Flush All other endpoint TX FIFOs.
  FlushTxFifo(0x10);

  // Flush the learning queue
  GRSTCTL::Get().FromValue(0).set_intknqflsh(1).WriteTo(mmio);

  // Enable interrupts for only EPO IN and OUT
  DAINTMSK::Get().FromValue((1 << DWC_EP0_IN) | (1 << DWC_EP0_OUT)).WriteTo(mmio);

  // Enable various endpoint specific interrupts
  DOEPMSK::Get()
      .FromValue(0)
      .set_setup(1)
      .set_stsphsercvd(1)
      .set_xfercompl(1)
      .set_ahberr(1)
      .set_epdisabled(1)
      .WriteTo(mmio);
  DIEPMSK::Get()
      .FromValue(0)
      .set_xfercompl(1)
      .set_timeout(1)
      .set_ahberr(1)
      .set_epdisabled(1)
      .WriteTo(mmio);

  // Clear device address
  DCFG::Get().ReadFrom(mmio).set_devaddr(0).WriteTo(mmio);

  SetConnected(false);
}

// Handler for usbsuspend interrupt.
void Dwc2::HandleSuspend() { SetConnected(false); }

// Handler for enumdone interrupt.
void Dwc2::HandleEnumDone() {
  SetConnected(true);

  auto* mmio = get_mmio();

  ep0_state_ = Ep0State::IDLE;

  endpoints_[DWC_EP0_IN]->max_packet_size = 64;
  endpoints_[DWC_EP0_OUT]->max_packet_size = 64;
  endpoints_[DWC_EP0_IN]->phys = static_cast<uint32_t>(ep0_buffer_.phys());
  endpoints_[DWC_EP0_OUT]->phys = static_cast<uint32_t>(ep0_buffer_.phys());

  DEPCTL0::Get(DWC_EP0_IN).ReadFrom(mmio).set_mps(DEPCTL0::MPS_64).WriteTo(mmio);
  DEPCTL0::Get(DWC_EP0_OUT).ReadFrom(mmio).set_mps(DEPCTL0::MPS_64).WriteTo(mmio);

  DCTL::Get().ReadFrom(mmio).set_cgnpinnak(1).WriteTo(mmio);

  GUSBCFG::Get().ReadFrom(mmio).set_usbtrdtim(metadata_.usb_turnaround_time).WriteTo(mmio);

  if (dci_intf_) {
    dci_intf_->SetSpeed(USB_SPEED_HIGH);
  }
  StartEp0();
}

// Handler for inepintr interrupt.
void Dwc2::HandleInEpInterrupt() {
  if (timeout_recovering_) {
    // TODO(105382) remove logging once timeout recovery has stabilized.
    zxlogf(ERROR, "(diepint.timeout) IN-ep interrupt with timeout_recovering_ = true");

    // We'll assume the condition is cleaned up as a side effect of irq dispatching.
    timeout_recovering_ = false;
  }

  auto* mmio = get_mmio();
  uint8_t ep_num = 0;

  // Read bits indicating which endpoints have inepintr active
  uint32_t ep_bits = DAINT::Get().ReadFrom(mmio).reg_value();
  ep_bits &= DAINTMSK::Get().ReadFrom(mmio).reg_value();
  ep_bits &= DWC_EP_IN_MASK;

  // Acknowledge the endpoint bits
  DAINT::Get().FromValue(DWC_EP_IN_MASK).WriteTo(mmio);

  // Loop through IN endpoints and handle those with interrupt raised
  while (ep_bits) {
    if (ep_bits & 1) {
      auto diepint = DIEPINT::Get(ep_num).ReadFrom(mmio);
      auto diepmsk = DIEPMSK::Get().ReadFrom(mmio);

      if (diepint.timeout()) {
        // TODO(105382) remove logging once timeout recovery has stabilized.
        zxlogf(ERROR, "(diepint.timeout) (ep%u) DIEPINT=0x%08x DIEPMSK=0x%08x", ep_num,
               diepint.reg_value(), diepmsk.reg_value());
      }

      diepint.set_reg_value(diepint.reg_value() & diepmsk.reg_value());

      if (diepint.xfercompl()) {
        if (timeout_recovering_) {
          // (special case) We're recovering from a diepint.timeout interrupt.
          // TODO(105382) remove logging once timeout recovery has stabilized.
          //   The current control logic should account for this.
          zxlogf(ERROR, "(diepint.timeout recovery) IN-EP0 xfer-complete interrupt");
          timeout_recovering_ = false;
        }

        DIEPINT::Get(ep_num).FromValue(0).set_xfercompl(1).WriteTo(mmio);

        if (ep_num == DWC_EP0_IN) {
          HandleEp0TransferComplete(true);
        } else {
          HandleTransferComplete(ep_num);
          if (diepint.nak()) {
            zxlogf(ERROR, "Unhandled interrupt diepint.nak ep_num %u", ep_num);
            DIEPINT::Get(ep_num).ReadFrom(mmio).set_nak(1).WriteTo(mmio);
          }
        }
      }

      // Timeout on receiving appropriate 2.0 token from host.
      if (diepint.timeout() && ep_num == DWC_EP0_IN) {
        switch (ep0_state_) {
          case Ep0State::DATA: {
            // TODO(105382) remove logging once timeout recovery has stabilized.
            //   - what does the core think it's doing at wrt NAK status?
            //   - Is the core globally NAK-ing all non-periodic IN endpoints at the moment?
            auto dctl = DCTL::Get().ReadFrom(mmio);
            auto depctl0 = DEPCTL0::Get(0).ReadFrom(mmio);
            zxlogf(ERROR,
                   "Got diepint.timeout for DATA_IN phase on ep%u, attempting to recover. "
                   "DCTL=0x%08x DEPCTL0=0x%08x",
                   ep_num, dctl.reg_value(), depctl0.reg_value());

            // The timeout is due to one of two cases:
            //   1. The core never received an ACK to sent IN-data. In this case, the host
            //      successfully received IN-data, and will subsequently ACK the transmission. That
            //      ACK was lost in transit to the core.
            //   2. IN-data was lost in transmission to the host. In this case, the host will
            //      re-issue an IN-token requesting the data be retransmitted.
            //
            // In the case of #1, the core is in a state where it NAKs all incoming tokens on
            // OUT-EP0. It needs to clear NAK state and prepare to receive an ACK token from the
            // host.
            //
            // In the case of #2, the core needs to prepare to retransmit the lost data (which
            // remains in the FIFO).
            HandleEp0TimeoutRecovery();

            // The recovery logic currently executes a soft disconnect and core reset. We don't want
            // to process any further interrupts.
            return;
          }
          case Ep0State::DISCONNECTED:
          case Ep0State::IDLE:
          case Ep0State::STATUS:
          case Ep0State::TIMEOUT_RECOVERY:
          case Ep0State::STALL:
            // The other cases should either theoretically never happen, or handled by the core
            // internally in dedicate-FIFO mode, but we'll log anyway.
            zxlogf(ERROR, "Unhandled IN EP0 diepint.timeout at txn phase %s",
                   Ep0StateToStr(ep0_state_));

            DIEPINT::Get(ep_num).ReadFrom(mmio).set_timeout(1).WriteTo(mmio);
            break;
        }
      }

      // TODO(voydanoff) Implement error recovery for these interrupts
      if (diepint.epdisabled()) {
        zxlogf(ERROR, "Unhandled interrupt diepint.epdisabled for ep_num %u", ep_num);
        DIEPINT::Get(ep_num).ReadFrom(mmio).set_epdisabled(1).WriteTo(mmio);
      }
      if (diepint.ahberr()) {
        zxlogf(ERROR, "Unhandled interrupt diepint.ahberr for ep_num %u", ep_num);
        DIEPINT::Get(ep_num).ReadFrom(mmio).set_ahberr(1).WriteTo(mmio);
      }
      if (diepint.timeout() && ep_num != DWC_EP0_IN) {
        // Note the DWC_EP0_IN case is handled above.
        if (ep_num == DWC_EP0_OUT) {
          // This should theoretically never happen in dedicated-FIFO mode, but we'll log anyway.
          zxlogf(ERROR, "Unhandled OUT EP0 diepint.timeout at txn phase %s",
                 Ep0StateToStr(ep0_state_));
        } else {
          zxlogf(ERROR, "Unhandled interrupt diepint.timeout for ep_num %u", ep_num);
        }
        DIEPINT::Get(ep_num).ReadFrom(mmio).set_timeout(1).WriteTo(mmio);
      }
      if (diepint.intktxfemp()) {
        zxlogf(ERROR, "Unhandled interrupt diepint.intktxfemp for ep_num %u", ep_num);
        DIEPINT::Get(ep_num).ReadFrom(mmio).set_intktxfemp(1).WriteTo(mmio);
      }
      if (diepint.intknepmis()) {
        zxlogf(ERROR, "Unhandled interrupt diepint.intknepmis for ep_num %u", ep_num);
        DIEPINT::Get(ep_num).ReadFrom(mmio).set_intknepmis(1).WriteTo(mmio);
      }
      if (diepint.inepnakeff()) {
        printf("Unhandled interrupt diepint.inepnakeff for ep_num %u\n", ep_num);
        DIEPINT::Get(ep_num).ReadFrom(mmio).set_inepnakeff(1).WriteTo(mmio);
      }
    }
    ep_num++;
    ep_bits >>= 1;
  }
}

// Handler for outepintr interrupt.
void Dwc2::HandleOutEpInterrupt() {
  if (timeout_recovering_) {
    // TODO(105382) remove logging once timeout recovery has stabilized.
    zxlogf(ERROR, "(diepint.timeout) OUT-ep interrupt with timeout_recovering_ = true");
  }

  auto* mmio = get_mmio();

  uint8_t ep_num = DWC_EP0_OUT;

  // Read bits indicating which endpoints have outepintr active
  auto ep_bits = DAINT::Get().ReadFrom(mmio).reg_value();
  auto ep_mask = DAINTMSK::Get().ReadFrom(mmio).reg_value();
  ep_bits &= ep_mask;
  ep_bits &= DWC_EP_OUT_MASK;
  ep_bits >>= DWC_EP_OUT_SHIFT;

  // Acknowledge the endpoint bits
  DAINT::Get().FromValue(DWC_EP_OUT_MASK).WriteTo(mmio);

  // Loop through OUT endpoints and handle those with interrupt raised
  while (ep_bits) {
    if (ep_bits & 1) {
      auto doepint = DOEPINT::Get(ep_num).ReadFrom(mmio);
      auto doepmsk = DOEPMSK::Get().ReadFrom(mmio);

      if (timeout_recovering_) {
        // TODO(105382) remove logging once timeout recovery has stabilized.
        // N.B. the use of "(diepint.timeout)" here is used for a searchable-term.
        zxlogf(ERROR, "(diepint.timeout) OUT-ep (ep%u, state=%s) DOEPINT=0x%08x DOEPMSK=0x%08x",
               ep_num, Ep0StateToStr(ep0_state_), doepint.reg_value(), doepmsk.reg_value());
      }

      doepint.set_reg_value(doepint.reg_value() & DOEPMSK::Get().ReadFrom(mmio).reg_value());

      if (doepint.sr()) {
        DOEPINT::Get(ep_num).ReadFrom(mmio).set_sr(1).WriteTo(mmio);
      }

      if (doepint.stsphsercvd()) {
        DOEPINT::Get(ep_num).ReadFrom(mmio).set_stsphsercvd(1).WriteTo(mmio);
      }

      if (doepint.setup()) {
        // TODO(voydanoff):   On this interrupt, the application must read the DOEPTSIZn
        // register to determine the number of SETUP packets received and process the last
        // received SETUP packet.
        DOEPINT::Get(ep_num).ReadFrom(mmio).set_setup(1).WriteTo(mmio);

        memcpy(&cur_setup_, ep0_buffer_.virt(), sizeof(cur_setup_));
        zxlogf(SERIAL,
               "SETUP bm_request_type: 0x%02x b_request: %u w_value: %u w_index: %u "
               "w_length: %u\n",
               cur_setup_.bm_request_type, cur_setup_.b_request, cur_setup_.w_value,
               cur_setup_.w_index, cur_setup_.w_length);

        HandleEp0Setup();
      }
      if (doepint.xfercompl()) {
        DOEPINT::Get(ep_num).FromValue(0).set_xfercompl(1).WriteTo(mmio);

        if (ep_num == DWC_EP0_OUT) {
          if (!doepint.setup()) {
            HandleEp0TransferComplete(false);
          }
        } else {
          HandleTransferComplete(ep_num);
        }
      }
      // TODO(voydanoff) Implement error recovery for these interrupts
      if (doepint.epdisabled()) {
        zxlogf(ERROR, "Unhandled interrupt doepint.epdisabled for ep_num %u", ep_num);
        DOEPINT::Get(ep_num).ReadFrom(mmio).set_epdisabled(1).WriteTo(mmio);
      }
      if (doepint.ahberr()) {
        zxlogf(ERROR, "Unhandled interrupt doepint.ahberr for ep_num %u", ep_num);
        DOEPINT::Get(ep_num).ReadFrom(mmio).set_ahberr(1).WriteTo(mmio);
      }
    }
    ep_num++;
    ep_bits >>= 1;
  }

  if (timeout_recovering_) {
    // TODO(105382) remove logging once timeout recovery has stabilized.
    // We'll assume the condition is cleaned up as a side effect of irq dispatching.
    zxlogf(ERROR, "(diepint.timeout) clearing timeout_recovering_ condition in state=%s",
           Ep0StateToStr(ep0_state_));
    timeout_recovering_ = false;
  }
}

// Handles setup requests from the host.
zx_status_t Dwc2::HandleSetupRequest(size_t* out_actual) {
  zx_status_t status;

  auto* setup = &cur_setup_;
  auto* buffer = ep0_buffer_.virt();
  zx::duration elapsed;
  zx::time now;
  if (setup->bm_request_type == (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE)) {
    // Handle some special setup requests in this driver
    switch (setup->b_request) {
      case USB_REQ_SET_ADDRESS:
        zxlogf(SERIAL, "SET_ADDRESS %d", setup->w_value);
        SetAddress(static_cast<uint8_t>(setup->w_value));
        now = zx::clock::get_monotonic();
        elapsed = now - irq_timestamp_;
        zxlogf(
            INFO,
            "Took %i microseconds to reply to SET_ADDRESS interrupt\nStarted waiting at %lx\nGot "
            "hardware IRQ at %lx\nFinished processing at %lx, context switch happened at %lx",
            static_cast<int>(elapsed.to_usecs()), wait_start_time_.get(), irq_timestamp_.get(),
            now.get(), irq_dispatch_timestamp_.get());
        if (elapsed.to_msecs() > 2) {
          zxlogf(ERROR, "Handling SET_ADDRESS took greater than 2ms");
        }
        *out_actual = 0;
        return ZX_OK;
      case USB_REQ_SET_CONFIGURATION:
        zxlogf(SERIAL, "SET_CONFIGURATION %d", setup->w_value);
        configured_ = true;
        if (dci_intf_) {
          status = dci_intf_->Control(setup, nullptr, 0, nullptr, 0, out_actual);
        } else {
          status = ZX_ERR_NOT_SUPPORTED;
        }
        if (status == ZX_OK && setup->w_value) {
          StartEndpoints();
        } else {
          configured_ = false;
        }
        return status;
      default:
        // fall through to dci_intf_->Control()
        break;
    }
  }

  bool is_in = ((setup->bm_request_type & USB_DIR_MASK) == USB_DIR_IN);
  auto length = le16toh(setup->w_length);

  if (dci_intf_) {
    if (length == 0) {
      status = dci_intf_->Control(setup, nullptr, 0, nullptr, 0, out_actual);
    } else if (is_in) {
      status = dci_intf_->Control(setup, nullptr, 0, reinterpret_cast<uint8_t*>(buffer), length,
                                  out_actual);
    } else {
      status = ZX_ERR_NOT_SUPPORTED;
    }
  } else {
    status = ZX_ERR_NOT_SUPPORTED;
  }
  if (status == ZX_OK) {
    auto& ep = endpoints_[DWC_EP0_OUT];
    ep->req_offset = 0;
    if (is_in) {
      ep->req_length = static_cast<uint32_t>(*out_actual);
    }
  }
  return status;
}

// Programs the device address received from the SET_ADDRESS command from the host
void Dwc2::SetAddress(uint8_t address) {
  auto* mmio = get_mmio();

  DCFG::Get().ReadFrom(mmio).set_devaddr(address).WriteTo(mmio);
}

// Reads number of bytes transfered on specified endpoint
uint32_t Dwc2::ReadTransfered(Endpoint* ep) {
  auto* mmio = get_mmio();
  return ep->req_xfersize - DEPTSIZ::Get(ep->ep_addr()).ReadFrom(mmio).xfersize();
}

// Prepares to receive next control request on endpoint zero.
void Dwc2::StartEp0() {
  auto* mmio = get_mmio();
  auto& ep = endpoints_[DWC_EP0_OUT];
  ep->req_offset = 0;
  ep->req_xfersize = 3 * sizeof(usb_setup_t);

  ep0_buffer_.CacheFlushInvalidate(0, sizeof(cur_setup_));

  DEPDMA::Get(DWC_EP0_OUT)
      .FromValue(0)
      .set_addr(static_cast<uint32_t>(ep0_buffer_.phys()))
      .WriteTo(get_mmio());

  DEPTSIZ0::Get(DWC_EP0_OUT)
      .FromValue(0)
      .set_supcnt(3)
      .set_pktcnt(1)
      .set_xfersize(ep->req_xfersize)
      .WriteTo(mmio);
  hw_wmb();

  DEPCTL::Get(DWC_EP0_OUT).ReadFrom(mmio).set_epena(1).WriteTo(mmio);
  hw_wmb();
}

// Queues the next USB request for the specified endpoint
void Dwc2::QueueNextRequest(Endpoint* ep) {
  if (ep->current_req || ep->queued_reqs.empty()) {
    return;
  }

  ep->current_req.emplace(std::move(ep->queued_reqs.front()));
  ep->queued_reqs.pop();

  auto status =
      std::visit([this](auto&& req) -> zx_status_t { return req.PhysMap(bti_); }, *ep->current_req);
  ZX_ASSERT_MSG(status == ZX_OK, "PhysMap failed");
  auto iters = ep->get_iter(*ep->current_req, zx_system_get_page_size());
  ZX_DEBUG_ASSERT(iters.is_ok());
  // Dwc2 currently does not support scatter gather as it is using Buffer DMA mode (Chapter 9 of
  // dwc2 specs). To use scatter gather, we need to use Scatter/Gather DMA mode (Chapter 10).
  ZX_ASSERT_MSG(iters->size() == 1, "Currently do not support scatter gather");
  auto iter = iters->at(0).begin();

  ep->phys = static_cast<uint32_t>((*iter).first);
  ep->req_offset = 0;
  ep->req_length = static_cast<uint32_t>((*iter).second);
  StartTransfer(ep, ep->req_length);
}

void Dwc2::StartTransfer(Endpoint* ep, uint32_t length) {
  auto ep_num = ep->ep_addr();
  auto* mmio = get_mmio();
  bool is_in = DWC_EP_IS_IN(ep_num);

  // FidlRequests should be flushed already by higher level drivers!
  if (length > 0 && (!ep->current_req || std::holds_alternative<Request>(*ep->current_req))) {
    if (is_in) {
      if (ep_num == DWC_EP0_IN) {
        ep0_buffer_.CacheFlush(ep->req_offset, length);
      } else {
        usb_request_cache_flush(std::get<Request>(ep->current_req.value()).request(),
                                ep->req_offset, length);
      }
    } else {
      if (ep_num == DWC_EP0_OUT) {
        ep0_buffer_.CacheFlushInvalidate(ep->req_offset, length);
      } else {
        usb_request_cache_flush_invalidate(std::get<Request>(ep->current_req.value()).request(),
                                           ep->req_offset, length);
      }
    }
  }

  // Program DMA address
  DEPDMA::Get(ep_num).FromValue(0).set_addr(ep->phys + ep->req_offset).WriteTo(mmio);

  uint32_t ep_mps = ep->max_packet_size;
  auto deptsiz = DEPTSIZ::Get(ep_num).FromValue(0);

  if (length == 0) {
    deptsiz.set_xfersize(is_in ? 0 : ep_mps);
    deptsiz.set_pktcnt(1);
  } else {
    deptsiz.set_pktcnt((length + (ep_mps - 1)) / ep_mps);
    deptsiz.set_xfersize(length);
  }
  deptsiz.set_mc(is_in ? 1 : 0);
  ep->req_xfersize = deptsiz.xfersize();
  deptsiz.WriteTo(mmio);
  hw_wmb();

  DEPCTL::Get(ep_num).ReadFrom(mmio).set_cnak(1).set_epena(1).WriteTo(mmio);
  hw_wmb();
}

void Dwc2::FlushTxFifo(uint32_t fifo_num) {
  auto* mmio = get_mmio();

  auto grstctl = GRSTCTL::Get().FromValue(0).set_txfflsh(1).set_txfnum(fifo_num).WriteTo(mmio);

  uint32_t count = 0;
  do {
    grstctl.ReadFrom(mmio);
    // Retry count of 10000 comes from Amlogic bootloader driver.
    if (++count > 10000) {
      zxlogf(ERROR, "took more than 10k cycles to TX-FIFO flush for FIFO-%d", fifo_num);
      break;
    }
  } while (grstctl.txfflsh() == 1);

  zx::nanosleep(zx::deadline_after(zx::usec(1)));
}

void Dwc2::FlushRxFifo() {
  auto* mmio = get_mmio();
  auto grstctl = GRSTCTL::Get().FromValue(0).set_rxfflsh(1).WriteTo(mmio);

  uint32_t count = 0;
  do {
    grstctl.ReadFrom(mmio);
    if (++count > 10000)
      break;
  } while (grstctl.rxfflsh() == 1);

  zx::nanosleep(zx::deadline_after(zx::usec(1)));
}

void Dwc2::FlushTxFifoRetryIndefinite(uint32_t fifo_num) {
  auto* mmio = get_mmio();

  auto grstctl = GRSTCTL::Get().FromValue(0).set_txfflsh(1).set_txfnum(fifo_num).WriteTo(mmio);

  do {
    grstctl.ReadFrom(mmio);
  } while (grstctl.txfflsh() == 1);

  zx::nanosleep(zx::deadline_after(zx::usec(1)));
}

void Dwc2::FlushRxFifoRetryIndefinite() {
  auto* mmio = get_mmio();
  auto grstctl = GRSTCTL::Get().FromValue(0).set_rxfflsh(1).WriteTo(mmio);

  do {
    grstctl.ReadFrom(mmio);
  } while (grstctl.rxfflsh() == 1);

  zx::nanosleep(zx::deadline_after(zx::usec(1)));
}

void Dwc2::StartEndpoints() {
  for (uint8_t ep_num = 1; ep_num < std::size(endpoints_); ep_num++) {
    auto& ep = endpoints_[ep_num];
    if (ep->enabled) {
      EnableEp(ep_num, true);

      fbl::AutoLock lock(&ep->lock);
      QueueNextRequest(&*ep);
    }
  }
}

void Dwc2::EnableEp(uint8_t ep_num, bool enable) {
  auto* mmio = get_mmio();

  fbl::AutoLock lock(&lock_);

  uint32_t bit = 1 << ep_num;

  auto mask = DAINTMSK::Get().ReadFrom(mmio).reg_value();
  if (enable) {
    auto daint = DAINT::Get().ReadFrom(mmio).reg_value();
    daint |= bit;
    DAINT::Get().FromValue(daint).WriteTo(mmio);
    mask |= bit;
  } else {
    mask &= ~bit;
  }
  DAINTMSK::Get().FromValue(mask).WriteTo(mmio);
}

void Dwc2::HandleEp0Setup() {
  auto* setup = &cur_setup_;

  auto length = letoh16(setup->w_length);
  bool is_in = ((setup->bm_request_type & USB_DIR_MASK) == USB_DIR_IN);
  size_t actual = 0;

  // No data to read, can handle setup now
  if (length == 0 || is_in) {
    // TODO(voydanoff) stall if this fails (after we implement stalling)
    [[maybe_unused]] zx_status_t _ = HandleSetupRequest(&actual);
  }

  if (length > 0) {
    ep0_state_ = Ep0State::DATA;
    auto& ep = endpoints_[is_in ? DWC_EP0_IN : DWC_EP0_OUT];
    ep->req_offset = 0;

    if (is_in) {
      ep->req_length = static_cast<uint32_t>(actual);
      fbl::AutoLock al(&ep->lock);
      StartTransfer(&*ep, (ep->req_length > 127 ? ep->max_packet_size : ep->req_length));
    } else {
      ep->req_length = length;
      fbl::AutoLock al(&ep->lock);
      StartTransfer(&*ep, (length > 127 ? ep->max_packet_size : length));
    }
  } else {
    // no data phase
    // status in IN direction
    HandleEp0Status(true);
  }
}

// Handles status phase of a setup request
void Dwc2::HandleEp0Status(bool is_in) {
  ep0_state_ = Ep0State::STATUS;
  uint8_t ep_num = (is_in ? DWC_EP0_IN : DWC_EP0_OUT);
  auto& ep = endpoints_[ep_num];
  fbl::AutoLock al(&ep->lock);
  StartTransfer(&*ep, 0);

  if (is_in) {
    StartEp0();
  }
}

// Handles transfer complete events for endpoint zero
void Dwc2::HandleEp0TransferComplete(bool is_in) {
  switch (ep0_state_) {
    case Ep0State::IDLE: {
      StartEp0();
      break;
    }
    case Ep0State::DATA: {
      auto& ep = endpoints_[is_in ? DWC_EP0_IN : DWC_EP0_OUT];
      auto transfered = ReadTransfered(&*ep);
      ep->req_offset += transfered;

      if (is_in) {  // data direction is IN-type (to the host).
        if (ep->req_offset == ep->req_length) {
          HandleEp0Status(false);
        } else {
          auto length = ep->req_length - ep->req_offset;
          if (length > 64) {
            length = 64;
          }

          // It's possible the data to be transmitted never makes it to the host. For all but the
          // last packet's worth of data, the core handles retransmission internally. To prepare to
          // (potentially) retransmit data, the last transmission's size is recorded.
          last_transmission_len_ = length;

          fbl::AutoLock al(&ep->lock);
          StartTransfer(&*ep, length);
        }
      } else {  // data direction is OUT-type (from the host).
        if (ep->req_offset == ep->req_length) {
          if (dci_intf_) {
            size_t actual;
            dci_intf_->Control(&cur_setup_, (uint8_t*)ep0_buffer_.virt(), ep->req_length, nullptr,
                               0, &actual);
          }
          HandleEp0Status(true);
        } else {
          auto length = ep->req_length - ep->req_offset;
          // Strangely, the controller can transfer up to 127 bytes in a single transaction.
          // But if length is > 127, the transfer must be done in multiple chunks, and those
          // chunks must be 64 bytes long.
          if (length > 127) {
            length = 64;
          }
          fbl::AutoLock al(&ep->lock);
          StartTransfer(&*ep, length);
        }
      }
      break;
    }
    case Ep0State::STATUS:
      ep0_state_ = Ep0State::IDLE;
      if (!is_in) {
        StartEp0();
      }
      break;
    case Ep0State::TIMEOUT_RECOVERY: {
      if (is_in) {
        // Timeout was due to lost data.
        auto& ep = endpoints_[DWC_EP0_IN];
        ep->req_offset += ReadTransfered(&*ep);
        ZX_ASSERT(ep->req_offset == ep->req_length);
        HandleEp0Status(false);
      } else {
        // Timeout was due to lost ACK. Prepare the core to receive STATUS data.
        HandleEp0Status(false);
      }
      break;
    }
    case Ep0State::STALL:
    default:
      zxlogf(ERROR, "EP0 state is %d, should not get here", static_cast<int>(ep0_state_));
      break;
  }
}

// Executes a soft port disconnect and issues a core reset.
void Dwc2::SoftDisconnect() {
  auto* mmio = get_mmio();

  DCTL::Get().ReadFrom(mmio).set_sftdiscon(1).WriteTo(mmio);
  auto grstctl = GRSTCTL::Get();
  grstctl.ReadFrom(mmio).set_csftrst(1).WriteTo(mmio);
  while (grstctl.ReadFrom(mmio).csftrst()) {
    zx::nanosleep(zx::deadline_after(zx::msec(1)));
  }
  zx::nanosleep(zx::deadline_after(zx::msec(5)));
}

// Handles the case where the core experiences a timeout due to lost data or ACK. For the time
// being, the recovery logic involves a soft port disconnect and controller reset. This appears to
// the host as a unplug-replug event.
void Dwc2::HandleEp0TimeoutRecovery() {
  fbl::AutoLock _(&lock_);
  SetConnected(false);
  SoftDisconnect();
  ep0_state_ = Ep0State::DISCONNECTED;
  zx::nanosleep(zx::deadline_after(zx::msec(50)));
  InitController(); // Clears the GRSTCTRL.sftdiscon condition.
}

// Handles transfer complete events for endpoints other than endpoint zero
void Dwc2::HandleTransferComplete(uint8_t ep_num) {
  ZX_DEBUG_ASSERT(ep_num != DWC_EP0_IN && ep_num != DWC_EP0_OUT);
  auto& ep = endpoints_[ep_num];

  ep->lock.Acquire();

  ep->req_offset += ReadTransfered(&*ep);
  // Make a copy since this is used outside the critical section.
  auto actual = ep->req_offset;

  if (ep->current_req) {
    auto req = std::move(ep->current_req.value());
    ep->current_req.reset();
    // It is necessary to set current_req = nullptr in order to make this re-entrant safe and
    // thread-safe. When we call request.Complete the callee may immediately re-queue this request.
    // if it is already in current_req it could be completed twice (since QueueNextRequest
    // would attempt to re-queue it, or CancelAll could take the lock on a separate thread and
    // forcefully complete it after we've already completed it).
    ep->lock.Release();
    ep->RequestComplete(ZX_OK, actual, std::move(req));
    ep->lock.Acquire();

    QueueNextRequest(&*ep);
  }
  ep->lock.Release();
}

zx_status_t Dwc2::InitController() {
  auto* mmio = get_mmio();

  auto gsnpsid = GSNPSID::Get().ReadFrom(mmio).reg_value();
  if (gsnpsid != 0x4f54400a && gsnpsid != 0x4f54330a) {
    zxlogf(WARNING,
           "DWC2 driver has not been tested with IP version 0x%08x. "
           "The IP has quirks, so things may not work as expected\n",
           gsnpsid);
  }

  auto ghwcfg2 = GHWCFG2::Get().ReadFrom(mmio);
  if (!ghwcfg2.dynamic_fifo()) {
    zxlogf(ERROR, "DWC2 driver requires dynamic FIFO support");
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto ghwcfg4 = GHWCFG4::Get().ReadFrom(mmio);
  if (!ghwcfg4.ded_fifo_en()) {
    zxlogf(ERROR, "DWC2 driver requires dedicated FIFO support");
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto grstctl = GRSTCTL::Get();
  while (grstctl.ReadFrom(mmio).ahbidle() == 0) {
    zx::nanosleep(zx::deadline_after(zx::msec(1)));
  }

  // Reset the controller
  grstctl.FromValue(0).set_csftrst(1).WriteTo(mmio);

  // Wait for reset to complete
  bool done = false;
  for (int i = 0; i < 1000; i++) {
    if (grstctl.ReadFrom(mmio).csftrst() == 0) {
      zx::nanosleep(zx::deadline_after(zx::msec(10)));
      done = true;
      break;
    }
    zx::nanosleep(zx::deadline_after(zx::msec(1)));
  }
  if (!done) {
    return ZX_ERR_TIMED_OUT;
  }

  zx::nanosleep(zx::deadline_after(zx::msec(10)));

  // Enable DMA
  GAHBCFG::Get()
      .FromValue(0)
      .set_dmaenable(1)
      .set_hburstlen(metadata_.dma_burst_len)
      .set_nptxfemplvl_txfemplvl(1)
      .WriteTo(mmio);

  // Set turnaround time based on metadata
  GUSBCFG::Get().ReadFrom(mmio).set_usbtrdtim(metadata_.usb_turnaround_time).WriteTo(mmio);
  DCFG::Get()
      .ReadFrom(mmio)
      .set_devaddr(0)
      .set_epmscnt(2)
      .set_descdma(0)
      .set_devspd(0)
      .set_perfrint(DCFG::PERCENT_80)
      .WriteTo(mmio);

  DCTL::Get().ReadFrom(mmio).set_sftdiscon(1).WriteTo(mmio);
  DCTL::Get().ReadFrom(mmio).set_sftdiscon(0).WriteTo(mmio);

  // Reset phy clock
  PCGCCTL::Get().FromValue(0).WriteTo(mmio);

  // Set fifo sizes based on metadata.
  GRXFSIZ::Get().FromValue(0).set_size(metadata_.rx_fifo_size).WriteTo(mmio);
  GNPTXFSIZ::Get()
      .FromValue(0)
      .set_depth(metadata_.nptx_fifo_size)
      .set_startaddr(metadata_.rx_fifo_size)
      .WriteTo(mmio);

  auto fifo_base = metadata_.rx_fifo_size + metadata_.nptx_fifo_size;
  auto dfifo_end = GHWCFG3::Get().ReadFrom(mmio).dfifo_depth();

  for (uint32_t i = 0; i < std::size(metadata_.tx_fifo_sizes); i++) {
    auto fifo_size = metadata_.tx_fifo_sizes[i];

    DTXFSIZ::Get(i + 1).FromValue(0).set_startaddr(fifo_base).set_depth(fifo_size).WriteTo(mmio);
    fifo_base += fifo_size;
  }

  GDFIFOCFG::Get().FromValue(0).set_gdfifocfg(dfifo_end).set_epinfobase(fifo_base).WriteTo(mmio);

  // Flush all FIFOs
  FlushTxFifo(0x10);
  FlushRxFifo();

  GRSTCTL::Get().FromValue(0).set_intknqflsh(1).WriteTo(mmio);

  // Clear all pending device interrupts
  DIEPMSK::Get().FromValue(0).WriteTo(mmio);
  DOEPMSK::Get().FromValue(0).WriteTo(mmio);
  DAINT::Get().FromValue(0xFFFFFFFF).WriteTo(mmio);
  DAINTMSK::Get().FromValue(0).WriteTo(mmio);

  for (uint32_t i = 0; i < DWC_MAX_EPS; i++) {
    DEPCTL::Get(i).FromValue(0).WriteTo(mmio);
    DEPTSIZ::Get(i).FromValue(0).WriteTo(mmio);
  }

  // Clear all pending OTG and global interrupts
  GOTGINT::Get().FromValue(0xFFFFFFFF).WriteTo(mmio);
  GINTSTS::Get().FromValue(0xFFFFFFFF).WriteTo(mmio);

  // Enable selected global interrupts
  GINTMSK::Get()
      .FromValue(0)
      .set_usbreset(1)
      .set_enumdone(1)
      .set_inepintr(1)
      .set_outepintr(1)
      .set_usbsuspend(1)
      .set_erlysuspend(1)
      .WriteTo(mmio);

  // Enable global interrupts
  GAHBCFG::Get().ReadFrom(mmio).set_glblintrmsk(1).WriteTo(mmio);

  return ZX_OK;
}

void Dwc2::SetConnected(bool connected) {
  if (connected == connected_) {
    return;
  }

  if (dci_intf_) {
    dci_intf_->SetConnected(connected);
  }
  if (usb_phy_) {
    usb_phy_->ConnectStatusChanged(connected);
  }

  if (!connected) {
    // Complete any pending requests
    for (size_t i = 0; i < std::size(endpoints_); i++) {
      auto& ep = endpoints_[i];

      std::queue<usb_endpoint::RequestVariant> complete_reqs;
      {
        fbl::AutoLock lock(&ep->lock);
        complete_reqs.swap(ep->queued_reqs);

        if (ep->current_req) {
          complete_reqs.emplace(std::move(*ep->current_req));
          ep->current_req.reset();
        }

        ep->enabled = false;
      }

      // Requests must be completed outside of the lock.
      while (true) {
        if (complete_reqs.empty()) {
          break;
        }

        ep->RequestComplete(ZX_ERR_IO_NOT_PRESENT, 0, std::move(complete_reqs.front()));
        complete_reqs.pop();
      }
    }
  }

  connected_ = connected;
}

zx_status_t Dwc2::Create(void* ctx, zx_device_t* parent) {
  auto dev = std::make_unique<Dwc2>(parent, fdf::Dispatcher::GetCurrent()->async_dispatcher());
  auto status = dev->Init();
  if (status != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* _ = dev.release();
  return ZX_OK;
}

zx_status_t Dwc2::Init() {
  pdev_ = ddk::PDevFidl::FromFragment(parent());
  if (!pdev_.is_valid()) {
    zxlogf(ERROR, "Dwc2::Create: could not get platform device protocol");
    return ZX_ERR_NOT_SUPPORTED;
  }

  // USB PHY protocol is optional.
  usb_phy_ = ddk::UsbPhyProtocolClient(parent(), "dwc2-phy");
  if (!usb_phy_->is_valid()) {
    usb_phy_.reset();
  }

  for (uint8_t i = 0; i < std::size(endpoints_); i++) {
    endpoints_[i].emplace(i, this);
  }

  size_t actual = 0;
  auto status = DdkGetFragmentMetadata("pdev", DEVICE_METADATA_PRIVATE, &metadata_,
                                       sizeof(metadata_), &actual);
  if (status != ZX_OK || actual != sizeof(metadata_)) {
    zxlogf(ERROR, "Dwc2::Init can't get driver metadata: %s, actual size: %ld expected size: %ld",
           zx_status_get_string(status), actual, sizeof(metadata_));
    return ZX_ERR_INTERNAL;
  }

  status = pdev_.MapMmio(0, &mmio_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Dwc2::Init MapMmio failed: %d", status);
    return status;
  }

  status = pdev_.GetInterrupt(0, &irq_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Dwc2::Init GetInterrupt failed: %d", status);
    return status;
  }

  status = pdev_.GetBti(0, &bti_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Dwc2::Init GetBti failed: %d", status);
    return status;
  }

  status = ep0_buffer_.Init(bti_.get(), UINT16_MAX, IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Dwc2::Init ep0_buffer_.Init failed: %d", status);
    return status;
  }

  status = ep0_buffer_.PhysMap();
  if (status != ZX_OK) {
    zxlogf(ERROR, "Dwc2::Init ep0_buffer_.PhysMap failed: %d", status);
    return status;
  }

  if ((status = InitController()) != ZX_OK) {
    zxlogf(ERROR, "Dwc2::Init InitController failed: %d", status);
    return status;
  }

  zx::result result = outgoing_.AddService<fuchsia_hardware_usb_dci::UsbDciService>(
      fuchsia_hardware_usb_dci::UsbDciService::InstanceHandler({
          .device = bindings_.CreateHandler(this, dispatcher_, fidl::kIgnoreBindingClosure),
      }));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to add service %s", result.status_string());
    return result.status_value();
  }
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.status_value();
  }
  result = outgoing_.Serve(std::move(endpoints->server));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to service the outgoing directory");
    return result.status_value();
  }

  std::array offers = {
      fuchsia_hardware_usb_dci::UsbDciService::Name,
  };
  status = DdkAdd(ddk::DeviceAddArgs("dwc2")
                      .forward_metadata(parent(), DEVICE_METADATA_USB_CONFIG)
                      .forward_metadata(parent(), DEVICE_METADATA_MAC_ADDRESS)
                      .forward_metadata(parent(), DEVICE_METADATA_SERIAL_NUMBER)
                      .set_fidl_service_offers(offers)
                      .set_outgoing_dir(endpoints->client.TakeChannel()));
  if (status != ZX_OK) {
    zxlogf(ERROR, "Dwc2::Init DdkAdd failed: %d", status);
    return status;
  }

  return ZX_OK;
}

void Dwc2::DdkInit(ddk::InitTxn txn) {
  int rc = thrd_create_with_name(
      &irq_thread_, [](void* arg) -> int { return reinterpret_cast<Dwc2*>(arg)->IrqThread(); },
      reinterpret_cast<void*>(this), "dwc2-interrupt-thread");
  if (rc == thrd_success) {
    irq_thread_started_ = true;
    txn.Reply(ZX_OK);
  } else {
    txn.Reply(ZX_ERR_INTERNAL);
  }
}

int Dwc2::IrqThread() {
  auto* mmio = get_mmio();
  const char* role_name = "fuchsia.devices.usb.drivers.dwc2.interrupt";
  const zx_status_t status = device_set_profile_by_role(parent_, thrd_get_zx_handle(thrd_current()),
                                                        role_name, strlen(role_name));
  if (status != ZX_OK) {
    // This should be an error since we won't be able to guarantee we can meet deadlines.
    // Failure to meet deadlines can result in undefined behavior on the bus.
    zxlogf(ERROR, "%s: Failed to apply role to IRQ thread: %s", __FUNCTION__,
           zx_status_get_string(status));
  }
  while (1) {
    wait_start_time_ = zx::clock::get_monotonic();
    auto wait_res = irq_.wait(&irq_timestamp_);
    irq_dispatch_timestamp_ = zx::clock::get_monotonic();
    if (wait_res == ZX_ERR_CANCELED) {
      break;
    } else if (wait_res != ZX_OK) {
      zxlogf(ERROR, "dwc_usb: irq wait failed, retcode = %d", wait_res);
    }

    if (timeout_recovering_) {
      // TODO(105382) remove logging once timeout recovery has stabilized.
      zxlogf(ERROR, "(diepint.timeout) Got interrupt with timeout_recovering_ = true");
    }

    // It doesn't seem that this inner loop should be necessary,
    // but without it we miss interrupts on some versions of the IP.
    while (1) {
      auto gintsts = GINTSTS::Get().ReadFrom(mmio);

      // After experiencing a diepint.timeout interrupt, this (inner) loop whips back seemingly one
      // more time. To determine why (and figure out why the timeout isn't being handled correctly),
      // we need to know what pending interrupt(s) is/are remaining after servicing the interrupt
      // handlers below.
      if (timeout_recovering_) {
        // TODO(105382) remove logging once timeout recovery has stabilized.
        zxlogf(ERROR, "(diepint.timeout) interrupt with timeout_recovering_ = true in loop");
      }

      auto gintmsk = GINTMSK::Get().ReadFrom(mmio);
      gintsts.WriteTo(mmio);

      if (timeout_recovering_) {
        // TODO(105382) remove logging once timeout recovery has stabilized.
        zxlogf(ERROR, "(diepint.timeout) GINTSTS=0x%08x (pre-mask)", gintsts.reg_value());
      }

      gintsts.set_reg_value(gintsts.reg_value() & gintmsk.reg_value());

      if (timeout_recovering_) {
        // TODO(105382) remove logging once timeout recovery has stabilized.
        zxlogf(ERROR, "(diepint.timeout) GINTSTS=0x%08x (post-mask)", gintsts.reg_value());
      }

      if (gintsts.reg_value() == 0) {
        break;
      }

      if (gintsts.usbreset()) {
        HandleReset();
      }
      if (gintsts.usbsuspend()) {
        HandleSuspend();
      }
      if (gintsts.enumdone()) {
        HandleEnumDone();
      }
      if (gintsts.inepintr()) {
        HandleInEpInterrupt();
      }
      if (gintsts.outepintr()) {
        HandleOutEpInterrupt();
      }
    }
  }

  zxlogf(INFO, "dwc_usb: irq thread finished");
  return 0;
}

void Dwc2::DdkUnbind(ddk::UnbindTxn txn) {
  irq_.destroy();
  if (irq_thread_started_) {
    irq_thread_started_ = false;
    thrd_join(irq_thread_, nullptr);
  }
  txn.Reply();
}

void Dwc2::DdkRelease() { delete this; }

void Dwc2::DdkSuspend(ddk::SuspendTxn txn) {
  fbl::AutoLock lock(&lock_);
  irq_.destroy();
  shutting_down_ = true;
  // Disconnect from host to prevent DMA from being started
  DCTL::Get().ReadFrom(&mmio_.value()).set_sftdiscon(1).WriteTo(&mmio_.value());
  auto grstctl = GRSTCTL::Get();
  auto mmio = &mmio_.value();
  // Start soft reset sequence -- I think this should clear the DMA FIFOs
  grstctl.FromValue(0).set_csftrst(1).WriteTo(mmio);

  // Wait for reset to complete
  while (grstctl.ReadFrom(mmio).csftrst()) {
    // Arbitrary sleep to yield our timeslice while we wait for
    // hardware to complete its reset.
    zx::nanosleep(zx::deadline_after(zx::msec(1)));
  }
  lock.release();

  if (irq_thread_started_) {
    irq_thread_started_ = false;
    thrd_join(irq_thread_, nullptr);
  }
  ep0_buffer_.release();
  txn.Reply(ZX_OK, 0);
}

void Dwc2::UsbDciRequestQueue(usb_request_t* req, const usb_request_complete_callback_t* cb) {
  uint8_t ep_num = DWC_ADDR_TO_INDEX(req->header.ep_address);
  if (ep_num == DWC_EP0_IN || ep_num == DWC_EP0_OUT || ep_num >= std::size(endpoints_)) {
    zxlogf(ERROR, "Dwc2::UsbDciRequestQueue: bad ep address 0x%02X", req->header.ep_address);
    usb_request_complete(req, ZX_ERR_INVALID_ARGS, 0, cb);
    return;
  }
  zxlogf(SERIAL, "UsbDciRequestQueue ep %u length %zu", ep_num, req->header.length);

  endpoints_[ep_num]->QueueRequest(Request(req, *cb, sizeof(usb_request_t)));
}

zx_status_t Dwc2::UsbDciSetInterface(const usb_dci_interface_protocol_t* interface) {
  if (dci_intf_) {
    zxlogf(ERROR, "%s: dci_intf_ already set", __func__);
    return ZX_ERR_BAD_STATE;
  }

  dci_intf_ = ddk::UsbDciInterfaceProtocolClient(interface);

  return ZX_OK;
}

zx_status_t Dwc2::UsbDciConfigEp(const usb_endpoint_descriptor_t* ep_desc,
                                 const usb_ss_ep_comp_descriptor_t* ss_comp_desc) {
  auto* mmio = get_mmio();

  uint8_t ep_num = DWC_ADDR_TO_INDEX(ep_desc->b_endpoint_address);
  if (ep_num == DWC_EP0_IN || ep_num == DWC_EP0_OUT || ep_num >= std::size(endpoints_)) {
    zxlogf(ERROR, "Dwc2::UsbDciConfigEp: bad ep address 0x%02X", ep_desc->b_endpoint_address);
    return ZX_ERR_INVALID_ARGS;
  }

  bool is_in = (ep_desc->b_endpoint_address & USB_DIR_MASK) == USB_DIR_IN;
  uint8_t ep_type = usb_ep_type(ep_desc);
  uint16_t max_packet_size = usb_ep_max_packet(ep_desc);

  if (ep_type == USB_ENDPOINT_ISOCHRONOUS) {
    zxlogf(ERROR, "Dwc2::UsbDciConfigEp: isochronous endpoints are not supported");
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto& ep = endpoints_[ep_num];
  fbl::AutoLock lock(&ep->lock);

  ep->max_packet_size = max_packet_size;
  ep->enabled = true;

  DEPCTL::Get(ep_num)
      .FromValue(0)
      .set_mps(ep->max_packet_size)
      .set_eptype(ep_type)
      .set_setd0pid(1)
      .set_txfnum(is_in ? ep_num : 0)
      .set_usbactep(1)
      .WriteTo(mmio);

  EnableEp(ep_num, true);

  if (configured_) {
    QueueNextRequest(&*ep);
  }

  return ZX_OK;
}

zx_status_t Dwc2::UsbDciDisableEp(uint8_t ep_address) {
  auto* mmio = get_mmio();

  unsigned ep_num = DWC_ADDR_TO_INDEX(ep_address);
  if (ep_num == DWC_EP0_IN || ep_num == DWC_EP0_OUT || ep_num >= std::size(endpoints_)) {
    zxlogf(ERROR, "Dwc2::UsbDciConfigEp: bad ep address 0x%02X", ep_address);
    return ZX_ERR_INVALID_ARGS;
  }

  auto& ep = endpoints_[ep_num];

  fbl::AutoLock lock(&ep->lock);

  DEPCTL::Get(ep_num).ReadFrom(mmio).set_usbactep(0).WriteTo(mmio);
  ep->enabled = false;

  return ZX_OK;
}

zx_status_t Dwc2::UsbDciEpSetStall(uint8_t ep_address) {
  // TODO(voydanoff) implement this
  return ZX_OK;
}

zx_status_t Dwc2::UsbDciEpClearStall(uint8_t ep_address) {
  // TODO(voydanoff) implement this
  return ZX_OK;
}

size_t Dwc2::UsbDciGetRequestSize() { return Request::RequestSize(sizeof(usb_request_t)); }

zx_status_t Dwc2::UsbDciCancelAll(uint8_t epid) {
  uint8_t ep_num = DWC_ADDR_TO_INDEX(epid);
  endpoints_[ep_num]->CancelAll();
  return ZX_OK;
}

void Dwc2::ConnectToEndpoint(ConnectToEndpointRequest& request,
                             ConnectToEndpointCompleter::Sync& completer) {
  uint8_t ep_num = DWC_ADDR_TO_INDEX(request.ep_addr());
  if (ep_num == DWC_EP0_IN || ep_num == DWC_EP0_OUT || ep_num >= std::size(endpoints_)) {
    zxlogf(ERROR, "Dwc2::UsbDciRequestQueue: bad ep address 0x%02X", request.ep_addr());
    completer.Reply(fit::as_error(ZX_ERR_IO_NOT_PRESENT));
    return;
  }

  endpoints_[ep_num]->Connect(endpoints_[ep_num]->dispatcher(), std::move(request.ep()));
  completer.Reply(fit::ok());
}

void Dwc2::Endpoint::QueueRequests(QueueRequestsRequest& request,
                                   QueueRequestsCompleter::Sync& completer) {
  for (auto& req : request.req()) {
    QueueRequest(usb::FidlRequest{std::move(req)});
  }
}

void Dwc2::Endpoint::QueueRequest(usb_endpoint::RequestVariant request) {
  {
    fbl::AutoLock l(&dwc2_->lock_);
    if (dwc2_->shutting_down_) {
      l.release();
      RequestComplete(ZX_ERR_IO_NOT_PRESENT, 0, std::move(request));
      return;
    }
  }

  // OUT transactions must have length > 0 and multiple of max packet size
  if (DWC_EP_IS_OUT(ep_addr())) {
    auto length = std::holds_alternative<Request>(request)
                      ? std::get<Request>(request).request()->header.length
                      : std::get<usb::FidlRequest>(request).length();
    if (length == 0 || length % max_packet_size != 0) {
      zxlogf(ERROR, "dwc_ep_queue: OUT transfers must be multiple of max packet size");
      RequestComplete(ZX_ERR_INVALID_ARGS, 0, std::move(request));
      return;
    }
  }

  fbl::AutoLock _(&lock);

  if (!enabled) {
    zxlogf(ERROR, "dwc_ep_queue ep not enabled!");
    RequestComplete(ZX_ERR_BAD_STATE, 0, std::move(request));
    return;
  }

  if (!dwc2_->configured_) {
    zxlogf(ERROR, "dwc_ep_queue not configured!");
    RequestComplete(ZX_ERR_BAD_STATE, 0, std::move(request));
    return;
  }

  queued_reqs.push(std::move(request));
  dwc2_->QueueNextRequest(this);
}

void Dwc2::Endpoint::CancelAll() {
  std::queue<usb_endpoint::RequestVariant> queue;
  {
    fbl::AutoLock _(&lock);
    if (DWC_EP_IS_OUT(ep_addr())) {
      dwc2_->FlushRxFifoRetryIndefinite();
    } else {
      dwc2_->FlushTxFifoRetryIndefinite(ep_addr());
    }
    queue = std::move(queued_reqs);
    if (current_req) {
      queue.push(std::move(current_req.value()));
    }
  }

  while (!queue.empty()) {
    auto req = std::move(queue.front());
    queue.pop();
    RequestComplete(ZX_ERR_IO_NOT_PRESENT, 0, std::move(req));
  }
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = Dwc2::Create;
  return ops;
}();

}  // namespace dwc2

ZIRCON_DRIVER(dwc2, dwc2::driver_ops, "zircon", "0.1");
