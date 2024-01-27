// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <thread>

#include <gtest/gtest.h>

#include "helper/platform_pci_device_helper.h"
#include "magma_util/short_macros.h"
#include "platform_pci_device.h"

TEST(PlatformPciDevice, Basic) {
  magma::PlatformPciDevice* platform_device = TestPlatformPciDevice::GetInstance();
  ASSERT_TRUE(platform_device);

  uint16_t vendor_id = 0;
  bool ret = platform_device->ReadPciConfig16(0, &vendor_id);
  EXPECT_TRUE(ret);
  EXPECT_NE(vendor_id, 0);
}

TEST(PlatformPciDevice, MapMmio) {
  magma::PlatformPciDevice* platform_device = TestPlatformPciDevice::GetInstance();
  ASSERT_TRUE(platform_device);

  uint32_t pci_bar = 0;

  // Map once
  auto mmio = platform_device->CpuMapPciMmio(pci_bar, magma::PlatformMmio::CACHE_POLICY_CACHED);
  EXPECT_TRUE(mmio);

  // Map again same policy
  auto mmio2 = platform_device->CpuMapPciMmio(pci_bar, magma::PlatformMmio::CACHE_POLICY_CACHED);
  EXPECT_TRUE(mmio2);

  // Map again different policy - this is now permitted though it's a bad idea.
  auto mmio3 = platform_device->CpuMapPciMmio(pci_bar, magma::PlatformMmio::CACHE_POLICY_UNCACHED);
  EXPECT_TRUE(mmio3);
}

TEST(PlatformPciDevice, RegisterInterrupt) {
  magma::PlatformPciDevice* platform_device = TestPlatformPciDevice::GetInstance();
  ASSERT_NE(platform_device, nullptr);

  auto interrupt = platform_device->RegisterInterrupt();
  // Interrupt may be null if no core device support.
  if (interrupt) {
    std::thread thread([interrupt_raw = interrupt.get()] {
      DLOG("waiting for interrupt");
      interrupt_raw->Wait();
      DLOG("returned from interrupt");
    });

    interrupt->Signal();

    DLOG("waiting for thread");
    thread.join();
  }
}
