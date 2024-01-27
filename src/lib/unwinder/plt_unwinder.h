// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_UNWINDER_PLT_UNWINDER_H_
#define SRC_LIB_UNWINDER_PLT_UNWINDER_H_

#include "src/lib/unwinder/cfi_unwinder.h"
#include "src/lib/unwinder/memory.h"
#include "src/lib/unwinder/registers.h"

namespace unwinder {

// Unwind when PC is in PLT, because lld doesn't generate CFI for PLT (fxbug.dev/112402).
class PltUnwinder {
 public:
  // We need |CfiUnwinder::IsValidPC|.
  explicit PltUnwinder(CfiUnwinder* cfi_unwinder) : cfi_unwinder_(cfi_unwinder) {}

  // Note that only the topmost frame could be in PLT.
  // This function assume the current frame has a trust level kContext to avoid false positives!
  Error Step(Memory* stack, const Registers& current, Registers& next);

 private:
  Error StepX64(Memory* stack, const Registers& current, Registers& next);
  Error StepArm64(Memory* stack, const Registers& current, Registers& next);

  CfiUnwinder* cfi_unwinder_;
};

}  // namespace unwinder

#endif  // SRC_LIB_UNWINDER_PLT_UNWINDER_H_
