// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <dev/hw_watchdog/generic32/init.h>
#include <dev/init.h>
#include <dev/uart/dw8250/init.h>
#include <ktl/type_traits.h>
#include <ktl/variant.h>
#include <phys/arch/arch-handoff.h>

#include <ktl/enforce.h>

void PlatformDriverHandoffEarly(const ArchPhysHandoff& arch_handoff) {
  // TODO-rvbringup: add PLIC initialization here
  // if (arch_handoff.generic32_watchdog_driver) {
  //  Generic32BitWatchdogEarlyInit(arch_handoff.generic32_watchdog_driver.value());
  //}
}

void PlatformDriverHandoffLate(const ArchPhysHandoff& arch_handoff) {
  // TODO-rvbringup: add PLIC initialization here
  // if (arch_handoff.generic32_watchdog_driver) {
  //  Generic32BitWatchdogLateInit();
  //}
}

namespace {

// Overloads for early UART initialization below.
void UartInitEarly(uint32_t extra, const uart::null::Driver::config_type& config) {}

void UartInitEarly(uint32_t extra, const zbi_dcfg_simple_t& config) {
  switch (extra) {
    case ZBI_KERNEL_DRIVER_DW8250_UART:
      Dw8250UartInitEarly(config);
      break;
  }
}

void UartInitLate(uint32_t extra) {
  switch (extra) {
    case ZBI_KERNEL_DRIVER_DW8250_UART:
      Dw8250UartInitLate();
      break;
  }
}

}  // namespace

void PlatformUartDriverHandoffEarly(const uart::all::Driver& serial) {
  ktl::visit([](const auto& uart) { UartInitEarly(uart.extra(), uart.config()); }, serial);
}

void PlatformUartDriverHandoffLate(const uart::all::Driver& serial) {
  ktl::visit([](const auto& uart) { UartInitLate(uart.extra()); }, serial);
}
