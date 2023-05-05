// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async_patterns/testing/cpp/dispatcher_bound.h>
#include <lib/driver/testing/cpp/driver_lifecycle.h>
#include <lib/driver/testing/cpp/driver_runtime_env.h>
#include <lib/driver/testing/cpp/test_environment.h>
#include <lib/driver/testing/cpp/test_node.h>
#include <lib/zx/result.h>

#include <gtest/gtest.h>
#include <src/devices/bus/testing/fake-pdev/fake-pdev.h>

#include "src/graphics/drivers/msd-arm-mali/src/registers.h"

enum InterruptIndex {
  kInterruptIndexJob = 0,
  kInterruptIndexMmu = 1,
  kInterruptIndexGpu = 2,
};

TEST(MsdArmDFv2, LoadDriver) {
  // This dispatcher is used by the driver itself.
  fdf::TestSynchronizedDispatcher driver_dispatcher{fdf::kDispatcherNoDefaultAllowSync};

  // This dispatcher is used by the test environment, and hosts the FakePDevFidl and incoming
  // directory.
  fdf::TestSynchronizedDispatcher test_env_dispatcher{{
      .is_default_dispatcher = true,
      .options = {},
      .dispatcher_name = "test-env-dispatcher",
  }};
  async_patterns::TestDispatcherBound<fdf_testing::TestNode> node_server{
      test_env_dispatcher.dispatcher(), std::in_place, std::string("root")};

  zx::result start_args = node_server.SyncCall(&fdf_testing::TestNode::CreateStartArgsAndServe);
  EXPECT_EQ(ZX_OK, start_args.status_value());

  ASSERT_TRUE(start_args.is_ok());

  // Initialize MMIOs and IRQs needed by the device.
  zx::interrupt gpu_interrupt;
  zx::result<fdf::MmioBuffer> mmio_buffer;
  fake_pdev::FakePDevFidl::Config config{.use_fake_bti = true, .use_fake_irq = true};
  {
    ASSERT_EQ(ZX_OK,
              zx::interrupt::create(zx::resource(0), 0, ZX_INTERRUPT_VIRTUAL, &gpu_interrupt));
    zx::interrupt dup_interrupt;
    ASSERT_EQ(ZX_OK, gpu_interrupt.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup_interrupt));
    config.irqs[kInterruptIndexGpu] = std::move(dup_interrupt);

    constexpr uint64_t kMmioSize = 0x100000;
    zx::vmo vmo;
    ASSERT_EQ(ZX_OK, zx::vmo::create(kMmioSize, 0, &vmo));
    zx::vmo dup_vmo;
    ASSERT_EQ(ZX_OK, vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup_vmo));
    mmio_buffer =
        fdf::MmioBuffer::Create(0, kMmioSize, std::move(dup_vmo), ZX_CACHE_POLICY_UNCACHED_DEVICE);
    ASSERT_EQ(ZX_OK, mmio_buffer.status_value());
    config.mmios[0] = fake_pdev::MmioInfo{.vmo = std::move(vmo), .size = kMmioSize};
  }

  std::optional<fdf_testing::TestEnvironment> test_environment;
  std::optional<fake_pdev::FakePDevFidl> pdev;
  EXPECT_EQ(ZX_OK,
            fdf::RunOnDispatcherSync(test_env_dispatcher.dispatcher(), [&]() {
              test_environment.emplace();
              EXPECT_EQ(ZX_OK, test_environment
                                   ->Initialize(std::move(start_args->incoming_directory_server))
                                   .status_value());
              pdev.emplace();
              pdev->SetConfig(std::move(config));

              auto result =
                  test_environment->incoming_directory()
                      .AddService<fuchsia_hardware_platform_device::Service>(
                          pdev->GetInstanceHandler(test_env_dispatcher.dispatcher()), "pdev");
              EXPECT_EQ(ZX_OK, result.status_value());
            }).status_value());

  // The GPU will be reset immediately, so mark that reset has completed.
  auto irq_status = registers::GpuIrqFlags::GetStatus().FromValue(0);
  irq_status.set_reset_completed(1);
  irq_status.WriteTo(&*mmio_buffer);
  gpu_interrupt.trigger(0, zx::time());

  // Mark that shader cores are ready.
  {
    constexpr uint64_t kCoresEnabled = 2;
    constexpr uint32_t kShaderReadyOffset =
        static_cast<uint32_t>(registers::CoreReadyState::CoreType::kShader) +
        static_cast<uint32_t>(registers::CoreReadyState::StatusType::kReady);
    mmio_buffer->Write32(kCoresEnabled, kShaderReadyOffset);
  }

  auto driver = fdf_testing::StartDriver(std::move(start_args->start_args), driver_dispatcher);

  ASSERT_TRUE(driver.is_ok());

  EXPECT_EQ(ZX_OK, fdf_testing::TeardownDriver(*driver, driver_dispatcher).status_value());
  EXPECT_EQ(ZX_OK, fdf::RunOnDispatcherSync(test_env_dispatcher.dispatcher(), [&]() {
                     pdev.reset();
                     test_environment.reset();
                   }).status_value());
}
