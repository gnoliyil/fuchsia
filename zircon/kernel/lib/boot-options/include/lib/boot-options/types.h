// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_BOOT_OPTIONS_INCLUDE_LIB_BOOT_OPTIONS_TYPES_H_
#define ZIRCON_KERNEL_LIB_BOOT_OPTIONS_INCLUDE_LIB_BOOT_OPTIONS_TYPES_H_

#include <zircon/limits.h>

#include <array>
#include <optional>
#include <string_view>

// This declares special types used for BootOptions members.  These, as well
// as std::string_view, bool, and uintNN_t, can be used in DEFINE_OPTIONS in
// "options.inc".

#if BOOT_OPTIONS_GENERATOR || defined(__aarch64__)
// This declares special types used by machine-specific options in "arm64.inc".
#include "arm64.h"
#endif

#if BOOT_OPTIONS_GENERATOR || defined(__riscv)
// This declares special types used by machine-specific options in "riscv64.inc".
#include "riscv64.h"
#endif

#if BOOT_OPTIONS_GENERATOR || defined(__x86_64__)
// This declares special types used by machine-specific options in "x86.inc".
#include "x86.h"
#endif

// This holds a C string always guaranteed to have a '\0' terminator.  As a
// simple invariant, SmallString::back() == '\0' is always maintained even if
// there is an earlier terminator.
using SmallString = std::array<char, 160>;

// This is used for passing in secure random bits as ASCII hex digits.  As a
// special exception to the normal constraint that the command line text be
// left as is in the ZBI item memory, the original command line text of the
// RedactedHex option's value is redacted (the buffer modified in place) so it
// does not propagate to userland.
struct RedactedHex {
  constexpr const char* c_str() const { return hex.data(); }

  constexpr explicit operator std::string_view() const { return {hex.data(), len}; }

  constexpr bool operator==(const RedactedHex& other) const {
    return std::string_view(*this) == std::string_view(other);
  }

  constexpr bool operator!=(const RedactedHex& other) const { return !(*this == other); }

  SmallString hex{};
  size_t len = 0;
};

// See kernel.oom.behavior.
enum class OomBehavior { kReboot, kJobKill };

// See kernel.entropy-test.len.
constexpr uint64_t kMaxEntropyLength = 1u << 20;

// See kernel.entropy-test.src.
enum class EntropyTestSource { kHwRng, kJitterEntropy };

// See kernel.page-scanner.eviction_policy.
enum class PageTableEvictionPolicy { kOnRequest, kNever, kAlways };

// See gfxconsole.font.
enum class GfxConsoleFont { k9x16, k18x32 };

// See kernel.enable-serial-syscalls.
enum class SerialDebugSyscalls {
  kDisabled,
  kEnabled,
  kOutputOnly,
};

// See kernel.root-job.behavior.
enum class RootJobBehavior {
  kHalt,
  kReboot,
  kBootloader,
  kRecovery,
  kShutdown,
};

// Wallclock options.
enum class WallclockType {
  kAutoDetect,
  kTsc,
  kPit,
  kHpet,
};

enum class ScannerLruAction {
  kNone,
  kEvictOnly,
  kCompressOnly,
  kEvictAndCompress,
};

enum class CompressionStrategy {
  kNone,
  kLz4,
};

enum class CompressionStorageStrategy {
  kNone,
  kTriPage,
};

// See kernel.test.ram.reserve.
struct RamReservation {
  std::optional<uint64_t> paddr;
  uint64_t size;
};

constexpr bool operator==(const RamReservation& lhs, const RamReservation& rhs) {
  return lhs.paddr == rhs.paddr && lhs.size == rhs.size;
}

constexpr bool operator!=(const RamReservation& lhs, const RamReservation& rhs) {
  return !(lhs == rhs);
}

// List of command lines argument names that are explicitly referenced in code.
// TODO(fxb/74740): remove all usages of this.
constexpr std::string_view kForceWatchdogDisabledName = "kernel.force-watchdog-disabled";
constexpr std::string_view kPageScannerEnableEvictionName = "kernel.page-scanner.enable-eviction";
constexpr std::string_view kPmmCheckerActionName = "kernel.pmm-checker.action";
constexpr std::string_view kPmmCheckerFillSizeName = "kernel.pmm-checker.fill-size";
constexpr std::string_view kRootJobBehaviorHaltName = "halt";
constexpr std::string_view kRootJobBehaviorRebootName = "reboot";
constexpr std::string_view kRootJobBehaviorBootloaderName = "bootloader";
constexpr std::string_view kRootJobBehaviorRecoveryName = "recovery";
constexpr std::string_view kRootJobBehaviorShutdownName = "shutdown";

#endif  // ZIRCON_KERNEL_LIB_BOOT_OPTIONS_INCLUDE_LIB_BOOT_OPTIONS_TYPES_H_
