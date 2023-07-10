// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_PHYS_BOOT_SHIM_INCLUDE_PHYS_BOOT_SHIM_DEVICETREE_H_
#define ZIRCON_KERNEL_PHYS_BOOT_SHIM_INCLUDE_PHYS_BOOT_SHIM_DEVICETREE_H_

#include <lib/boot-options/boot-options.h>
#include <lib/boot-shim/devicetree.h>
#include <lib/fit/defer.h>
#include <lib/fit/result.h>
#include <lib/zbi-format/driver-config.h>
#include <lib/zbi-format/zbi.h>
#include <lib/zbitl/error-stdio.h>
#include <lib/zbitl/item.h>
#include <lib/zbitl/storage-traits.h>
#include <lib/zbitl/view.h>

#include <fbl/alloc_checker.h>
#include <ktl/span.h>
#include <ktl/string_view.h>
#include <ktl/type_traits.h>
#include <phys/allocation.h>
#include <phys/zbitl-allocation.h>

// Initializes the shim's uart from the devicetree bootshim's chosen item properties, such as
// stdout-path, ramdisk and command line. As a side effect |boot_opts| is initialized with the
// contents of commandline as well.
void DevicetreeInitUart(const boot_shim::DevicetreeBootstrapChosenNodeItem<>& chosen_item,
                        BootOptions& boot_opts);

// Performs all necessary work for initializing memory from the ranges extracted from the
// devicetree and information encoded in the chosen item, such as the zbi range.
//
// |DevicetreeInitUart| should be called before bootstrapping memory.
void DevicetreeInitMemory(const boot_shim::DevicetreeBootstrapChosenNodeItem<>& chosen_item,
                          const boot_shim::DevicetreeMemoryItem& memory_item);

// Returns an allocation pointing to a new allocated ZBI, representing the collection
// of the ZBI provided by the ramdisk, extended with the items generated by both, the
// bootstrap shim and the actual shim.
//
// Requires that both |bootstrap_shim| and |shim| being initialized.
template <typename BootstrapShim, typename Shim>
fit::result<std::string_view, zbitl::Image<Allocation>> DevicetreeLoadZbi(
    ktl::string_view input_zbi, BootstrapShim& bootstrap_shim, Shim& shim) {
  // TODO(fxbug.dev/129617): Add  real support for generating a UART item from
  // the IRQ parsing item. For the moment, we just forward the uart pointed by the bootstrap
  // shim. The IRQ is not necessary yet for the set of tests we are running.
  const auto& chosen_item =
      bootstrap_shim.template Get<boot_shim::DevicetreeBootstrapChosenNodeItem<>>();
  size_t uart_item_size = 0;
  
  chosen_item.uart().Visit([&](const auto& driver) {
    if (std::is_same_v<std::decay_t<decltype(driver.uart().config())>, zbi_dcfg_simple_t>) {
      uart_item_size = zbitl::AlignedItemLength(sizeof(zbi_dcfg_simple_t));
    }
  });
      
  size_t zbi_size =
      input_zbi.size() + bootstrap_shim.size_bytes() + shim.size_bytes() + uart_item_size;

  fbl::AllocChecker ac;
  Allocation raw_zbi = Allocation::New(ac, memalloc::Type::kDataZbi, zbi_size);
  if (!ac.check()) {
    return fit::error("Failed to allocate space for data ZBI."sv);
  }

  zbitl::Image<Allocation> new_zbi(ktl::move(raw_zbi));

  if (auto res = new_zbi.clear(); res.is_error()) {
    zbitl::PrintViewError(res.error_value());
    return fit::error("Failed to initialize data ZBI.");
  }

  zbitl::View input_zbi_view(
      zbitl::StorageFromRawHeader(reinterpret_cast<const zbi_header_t*>(input_zbi.data())));
  auto clear_error = fit::defer([&]() { input_zbi_view.ignore_error(); });
  if (auto res = new_zbi.Extend(input_zbi_view.begin(), input_zbi_view.end()); res.is_error()) {
    zbitl::PrintViewCopyError(res.error_value());
    return fit::error("Failed to load input zbi.");
  }

  zbitl::Image writable_bytes_zbi(new_zbi.storage().data());

  if (auto res = bootstrap_shim.AppendItems(writable_bytes_zbi); res.is_error()) {
    zbitl::PrintViewError(res.error_value());
    return fit::error("Failed to append bootstrap items to data ZBI.");
  }

  if (auto res = shim.AppendItems(writable_bytes_zbi); res.is_error()) {
    zbitl::PrintViewError(res.error_value());
    return fit::error("Failed to append items to data ZBI.");
  }

  // TODO(fxbug.dev/129617): Add  real support for generating a UART item from
  // the IRQ parsing item. For the moment, we just forward the uart pointed by the bootstrap
  // shim. The IRQ is not necessary yet for the set of tests we are running.
  if (uart_item_size > 0) {
    const char* error = nullptr;
    chosen_item.uart().Visit( [&](const auto& driver) {
          const auto& dcfg = driver.uart();
          if constexpr (std::is_same_v<zbi_dcfg_simple_t, ktl::decay_t<decltype(dcfg.config())>>) {
            zbi_header_t header;
            zbi_dcfg_simple_t payload;
            header.type = ZBI_TYPE_KERNEL_DRIVER;
            header.extra = dcfg.extra();
            dcfg.FillItem(&payload);

            if (new_zbi.Append(header, ktl::as_writable_bytes(ktl::span(&payload, 1))).is_error()) {
              error = "Failed to append UART item.";
            }
          }
        }
    );
    if (error != nullptr) {
      return fit::error(error);
    }
  }

  return fit::ok(std::move(new_zbi));
}

#endif  // ZIRCON_KERNEL_PHYS_BOOT_SHIM_INCLUDE_PHYS_BOOT_SHIM_DEVICETREE_H_
