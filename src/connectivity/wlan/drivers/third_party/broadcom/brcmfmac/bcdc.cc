/*
 * Copyright (c) 2010 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*******************************************************************************
 * Communicates with the dongle by using dcmd codes.
 * For certain dcmd codes, the dongle interprets string data from the host.
 ******************************************************************************/

#include "bcdc.h"

#include <zircon/errors.h>
#include <zircon/status.h>

#include <algorithm>
#include <limits>

#include "brcmu_utils.h"
#include "brcmu_wifi.h"
#include "bus.h"
#include "core.h"
#include "debug.h"
#include "fwil.h"
#include "proto.h"

/*
 * maximum length of firmware signal data between
 * the BCDC header and packet data in the tx path.
 */
#define BRCMF_PROT_FW_SIGNAL_MAX_TXBYTES 12

#define RETRIES 2 /* # of retries to retrieve matching dcmd response */

static zx_status_t brcmf_proto_bcdc_msg(struct brcmf_pub* drvr, int ifidx, uint cmd, void* buf,
                                        uint buflen, bool set) {
  struct brcmf_bcdc* bcdc = static_cast<struct brcmf_bcdc*>(drvr->proto->pd);
  uint cmdlen = buflen;
  uint32_t flags;

  if (cmd == BRCMF_C_GET_VAR) {
    // buf starts with a NULL-terminated string
    BRCMF_DBG(BCDC, "Getting iovar '%.*s'", buflen, static_cast<char*>(buf));
  } else if (cmd == BRCMF_C_SET_VAR) {
    // buf starts with a NULL-terminated string
    BRCMF_DBG(BCDC, "Setting iovar '%.*s'", buflen, static_cast<char*>(buf));
  } else {
    BRCMF_DBG(BCDC, "Enter");
  }

  if (cmdlen > BCDC_TX_IOCTL_MAX_MSG_SIZE) {
    BRCMF_DBG(BCDC, "Only the first %lu bytes of the buffer (%u bytes) will be transmitted.",
              BCDC_TX_IOCTL_MAX_MSG_SIZE, buflen);
    cmdlen = BCDC_TX_IOCTL_MAX_MSG_SIZE;
  }

  /* Initialize bcdc->msg */
  memset(&bcdc->msg, 0, sizeof(bcdc->msg));
  bcdc->msg.cmd = cmd;
  // Even though we only transmit at most BCDC_TX_IOCTL_MAX_MSG_SIZE bytes, the bcdc->msg.len field
  // should still contain the actual length of the buffer so the firmware knows the upper bound of
  // bytes it can respond with.
  bcdc->msg.len = buflen;

  flags = (++bcdc->reqid << BCDC_DCMD_ID_SHIFT);
  if (set) {
    flags |= BCDC_DCMD_SET;
  }
  flags = (flags & ~BCDC_DCMD_IF_MASK) | (ifidx << BCDC_DCMD_IF_SHIFT);
  bcdc->msg.flags = flags;

  if (buf) {
    // Copy the entire buffer into bcdc->buf even though only the first BCDC_TX_IOCTL_MAX_MSG_SIZE
    // bytes may be transmitted. This is to ensure the received data will be correct on the
    // other end in case the entire buffer is not overwritten by the response.
    memcpy(bcdc->buf, buf, buflen);
  }

  /* Send request */
  BRCMF_DBG_HEX_DUMP(BRCMF_IS_ON(BCDC) && BRCMF_IS_ON(BYTES), &bcdc->msg, bcdc->msg.len,
                     "Sending BCDC Message (%u bytes)", bcdc->msg.len);
  if (cmdlen < bcdc->msg.len) {
    BRCMF_DBG_HEX_DUMP(BRCMF_IS_ON(BCDC) && BRCMF_IS_ON(BYTES), (unsigned char*)buf + cmdlen,
                       bcdc->msg.len - cmdlen,
                       "IOCTL from host to device is limited to %lu bytes. The following bytes "
                       "at the end of the BCDC message were not transmitted.",
                       BCDC_TX_IOCTL_MAX_MSG_SIZE);
  }
  return brcmf_bus_txctl(drvr->bus_if, reinterpret_cast<unsigned char*>(&bcdc->msg),
                         sizeof(struct brcmf_proto_bcdc_dcmd) + cmdlen);
}

