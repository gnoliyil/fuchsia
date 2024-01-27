// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phys/efi/main.h"

#include <stdio.h>

#include <phys/stdio.h>

#include "backends.h"
#include "fastboot_tcp.h"
#include "gigaboot/src/netifc.h"
#include "gigaboot/src/util.h"
#include "lib/zircon_boot/zircon_boot.h"
#include "utils.h"
#include "xefi.h"
#include "zircon_boot_ops.h"

#if defined(__x86_64__)
#include <cpuid.h>
#endif

// This is necessary because the Fastboot class inherits from FastbootBase,
// which is an abstract class with pure virtual functions.
// The _purecall definition is not provided in efi compilation, which leads to an
// 'undefined' error in asan builds.
extern "C" int _purecall(void) { return 0; }

namespace {
// Check for KVM or non-KVM QEMU using Hypervisor Vendor ID.
bool IsQemu() {
#if defined(__x86_64__)
  uint32_t eax;
  uint32_t name[3];
  __cpuid(0x40000000, eax, name[0], name[1], name[2]);
  std::string_view name_str(reinterpret_cast<const char*>(name), sizeof(name));
  return name_str == "TCGTCGTCGTCG"sv || name_str == "KVMKVMKVM\0\0\0"sv;
#else
  return false;
#endif
}

// Always enable serial output if not already. Infra relies on serial to get device feedback.
// Without it, some CI/CQs fail.
void SetSerial() {
  PhysConsole& console = PhysConsole::Get();
  if (*console.serial() != FILE()) {
    return;
  }

  // QEMU routes serial output to the console. To avoid double printing, avoid using serial
  // output when running in QEMU.
  if (IsQemu()) {
    return;
  }

  // Temporarily set `gEfiSystemTable->ConOut` to NULL to force serial console setup.
  // This won't affect the graphic set up done earlier.
  //
  // This function can be removed once SetEfiStdout() can correctly set up both graphics and serial
  // output.

  auto conn_out = gEfiSystemTable->ConOut;
  gEfiSystemTable->ConOut = nullptr;
  SetEfiStdout(gEfiSystemTable);
  gEfiSystemTable->ConOut = conn_out;
}
}  // namespace

int main(int argc, char** argv) {
  SetSerial();
  printf("Gigaboot main\n");

  auto is_secureboot_on = gigaboot::IsSecureBootOn();
  if (is_secureboot_on.is_error()) {
    printf("Failed to query SecureBoot variable\n");
  } else {
    printf("Secure Boot: %s\n", *is_secureboot_on ? "On" : "Off");
  }

  // TODO(b/235489025): We reuse some legacy C gigaboot code for stuff like network stack.
  // This initializes the global variables the legacy code needs. Once these needed features are
  // re-implemented, remove these dependencies.
  xefi_init(gEfiImageHandle, gEfiSystemTable);

  // The following check/initialize network interface and generate ip6 address.
  if (netifc_open()) {
    printf("netifc: Failed to open network interface\n");
  } else {
    printf("netifc: network interface opened\n");
  }

  // Log TPM info if the device has one.
  if (efi_status res = gigaboot::PrintTpm2Capability(); res != EFI_SUCCESS) {
    printf("Failed to log TPM 2.0 capability %s. TPM 2.0 may not be supported\n",
           gigaboot::EfiStatusToString(res));
  }

  gigaboot::RebootMode reboot_mode =
      gigaboot::GetRebootMode().value_or(gigaboot::RebootMode::kNormal);

  // Reset previous reboot mode immediately to prevent it from being sticky.
  if (reboot_mode != gigaboot::RebootMode::kNormal &&
      !SetRebootMode(gigaboot::RebootMode::kNormal)) {
    printf("Failed to reset reboot mode\n");
    return 1;
  }

  bool enter_fastboot = reboot_mode == gigaboot::RebootMode::kBootloader;
  if (enter_fastboot) {
    printf("Your BIOS instructs Gigaboot to directly enter fastboot and skip normal boot.\n");
  } else {
    printf("Auto boot in 2 seconds. Press f to enter fastboot.\n");
    // If time out, the first char in the `valid_keys` argument will be returned. Thus
    // we put a random different char here, so that we don't always drop to fastboot.
    // TODO(b/235489025): The function comes from legacy gigaboot. Implement a
    // similar function in C++ and remove this.
    char key = key_prompt("0f", 2);
    enter_fastboot = key == 'f';
  }

  if (enter_fastboot) {
    zx::result<> ret = gigaboot::FastbootTcpMain();
    if (ret.is_error()) {
      printf("Fastboot failed\n");
      return 1;
    }
  }

  ZirconBootMode boot_mode = reboot_mode == gigaboot::RebootMode::kRecovery
                                 ? kZirconBootModeForceRecovery
                                 : kZirconBootModeAbr;

  // TODO(b/236039205): Implement logic to construct these arguments for the API. This
  // is currently a placeholder for testing compilation/linking.
  ZirconBootOps zircon_boot_ops = gigaboot::GetZirconBootOps();
  ZirconBootResult res = LoadAndBoot(&zircon_boot_ops, boot_mode);
  if (res != kBootResultOK) {
    printf("Failed to boot zircon\n");
    return 1;
  }

  return 0;
}
