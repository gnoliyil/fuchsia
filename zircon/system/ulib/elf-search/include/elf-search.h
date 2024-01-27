// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ELF_SEARCH_H_
#define ELF_SEARCH_H_

#include <elf.h>
#include <lib/fit/function.h>
#include <lib/stdcompat/span.h>
#include <lib/zx/process.h>
#include <stdint.h>

#include <string_view>

namespace elf_search {

struct ModuleInfo {
  std::string_view name;
  uintptr_t vaddr;
  cpp20::span<const uint8_t> build_id;
  const Elf64_Ehdr& ehdr;
  cpp20::span<const Elf64_Phdr> phdrs;
};

using ModuleAction = fit::function<void(const ModuleInfo&)>;
extern zx_status_t ForEachModule(const zx::process&, ModuleAction);

}  // namespace elf_search

#endif  // ELF_SEARCH_H_
