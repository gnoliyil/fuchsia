// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>

#include "debug.h"

// clang-format off

// UART register offsets
#define UTXD    (0x40)
#define USR1    (0x94)

#define USR1_TRDY_MASK          (0x2000)
#define UTXD_TX_DATA_MASK       (0xff)

#define UARTREG(base, reg)      (*(volatile uint32_t*)((base)  + (reg)))

#define DEBUG_UART_BASE         (0x30890000)

// clang-format on

void uart_pputc(char c) {
  while ((UARTREG(DEBUG_UART_BASE, USR1) & USR1_TRDY_MASK) == 0)
    ;
  UARTREG(DEBUG_UART_BASE, UTXD) = (uint32_t)c & UTXD_TX_DATA_MASK;
}
