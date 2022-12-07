// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_C_TEST_SANITIZER_CTOR_ORDER_TEST_H_
#define ZIRCON_SYSTEM_ULIB_C_TEST_SANITIZER_CTOR_ORDER_TEST_H_

#include <zircon/compiler.h>

enum InterposeStatus {
  DidNotInterpose,
  SuccessfullyInterposed,
  SuccessfullyInterposedByWeakSymbol,
};

struct Global {
  Global();
  InterposeStatus interposed;
};

#endif  // ZIRCON_SYSTEM_ULIB_C_TEST_SANITIZER_CTOR_ORDER_TEST_H_
