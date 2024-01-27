// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "helper/platform_msd_device_helper.h"
#include "mock/mock_mmio.h"
#include "src/graphics/drivers/msd-arm-mali/src/msd_arm_device.h"
#include "src/graphics/drivers/msd-arm-mali/src/registers.h"

namespace {
bool IsStringInDump(const std::vector<std::string>& dump, const std::string& str) {
  return std::any_of(dump.begin(), dump.end(), [str](const std::string& input_str) {
    return input_str.find(str) != std::string::npos;
  });
}

class TestPerfCountManager : public PerformanceCountersManager {
 public:
  std::vector<uint64_t> EnabledPerfCountFlags() override {
    return enabled_ ? std::vector<uint64_t>{1} : std::vector<uint64_t>{};
  }

  void set_enabled(bool enabled) { enabled_ = enabled; }

 private:
  bool enabled_ = false;
};
}  // namespace

// These tests are unit testing the functionality of MsdArmDevice.
// All of these tests instantiate the device in test mode, that is without the device thread active.
class TestMsdArmDevice {
 public:
  void CreateAndDestroy() {
    std::unique_ptr<MsdArmDevice> device = MsdArmDevice::Create(GetTestDeviceHandle(), false);
    EXPECT_NE(device, nullptr);
  }

  void Dump() {
    std::unique_ptr<MsdArmDevice> device = MsdArmDevice::Create(GetTestDeviceHandle(), false);
    EXPECT_NE(device, nullptr);

    MsdArmDevice::DumpState dump_state;
    device->Dump(&dump_state, true);
    ASSERT_EQ(12u, dump_state.power_states.size());
    EXPECT_EQ(std::string("L2 Cache"), dump_state.power_states[0].core_type);
    EXPECT_EQ(std::string("Present"), dump_state.power_states[0].status_type);
    EXPECT_EQ(1lu, dump_state.power_states[0].bitmask);

    EXPECT_EQ(0u, dump_state.gpu_fault_status);
    EXPECT_EQ(0u, dump_state.gpu_fault_address);

    EXPECT_EQ(3u, dump_state.job_slot_status.size());
    for (size_t i = 0; i < dump_state.job_slot_status.size(); i++)
      EXPECT_EQ(0u, dump_state.job_slot_status[i].status);

    EXPECT_EQ(8u, dump_state.address_space_status.size());
    for (size_t i = 0; i < dump_state.address_space_status.size(); i++)
      EXPECT_EQ(0u, dump_state.address_space_status[i].status);

    std::vector<std::string> dump_string;
    device->FormatDump(dump_state, &dump_string);
    EXPECT_TRUE(IsStringInDump(dump_string, "Core type L2 Cache state Present bitmap: 0x1"));
    EXPECT_TRUE(IsStringInDump(dump_string, "Job slot 2 status 0x0 head 0x0 tail 0x0 config 0x0"));
    EXPECT_TRUE(IsStringInDump(dump_string, "AS 7 status 0x0 fault status 0x0 fault address 0x0"));
    EXPECT_TRUE(IsStringInDump(
        dump_string, "Fault source_id 0, access type \"unknown\", exception type: \"Unknown\""));
    EXPECT_TRUE(IsStringInDump(dump_string, "Time since last IRQ handler"));
    EXPECT_TRUE(IsStringInDump(dump_string, "Last job interrupt time:"));
  }

  // Check that if there's a waiting request for the device thread and it's
  // descheduled for a long time for some reason that it doesn't immediately
  // think the GPU's hung before processing the request.

