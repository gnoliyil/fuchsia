// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.hardware.light/cpp/wire.h>
#include <lib/fdio/fdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fbl/unique_fd.h>

#include "lights-cli.h"

// LINT.IfChange
constexpr char kUsageMessage[] = R"""(Usage:
  lights-cli <device_path> print <id>
  lights-cli <device_path> set <id> <brightness>
  lights-cli <device_path> set <id> <red> <green> <blue>
  lights-cli <device_path> summary

Get information about lights and control their brightness.

Commands:
  print             View the brightness and color (if applicable) of a light.
                    The reported values are floating point numbers between `0.0`
                    (completely off) and `1.0` (completely on).
  set               Set the brightness or color of a light. For lights that
                    support pulse-width modulation the brightness or color
                    values can be any number between `0.0` (completely off) and
                    `1.0` (completely on). For lights that only support simple
                    on and off states <brightness> should only be `0.0` (off) or
                    `1.0` (on).
  summary           View the total light count as well as the brightness and
                    capabilities of each light. Currently supported capabilities
                    are `Brightness`, `Rgb`, and `Simple`. `Brightness` is a
                    value between `0.0` and `1.0` as explained in the `set`
                    command's description. `Rgb` is the RGB value of the light.
                    `Simple` indicates whether the light supports pulse-width
                    modulation or only simple on and off states.

Examples:
  View the brightness of a light:
  $ lights-cli /dev/class/light/000 print AMBER_LED
  Value of AMBER_LED: Brightness 1.000000

  View the brightness and color of a light:
  $ lights-cli /dev/class/light/000 print 1
  Value of lp50xx-led-1: Brightness 0.745098 RGB 0.235294 0.176471 0.164706

  Set the brightness of a light:
  $ lights-cli /dev/class/light/000 set AMBER_LED 0.5
  # This command exits silently.

  Set a light to display the color purple:
  $ lights-cli /dev/class/light/000 set 5 0.5 0 0.5
  # This command exits silently.

  View the total light count and each light's brightness and capabilities:
  $ lights-cli /dev/class/light/000 summary
  Total 1 lights
  Value of AMBER_LED: Brightness 0.500000
      Capabilities: Brightness

Notes:
  Source code for `lights-cli`: https://cs.opensource.google/fuchsia/fuchsia/+/main:src/ui/light/bin/lights-cli/
)""";
// LINT.ThenChange(//docs/reference/tools/hardware/lights-cli.md)

zx_status_t GetDeviceHandle(const char* path, zx::channel* handle) {
  fbl::unique_fd fd(open(path, O_RDWR));
  if (fd.get() < -1) {
    fprintf(stderr, "Failed to open lights device: %d\n", fd.get());
    return ZX_ERR_IO;
  }

  zx_status_t status = fdio_get_service_handle(fd.release(), handle->reset_and_get_address());
  if (status != ZX_OK) {
    fprintf(stderr, "Failed to get FDIO handle for lights device: %d\n", status);
  }
  return status;
}

int main(int argc, char** argv) {
  if (argc <= 2) {
    printf("%s expects at least 3 arguments\n", argv[0]);
    printf(kUsageMessage);
    return 1;
  }

  zx::channel channel;
  zx_status_t status = GetDeviceHandle(argv[1], &channel);
  if (status != ZX_OK) {
    printf("Failed to open lights device at '%s'\n", argv[1]);
    return 1;
  }

  LightsCli lights_cli(std::move(channel));

  if (strcmp(argv[2], "print") == 0 && argc == 4) {
    status = lights_cli.PrintValue(atoi(argv[3]));
  } else if (strcmp(argv[2], "set") == 0 && argc == 5) {
    status = lights_cli.SetBrightness(atoi(argv[3]), atof(argv[4]));
  } else if (strcmp(argv[2], "set") == 0 && argc == 7) {
    status = lights_cli.SetRgb(atoi(argv[3]), atof(argv[4]), atof(argv[5]), atof(argv[6]));
  } else if (strcmp(argv[2], "summary") == 0 && argc == 3) {
    status = lights_cli.Summary();
  } else {
    printf("%s", kUsageMessage);
    return 1;
  }

  if (status != ZX_OK) {
    return 1;
  }
  return 0;
}
