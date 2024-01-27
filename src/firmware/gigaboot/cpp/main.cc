// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phys/efi/main.h"

#include <stdio.h>

#include "backends.h"
#include "fastboot_tcp.h"
#include "gigaboot/src/netifc.h"
#include "utils.h"
#include "xefi.h"
#include "zircon_boot_ops.h"

// TODO(b/235489025): The function comes from legacy gigaboot. Implement a
// similar function in C++ and remove this.
extern "C" char key_prompt(const char* valid_keys, int timeout_s);

// This is necessary because the Fastboot class inherits from FastbootBase,
// which is an abstract class with pure virtual functions.
// The _purecall definition is not provided in efi compilation, which leads to an
// 'undefined' error in asan builds.
extern "C" int _purecall(void) { return 0; }

int main(int argc, char** argv) {
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
  bool enter_fastboot = reboot_mode == gigaboot::RebootMode::kBootloader;
  if (!enter_fastboot) {
    printf("Auto boot in 2 seconds. Press f to enter fastboot.\n");
    // If time out, the first char in the `valid_keys` argument will be returned. Thus
    // we put a random different char here, so that we don't always drop to fastboot.
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

  ForceRecovery force_recovery_option =
      reboot_mode == gigaboot::RebootMode::kRecovery ? kForceRecoveryOn : kForceRecoveryOff;

  // TODO(b/236039205): Implement logic to construct these arguments for the API. This is currently
  // a placeholder for testing compilation/linking.
  ZirconBootOps zircon_boot_ops = gigaboot::GetZirconBootOps();
  ZirconBootResult res = LoadAndBoot(&zircon_boot_ops, force_recovery_option);
  if (res != kBootResultOK) {
    printf("Failed to boot zircon\n");
    return 1;
  }

  return 0;
}
