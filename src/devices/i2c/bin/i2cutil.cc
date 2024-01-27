// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.i2c/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/stdcompat/span.h>
#include <lib/zx/result.h>
#include <stdio.h>
#include <zircon/status.h>

#include <filesystem>

// LINT.IfChange
constexpr char kUsageSummary[] = R"""(
Usage:
  i2cutil read <device> <address> [<address>...]
  i2cutil write <device> <address> [<address>...] <data> [<data>...]
  i2cutil transact <device> (r <bytes>|w <address> [<address>...] [<data>...])...
  i2cutil ping
  i2cutil help
)""";

constexpr char kUsageDetails[] = R"""(
Commands:
  read | r              Read one byte from an I2C device. Use `i2cutil transact`
                        to read multiple bytes. <device> can be the full path of
                        a devfs node (example: `/dev/class/i2c/031`) or only the
                        devfs node's index (example: `31`). Use `i2cutil ping` to
                        get devfs node paths and indexes. <address> is the internal
                        register of <device> to read from. Use multiple <address>
                        values to access a multi-byte (little-endian) register address.
                        For example `i2cutil read 4 0x20 0x3D` to read the register at
                        `0x203D`.
  write | w             Write one or more bytes (<data>) to an I2C device. See the
                        `i2cutil read` description for explanations of <device>
                        and <address>.
  transact | t          Perform a transaction with multiple segments. Each segment
                        can be a write (`w`) or a read (`r`).
  ping | p              Ping all I2C devices under devfs path `/dev/class/i2c` by
                        reading from each device's 0x00 address.
  help | h              Print this help text.

Examples:
  Read one byte from the register at `0x20` of the I2C device represented by
  devfs node index `4`:
  $ i2cutil read 4 0x20

  Read three bytes from the same I2C device and register as the last example:
  $ i2cutil transact 4 w 0x20 r 3

  Read one byte from the register at the multi-byte address `0x203D` of the
  I2C device represented by devfs node index `4`:
  $ i2cutil read 4 0x20 0x3D

  Same as the last example but represent the I2C device with a devfs node path:
  $ i2cutil read /dev/class/i2c/004 0x20 0x3D

  Write byte `0x12` to the register at `0x2C` of the I2C device represented by
  devfs node index `3`:
  $ i2cutil write 3 0x2C 0x12

  Write byte `0x121B` to the same device and register as the last example:
  $ i2cutil write 3 0x2C 0x12 0x1B

  Write byte `0x1B to register `0x2C12` of a different device (note that
  this is the exact same command as the last example; the meaning of the
  arguments depends on the I2C device):
  $ i2cutil write 3 0x2C 0x12 0x1B

  Ping all I2C devices:
  $ i2cutil ping
  /dev/class/i2c/821: OK
  /dev/class/i2c/822: OK
  /dev/class/i2c/823: OK
  /dev/class/i2c/824: OK
  Error ZX_ERR_TIMED_OUT
  /dev/class/i2c/825: ERROR

Notes:
  Source code for `i2cutil`: https://cs.opensource.google/fuchsia/fuchsia/+/main:src/devices/i2c/bin/i2cutil.cc
)""";
// LINT.ThenChange(//docs/reference/tools/hardware/i2cutil.md)

static void usage(bool show_details) {
  printf(kUsageSummary);
  if (!show_details) {
    printf("\nUse `i2cutil help` to see full help text\n");
  } else {
    printf(kUsageDetails);
  }
}

template <typename T>
static zx_status_t convert_args(char** argv, size_t length, T* buffer) {
  for (size_t i = 0; i < length; i++) {
    char* end = nullptr;
    unsigned long value = strtoul(argv[i], &end, 0);
    if (value > 0xFF || *end != '\0') {
      return ZX_ERR_INVALID_ARGS;
    }
    buffer[i] = static_cast<T>(value);
  }
  return ZX_OK;
}

static zx_status_t write_bytes(fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client,
                               cpp20::span<uint8_t> write_buffer) {
  auto write_data =
      fidl::VectorView<uint8_t>::FromExternal(write_buffer.data(), write_buffer.size());

  fidl::Arena arena;
  auto transactions = fidl::VectorView<fuchsia_hardware_i2c::wire::Transaction>(arena, 1);
  transactions[0] =
      fuchsia_hardware_i2c::wire::Transaction::Builder(arena)
          .data_transfer(fuchsia_hardware_i2c::wire::DataTransfer::WithWriteData(arena, write_data))
          .Build();

  auto read = client->Transfer(transactions);
  auto status = read.status();
  if (status == ZX_OK && read->is_error()) {
    status = ZX_ERR_INTERNAL;
  }
  return status;
}

