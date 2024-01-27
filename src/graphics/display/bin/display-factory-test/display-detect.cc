// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <endian.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.gpio/cpp/wire.h>
#include <fidl/fuchsia.sysinfo/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <cstdint>
#include <string_view>

#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>

namespace sysinfo = fuchsia_sysinfo;
namespace gpio = fuchsia_hardware_gpio;

enum Boards {
  SHERLOCK,
  LUIS,
  UNKNOWN_BOARD,
  BOARD_COUNT,
};

Boards board = UNKNOWN_BOARD;

fbl::StringBuffer<sysinfo::wire::kBoardNameLen> board_name;

Boards GetBoard() {
  zx::channel sysinfo_server_channel, sysinfo_client_channel;
  auto status = zx::channel::create(0, &sysinfo_server_channel, &sysinfo_client_channel);
  if (status != ZX_OK) {
    return UNKNOWN_BOARD;
  }

  const char* sysinfo_path = "svc/fuchsia.sysinfo.SysInfo";
  fbl::unique_fd sysinfo_fd(open(sysinfo_path, O_RDWR));
  if (!sysinfo_fd) {
    return UNKNOWN_BOARD;
  }
  fdio_cpp::FdioCaller caller_sysinfo(std::move(sysinfo_fd));
  auto result = fidl::WireCall(caller_sysinfo.borrow_as<sysinfo::SysInfo>())->GetBoardName();
  if (!result.ok() || result.value().status != ZX_OK) {
    return UNKNOWN_BOARD;
  }

  board_name.Clear();
  board_name.Append(result.value().name.data(), result.value().name.size());

  auto board_name_cmp = std::string_view(board_name.data(), board_name.size());
  if (board_name_cmp.find("sherlock") != std::string_view::npos) {
    return SHERLOCK;
  }
  if (board_name_cmp.find("luis") != std::string_view::npos) {
    return LUIS;
  }
  return UNKNOWN_BOARD;
}

zx::result<uint8_t> GetGpioValue(const char* gpio_path) {
  zx::result client_end = component::Connect<gpio::Device>(gpio_path);
  if (client_end.is_error()) {
    return client_end.take_error();
  }
  zx::result endpoints = fidl::CreateEndpoints<gpio::Gpio>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }
  auto& [client, server] = endpoints.value();
  const fidl::Status status = fidl::WireCall(client_end.value())->OpenSession(std::move(server));
  if (!status.ok()) {
    return zx::error(status.status());
  }
  const fidl::WireResult result = fidl::WireCall(client)->Read();
  if (!result.ok()) {
    return zx::error(result.status());
  }
  fit::result response = result.value();
  if (response.is_error()) {
    return response.take_error();
  }
  return zx::ok(response.value()->value);
}

int main(int argc, const char* argv[]) {
  if (GetBoard() == SHERLOCK) {
    const auto* path = "/dev/sys/platform/05:04:1/aml-axg-gpio/gpio-76";
    zx::result val = GetGpioValue(path);
    if (val.is_error()) {
      printf("MIPI device detect failed: %s\n", val.status_string());
      return -1;
    }
    printf("MIPI device detect type: %s\n", val.value() ? "Innolux" : "BOE");
  } else if (GetBoard() == LUIS) {
    printf("MIPI device detect type: BOE\n");
  } else {
    printf("Unknown board\n");
  }
  return 0;
}
