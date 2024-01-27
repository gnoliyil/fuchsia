// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <stdio.h>
#include <zircon/assert.h>

#include <phys/address-space.h>
#include <phys/allocation.h>
#include <phys/symbolize.h>

#include "legacy-boot.h"
#include "test-main.h"

int TestMain(void* ptr, arch::EarlyTicks) {
  MainSymbolize symbolize("paging-test");

  InitMemory(ptr);

  ArchSetUpAddressSpaceLate();

  static volatile int datum = 17;
  ZX_ASSERT(datum == 17);
  datum = 23;
  ZX_ASSERT(datum == 23);

  // If we're still here, virtual memory works.
  printf("Hello virtual world!\n");

  return 0;
}
