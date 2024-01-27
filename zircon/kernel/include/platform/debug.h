// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2008 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_PLATFORM_DEBUG_H_
#define ZIRCON_KERNEL_INCLUDE_PLATFORM_DEBUG_H_

#include <stdarg.h>
#include <stdbool.h>
#include <sys/types.h>
#include <zircon/compiler.h>

void platform_dputs_thread(const char* str, size_t len);
void platform_dputs_irq(const char* str, size_t len);

// Returns 1 if a character was retrieved, 0 if no character was retrieved,
// and negative on error.
int platform_dgetc(char* c, bool wait);

static inline void platform_dputc(char c) { platform_dputs_thread(&c, 1); }

// The "p" variants should be available even if the system has panicked.

// Polls for a character.  Returns 0 on success, negative value on error.
int platform_pgetc(char* c);

void platform_pputc(char c);

#endif  // ZIRCON_KERNEL_INCLUDE_PLATFORM_DEBUG_H_
