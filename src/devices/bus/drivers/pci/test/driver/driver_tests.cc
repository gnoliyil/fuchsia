// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bus/drivers/pci/test/driver/driver_tests.h"

#include <fidl/fuchsia.device.test/cpp/wire.h>
#include <getopt.h>
#include <lib/ddk/platform-defs.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/cpp/caller.h>
#include <sys/stat.h>

#include <array>

#include <fbl/unique_fd.h>
#include <zxtest/zxtest.h>

uint32_t test_log_level = 0;
using driver_integration_test::IsolatedDevmgr;
const board_test::DeviceEntry kDeviceEntry = []() {
  board_test::DeviceEntry entry = {};
  strcpy(entry.name, kFakeBusDriverName);
  entry.vid = PDEV_VID_TEST;
  entry.pid = PDEV_PID_PCI_TEST;
  entry.did = 0;
  return entry;
}();

class PciDriverTests : public zxtest::Test {
 protected:
  IsolatedDevmgr devmgr_;
  fbl::unique_fd pcictl_fd_;
  fbl::unique_fd protocol_fd_;
};

// This test builds the foundation for PCI Protocol tests. After the
// IsolatedDevmgr loads a new platform bus, it will bind the fake PCI bus
// driver. The fake bus driver creates a real device backed by the fake ecam,
// which results in our protocol test driver being loaded. The protocol test
// driver exposes a FIDL RunTests interface for the test runner to request tests
// be run and receive a summary report. Protocol tests are run in the proxied
// devhost against the real PCI protocol implementation speaking to a real PCI
// device interface, backed by the fake bus driver.
//
// Illustrated:
//
// TestRunner(driver_tests) -> pbus -> fake_pci <-> ProtocolTestDriver(pci.proxy)
//       \---------------> Fuchsia.Device.Test <-------------/
TEST_F(PciDriverTests, TestRunner) {
  IsolatedDevmgr::Args args;

  args.device_list.push_back(kDeviceEntry);
  for (auto driver_name : {"fake_pci_bus_driver", "pci"}) {
    fuchsia::driver::test::DriverLog log;
    log.name = driver_name;
    switch (test_log_level) {
      case 1:
        log.log_level = fuchsia::diagnostics::Severity::DEBUG;
        args.log_level.push_back(log);
        break;
      case 2:
        log.log_level = fuchsia::diagnostics::Severity::TRACE;
        args.log_level.push_back(log);
        break;
    }
  }
  ASSERT_OK(IsolatedDevmgr::Create(&args, &devmgr_));

  // The final path is made up of the FakeBusDriver, the bind point it creates, and
  // the final protocol test driver.
  std::array<char, 64> proto_driver_path = {};
  snprintf(proto_driver_path.data(), proto_driver_path.max_size(),
           "sys/platform/%02x:%02x:%01x/%s/%02x:%02x.%1x/%s", kDeviceEntry.vid, kDeviceEntry.pid,
           kDeviceEntry.did, kDeviceEntry.name, PCI_TEST_BUS_ID, PCI_TEST_DEV_ID, PCI_TEST_FUNC_ID,
           kProtocolTestDriverName);
  zx::result channel =
      device_watcher::RecursiveWaitForFile(devmgr_.devfs_root().get(), proto_driver_path.data());
  ASSERT_OK(channel.status_value());
  fidl::ClientEnd<fuchsia_device_test::Test> client_end(std::move(channel.value()));
  // Flush the output to this point so it doesn't interleave with the proxy's
  // test output.
  fflush(stdout);
  const fidl::WireResult result = fidl::WireCall(client_end)->RunTests();
  ASSERT_OK(result.status());
  const fidl::WireResponse response = result.value();
  ASSERT_OK(response.status);
  const fuchsia_device_test::wire::TestReport& report = response.report;
  ASSERT_NE(report.test_count, 0);
  ASSERT_EQ(report.test_count, report.success_count);
  EXPECT_EQ(report.failure_count, 0);
}

int main(int argc, char* argv[]) {
  int opt;
  int v_position = 0;
  while (!v_position && (opt = getopt(argc, argv, "hv")) != -1) {
    switch (opt) {
      case 'v':
        test_log_level++;
        v_position = optind - 1;
        break;
      case 'h':
        fprintf(stderr,
                "    Test-Specific Usage: %s [OPTIONS]\n\n"
                "    [OPTIONS]\n"
                "    -v                                                  Enable DEBUG logs\n"
                "    -vv                                                 Enable TRACE logs\n\n",
                argv[0]);
        break;
    }
  }

  // Do the minimal amount of work to forward the rest of the args to zxtest
  // with our consumed '-v' / '-vv' removed. Don't worry about additional -v
  // usage because the zxtest help will point out the invalid nature of it.
  if (v_position) {
    for (int p = v_position; p < argc - 1; p++) {
      argv[p] = argv[p + 1];
    }
    argc--;
  }
  return RUN_ALL_TESTS(argc, argv);
}
