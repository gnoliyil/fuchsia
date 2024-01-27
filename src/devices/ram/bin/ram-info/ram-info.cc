// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ram-info.h"

#include <lib/component/incoming/cpp/protocol.h>

#include <filesystem>

#include <soc/aml-common/aml-ram.h>

namespace ram_metrics = fuchsia_hardware_ram_metrics;

namespace {

// TODO(fxbug.dev/48254): Get default channel information through the FIDL API.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc99-designator"
constexpr ram_info::RamDeviceInfo
    kDevices[] =
        {
            {
                // Astro
                .devfs_path = "/dev/sys/platform/05:03:24/ram",
                .default_cycles_to_measure = 456000000 / 20,  // 456 Mhz, 50 ms.
                .default_channels =
                    {
                        [0] = {.name = "cpu", .mask = aml_ram::kDefaultChannelCpu},
                        [1] = {.name = "gpu", .mask = aml_ram::kDefaultChannelGpu},
                        [2] = {.name = "vdec", .mask = aml_ram::kDefaultChannelVDec},
                        [3] = {.name = "vpu", .mask = aml_ram::kDefaultChannelVpu},
                    },
            },
            {
                // Sherlock
                .devfs_path = "/dev/sys/platform/05:04:24/ram",
                .default_cycles_to_measure = 792000000 / 20,  // 792 Mhz, 50 ms.
                .default_channels =
                    {
                        [0] = {.name = "cpu", .mask = aml_ram::kDefaultChannelCpu},
                        [1] = {.name = "gpu", .mask = aml_ram::kDefaultChannelGpu},
                        [2] = {.name = "vdec", .mask = aml_ram::kDefaultChannelVDec},
                        [3] = {.name = "vpu", .mask = aml_ram::kDefaultChannelVpu},
                    },
            },
            {
                // Nelson
                .devfs_path = "/dev/sys/platform/05:05:24/ram",
                .default_cycles_to_measure = 456000000 / 20,  // 456 Mhz, 50 ms.
                .default_channels =
                    {
                        [0] = {.name = "cpu", .mask = aml_ram::kDefaultChannelCpu},
                        [1] = {.name = "gpu", .mask = aml_ram::kDefaultChannelGpu},
                        [2] = {.name = "vdec", .mask = aml_ram::kDefaultChannelVDec},
                        [3] = {.name = "vpu", .mask = aml_ram::kDefaultChannelVpu},
                    },
            },
            {
                // Av400
                .devfs_path = "/dev/sys/platform/05:07:24/ram",
                .default_cycles_to_measure = 660'000'000 / 20,  // 660 Mhz, 50 ms.
                .default_channels =
                    {
                        [0] = {.name = "cpu", .mask = aml_ram::kDefaultChannelCpu},
                        [1] = {.name = "nna", .mask = aml_ram::kA5PortIdNNA},
                        [2] = {.name = "dsp", .mask = aml_ram::kA5PortIdDsp},
                        [3] = {.name = "test", .mask = aml_ram::kA5PortIdTest},
                    },
            },
            {
                // Clover
                .devfs_path = "/dev/sys/platform/05:08:24/ram",
                .default_cycles_to_measure = 384'000'000 / 20,  // 384 Mhz, 50 ms.
                .default_channels =
                    {
                        [0] = {.name = "cpu", .mask = aml_ram::kDefaultChannelCpu},
                        [1] = {.name = "dspa", .mask = aml_ram::kA1PortIdDspa},
                        [2] = {.name = "dspb", .mask = aml_ram::kA1PortIdDspb},
                    },
            },
};
#pragma GCC diagnostic pop

double CounterToBandwidthMBs(uint64_t cycles, uint64_t frequency, uint64_t cycles_measured,
                             uint64_t bytes_per_cycle) {
  double bandwidth_rw = static_cast<double>(cycles * frequency * bytes_per_cycle);
  bandwidth_rw /= static_cast<double>(cycles_measured);
  bandwidth_rw /= 1024.0 * 1024.0;
  return bandwidth_rw;
}

}  // namespace

