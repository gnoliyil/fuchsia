// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "helper/platform_msd_device_helper.h"
#include "msd.h"
#include "platform_semaphore.h"

TEST(MsdSemaphore, ImportAndDestroy) {
  auto msd_driver = msd::Driver::Create();
  ASSERT_NE(msd_driver, nullptr);

  auto msd_device = msd_driver->CreateDevice(GetTestDeviceHandle());
  ASSERT_NE(msd_device, nullptr);
  auto semaphore = magma::PlatformSemaphore::Create();
  ASSERT_NE(semaphore, nullptr);

  uint32_t duplicate_handle;
  ASSERT_TRUE(semaphore->duplicate_handle(&duplicate_handle));

  std::unique_ptr<msd::Semaphore> msd_sem = nullptr;
  EXPECT_EQ(MAGMA_STATUS_OK,
            msd_driver->ImportSemaphore(zx::event(duplicate_handle), semaphore->id(), &msd_sem));

  ASSERT_NE(msd_sem, nullptr);

  msd_sem.reset();
  msd_device.reset();
  msd_driver.reset();
}