static zx_status_t brcmf_proto_bcdc_cmplt(struct brcmf_pub* drvr, uint32_t id, uint32_t len,
                                          int* rxbuflen) {
  zx_status_t ret;
  struct brcmf_bcdc* bcdc = static_cast<struct brcmf_bcdc*>(drvr->proto->pd);
  int rxlen_out = 0;

  BRCMF_DBG(BCDC, "Enter");
  do {
    ret = brcmf_bus_rxctl(drvr->bus_if, reinterpret_cast<unsigned char*>(&bcdc->msg),
                          sizeof(struct brcmf_proto_bcdc_dcmd) + len, &rxlen_out);
    if (ret != ZX_OK) {
      break;
    }
    // bcdc->msg.flags is written to by the brcmf_bus_rxctl call as long as msglen is
    // at least sizeof(struct brcmf_proto_bcdc_dcmd) bytes
  } while (BCDC_DCMD_ID(bcdc->msg.flags) != id);

  if (rxbuflen) {
    *rxbuflen = std::max(0, rxlen_out - static_cast<int>(sizeof(struct brcmf_proto_bcdc_dcmd)));

    if (*rxbuflen < static_cast<int>(len)) {
      BRCMF_DBG(BCDC, "brcmf_bus_rxctl only overwrote the first %d bytes of bcdc->buf (%u bytes)",
                *rxbuflen, len);
    }
  }

  BRCMF_DBG_HEX_DUMP(BRCMF_IS_ON(BCDC) && BRCMF_IS_ON(BYTES), &bcdc->msg, bcdc->msg.len,
                     "Received BCDC Message (%d bytes)", bcdc->msg.len);

  return ret;
}

static zx_status_t brcmf_proto_bcdc_query_dcmd(struct brcmf_pub* drvr, int ifidx, uint cmd,
                                               void* buf, uint len, bcme_status_t* fwerr) {
  struct brcmf_bcdc* bcdc = static_cast<struct brcmf_bcdc*>(drvr->proto->pd);
  struct brcmf_proto_bcdc_dcmd* msg = &bcdc->msg;
  void* info;
  zx_status_t ret = ZX_OK;
  int retries = 0;
  int rxbuflen;
  uint32_t id, flags;

  BRCMF_DBG(BCDC, "Enter, cmd %d len %d", cmd, len);

  *fwerr = BCME_OK;
  ret = brcmf_proto_bcdc_msg(drvr, ifidx, cmd, buf, len, false);
  if (ret != ZX_OK) {
    BRCMF_ERR("brcmf_proto_bcdc_msg failed w/status %s", zx_status_get_string(ret));
    goto done;
  }

retry:
  /* wait for interrupt and get first fragment */
  ret = brcmf_proto_bcdc_cmplt(drvr, bcdc->reqid, len, &rxbuflen);
  if (ret != ZX_OK) {
    goto done;
  }

  flags = msg->flags;
  id = (flags & BCDC_DCMD_ID_MASK) >> BCDC_DCMD_ID_SHIFT;

  if ((id < bcdc->reqid) && (++retries < RETRIES)) {
    goto retry;
  }
  if (id != bcdc->reqid) {
    BRCMF_ERR("%s: unexpected request id %d (expected %d)",
              brcmf_ifname(brcmf_get_ifp(drvr, ifidx)), id, bcdc->reqid);
    ret = ZX_ERR_BAD_STATE;
    goto done;
  }

  /* Check info buffer */
  info = static_cast<void*>(&bcdc->buf[0]);

  /* Copy info buffer */
  if (buf) {
    if (rxbuflen < static_cast<int>(len)) {
      len = rxbuflen;
    }
    memcpy(buf, info, len);
  }

  ret = ZX_OK;

  /* Check the ERROR flag */
  if (flags & BCDC_DCMD_ERROR) {
    BRCMF_DBG(BCDC, "fwerr %s", brcmf_fil_get_errstr(msg->status));
    *fwerr = msg->status;
  }
done:
  return ret;
}

