// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_GRAPHICS_LIB_MAGMA_TESTS_HELPER_PLATFORM_PCI_DEVICE_HELPER_H_
#define SRC_GRAPHICS_LIB_MAGMA_TESTS_HELPER_PLATFORM_PCI_DEVICE_HELPER_H_

#include <memory>

#include "platform_pci_device.h"

class TestPlatformPciDevice {
 public:
  // Return a singleton PlatformPciDevice instance that can be used in tests. This method must be
  // implemented by the MSD.
  static magma::PlatformPciDevice* GetInstance();
};

#endif  // SRC_GRAPHICS_LIB_MAGMA_TESTS_HELPER_PLATFORM_PCI_DEVICE_HELPER_H_
