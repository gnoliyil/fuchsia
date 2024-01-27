// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.input/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/zx/event.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/threads.h>
#include <zircon/types.h>

#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include <fbl/algorithm.h>
#include <hid-parser/parser.h>
#include <hid-parser/report.h>
#include <hid-parser/units.h>
#include <hid-parser/usages.h>

// defined in report.cpp
void print_report_descriptor(const uint8_t* rpt_desc, size_t desc_len);

#define DEV_INPUT "/dev/class/input"

static bool verbose = false;
#define xprintf(fmt...) \
  do {                  \
    if (verbose)        \
      printf(fmt);      \
  } while (0)

enum class Command { read, read_all, get, set, descriptor, descriptor_all };

void usage() {
  printf("usage: hid [-v] <command> [<args>]\n\n");
  printf("  commands:\n");
  printf("    read [<devpath> [num reads]]\n");
  printf("    get <devpath> <in|out|feature> <id>\n");
  printf("    set <devpath> <in|out|feature> <id> [0xXX *]\n");
  printf("    descriptor [<devpath>]\n");
}

constexpr size_t kDevPathSize = 128;

struct input_args_t {
  Command command;

  fidl::WireSyncClient<fuchsia_hardware_input::Device> sync_client;

  char devpath[kDevPathSize];
  size_t num_reads;

  fuchsia_hardware_input::wire::ReportType report_type;
  uint8_t report_id;

  const char** data;
  size_t data_size;
};

static mtx_t print_lock = MTX_INIT;
#define lprintf(fmt...)      \
  do {                       \
    mtx_lock(&print_lock);   \
    printf(fmt);             \
    mtx_unlock(&print_lock); \
  } while (0)

static void print_hex(const uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    printf("%02x ", buf[i]);
    if (i % 16 == 15)
      printf("\n");
  }
  printf("\n");
}

