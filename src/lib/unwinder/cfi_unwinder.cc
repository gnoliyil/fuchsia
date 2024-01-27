// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/unwinder/cfi_unwinder.h"

#include <cinttypes>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/lib/unwinder/cfi_module.h"
#include "src/lib/unwinder/error.h"

namespace unwinder {

CfiUnwinder::CfiUnwinder(const std::vector<Module>& modules) {
  for (const auto& module : modules) {
    module_map_.emplace(module.load_address, std::make_pair(module, nullptr));
  }
}

Error CfiUnwinder::Step(Memory* stack, Registers current, Registers& next, bool is_return_address) {
  uint64_t pc;
  if (auto err = current.GetPC(pc); err.has_err()) {
    return err;
  }

  // is_return_address indicates whether pc in the current registers is a return address from a
  // previous "Step". If it is, we need to subtract 1 to find the call site because "call" could
  // be the last instruction of a nonreturn function and now the PC is pointing outside of the
  // valid code boundary.
  //
  // Subtracting 1 is sufficient here because in CfiParser::ParseInstructions, we scan CFI until
  // pc > pc_limit. So it's still correct even if pc_limit is not pointing to the beginning of an
  // instruction.
  if (is_return_address) {
    pc -= 1;
    current.SetPC(pc);
  }

  CfiModule* cfi;
  if (auto err = GetCfiModuleFor(pc, &cfi); err.has_err()) {
    return err;
  }
  if (auto err = cfi->Step(stack, current, next); err.has_err()) {
    return err;
  }
  return Success();
}

bool CfiUnwinder::IsValidPC(uint64_t pc) {
  CfiModule* cfi;
  return GetCfiModuleFor(pc, &cfi).ok();
}

Error CfiUnwinder::GetCfiModuleFor(uint64_t pc, CfiModule** out) {
  auto module_it = module_map_.upper_bound(pc);
  if (module_it == module_map_.begin()) {
    return Error("%#" PRIx64 " is not covered by any module", pc);
  }
  module_it--;
  uint64_t module_address = module_it->first;
  auto& [module, cfi] = module_it->second;

  if (!cfi) {
    cfi = std::make_unique<CfiModule>(module.memory, module_address, module.mode);
    if (auto err = cfi->Load(); err.has_err()) {
      return err;
    }
  }

  if (!cfi->IsValidPC(pc)) {
    return Error("%#" PRIx64 " is not covered by any module", pc);
  }
  *out = cfi.get();
  return Success();
}

}  // namespace unwinder
