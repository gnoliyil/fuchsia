// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/sdio/cpp/banjo-mock.h>

#include <zxtest/zxtest.h>

#include "src/connectivity/bluetooth/hci/vendor/marvell/bt-hci-marvell.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

// This function is used by the mock SDIO library to validate calls to DoRwTxn. This is currently
// not expected to be used, so fail if it is.
bool operator==(const sdio_rw_txn_t& actual, const sdio_rw_txn_t& expected) { return false; }

// This function is used by the mock SDIO library to validate calls to DoRwTxnNew. This is currently
// not expected to be used, so fail if it is.
bool operator==(const sdio_rw_txn_new_t& expected, const sdio_rw_txn_new_t& actual) {
  return false;
}

namespace bt_hci_marvell {

class BtHciMarvellTest : public zxtest::Test {
 public:
  // Go through all of the steps to set up our test environment: create our two devices (SDIO fake
  // and our dut) and driver interfaces (SDIO fake and our own driver). Performs the simulated bind
  // and calls DdkInit().
  void SetUp() override {
    zxtest::Test::SetUp();

    // Construct the root of our mock device tree
    const sdio_protocol_t* sdio_proto = mock_sdio_device_.GetProto();
    fake_parent_->AddProtocol(ZX_PROTOCOL_SDIO, sdio_proto->ops, sdio_proto->ctx);

    Bind();

    // Find the device that represents our Marvell device in the mock device tree
    dut_ = fake_parent_->GetLatestChild();
    ASSERT_NOT_NULL(dut_);

    // Find the Marvell driver
    driver_instance_ = dut_->GetDeviceContext<BtHciMarvell>();
    ASSERT_NOT_NULL(driver_instance_);

    // Call DdkInit
    Init();

    // Run all of the checks to verify that the expected calls into the mock SDIO API were made
    // during initialization
    ASSERT_NO_FATAL_FAILURE(mock_sdio_device_.VerifyAndClear());
  }

  void TearDown() override {
    zxtest::Test::TearDown();

    // Deconstruct the mock device tree
    UnbindAndRelease();
  }

  void Bind() {
    // Create our driver instance, which should bind to the SDIO mock
    ASSERT_EQ(ZX_OK, BtHciMarvell::Bind(nullptr, fake_parent_.get()));
  }

  void Init() {
    // Invoke ddkInit()
    dut_->InitOp();

    // Verify that the Init operation returned a completion status
    ASSERT_EQ(ZX_OK, dut_->WaitUntilInitReplyCalled());

    // Verify that the status was success
    ASSERT_EQ(ZX_OK, dut_->InitReplyCallStatus());
  }

  void UnbindAndRelease() {
    // Flag our mock device for removal
    device_async_remove(dut_);

    // Remove from the mock device tree
    mock_ddk::ReleaseFlaggedDevices(fake_parent_.get());
  }

 protected:
  // The root of our MockDevice tree
  std::shared_ptr<MockDevice> fake_parent_ = MockDevice::FakeRootParent();

  // This is the object that will handle all of our SDIO calls to the fake parent
  ddk::MockSdio mock_sdio_device_;

 private:
  MockDevice* dut_;
  BtHciMarvell* driver_instance_;
};

// Verify that we can successfully bind, initialize, unbind, and release the driver.
TEST_F(BtHciMarvellTest, LifecycleTest) {}

}  // namespace bt_hci_marvell
