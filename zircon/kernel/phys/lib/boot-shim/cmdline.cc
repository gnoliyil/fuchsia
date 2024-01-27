// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/boot-shim/boot-shim.h>
#include <zircon/assert.h>

#include <optional>

namespace boot_shim {

// Private subroutine of the two methods below.
size_t BootShimBase::Cmdline::Collect(std::optional<ItemBase::WritableBytes> payload) const {
  size_t total = 0;
  auto add = [payload, &total](std::string_view str) mutable {
    if (payload) {
      auto data = reinterpret_cast<char*>(payload->data());
      ZX_ASSERT(payload->size() >= str.size());
      payload = payload->subspan(str.copy(data, payload->size()));
    }
    total += str.size();
  };

  auto add_chunk = [this, &add](std::string_view prefix, Index i) {
    if (!chunks_[i].empty()) {
      add(prefix);
      add(chunks_[i]);
    }
  };

  add_chunk("bootloader.name=", kName);
  add_chunk(" bootloader.info=", kInfo);
  if (!build_id_.desc.empty()) {
    add(" bootloader.build_id=");
    build_id_.HexDump([&add](char c) { add({&c, 1}); });
  }
  add_chunk(" ", kLegacy);

  auto add_strings = [add_one = [&add](std::string_view str) {
    add(" ");
    add(str);
  }](auto strings) { std::for_each(strings.begin(), strings.end(), add_one); };

  add_strings(strings_);
  add_strings(cstr_);

  return total;
}

size_t BootShimBase::Cmdline::size_bytes() const { return ItemSize(Collect()); }

fit::result<BootShimBase::DataZbi::Error> BootShimBase::Cmdline::AppendItems(
    BootShimBase::DataZbi& zbi) const {
  auto result = zbi.Append({
      .type = ZBI_TYPE_CMDLINE,
      .length = static_cast<uint32_t>(Collect()),
  });
  if (result.is_error()) {
    return result.take_error();
  }
  Collect(result.value()->payload);
  return fit::ok();
}

}  // namespace boot_shim
