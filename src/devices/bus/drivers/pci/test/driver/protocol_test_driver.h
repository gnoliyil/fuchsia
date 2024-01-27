// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_DEVICES_BUS_DRIVERS_PCI_TEST_DRIVER_PROTOCOL_TEST_DRIVER_H_
#define SRC_DEVICES_BUS_DRIVERS_PCI_TEST_DRIVER_PROTOCOL_TEST_DRIVER_H_

#include <fidl/fuchsia.device.test/cpp/wire.h>
#include <fuchsia/hardware/pci/cpp/banjo.h>
#include <lib/ddk/device.h>
#include <lib/device-protocol/pci.h>

#include <ddktl/device.h>
#include <pretty/hexdump.h>
#include <zxtest/base/observer.h>
#include <zxtest/zxtest.h>

#include "src/devices/bus/drivers/pci/test/driver/driver_tests.h"

class TestObserver : public zxtest::LifecycleObserver {
 public:
  void OnTestStart(const zxtest::TestCase&, const zxtest::TestInfo&) final { report_.test_count++; }
  void OnTestSuccess(const zxtest::TestCase&, const zxtest::TestInfo&) final {
    report_.success_count++;
  }
  void OnTestFailure(const zxtest::TestCase&, const zxtest::TestInfo&) final {
    report_.failure_count++;
  }
  const fuchsia_device_test::wire::TestReport& report() const { return report_; }

 private:
  fuchsia_device_test::wire::TestReport report_ = {};
};

class ProtocolTestDriver;
using ProtocolTestDriverType =
    ddk::Device<ProtocolTestDriver, ddk::Messageable<fuchsia_device_test::Test>::Mixin>;
class ProtocolTestDriver : public ProtocolTestDriverType, public TestObserver {
 public:
  // A singleton instance is used so that the test fixture has no issues working
  // with the PCI protocol.
  static zx_status_t Create(zx_device_t* parent) {
    instance_ = new ProtocolTestDriver(parent);
    if (!instance_->pci().is_valid()) {
      return ZX_ERR_INTERNAL;
    }

    return instance_->DdkAdd(kProtocolTestDriverName);
  }

  static ProtocolTestDriver* GetInstance() { return instance_; }
  const ddk::Pci& pci() { return pci_; }

  void RunTests(RunTestsCompleter::Sync& completer) override;
  void DdkRelease() { delete this; }

 private:
  explicit ProtocolTestDriver(zx_device_t* parent) : ProtocolTestDriverType(parent), pci_(parent) {}

  static ProtocolTestDriver* instance_;
  ddk::Pci pci_;
};

#endif  // SRC_DEVICES_BUS_DRIVERS_PCI_TEST_DRIVER_PROTOCOL_TEST_DRIVER_H_
