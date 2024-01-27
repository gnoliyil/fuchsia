// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/elfldltl/vmar-loader.h"

#include <lib/stdcompat/bit.h>
#include <lib/stdcompat/span.h>
#include <lib/zx/vmar.h>
#include <zircon/assert.h>
#include <zircon/status.h>

namespace {

constexpr std::string_view kVmoNameUnknown = "<unknown ELF file>";

constexpr std::string_view kVmoNamePrefixData = "data";
constexpr std::string_view kVmoNamePrefixBss = "bss";

constexpr char kHexDigits[] = "0132456789ABCDEF";

template <const std::string_view& Prefix>
void SetVmoName(zx::unowned_vmo vmo, std::string_view base_name, size_t n) {
  std::array<char, ZX_MAX_NAME_LEN> buffer{};
  cpp20::span vmo_name(buffer);

  // First, "data" or "bss".
  size_t name_idx = Prefix.copy(vmo_name.data(), vmo_name.size());

  // Then the ordinal in hex (almost surely just one digit, but who knows).
  // Count the bits and divide with round-up to count the nybbles.
  const size_t hex_chars = (cpp20::bit_width(n | 1) + 3) / 4;
  cpp20::span hex = cpp20::span(vmo_name).subspan(name_idx, hex_chars);
  for (auto it = hex.rbegin(); it != hex.rend(); ++it) {
    *it = kHexDigits[n & 0xf];
    n >>= 4;
  }
  name_idx += hex.size();

  // Then `:`, it's guaranteed that the worst case "dataffffffffffffffff:" (21)
  // definitely fits in ZX_MAX_NAME_LEN (32).
  vmo_name[name_idx++] = ':';

  // Finally append the original VMO name, however much fits.
  name_idx += base_name.copy(vmo_name.data(), vmo_name.size(), name_idx);
  ZX_DEBUG_ASSERT(name_idx <= vmo_name.size());

  vmo->set_property(ZX_PROP_NAME, vmo_name.data(), vmo_name.size());
}

}  // namespace

namespace elfldltl {

VmarLoader::VmoName VmarLoader::GetVmoName(zx::unowned_vmo vmo) {
  VmarLoader::VmoName base_vmo_name{};
  zx_status_t status = vmo->get_property(ZX_PROP_NAME, base_vmo_name.data(), base_vmo_name.size());

  if (status != ZX_OK || base_vmo_name.front() == '\0') {
    kVmoNameUnknown.copy(base_vmo_name.data(), base_vmo_name.size());
  }

  return base_vmo_name;
}

zx_status_t VmarLoader::AllocateVmar(size_t vaddr_size, size_t vaddr_start) {
  zx_vaddr_t child_addr;
  zx_status_t status = vmar_->allocate(
      ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_WRITE | ZX_VM_CAN_MAP_EXECUTE | ZX_VM_CAN_MAP_SPECIFIC, 0,
      vaddr_size, &load_image_vmar_, &child_addr);

  if (status == ZX_OK) {
    // Convert the absolute address of the child VMAR to a platform-generic
    // address type needed for the generic DirectMemory abstraction.
    memory_.set_image({reinterpret_cast<std::byte*>(child_addr), vaddr_size});
    memory_.set_base(vaddr_start);
  }

  return status;
}

zx_status_t VmarLoader::MapWritable(uintptr_t vmar_offset, zx::unowned_vmo vmo,
                                    std::string_view base_name, uint64_t vmo_offset, size_t size,
                                    size_t& num_data_segments) {
  zx::vmo writable_vmo;
  zx_status_t status =
      vmo->create_child(ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE, vmo_offset, size, &writable_vmo);
  if (status != ZX_OK) [[unlikely]] {
    return status;
  }

  SetVmoName<kVmoNamePrefixData>(writable_vmo.borrow(), base_name, num_data_segments++);

  return Map(vmar_offset, kMapWritable, writable_vmo.borrow(), 0, size);
}

zx_status_t VmarLoader::MapZeroFill(uintptr_t vmar_offset, std::string_view base_name, size_t size,
                                    size_t& num_zero_segments) {
  zx::vmo vmo;
  zx_status_t status = zx::vmo::create(size, 0, &vmo);
  if (status != ZX_OK) [[unlikely]] {
    return status;
  }

  SetVmoName<kVmoNamePrefixBss>(vmo.borrow(), base_name, num_zero_segments++);

  return Map(vmar_offset, kMapWritable, vmo.borrow(), 0, size);
}

}  // namespace elfldltl
