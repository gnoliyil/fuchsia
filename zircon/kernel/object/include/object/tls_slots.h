// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_TLS_SLOTS_H_
#define ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_TLS_SLOTS_H_

#include <kernel/thread.h>

// These are the used tls entries for Thread's tls_get(), tls_set()
// and tls_set_callback(). Add entries here up to THREAD_MAX_TLS_ENTRY - 1.

#define TLS_ENTRY_KOBJ_DELETER 0
#define TLS_ENTRY_LAST 1

static_assert(TLS_ENTRY_LAST <= (THREAD_MAX_TLS_ENTRY - 1), "");

#endif  // ZIRCON_KERNEL_OBJECT_INCLUDE_OBJECT_TLS_SLOTS_H_