namespace ram_info {

void DefaultPrinter::Print(const ram_metrics::wire::BandwidthInfo& info) const {
  fprintf(file_, "channel \t\t usage (MB/s)  time: %lu ms\n", info.timestamp / ZX_MSEC(1));
  size_t ix = 0;
  double total_bandwidth_rw = 0;
  for (const auto& row : rows_) {
    if (row.empty()) {
      continue;
    }
    // We discard read-only and write-only counters as they are not supported
    // by current hardware.
    double bandwidth_rw = CounterToBandwidthMBs(info.channels[ix].readwrite_cycles, info.frequency,
                                                cycles_to_measure_, info.bytes_per_cycle);
    total_bandwidth_rw += bandwidth_rw;
    fprintf(file_, "%s (rw) \t\t %g\n", row.c_str(), bandwidth_rw);
    ++ix;
  }
  // Use total read-write cycles if supported.
  if (info.total.readwrite_cycles) {
    total_bandwidth_rw = CounterToBandwidthMBs(info.total.readwrite_cycles, info.frequency,
                                               cycles_to_measure_, info.bytes_per_cycle);
  }
  fprintf(file_, "total (rw) \t\t %g\n", total_bandwidth_rw);
}

void CsvPrinter::Print(const ram_metrics::wire::BandwidthInfo& info) const {
  size_t row_count = 0;
  for (const auto& row : rows_) {
    if (!row.empty()) {
      row_count++;
    }
  }

  fprintf(file_, "time,");

  size_t ix = 0;
  for (const auto& row : rows_) {
    if (row.empty()) {
      continue;
    }

    fprintf(file_, "\"%s\"%s", row.c_str(), (ix < row_count - 1) ? "," : "");
    ix++;
  }

  fprintf(file_, "\n%lu,", info.timestamp / ZX_MSEC(1));

  ix = 0;
  for (const auto& row : rows_) {
    if (row.empty()) {
      continue;
    }

    double bandwidth_rw = CounterToBandwidthMBs(info.channels[ix].readwrite_cycles, info.frequency,
                                                cycles_to_measure_, info.bytes_per_cycle);
    fprintf(file_, "%g%s", bandwidth_rw, (ix < row_count - 1) ? "," : "\n");
    ix++;
  }
}

zx::result<std::array<uint64_t, ram_metrics::wire::kMaxCountChannels>> ParseChannelString(
    std::string_view str) {
  if (str[0] == '\0') {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  std::array<uint64_t, ram_metrics::wire::kMaxCountChannels> channels = {};
  std::string_view next_channel = str;

  for (uint64_t& channel : channels) {
    errno = 0;
    char* endptr;
    channel = strtoul(next_channel.data(), &endptr, 0);
    if (endptr > &(*next_channel.cend())) {
      return zx::error_result(ZX_ERR_BAD_STATE);
    }

    next_channel = endptr;

    if (channel == ULONG_MAX && errno == ERANGE) {
      return zx::error(ZX_ERR_OUT_OF_RANGE);
    }

    if (next_channel[0] == '\0') {
      break;
    }

    // Only a comma separator is allowed.
    if (next_channel[0] != ',') {
      return zx::error_result(ZX_ERR_INVALID_ARGS);
    }

    next_channel = next_channel.data() + 1;
  }

  // Make sure there are no trailing characters.
  if (next_channel[0] != '\0') {
    return zx::error_result(ZX_ERR_INVALID_ARGS);
  }

  return zx::ok(channels);
}

std::tuple<fidl::ClientEnd<fuchsia_hardware_ram_metrics::Device>, ram_info::RamDeviceInfo>
ConnectToRamDevice() {
  for (const auto& info : kDevices) {
    if (!std::filesystem::exists(info.devfs_path)) {
      continue;
    }
    zx::result client_end =
        component::Connect<fuchsia_hardware_ram_metrics::Device>(info.devfs_path);
    if (client_end.is_ok()) {
      return {std::move(client_end.value()), info};
    }
  }

  return {};
}

zx_status_t MeasureBandwith(const Printer* const printer,
                            fidl::UnownedClientEnd<fuchsia_hardware_ram_metrics::Device> client_end,
                            const ram_metrics::wire::BandwidthMeasurementConfig& config) {
  auto info = fidl::WireCall(client_end)->MeasureBandwidth(config);
  if (!info.ok()) {
    return info.status();
  }
  if (info->is_error()) {
    return info->error_value();
  }

  printer->Print(info->value()->info);
  return ZX_OK;
}

zx_status_t GetDdrWindowingResults(
    fidl::UnownedClientEnd<fuchsia_hardware_ram_metrics::Device> client_end) {
  auto info = fidl::WireCall(client_end)->GetDdrWindowingResults();
  if (!info.ok()) {
    return info.status();
  }
  if (info->is_error()) {
    return info->error_value();
  }

  printf("register value: 0x%x\n", info->value()->value);
  return ZX_OK;
}

}  // namespace ram_info
