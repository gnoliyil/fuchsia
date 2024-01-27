/*
 * Copyright (c) 2013 Broadcom Corporation
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
#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_PROTO_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_PROTO_H_

#include <lib/stdcompat/span.h>

#include <memory>

#include <wlan/drivers/components/frame.h>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/core.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil.h"

enum proto_addr_mode { ADDR_INDIRECT = 0, ADDR_DIRECT };

struct brcmf_proto {
  void (*add_iface)(struct brcmf_pub* drvr, int ifidx);
  void (*del_iface)(struct brcmf_pub* drvr, int ifidx);
  void (*reset_iface)(struct brcmf_pub* drvr, int ifidx);
  void (*configure_addr_mode)(struct brcmf_pub* drvr, int ifidx, enum proto_addr_mode addr_mode);
  zx_status_t (*query_dcmd)(struct brcmf_pub* drvr, int ifidx, uint cmd, void* buf, uint len,
                            bcme_status_t* fwerr);
  zx_status_t (*set_dcmd)(struct brcmf_pub* drvr, int ifidx, uint cmd, void* buf, uint len,
                          bcme_status_t* fwerr);
  zx_status_t (*reset)(struct brcmf_pub* drvr);
  zx_status_t (*tx_queue_frames)(struct brcmf_pub* drvr,
                                 cpp20::span<wlan::drivers::components::Frame> frames);
  void* pd;

  // Deprecated entry points.
  zx_status_t (*hdrpull_frame)(struct brcmf_pub* drvr, wlan::drivers::components::Frame& frame,
                               struct brcmf_if** ifp);

  // Unimplemented entry points.
  void (*delete_peer)(struct brcmf_pub* drvr, int ifidx, uint8_t peer[ETH_ALEN]);
  void (*add_tdls_peer)(struct brcmf_pub* drvr, int ifidx, uint8_t peer[ETH_ALEN]);
};

static inline void brcmf_proto_add_iface(struct brcmf_pub* drvr, int ifidx) {
  if (drvr->proto == nullptr) {
    BRCMF_WARN("brcmf_proto doesn't exist.");
    return;
  }
  drvr->proto->add_iface(drvr, ifidx);
}
static inline void brcmf_proto_del_iface(struct brcmf_pub* drvr, int ifidx) {
  if (drvr->proto == nullptr) {
    BRCMF_WARN("brcmf_proto doesn't exist.");
    return;
  }
  drvr->proto->del_iface(drvr, ifidx);
}
static inline void brcmf_proto_reset_iface(struct brcmf_pub* drvr, int ifidx) {
  if (drvr->proto == nullptr) {
    BRCMF_WARN("brcmf_proto doesn't exist.");
    return;
  }
  drvr->proto->reset_iface(drvr, ifidx);
}
static inline void brcmf_proto_configure_addr_mode(struct brcmf_pub* drvr, int ifidx,
                                                   enum proto_addr_mode addr_mode) {
  if (drvr->proto == nullptr) {
    BRCMF_WARN("brcmf_proto doesn't exist.");
    return;
  }
  drvr->proto->configure_addr_mode(drvr, ifidx, addr_mode);
}
static inline zx_status_t brcmf_proto_query_dcmd(struct brcmf_pub* drvr, int ifidx, uint cmd,
                                                 void* buf, uint len, bcme_status_t* fwerr) {
  if (drvr->proto == nullptr) {
    BRCMF_WARN("brcmf_proto doesn't exist.");
    return ZX_ERR_INVALID_ARGS;
  }
  return drvr->proto->query_dcmd(drvr, ifidx, cmd, buf, len, fwerr);
}
static inline zx_status_t brcmf_proto_set_dcmd(struct brcmf_pub* drvr, int ifidx, uint cmd,
                                               void* buf, uint len, bcme_status_t* fwerr) {
  if (drvr->proto == nullptr) {
    BRCMF_WARN("brcmf_proto doesn't exist.");
    return ZX_ERR_INVALID_ARGS;
  }
  return drvr->proto->set_dcmd(drvr, ifidx, cmd, buf, len, fwerr);
}

static inline zx_status_t brcmf_tx_queue_frames(
    struct brcmf_pub* drvr, cpp20::span<wlan::drivers::components::Frame> frames) {
  return drvr->proto->tx_queue_frames(drvr, frames);
}

inline int brcmf_proto_hdrpull_frame(struct brcmf_pub* drvr,
                                     wlan::drivers::components::Frame& frame,
                                     struct brcmf_if** ifp) {
  struct brcmf_if* tmp = nullptr;

  // Assure protocol is always called with non-null initialized pointer.
  if (ifp) {
    *ifp = nullptr;
  } else {
    ifp = &tmp;
  }
  return drvr->proto->hdrpull_frame(drvr, frame, ifp);
}

static inline void brcmf_proto_delete_peer(struct brcmf_pub* drvr, int ifidx,
                                           uint8_t peer[ETH_ALEN]) {
  if (drvr->proto == nullptr) {
    BRCMF_WARN("brcmf_proto doesn't exist.");
    return;
  }
  drvr->proto->delete_peer(drvr, ifidx, peer);
}
static inline void brcmf_proto_add_tdls_peer(struct brcmf_pub* drvr, int ifidx,
                                             uint8_t peer[ETH_ALEN]) {
  if (drvr->proto == nullptr) {
    BRCMF_WARN("brcmf_proto doesn't exist.");
    return;
  }
  drvr->proto->add_tdls_peer(drvr, ifidx, peer);
}

static inline zx_status_t brcmf_proto_reset(struct brcmf_pub* drvr) {
  if (!drvr->proto->reset) {
    return ZX_OK;
  }
  return drvr->proto->reset(drvr);
}

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_PROTO_H_
