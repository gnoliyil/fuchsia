// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.dsp/cpp/wire.h>
#include <getopt.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/status.h>

#include <fbl/unique_fd.h>

using DspClient = fidl::WireSyncClient<fuchsia_hardware_dsp::DspDevice>;

class DspClientHelper {
 public:
  explicit DspClientHelper(DspClient client) : client_(std::move(client)) {}
  zx_status_t Start();
  zx_status_t Stop();
  zx_status_t Load(fidl::StringView fw_name);

 private:
  DspClient client_;
};

void showUsage(char* arg) {
  fprintf(stderr, "Usage: %s <options>*\n", arg);
  fprintf(stderr,
          " dsputil DEVICE --load = FILE_NAME        load firmware to sram\n"
          " dsputil DEVICE --start                   set dsp clk enable and power on\n"
          " dsputil DEVICE --stop                    set dsp clk disable and power off\n"
          " FILE_NAME                                the file name for downloaded file.\n");
}

zx_status_t DspClientHelper::Load(fidl::StringView fw_name) {
  auto dsp_load_result = client_->LoadFirmware(fw_name);
  if (!dsp_load_result.ok()) {
    fprintf(stderr, "Failed to dsp load firmware: %s\n",
            zx_status_get_string(dsp_load_result.status()));
    return dsp_load_result.status();
  }
  return ZX_OK;
}

zx_status_t DspClientHelper::Start() {
  auto dsp_start_result = client_->Start();
  if (!dsp_start_result.ok()) {
    fprintf(stderr, "Failed to dsp start: %s\n", zx_status_get_string(dsp_start_result.status()));
    return dsp_start_result.status();
  }
  return ZX_OK;
}

zx_status_t DspClientHelper::Stop() {
  auto dsp_stop_result = client_->Stop();
  if (!dsp_stop_result.ok()) {
    fprintf(stderr, "Failed to dsp stop: %s\n", zx_status_get_string(dsp_stop_result.status()));
    return dsp_stop_result.status();
  }
  return ZX_OK;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    showUsage(argv[0]);
    return -1;
  }

  zx::result device = component::Connect<fuchsia_hardware_dsp::DspDevice>(argv[1]);
  if (device.is_error()) {
    fprintf(stderr, "%s: %s\n", argv[1], device.status_string());
    return -1;
  }

  DspClient client(std::move(device.value()));
  DspClientHelper dsp_client(std::move(client));

  static const struct option opts[] = {
      {"start", no_argument, nullptr, 's'},
      {"stop", no_argument, nullptr, 'q'},
      {"load", required_argument, nullptr, 'l'},
      {"help", no_argument, nullptr, 'h'},
  };

  for (int opt; (opt = getopt_long(argc, argv, "", opts, nullptr)) != -1;) {
    switch (opt) {
      case 's':
        if (zx_status_t status = dsp_client.Start(); status != ZX_OK) {
          fprintf(stderr, "DSP start failed: %s\n", zx_status_get_string(status));
          return -1;
        }
        break;

      case 'q':
        if (zx_status_t status = dsp_client.Stop(); status != ZX_OK) {
          fprintf(stderr, "DSP stop failed: %s\n", zx_status_get_string(status));
          return -1;
        }
        break;

      case 'l': {
        char* fw_name = optarg;
        if (!fw_name) {
          fprintf(stderr, "The firmware name is empty\n");
          return -1;
        }
        if (zx_status_t status = dsp_client.Load(fidl::StringView::FromExternal(fw_name));
            status != ZX_OK) {
          fprintf(stderr, "DSP load firmware failed: %s\n", zx_status_get_string(status));
          return -1;
        }
      } break;

      case 'h':
        showUsage(argv[0]);
        break;

      default:
        showUsage(argv[0]);
        return -1;
    }
  }

  return 0;
}