  void TestIdle() {
    std::unique_ptr<MsdArmDevice> device = MsdArmDevice::Create(GetTestDeviceHandle(), false);
    EXPECT_NE(device, nullptr);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    MsdArmDevice::DumpState dump_state;
    do {
      device->Dump(&dump_state, false);

      // Ensure that the GPU is idle and not doing anything at this point. A
      // failure in this may be caused by a previous test.
      if (dump_state.gpu_status == 0)
        return;

      // On reset the GPU may have to do some extra work before it becomes idle, so retry.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    // Waiting must have timed out.
    EXPECT_EQ(0u, dump_state.gpu_status);
  }

  void ProtectedMode() {
    // Use device thread so the test can wait for a reset interrupt.
    std::unique_ptr<MsdArmDevice> device = MsdArmDevice::Create(GetTestDeviceHandle(), true);
    ASSERT_NE(nullptr, device);
    if (!device->IsProtectedModeSupported()) {
      printf("Protected mode not supported, skipping test\n");
      return;
    }

    // Run on the device thread to make the thread checker happy.
    auto reply = device->RunTaskOnDeviceThread([](MsdArmDevice* device) {
      EXPECT_FALSE(device->IsInProtectedMode());
      EXPECT_EQ(1u, device->power_manager_->l2_ready_status());

      TestPerfCountManager perf_count_manager;
      perf_count_manager.set_enabled(true);
      device->perf_counters_->AddManager(&perf_count_manager);
      device->perf_counters_->Update();

      EXPECT_TRUE(device->perf_counters_->running());
      device->EnterProtectedMode();
      EXPECT_EQ(1u, device->power_manager_->l2_ready_status());
      EXPECT_TRUE(device->IsInProtectedMode());
      EXPECT_FALSE(device->perf_counters_->running());

      EXPECT_TRUE(device->ExitProtectedMode());
      EXPECT_EQ(1u, device->power_manager_->l2_ready_status());
      EXPECT_FALSE(device->IsInProtectedMode());
      // Exiting protected mode should disable and then re-enable performance counters.
      EXPECT_TRUE(device->perf_counters_->running());
      return MAGMA_STATUS_OK;
    });
    EXPECT_EQ(MAGMA_STATUS_OK, reply->Wait().get());
  }

  void PowerDownL2() {
    // Use device thread so the test can wait for a power down interrupt.
    std::unique_ptr<MsdArmDevice> device = MsdArmDevice::Create(GetTestDeviceHandle(), true);
    ASSERT_NE(nullptr, device);

    // In theory this could work without protected mode, but it's not needed. On the amlogic
    // T820 in the VIM2, powering down the L2 seems to cause GPU faults when the shader cores
    // are later powered back up again.
    if (!device->IsProtectedModeSupported()) {
      printf("Protected mode not supported, skipping test\n");
      return;
    }

    EXPECT_TRUE(device->PowerDownL2());
    EXPECT_EQ(0u, device->power_manager_->l2_ready_status());
  }

  static void QueryTimestamp() {
    std::unique_ptr<MsdArmDevice> device =
        MsdArmDevice::Create(GetTestDeviceHandle(), /*enable_device_thread*/ false);
    ASSERT_NE(nullptr, device);
    EXPECT_NE(device, nullptr);

    uint64_t last_timestamp = 0;

    for (uint32_t i = 0; i < 10; i++) {
      auto buffer = std::shared_ptr<magma::PlatformBuffer>(
          magma::PlatformBuffer::Create(magma::page_size(), "timestamp test"));
      ASSERT_TRUE(buffer);

      ASSERT_EQ(MAGMA_STATUS_OK, device->ProcessTimestampRequest(buffer).get());

      void* ptr;
      ASSERT_TRUE(buffer->MapCpu(&ptr));

      auto query = reinterpret_cast<magma_arm_mali_device_timestamp_return*>(ptr);

      EXPECT_GT(query->device_timestamp, last_timestamp);
      last_timestamp = query->device_timestamp;

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};

TEST(MsdArmDevice, CreateAndDestroy) {
  TestMsdArmDevice test;
  test.CreateAndDestroy();
}

TEST(MsdArmDevice, Dump) {
  TestMsdArmDevice test;
  test.Dump();
}

TEST(MsdArmDevice, Idle) {
  TestMsdArmDevice test;
  test.TestIdle();
}

TEST(MsdArmDevice, ProtectMode) {
  TestMsdArmDevice test;
  test.ProtectedMode();
}

TEST(MsdArmDevice, PowerDownL2) {
  TestMsdArmDevice test;
  test.PowerDownL2();
}

TEST(MsdArmDevice, QueryTimestamp) {
  TestMsdArmDevice test;
  test.QueryTimestamp();
}
