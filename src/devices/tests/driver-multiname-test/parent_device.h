// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_TESTS_DRIVER_MULTINAME_TEST_PARENT_DEVICE_H_
#define SRC_DEVICES_TESTS_DRIVER_MULTINAME_TEST_PARENT_DEVICE_H_

#include <fidl/driver.multiname.test/cpp/wire.h>
#include <lib/inspect/cpp/inspect.h>

#include <ddktl/device.h>

namespace parent_device {

class ParentDevice;
using DeviceType = ddk::Device<ParentDevice, ddk::Initializable,
                               ddk::Messageable<driver_multiname_test::TestAddDevice>::Mixin>;
class ParentDevice : public DeviceType {
 public:
  explicit ParentDevice(zx_device_t* parent) : DeviceType(parent) {}
  ~ParentDevice() override = default;

  void DdkInit(ddk::InitTxn txn);
  void DdkRelease();

 private:
  void AddDevice(AddDeviceCompleter::Sync& completer) override;
};

}  // namespace parent_device

#endif  // SRC_DEVICES_TESTS_DRIVER_MULTINAME_TEST_PARENT_DEVICE_H_