static zx_status_t brcmf_proto_bcdc_set_dcmd(struct brcmf_pub* drvr, int ifidx, uint cmd, void* buf,
                                             uint len, bcme_status_t* fwerr) {
  struct brcmf_bcdc* bcdc = static_cast<struct brcmf_bcdc*>(drvr->proto->pd);
  struct brcmf_proto_bcdc_dcmd* msg = &bcdc->msg;
  zx_status_t ret;
  uint32_t flags, id;

  BRCMF_DBG(BCDC, "Enter, cmd %d len %d", cmd, len);

  *fwerr = BCME_OK;
  ret = brcmf_proto_bcdc_msg(drvr, ifidx, cmd, buf, len, true);
  if (ret != ZX_OK) {
    goto done;
  }

  ret = brcmf_proto_bcdc_cmplt(drvr, bcdc->reqid, len, nullptr);
  if (ret != ZX_OK) {
    BRCMF_DBG(TEMP, "Just got back from message cmplt, result %d", ret);
    goto done;
  }

  flags = msg->flags;
  id = (flags & BCDC_DCMD_ID_MASK) >> BCDC_DCMD_ID_SHIFT;

  if (id != bcdc->reqid) {
    BRCMF_ERR("%s: unexpected request id %d (expected %d)",
              brcmf_ifname(brcmf_get_ifp(drvr, ifidx)), id, bcdc->reqid);
    ret = ZX_ERR_BAD_STATE;
    goto done;
  }

  BRCMF_DBG(BCDC, "Set DCMD message received.");
  ret = ZX_OK;

  /* Check the ERROR flag */
  if (flags & BCDC_DCMD_ERROR) {
    *fwerr = msg->status;
  }

done:
  return ret;
}

static void brcmf_proto_bcdc_hdrpopulate(struct brcmf_pub* drvr, int ifidx, uint8_t offset,
                                         int priority, brcmf_proto_bcdc_header* hdr) {
  hdr->flags = (BCDC_PROTO_VER << BCDC_FLAG_VER_SHIFT);

  hdr->priority = (priority & BCDC_PRIORITY_MASK);
  hdr->flags2 = 0;
  hdr->data_offset = offset;
  ZX_DEBUG_ASSERT(0 <= ifidx && ifidx <= std::numeric_limits<uint8_t>::max());
  BCDC_SET_IF_IDX(hdr, static_cast<uint8_t>(ifidx));
}

static zx_status_t brcmf_proto_bcdc_hdrpull(struct brcmf_pub* drvr, brcmf_proto_bcdc_header* hdr,
                                            uint32_t size, struct brcmf_if** ifp,
                                            uint32_t* shrinkage, int* priority) {
  if (size <= BCDC_HEADER_LEN) {
    BRCMF_DBG(BCDC, "rx data too short (%u <= %d)", size, BCDC_HEADER_LEN);
    return ZX_ERR_IO_DATA_INTEGRITY;
  }

  struct brcmf_if* tmp_if = brcmf_get_ifp(drvr, BCDC_GET_IF_IDX(hdr));
  if (!tmp_if) {
    BRCMF_ERR("no matching ifp found");
    return ZX_ERR_NOT_FOUND;
  }
  if (((hdr->flags & BCDC_FLAG_VER_MASK) >> BCDC_FLAG_VER_SHIFT) != BCDC_PROTO_VER) {
    BRCMF_ERR("%s: non-BCDC packet received, flags 0x%x", brcmf_ifname(tmp_if), hdr->flags);
    return ZX_ERR_IO_DATA_INTEGRITY;
  }

  if (hdr->flags & BCDC_FLAG_SUM_GOOD) {
    BRCMF_DBG(BCDC, "%s: BDC rcv, good checksum, flags 0x%x", brcmf_ifname(tmp_if), hdr->flags);
  }

  *priority = hdr->priority & BCDC_PRIORITY_MASK;

  *shrinkage = BCDC_HEADER_LEN + (hdr->data_offset << 2);

  if (*shrinkage >= size) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  }

  if (ifp) {
    *ifp = tmp_if;
  }

  return ZX_OK;
}

static zx_status_t brcmf_proto_bcdc_hdrpull(struct brcmf_pub* drvr,
                                            wlan::drivers::components::Frame& frame,
                                            struct brcmf_if** ifp) {
  BRCMF_DBG(BCDC, "Enter");

  auto hdr = reinterpret_cast<struct brcmf_proto_bcdc_header*>(frame.Data());

  uint32_t shrinkage = 0;
  int priority = 0;
  zx_status_t err = brcmf_proto_bcdc_hdrpull(drvr, hdr, frame.Size(), ifp, &shrinkage, &priority);

  if (err == ZX_OK) {
    frame.SetPriority(priority);
    frame.ShrinkHead(shrinkage);
  }

  return err;
}

static zx_status_t brcmf_proto_bcdc_tx_queue_frames(
    struct brcmf_pub* drvr, cpp20::span<wlan::drivers::components::Frame> frames) {
  for (auto& frame : frames) {
    frame.GrowHead(BCDC_HEADER_LEN);
    auto h = reinterpret_cast<struct brcmf_proto_bcdc_header*>(frame.Data());
    brcmf_proto_bcdc_hdrpopulate(drvr, frame.PortId(), 0, frame.Priority(), h);
  }
  return brcmf_bus_tx_frames(drvr->bus_if, frames);
}