static zx::result<uint8_t> read_byte(fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client,
                                     cpp20::span<uint8_t> address) {
  auto write_data = fidl::VectorView<uint8_t>::FromExternal(address.data(), address.size());

  fidl::Arena arena;
  auto transactions = fidl::VectorView<fuchsia_hardware_i2c::wire::Transaction>(arena, 2);
  transactions[0] =
      fuchsia_hardware_i2c::wire::Transaction::Builder(arena)
          .data_transfer(fuchsia_hardware_i2c::wire::DataTransfer::WithWriteData(arena, write_data))
          .Build();
  transactions[1] = fuchsia_hardware_i2c::wire::Transaction::Builder(arena)
                        .data_transfer(fuchsia_hardware_i2c::wire::DataTransfer::WithReadSize(1))
                        .Build();

  auto read = client->Transfer(transactions);
  auto status = read.status();
  if (status != ZX_OK) {
    return zx::error(read.status());
  }
  if (read->is_error()) {
    return zx::error(read->error_value());
  }
  return zx::ok(read->value()->read_data[0].data()[0]);
}

static zx_status_t transact(fidl::WireSyncClient<fuchsia_hardware_i2c::Device> client, int argc,
                            char** argv) {
  size_t n_elements = argc - 3;
  size_t n_segments = 0;
  size_t n_writes = 0;
  // We know n_segments and total data will be smaller than n_elements, so we use it as max.
  auto segment_start = std::make_unique<size_t[]>(n_elements);
  auto writes_start = std::make_unique<size_t[]>(n_elements);
  auto write_buffer = std::make_unique<uint8_t[]>(n_elements);

  // Find n_segments, segment starts and writes starts.
  for (size_t i = 0; i < n_elements; ++i) {
    if (argv[3 + i][0] == 'r') {
      segment_start[n_segments++] = i + 1;
    } else if (argv[3 + i][0] == 'w') {
      segment_start[n_segments++] = i + 1;
      writes_start[n_writes++] = i + 1;
    }
  }

  // Must have at least one segment and start with a w or r.
  if (n_segments == 0 || (argv[3][0] != 'r' && argv[3][0] != 'w')) {
    usage(false);
    return -1;
  }
  if (n_segments > fuchsia_hardware_i2c::wire::kMaxCountTransactions) {
    printf("No more than %u segments allowed\n", fuchsia_hardware_i2c::wire::kMaxCountTransactions);
    return -1;
  }

  // For the last segment we pretend that data starts after a pretend w/r, this makes
  // calculations below consistent for the last actual segment without a segment to follow.
  segment_start[n_segments] = n_elements + 1;

  auto write_data = std::make_unique<fidl::VectorView<uint8_t>[]>(n_writes);
  auto read_lengths = std::make_unique<uint32_t[]>(n_segments - n_writes);
  auto is_write = std::make_unique<bool[]>(n_segments);

  fidl::Arena arena;
  auto transactions = fidl::VectorView<fuchsia_hardware_i2c::wire::Transaction>(arena, n_segments);

  size_t element_cnt = 0;
  size_t segment_cnt = 0;
  size_t read_cnt = 0;
  size_t write_cnt = 0;
  uint8_t* write_buffer_pos = write_buffer.get();
  while (element_cnt < n_elements) {
    if (argv[3 + element_cnt][0] == 'w') {
      is_write[segment_cnt] = true;
      element_cnt++;
    } else if (argv[3 + element_cnt][0] == 'r') {
      is_write[segment_cnt] = false;
      element_cnt++;
    } else {
      if (is_write[segment_cnt]) {
        auto write_len = segment_start[segment_cnt + 1] - segment_start[segment_cnt] - 1;
        auto status = convert_args(&argv[3 + element_cnt], write_len, write_buffer_pos);
        if (status != ZX_OK) {
          usage(false);
          return status;
        }
        write_data[write_cnt] =
            fidl::VectorView<uint8_t>::FromExternal(write_buffer_pos, write_len);
        transactions[segment_cnt] =
            fuchsia_hardware_i2c::wire::Transaction::Builder(arena)
                .data_transfer(fuchsia_hardware_i2c::wire::DataTransfer::WithWriteData(
                    arena, write_data[write_cnt]))
                .Build();
        write_buffer_pos += write_len;
        write_cnt++;
        element_cnt += write_len;
      } else {
        auto status = convert_args(&argv[3 + element_cnt], 1, &read_lengths[read_cnt]);
        if (status != ZX_OK) {
          usage(false);
          return status;
        }
        transactions[segment_cnt] =
            fuchsia_hardware_i2c::wire::Transaction::Builder(arena)
                .data_transfer(
                    fuchsia_hardware_i2c::wire::DataTransfer::WithReadSize(read_lengths[read_cnt]))
                .Build();
        read_cnt++;
        element_cnt++;
      }
      segment_cnt++;
    }
  }
  if (write_cnt != n_writes || read_cnt + write_cnt != segment_cnt) {
    usage(false);
    return ZX_ERR_INVALID_ARGS;
  }

  if (n_writes != 0) {
    printf("Writes:");
    for (size_t i = 0; i < n_writes; ++i) {
      printf(" ");
      for (size_t j = 0; j < write_data[i].count(); ++j) {
        printf("0x%02X ", write_data[i].data()[j]);
      }
    }
    printf("\n");
  }
  auto read = client->Transfer(transactions);
  auto status = read.status();
  if (status == ZX_OK) {
    if (read->is_error()) {
      return ZX_ERR_INTERNAL;
    } else {
      auto& read_data = read->value()->read_data;
      if (read_data.count() != 0) {
        printf("Reads:");
        for (auto& i : read_data) {
          printf(" ");
          for (size_t j = 0; j < i.count(); ++j) {
            printf("0x%02X ", i.data()[j]);
          }
        }
        printf("\n");
      }
    }
  }
  return status;
}

