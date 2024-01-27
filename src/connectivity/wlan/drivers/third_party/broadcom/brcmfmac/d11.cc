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
/*********************channel spec common functions*********************/

#include <fuchsia/wlan/common/c/banjo.h>
#include <zircon/assert.h>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/brcmu_d11.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/brcmu_utils.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/brcmu_wifi.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/debug.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/linuxisms.h"

static uint16_t d11n_sb(enum brcmu_chan_sb sb) {
  switch (sb) {
    case BRCMU_CHAN_SB_NONE:
      return BRCMU_CHSPEC_D11N_SB_N;
    case BRCMU_CHAN_SB_L:
      return BRCMU_CHSPEC_D11N_SB_L;
    case BRCMU_CHAN_SB_U:
      return BRCMU_CHSPEC_D11N_SB_U;
    default:
      WARN_ON(1);
  }
  return 0;
}

static uint16_t d11n_bw(enum brcmu_chan_bw bw) {
  switch (bw) {
    case BRCMU_CHAN_BW_20:
      return BRCMU_CHSPEC_D11N_BW_20;
    case BRCMU_CHAN_BW_40:
      return BRCMU_CHSPEC_D11N_BW_40;
    default:
      WARN_ON(1);
  }
  return 0;
}

static void brcmu_d11n_encchspec(struct brcmu_chan* ch) {
  if (ch->bw == BRCMU_CHAN_BW_20) {
    ch->sb = BRCMU_CHAN_SB_NONE;
  }

  ch->chspec = 0;
  brcmu_maskset16(&ch->chspec, BRCMU_CHSPEC_CH_MASK, BRCMU_CHSPEC_CH_SHIFT, ch->chnum);
  brcmu_maskset16(&ch->chspec, BRCMU_CHSPEC_D11N_SB_MASK, 0, d11n_sb(ch->sb));
  brcmu_maskset16(&ch->chspec, BRCMU_CHSPEC_D11N_BW_MASK, 0, d11n_bw(ch->bw));

  if (ch->chnum <= CH_MAX_2G_CHANNEL) {
    ch->chspec |= BRCMU_CHSPEC_D11N_BND_2G;
  } else {
    ch->chspec |= BRCMU_CHSPEC_D11N_BND_5G;
  }
}

static uint16_t d11ac_bw(enum brcmu_chan_bw bw) {
  switch (bw) {
    case BRCMU_CHAN_BW_20:
      return BRCMU_CHSPEC_D11AC_BW_20;
    case BRCMU_CHAN_BW_40:
      return BRCMU_CHSPEC_D11AC_BW_40;
    case BRCMU_CHAN_BW_80:
      return BRCMU_CHSPEC_D11AC_BW_80;
    default:
      WARN_ON(1);
  }
  return 0;
}

static void brcmu_d11ac_encchspec(struct brcmu_chan* ch) {
  if (ch->bw == BRCMU_CHAN_BW_20 || ch->sb == BRCMU_CHAN_SB_NONE) {
    ch->sb = BRCMU_CHAN_SB_L;
  }

  brcmu_maskset16(&ch->chspec, BRCMU_CHSPEC_CH_MASK, BRCMU_CHSPEC_CH_SHIFT, ch->chnum);
  brcmu_maskset16(&ch->chspec, BRCMU_CHSPEC_D11AC_SB_MASK, BRCMU_CHSPEC_D11AC_SB_SHIFT, ch->sb);
  brcmu_maskset16(&ch->chspec, BRCMU_CHSPEC_D11AC_BW_MASK, 0, d11ac_bw(ch->bw));

  ch->chspec &= ~BRCMU_CHSPEC_D11AC_BND_MASK;
  if (ch->chnum <= CH_MAX_2G_CHANNEL) {
    ch->chspec |= BRCMU_CHSPEC_D11AC_BND_2G;
  } else {
    ch->chspec |= BRCMU_CHSPEC_D11AC_BND_5G;
  }
}

static void brcmu_d11n_decchspec(struct brcmu_chan* ch) {
  uint16_t val;

  ch->chnum = (uint8_t)(ch->chspec & BRCMU_CHSPEC_CH_MASK);
  ch->control_ch_num = ch->chnum;

  switch (ch->chspec & BRCMU_CHSPEC_D11N_BW_MASK) {
    case BRCMU_CHSPEC_D11N_BW_20:
      ch->bw = BRCMU_CHAN_BW_20;
      ch->sb = BRCMU_CHAN_SB_NONE;
      break;
    case BRCMU_CHSPEC_D11N_BW_40:
      ch->bw = BRCMU_CHAN_BW_40;
      val = ch->chspec & BRCMU_CHSPEC_D11N_SB_MASK;
      if (val == BRCMU_CHSPEC_D11N_SB_L) {
        ch->sb = BRCMU_CHAN_SB_L;
        ch->control_ch_num -= CH_10MHZ_APART;
      } else {
        ch->sb = BRCMU_CHAN_SB_U;
        ch->control_ch_num += CH_10MHZ_APART;
      }
      break;
    default:
      WARN_ON_ONCE(1);
      break;
  }

  switch (ch->chspec & BRCMU_CHSPEC_D11N_BND_MASK) {
    case BRCMU_CHSPEC_D11N_BND_5G:
      ch->band = BRCMU_CHAN_BAND_5G;
      break;
    case BRCMU_CHSPEC_D11N_BND_2G:
      ch->band = BRCMU_CHAN_BAND_2G;
      break;
    default:
      WARN_ON_ONCE(1);
      break;
  }
}