void brcmf_proto_bcdc_txcomplete(brcmf_pub* drvr,
                                 cpp20::span<wlan::drivers::components::Frame> frames,
                                 zx_status_t result) {
  if (frames.empty()) {
    return;
  }

  brcmf_tx_complete(drvr, std::move(frames), result);
}

static void brcmf_proto_bcdc_add_iface(struct brcmf_pub* drvr, int ifidx) {}

static void brcmf_proto_bcdc_del_iface(struct brcmf_pub* drvr, int ifidx) {}

static void brcmf_proto_bcdc_reset_iface(struct brcmf_pub* drvr, int ifidx) {}

static void brcmf_proto_bcdc_configure_addr_mode(struct brcmf_pub* drvr, int ifidx,
                                                 enum proto_addr_mode addr_mode) {}

static void brcmf_proto_bcdc_delete_peer(struct brcmf_pub* drvr, int ifidx,
                                         uint8_t peer[ETH_ALEN]) {}

static void brcmf_proto_bcdc_add_tdls_peer(struct brcmf_pub* drvr, int ifidx,
                                           uint8_t peer[ETH_ALEN]) {}

zx_status_t brcmf_proto_bcdc_attach(struct brcmf_pub* drvr) {
  struct brcmf_proto* proto = nullptr;
  struct brcmf_bcdc* bcdc = nullptr;

  proto = static_cast<decltype(proto)>(calloc(1, sizeof(*proto)));
  if (!proto) {
    goto fail;
  }

  bcdc = static_cast<decltype(bcdc)>(calloc(1, sizeof(*bcdc)));
  if (!bcdc) {
    goto fail;
  }

  /* ensure that the msg buf directly follows the cdc msg struct */
  if (reinterpret_cast<size_t>(&bcdc->msg + 1) != reinterpret_cast<size_t>(bcdc->buf)) {
    BRCMF_ERR("struct brcmf_proto_bcdc is not correctly defined");
    goto fail;
  }

  proto->add_iface = brcmf_proto_bcdc_add_iface;
  proto->del_iface = brcmf_proto_bcdc_del_iface;
  proto->reset_iface = brcmf_proto_bcdc_reset_iface;
  proto->configure_addr_mode = brcmf_proto_bcdc_configure_addr_mode;
  proto->query_dcmd = brcmf_proto_bcdc_query_dcmd;
  proto->set_dcmd = brcmf_proto_bcdc_set_dcmd;
  proto->reset = brcmf_proto_bcdc_reset;
  proto->tx_queue_frames = brcmf_proto_bcdc_tx_queue_frames;
  proto->pd = bcdc;

  proto->hdrpull_frame = brcmf_proto_bcdc_hdrpull;
  proto->delete_peer = brcmf_proto_bcdc_delete_peer;
  proto->add_tdls_peer = brcmf_proto_bcdc_add_tdls_peer;

  drvr->proto = proto;
  drvr->hdrlen += BCDC_HEADER_LEN + BRCMF_PROT_FW_SIGNAL_MAX_TXBYTES;
  drvr->bus_if->maxctl = BRCMF_DCMD_MAXLEN + sizeof(struct brcmf_proto_bcdc_dcmd);
  return ZX_OK;

fail:
  free(bcdc);
  free(proto);
  return ZX_ERR_NO_MEMORY;
}

void brcmf_proto_bcdc_detach(struct brcmf_pub* drvr) {
  if (drvr->proto == nullptr) {
    return;
  }

  struct brcmf_bcdc* bcdc = static_cast<decltype(bcdc)>(drvr->proto->pd);

  drvr->proto->pd = nullptr;
  free(bcdc);
  free(drvr->proto);
  drvr->proto = nullptr;
}

zx_status_t brcmf_proto_bcdc_reset(struct brcmf_pub* drvr) {
  if (drvr->proto == nullptr) {
    BRCMF_ERR("No protocol for driver now.");
    return ZX_ERR_BAD_STATE;
  }

  struct brcmf_bcdc* bcdc = static_cast<decltype(bcdc)>(drvr->proto->pd);

  memset(bcdc->buf, 0, BRCMF_DCMD_MAXLEN);
  return ZX_OK;
}