static zx_status_t parse_uint_arg(const char* arg, uint32_t min, uint32_t max, uint32_t* out_val) {
  if ((arg == nullptr) || (out_val == nullptr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  bool is_hex = (arg[0] == '0') && (arg[1] == 'x');
  if (sscanf(arg, is_hex ? "%x" : "%u", out_val) != 1) {
    return ZX_ERR_INVALID_ARGS;
  }

  if ((*out_val < min) || (*out_val > max)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  return ZX_OK;
}

static zx_status_t parse_input_report_type(const char* arg,
                                           fuchsia_hardware_input::wire::ReportType* out_type) {
  if ((arg == nullptr) || (out_type == nullptr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  static const struct {
    const char* name;
    fuchsia_hardware_input::wire::ReportType type;
  } LUT[] = {
      {.name = "in", .type = fuchsia_hardware_input::wire::ReportType::kInput},
      {.name = "out", .type = fuchsia_hardware_input::wire::ReportType::kOutput},
      {.name = "feature", .type = fuchsia_hardware_input::wire::ReportType::kFeature},
  };

  for (auto i : LUT) {
    if (!strcasecmp(arg, i.name)) {
      *out_type = i.type;
      return ZX_OK;
    }
  }

  return ZX_ERR_INVALID_ARGS;
}

static zx_status_t print_hid_protocol(input_args_t* args) {
  auto result = args->sync_client->GetBootProtocol();
  if (result.status() != ZX_OK) {
    lprintf("hid: could not get protocol from %s (status=%d)\n", args->devpath, result.status());
  } else {
    lprintf("hid: %s proto=%d\n", args->devpath, static_cast<uint32_t>(result.value().protocol));
  }
  return ZX_OK;
}

static zx_status_t print_report_desc(input_args_t* args) {
  auto result = args->sync_client->GetReportDesc();
  if (result.status() != ZX_OK) {
    lprintf("hid: could not get report descriptor from %s (status=%d)\n", args->devpath,
            result.status());
    return result.status();
  }

  lprintf("hid: %s report descriptor len=%zu\n", args->devpath, result.value().desc.count());

  mtx_lock(&print_lock);
  printf("hid: %s report descriptor:\n", args->devpath);
  print_hex(result.value().desc.data(), result.value().desc.count());
  if (verbose) {
    print_report_descriptor(result.value().desc.data(), result.value().desc.count());
  }
  mtx_unlock(&print_lock);
  return ZX_OK;
}

#define TRY(fn)              \
  do {                       \
    zx_status_t status = fn; \
    if (status != ZX_OK)     \
      return status;         \
  } while (0)

static zx_status_t print_hid_status(input_args_t* args) {
  TRY(print_hid_protocol(args));

  hid::DeviceDescriptor* dev_desc = nullptr;
  {
    auto result = args->sync_client->GetReportDesc();
    if (result.status() != ZX_OK) {
      return result.status();
    }
    auto parse_result = hid::ParseReportDescriptor(result.value().desc.data(),
                                                   result.value().desc.count(), &dev_desc);
    if (parse_result != hid::ParseResult::kParseOk) {
      return ZX_ERR_INTERNAL;
    }
  }
  auto cleanup = fit::defer([&dev_desc]() { hid::FreeDeviceDescriptor(dev_desc); });

  mtx_lock(&print_lock);
  printf("hid: %s num reports: %zu\n", args->devpath, dev_desc->rep_count);

  printf("hid: %s report ids...\n", args->devpath);
  for (size_t i = 0; i < dev_desc->rep_count; i++) {
    if (dev_desc->report[i].input_byte_sz != 0) {
      printf("  ID 0x%02x : TYPE %7s : SIZE %lu bytes\n", dev_desc->report[i].report_id, "Input",
             dev_desc->report[i].input_byte_sz);
    }
    if (dev_desc->report[i].output_byte_sz != 0) {
      printf("  ID 0x%02x : TYPE %7s : SIZE %lu bytes\n", dev_desc->report[i].report_id, "Output",
             dev_desc->report[i].output_byte_sz);
    }
    if (dev_desc->report[i].feature_byte_sz != 0) {
      printf("  ID 0x%02x : TYPE %7s : SIZE %lu bytes\n", dev_desc->report[i].report_id, "Feature",
             dev_desc->report[i].feature_byte_sz);
    }
  }
  mtx_unlock(&print_lock);

  return ZX_OK;
}

static zx_status_t parse_rpt_descriptor(input_args_t* args) {
  TRY(print_report_desc(args));
  return ZX_OK;
}

#undef TRY

static zx_status_t hid_input_read_report(input_args_t* args, const zx::event& report_event,
                                         size_t report_size, uint8_t* report_data,
                                         size_t* returned_size) {
  while (true) {
    auto result = args->sync_client->ReadReport();
    if (result.status() != ZX_OK) {
      return result.status();
    }
    if (result.value().status == ZX_ERR_SHOULD_WAIT) {
      report_event.wait_one(ZX_USER_SIGNAL_0, zx::time::infinite(), nullptr);
      continue;
    }
    if (result.value().data.count() > report_size) {
      return ZX_ERR_BUFFER_TOO_SMALL;
    }

    *returned_size = result.value().data.count();
    memcpy(report_data, result.value().data.data(), result.value().data.count());

    return ZX_OK;
  }
}

static int hid_read_reports(input_args_t* args) {
  zx_status_t status = print_hid_status(args);
  if (status != ZX_OK) {
    return static_cast<int>(status);
  }

  zx::event report_event;
  auto result = args->sync_client->GetReportsEvent();
  if ((result.status() != ZX_OK) || (result.value().status != ZX_OK)) {
    mtx_lock(&print_lock);
    printf("read returned error: (call_status=%d) (status=%d)\n", result.status(),
           result.value().status);
    mtx_unlock(&print_lock);
    return ZX_ERR_INTERNAL;
  }
  report_event = std::move(result.value().event);

  std::vector<uint8_t> report(fuchsia_hardware_input::wire::kMaxReportLen);
  for (uint32_t i = 0; i < args->num_reads; i++) {
    size_t returned_size;
    status =
        hid_input_read_report(args, report_event, report.size(), report.data(), &returned_size);
    if (status != ZX_OK) {
      mtx_lock(&print_lock);
      printf("hid_input_read_report returned %d\n", status);
      mtx_unlock(&print_lock);
      break;
    }

    mtx_lock(&print_lock);
    printf("read returned %ld bytes\n", returned_size);
    printf("hid: input from %s\n", args->devpath);
    print_hex(report.data(), returned_size);
    mtx_unlock(&print_lock);
  }

  lprintf("hid: closing %s\n", args->devpath);
  return ZX_OK;
}

static int hid_input_thread(void* arg) {
  input_args_t* args = static_cast<input_args_t*>(arg);
  lprintf("hid: thread started for %s\n", args->devpath);

  zx_status_t status = ZX_OK;
  if (args->command == Command::read) {
    status = hid_read_reports(args);
  } else if (args->command == Command::descriptor) {
    status = parse_rpt_descriptor(args);
  } else {
    lprintf("hid: thread found wrong command %d\n", args->command);
  }
  fflush(stdout);

  delete args;
  return status;
}

static zx_status_t hid_input_device_added(int dirfd, int event, const char* fn, void* cookie) {
  if (event != WATCH_EVENT_ADD_FILE) {
    return ZX_OK;
  }
  if (std::string_view{fn} == ".") {
    return ZX_OK;
  }

  fdio_cpp::UnownedFdioCaller caller(dirfd);
  zx::result controller =
      component::ConnectAt<fuchsia_hardware_input::Controller>(caller.directory(), fn);
  if (controller.is_error()) {
    return controller.error_value();
  }
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_input::Device>();
  if (endpoints.is_error()) {
    return endpoints.error_value();
  }
  auto& [device, server] = endpoints.value();
  const fidl::Status status = fidl::WireCall(controller.value())->OpenSession(std::move(server));
  if (!status.ok()) {
    return status.status();
  }

  input_args_t* args = new input_args_t;
  args->command = *reinterpret_cast<Command*>(cookie);

  args->sync_client = fidl::WireSyncClient<fuchsia_hardware_input::Device>(std::move(device));

  // TODO: support setting num_reads across all devices. requires a way to
  // signal shutdown to all input threads.
  args->num_reads = ULONG_MAX;
  snprintf(args->devpath, kDevPathSize, "%s", fn);

  thrd_t t;
  int ret = thrd_create_with_name(&t, hid_input_thread, args, args->devpath);
  if (ret != thrd_success) {
    printf("hid: thread %s did not start (error=%d)\n", args->devpath, ret);
    return thrd_status_to_zx_status(ret);
  }
  thrd_detach(t);
  return ZX_OK;
}

int watch_all_devices(Command command) {
  int dirfd = open(DEV_INPUT, O_DIRECTORY | O_RDONLY);
  if (dirfd < 0) {
    printf("hid: error opening %s\n", DEV_INPUT);
    return ZX_ERR_INTERNAL;
  }
  fdio_watch_directory(dirfd, hid_input_device_added, ZX_TIME_INFINITE, &command);
  close(dirfd);
  return 0;
}

// Get a single report from the device with a given report id and then print it.
int get_report(input_args_t* args) {
  auto result = args->sync_client->GetReport(args->report_type, args->report_id);
  if (result.status() != ZX_OK || result.value().status != ZX_OK) {
    printf("hid: could not get report: %d, %d\n", result.status(), result.value().status);
    return -1;
  }

  printf("hid: got %zu bytes\n", result.value().report.count());
  print_hex(result.value().report.data(), result.value().report.count());
  return 0;
}

int set_report(input_args_t* args) {
  xprintf("hid: setting report (id=0x%02x payload size=%lu)\n", args->report_id, args->data_size);

  std::unique_ptr<uint8_t[]> report(new uint8_t[args->data_size]);
  for (size_t i = 0; i < args->data_size; i++) {
    uint32_t tmp;
    zx_status_t res = parse_uint_arg(args->data[i], 0, 255, &tmp);
    if (res != ZX_OK) {
      printf("Failed to parse payload byte \"%s\" (res = %d)\n", args->data[i], res);
      return res;
    }

    report[i] = static_cast<uint8_t>(tmp);
  }

  auto report_view = fidl::VectorView<uint8_t>::FromExternal(report.get(), args->data_size);
  auto res = args->sync_client->SetReport(args->report_type, args->report_id, report_view);
  if (res.status() != ZX_OK || res.value().status != ZX_OK) {
    printf("hid: could not set report: %d, %d\n", res.status(), res.value().status);
    return -1;
  }

  printf("hid: success\n");
  return 0;
}

zx_status_t parse_input_args(int argc, const char** argv, input_args_t* args) {
  zx_status_t status;
  // Move past the first arg which is just the binary.
  argc--;
  argv++;

  if (argc == 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!strcmp("-v", argv[0])) {
    verbose = true;
    argc--;
    argv++;
  }

  if (argc == 0) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Parse the command name.
  if (!strcmp("read", argv[0])) {
    if (argc == 1) {
      args->command = Command::read_all;
      return ZX_OK;
    }
    args->command = Command::read;
  } else if (!strcmp("get", argv[0])) {
    args->command = Command::get;
  } else if (!strcmp("set", argv[0])) {
    args->command = Command::set;
  } else if (!strcmp("descriptor", argv[0])) {
    if (argc == 1) {
      args->command = Command::descriptor_all;
      return ZX_OK;
    }
    args->command = Command::descriptor;
  } else {
    return ZX_ERR_INVALID_ARGS;
  }

  if (argc < 2) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Parse <devpath>
  zx::result controller = component::Connect<fuchsia_hardware_input::Controller>(argv[1]);
  if (controller.is_error()) {
    printf("could not open %s: %s\n", argv[1], controller.status_string());
    return controller.error_value();
  }
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_input::Device>();
  if (endpoints.is_error()) {
    printf("could not create endpoints: %s\n", endpoints.status_string());
    return endpoints.error_value();
  }
  auto& [device, server] = endpoints.value();
  {
    const fidl::Status status = fidl::WireCall(controller.value())->OpenSession(std::move(server));
    if (!status.ok()) {
      printf("could not create session %s: %s\n", argv[1], status.status_string());
      return status.status();
    }
  }
  args->sync_client = fidl::WireSyncClient(std::move(device));
  snprintf(args->devpath, kDevPathSize, "%s", argv[1]);

  if (args->command == Command::descriptor) {
    if (argc > 2) {
      return ZX_ERR_INTERNAL;
    }
    return ZX_OK;
  }

  // Parse Read arguments.
  if (args->command == Command::read) {
    if (argc == 3) {
      uint32_t tmp = std::numeric_limits<uint32_t>::max();
      status = parse_uint_arg(argv[2], 0, std::numeric_limits<uint32_t>::max(), &tmp);
      if (status != ZX_OK) {
        return status;
      }
      args->num_reads = tmp;
    } else if (argc == 2) {
      args->num_reads = std::numeric_limits<uint32_t>::max();
    } else {
      return ZX_ERR_INVALID_ARGS;
    }
    return ZX_OK;
  }

  if (argc < 4) {
    return ZX_ERR_INTERNAL;
  }

  // Parse ReportType argument.
  fuchsia_hardware_input::wire::ReportType report_type;
  status = parse_input_report_type(argv[2], &report_type);
  if (status != ZX_OK) {
    return status;
  }
  args->report_type = report_type;

  // Parse Report id.
  uint32_t report_id = 255;
  status = parse_uint_arg(argv[3], 0, 255, &report_id);
  if (status != ZX_OK) {
    return status;
  }
  args->report_id = static_cast<uint8_t>(report_id);

  if ((args->command == Command::get) && (argc > 4)) {
    return ZX_ERR_INVALID_ARGS;
  }

  if ((args->command == Command::set) && (argc > 4)) {
    args->data_size = argc - 4;
    args->data = &argv[4];
  }

  return ZX_OK;
}

int main(int argc, const char** argv) {
  input_args_t args = {};
  zx_status_t status = parse_input_args(argc, argv, &args);
  if (status != ZX_OK) {
    usage();
    return 1;
  }

  if (args.command == Command::descriptor) {
    return parse_rpt_descriptor(&args);
  }
  if (args.command == Command::get) {
    return get_report(&args);
  }
  if (args.command == Command::set) {
    return set_report(&args);
  }
  if (args.command == Command::read) {
    return hid_read_reports(&args);
  }
  if (args.command == Command::read_all) {
    return watch_all_devices(Command::read);
  }
  if (args.command == Command::descriptor_all) {
    return watch_all_devices(Command::descriptor);
  }

  return 1;
}
