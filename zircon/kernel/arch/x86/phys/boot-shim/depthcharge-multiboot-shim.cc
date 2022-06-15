// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/arch/zbi-boot.h>
#include <lib/boot-shim/boot-shim.h>
#include <lib/boot-shim/test-serial-number.h>
#include <lib/fit/defer.h>
#include <lib/memalloc/pool.h>
#include <lib/uart/all.h>
#include <lib/zbitl/image.h>
#include <stdlib.h>
#include <zircon/boot/driver-config.h>
#include <zircon/boot/image.h>
#include <zircon/pixelformat.h>

#include <ktl/type_traits.h>
#include <phys/main.h>
#include <phys/page-table.h>
#include <phys/stdio.h>
#include <phys/symbolize.h>
#include <phys/trampoline-boot.h>
#include <phys/uart.h>

#include "legacy-boot-shim.h"
#include "stdout.h"

// Declared in legacy-boot-shim.h
const char* kLegacyShimName = "depthcharge-multiboot-shim";

namespace {

// The old Depthcharge code uses some obsolete item types, so we can
// translate those.

constexpr uint32_t kLegacyBootdataDebugUart = 0x54524155;  // UART

struct LegacyBootdataUart {
  enum class Type : uint32_t { kPio = 1, kMmio = 2 };

  uint64_t base;
  Type type;
  uint32_t reserved;
};

// TODO(crbug.com/917455): Depthcharge as of
// https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/firmware-eve-9584.B
// prepends items and adjusts the ZBI container header, but fails to update the
// Multiboot module_t header to match.  This is now fixed upstream by
// https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/b80fb0a9b04c97769ffe73babddf0aa9e3bc0b94#
// but not yet rolled out to all devices.  So if there is a valid ZBI container
// header that says it's bigger than the Multiboot module header says it is,
// believe the ZBI header and not the outer Multiboot header.
void FixRamdiskSize() {
  if (gLegacyBoot.ramdisk.size() > sizeof(zbi_header_t)) {
    auto hdr = reinterpret_cast<zbi_header_t*>(gLegacyBoot.ramdisk.data());
    size_t zbi_size = zbitl::StorageFromRawHeader(hdr).size();
    if (zbi_size > gLegacyBoot.ramdisk.size()) {
      gLegacyBoot.ramdisk = {gLegacyBoot.ramdisk.data(), zbi_size};
    }
  }
}

// Up until
// https://chromium.googlesource.com/chromiumos/platform/depthcharge/+/b8719e3e8693edce7a91db4694c4e61b157427eb
// on May 06 2021, depthcharge passed the legacy pixel format values. In case
// an older version is encountered, we convert the older format to the newer
// one, which the kernel expects.
uint32_t FixPixelFormat(uint32_t format) {
  switch (format) {
    case 1:
      return ZX_PIXEL_FORMAT_RGB_565;
    case 2:
      return ZX_PIXEL_FORMAT_RGB_332;
    case 3:
      return ZX_PIXEL_FORMAT_RGB_2220;
    case 4:
      return ZX_PIXEL_FORMAT_ARGB_8888;
    case 5:
      return ZX_PIXEL_FORMAT_RGB_x888;
    default:
      return format;
  }
}

bool AppendDepthChargeItems(LegacyBootShim& shim, TrampolineBoot::Zbi& zbi,
                            LegacyBootShim::InputZbi::iterator kernel_item) {
  auto append = [&shim, &zbi](const zbi_header_t& header, auto payload) {
    return shim.Check("Failed to append boot loader items to data ZBI",
                      zbi.Append(header, payload));
  };

  // Any unhandled path should have no errors.
  auto& input_zbi = shim.input_zbi();
  auto cleanup = fit::defer([&input_zbi]() { ZX_ASSERT(input_zbi.take_error().is_ok()); });
  for (auto it = input_zbi.begin(); it != kernel_item && it != input_zbi.end(); ++it) {
    auto [header, payload] = *it;
    switch (header->type) {
      case kLegacyBootdataDebugUart: {
        if (payload.size() >= sizeof(LegacyBootdataUart)) {
          LegacyBootdataUart uart;
          memcpy(&uart, payload.data(), sizeof(uart));
          switch (uart.type) {
            case LegacyBootdataUart::Type::kPio: {
              const dcfg_simple_pio_t pio = {
                  .base = static_cast<uint16_t>(uart.base),
              };
              if (!append(
                      {
                          .type = ZBI_TYPE_KERNEL_DRIVER,
                          .extra = KDRV_I8250_PIO_UART,
                      },
                      zbitl::AsBytes(pio))) {
                return false;
              }
              break;
            }
            case LegacyBootdataUart::Type::kMmio: {
              const dcfg_simple_t mmio = {.mmio_phys = uart.base};
              if (!append(
                      {
                          .type = ZBI_TYPE_KERNEL_DRIVER,
                          .extra = KDRV_I8250_MMIO_UART,
                      },
                      zbitl::AsBytes(mmio))) {
                return false;
              }
              break;
            }
          }
        }
        break;
      }
      case ZBI_TYPE_FRAMEBUFFER: {
        ZX_ASSERT(payload.size() >= sizeof(zbi_swfb_t));
        zbi_swfb_t framebuffer = *reinterpret_cast<const zbi_swfb_t*>(payload.data());
        framebuffer.format = FixPixelFormat(framebuffer.format);
        if (!append(*header, zbitl::AsBytes(framebuffer))) {
          return false;
        }
        break;
      }
      default:
        if (!append(*header, payload)) {
          return false;
        }
        break;
    }
  }
  cleanup.cancel();
  return shim.Check("ZBI iteration error while appending depthcharge zbi items.", zbi.take_error());
}

// The old depthcharge code prepends its items before the kernel rather than
// appending them as the protocol requires.
bool LoadDepthchargeZbi(LegacyBootShim& shim, TrampolineBoot& boot) {
  auto& input_zbi = shim.input_zbi();
  auto kernel_item = input_zbi.find(arch::kZbiBootKernelType);
  if (shim.Check("ZBI Iteration error.", input_zbi.take_error()); kernel_item == input_zbi.end()) {
    printf("%s: No kernel item in the ZBI!.", ProgramName());
    return false;
  }

  uint32_t early_items_size = kernel_item.item_offset() - sizeof(zbi_header_t);

  return shim.Check("Not a bootable ZBI", boot.Init(input_zbi, kernel_item)) &&
         shim.Check("Failed to load ZBI", boot.Load(shim.size_bytes() + early_items_size)) &&
         shim.Check("Failed to append boot loader items to data ZBI",
                    shim.AppendItems(boot.DataZbi())) &&
         AppendDepthChargeItems(shim, boot.DataZbi(), kernel_item);
}

}  // namespace

void LegacyBootQuirks() { FixRamdiskSize(); }

// Overrides the default, weak definition.
void LegacyBootSetUartConsole(const uart::all::Driver& uart) {
  SetUartConsole(uart);
  GetUartDriver().Visit([](auto&& driver) { driver.SetLineControl(); });
}

bool LegacyBootShim::BootQuirksLoad(TrampolineBoot& boot) {
  return !IsProperZbi() && LoadDepthchargeZbi(*this, boot);
}