static int device_cmd(int argc, char** argv, bool print_out) {
  if (argc < 3) {
    usage(false);
    return -1;
  }

  const char* path = argv[2];
  char new_path[32];
  int id = -1;
  if (sscanf(path, "%u", &id) == 1) {
    if (snprintf(new_path, sizeof(new_path), "/dev/class/i2c/%03u", id) >= 0) {
      path = new_path;
    }
  }

  zx::result client_end = component::Connect<fuchsia_hardware_i2c::Device>(path);
  if (client_end.is_error()) {
    printf("%s: connect failed: %s\n", argv[2], client_end.status_string());
    usage(false);
    return -1;
  }
  fidl::WireSyncClient client{std::move(client_end.value())};

  zx_status_t status = ZX_OK;

  switch (argv[1][0]) {
    case 'w': {
      if (argc < 4) {
        usage(false);
        return -1;
      }

      size_t n_write_bytes = argc - 3;
      auto write_buffer = std::make_unique<uint8_t[]>(n_write_bytes);
      status = convert_args(&argv[3], n_write_bytes, write_buffer.get());
      if (status != ZX_OK) {
        usage(false);
        return status;
      }

      status =
          write_bytes(std::move(client), cpp20::span<uint8_t>(write_buffer.get(), n_write_bytes));
      if (status == ZX_OK && print_out) {
        printf("Write: ");
        for (size_t i = 0; i < n_write_bytes; ++i) {
          printf("0x%02X ", write_buffer[i]);
        }
        printf("\n");
      }
      break;
    }

    case 'r': {
      if (argc < 4) {
        usage(false);
        return -1;
      }

      size_t n_write_bytes = argc - 3;
      auto write_buffer = std::make_unique<uint8_t[]>(n_write_bytes);
      status = convert_args(&argv[3], n_write_bytes, write_buffer.get());
      if (status != ZX_OK) {
        usage(false);
        return status;
      }

      auto byte =
          read_byte(std::move(client), cpp20::span<uint8_t>(write_buffer.get(), n_write_bytes));
      status = byte.status_value();
      if (byte.is_ok() && print_out) {
        printf("Read from");
        for (size_t i = 0; i < n_write_bytes; ++i) {
          printf(" 0x%02X", write_buffer[i]);
        }
        printf(": 0x%02X\n", byte.value());
      }
      break;
    }

    case 't': {
      if (argc < 5) {
        usage(false);
        return -1;
      }

      status = transact(std::move(client), argc, argv);
      break;
    }

    default:
      printf("%c: unrecognized command\n", argv[2][0]);
      usage(false);
      return -1;
  }
  if (status != ZX_OK) {
    printf("Error %s\n", zx_status_get_string(status));
  }
  return status;
}

static int ping_cmd() {
  constexpr char kDir[] = "/dev/class/i2c";
  for (auto const& dir_entry : std::filesystem::directory_iterator{kDir}) {
    const std::filesystem::path& dev_path = dir_entry.path();
    const char* argv[] = {"i2cutil_ping", "r", dev_path.c_str(), "0x00"};
    char** argv_main = (char**)(&argv);
    auto status = device_cmd(std::size(argv), argv_main, false);
    printf("%s: %s\n", dev_path.c_str(), status == ZX_OK ? "OK" : "ERROR");
  }

  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    usage(false);
    return -1;
  }
  switch (argv[1][0]) {
    case 'w':
    case 'r':
    case 't':
      return device_cmd(argc, argv, true);
      break;
    case 'p':
      return ping_cmd();
      break;
    case 'h':
      usage(true);
      break;
    default:
      usage(false);
      return -1;
  }

  return 0;
}