static void brcmu_d11ac_decchspec(struct brcmu_chan* ch) {
  uint16_t val;

  ch->chnum = (uint8_t)(ch->chspec & BRCMU_CHSPEC_CH_MASK);
  ch->control_ch_num = ch->chnum;

  switch (ch->chspec & BRCMU_CHSPEC_D11AC_BW_MASK) {
    case BRCMU_CHSPEC_D11AC_BW_20:
      ch->bw = BRCMU_CHAN_BW_20;
      ch->sb = BRCMU_CHAN_SB_NONE;
      break;
    case BRCMU_CHSPEC_D11AC_BW_40:
      ch->bw = BRCMU_CHAN_BW_40;
      val = ch->chspec & BRCMU_CHSPEC_D11AC_SB_MASK;
      if (val == BRCMU_CHSPEC_D11AC_SB_L) {
        ch->sb = BRCMU_CHAN_SB_L;
        ch->control_ch_num -= CH_10MHZ_APART;
      } else if (val == BRCMU_CHSPEC_D11AC_SB_U) {
        ch->sb = BRCMU_CHAN_SB_U;
        ch->control_ch_num += CH_10MHZ_APART;
      } else {
        WARN_ON_ONCE(1);
      }
      break;
    case BRCMU_CHSPEC_D11AC_BW_80:
      ch->bw = BRCMU_CHAN_BW_80;
      ch->sb = static_cast<brcmu_chan_sb>(
          brcmu_maskget16(ch->chspec, BRCMU_CHSPEC_D11AC_SB_MASK, BRCMU_CHSPEC_D11AC_SB_SHIFT));
      switch (ch->sb) {
        case BRCMU_CHAN_SB_LL:
          ch->control_ch_num -= CH_30MHZ_APART;
          break;
        case BRCMU_CHAN_SB_LU:
          ch->control_ch_num -= CH_10MHZ_APART;
          break;
        case BRCMU_CHAN_SB_UL:
          ch->control_ch_num += CH_10MHZ_APART;
          break;
        case BRCMU_CHAN_SB_UU:
          ch->control_ch_num += CH_30MHZ_APART;
          break;
        default:
          WARN_ON_ONCE(1);
          break;
      }
      break;
    case BRCMU_CHSPEC_D11AC_BW_8080:
    case BRCMU_CHSPEC_D11AC_BW_160:
    default:
      WARN_ON_ONCE(1);
      break;
  }

  switch (ch->chspec & BRCMU_CHSPEC_D11AC_BND_MASK) {
    case BRCMU_CHSPEC_D11AC_BND_5G:
      ch->band = BRCMU_CHAN_BAND_5G;
      break;
    case BRCMU_CHSPEC_D11AC_BND_2G:
      ch->band = BRCMU_CHAN_BAND_2G;
      break;
    default:
      WARN_ON_ONCE(1);
      break;
  }
}

uint16_t channel_to_chanspec(const brcmu_d11inf* d11inf, const wlan_channel_t* ch) {
  struct brcmu_chan ch_inf;

  ch_inf.chnum = ch->primary;

  switch (ch->cbw) {
    case CHANNEL_BANDWIDTH_CBW20:
      ch_inf.bw = BRCMU_CHAN_BW_20;
      ch_inf.sb = BRCMU_CHAN_SB_NONE;
      break;
    case CHANNEL_BANDWIDTH_CBW40:
      ch_inf.bw = BRCMU_CHAN_BW_40;
      ch_inf.sb = BRCMU_CHAN_SB_U;
      break;
    case CHANNEL_BANDWIDTH_CBW40BELOW:
      ch_inf.bw = BRCMU_CHAN_BW_40;
      ch_inf.sb = BRCMU_CHAN_SB_L;
      break;
    case CHANNEL_BANDWIDTH_CBW80:
    case CHANNEL_BANDWIDTH_CBW160:
    case CHANNEL_BANDWIDTH_CBW80P80:
    default:
      BRCMF_ERR("unsupported channel width");
      break;
  }

  // ch_info.band is handled by encchspec

  d11inf->encchspec(&ch_inf);

  return ch_inf.chspec;
}

void chanspec_to_channel(const brcmu_d11inf* d11_inf, uint16_t chanspec, wlan_channel_t* ch) {
  brcmu_chan ch_inf = {.chspec = chanspec};
  d11_inf->decchspec(&ch_inf);

  ch->primary = ch_inf.chnum;
  ch->secondary80 = 0;

  switch (ch_inf.bw) {
    case BRCMU_CHAN_BW_20:
      ch->cbw = CHANNEL_BANDWIDTH_CBW20;
      break;
    case BRCMU_CHAN_BW_40:
      switch (ch_inf.sb) {
        case BRCMU_CHAN_SB_U:
          ch->cbw = CHANNEL_BANDWIDTH_CBW40;
          break;
        case BRCMU_CHAN_SB_L:
          ch->cbw = CHANNEL_BANDWIDTH_CBW40BELOW;
          break;
        default:
          ZX_DEBUG_ASSERT(0);
          break;
      }
      break;
    case BRCMU_CHAN_BW_80:
      ch->cbw = CHANNEL_BANDWIDTH_CBW80;
      break;
    default:
      BRCMF_ERR("unsupported channel width");
      break;
  }
}

void brcmu_d11_attach(struct brcmu_d11inf* d11inf) {
  if (d11inf->io_type == BRCMU_D11N_IOTYPE) {
    d11inf->encchspec = brcmu_d11n_encchspec;
    d11inf->decchspec = brcmu_d11n_decchspec;
  } else {
    d11inf->encchspec = brcmu_d11ac_encchspec;
    d11inf->decchspec = brcmu_d11ac_decchspec;
  }
}
