// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/arch/nop.h>
#include <lib/code-patching/code-patching.h>
#include <lib/fit/result.h>
#include <lib/zbitl/error-stdio.h>

#include <ktl/move.h>
#include <ktl/string_view.h>
#include <phys/main.h>

#include <ktl/enforce.h>

namespace code_patching {

fit::result<Patcher::Error> Patcher::Init(BootfsDir bootfs) {
  ZX_ASSERT(!bootfs.directory().empty());
  bootfs_ = bootfs;

  auto it = bootfs_.find(kPatchesBin);
  if (auto result = bootfs_.take_error(); result.is_error()) {
    return result;
  }
  if (it == bootfs_.end()) {
    return fit::error{Error{.reason = "failed to find patch directives"sv}};
  }

  if (it->data.size() % sizeof(Directive) != 0) {
    fit::error{Error{
        .reason = "patch directive payload has bad size"sv,
        .filename = it->name,
        .entry_offset = it.dirent_offset(),
    }};
  }

  patches_ = {
      reinterpret_cast<const Directive*>(it->data.data()),
      it->data.size() / sizeof(Directive),
  };
  return fit::ok();
}

fit::result<Patcher::Error> Patcher::PatchWithAlternative(ktl::span<ktl::byte> instructions,
                                                          ktl::string_view alternative) {
  Bytes bytes;
  if (auto result = GetPatchAlternative(alternative); result.is_ok()) {
    bytes = result.value();
  } else {
    return result.take_error();
  }

  ZX_ASSERT_MSG(
      instructions.size() >= bytes.size(),
      "instruction range (%zu bytes) is too small for patch alternative \"%.*s\" (%zu bytes)",
      instructions.size(), static_cast<int>(alternative.size()), alternative.data(), bytes.size());

  memcpy(instructions.data(), bytes.data(), bytes.size());
  sync_(instructions);
  return fit::ok();
}

void Patcher::MandatoryPatchWithAlternative(ktl::span<ktl::byte> instructions,
                                            ktl::string_view alternative) {
  auto result = PatchWithAlternative(instructions, alternative);
  if (result.is_error()) {
    printf("%s: code-patching: failed to patch with alternative \"%.*s\": ", ProgramName(),
           static_cast<int>(alternative.size()), alternative.data());
    code_patching::PrintPatcherError(result.error_value());
    abort();
  }
}

void Patcher::NopFill(ktl::span<ktl::byte> instructions) {
  arch::NopFill(instructions);
  sync_(instructions);
}

fit::result<Patcher::Error, Patcher::Bytes> Patcher::GetPatchAlternative(ktl::string_view name) {
  auto it = bootfs_.find({kPatchAlternativeDir, name});
  if (auto result = bootfs_.take_error(); result.is_error()) {
    return result.take_error();
  }
  if (it == bootfs_.end()) {
    return fit::error{Error{.reason = "failed to find patch alternative"sv}};
  }
  return fit::ok(it->data);
}

void PrintPatcherError(const Patcher::Error& error, FILE* f) {
  return zbitl::PrintBootfsError(error, f);
}

}  // namespace code_patching
