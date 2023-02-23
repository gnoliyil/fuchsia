// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zircon/system/utest/device-enumeration/common.h"

namespace {

TEST_F(DeviceEnumerationTest, PinecrestTest) {
  static const char* kDevicePaths[] = {
      "sys/platform/00:00:1b/sysmem/sysmem-fidl",
      "sys/platform/14:01:1/as370-gpio",
      "sys/platform/00:00:9/dw-i2c",
      "sys/platform/14:01:2/as370-usb-phy",
      "sys/platform/14:01:a/as370-sdhci/sdhci/sdmmc/sdmmc-sdio/sdmmc-sdio-1",
      "sys/platform/14:01:a/as370-sdhci/sdhci/sdmmc/sdmmc-sdio/sdmmc-sdio-2",
      "sys/platform/14:01:11/pinecrest-emmc/as370-sdhci/sdhci/sdmmc/sdmmc-mmc/user/block/part-000",
      "sys/platform/14:01:11/pinecrest-emmc/as370-sdhci/sdhci/sdmmc/sdmmc-mmc/boot1/block",
      "sys/platform/14:01:11/pinecrest-emmc/as370-sdhci/sdhci/sdmmc/sdmmc-mmc/boot2/block",
      "sys/platform/14:01:11/pinecrest-emmc/as370-sdhci/sdhci/sdmmc/sdmmc-mmc/rpmb",
      "sys/platform/14:01:2/as370-usb-phy/dwc2/dwc2-usb/dwc2",
      "sys/platform/00:00:9/dw-i2c/i2c/i2c-0-97/power/as370-power",
      "sys/platform/14:00:8/thermal/as370-thermal",
      "sys/platform/10:02:5/lp5018-light/lp50xx-light",
      "sys/platform/00:00:9/dw-i2c/i2c/i2c-1-55/pinecrest-touch/cy8cmbr3108/hid-device/InputReport",
      "sys/platform/14:01:6/synaptics-dhub/pinecrest-audio-in/as370-audio-in",
      "sys/platform/14:01:6/synaptics-dhub/pinecrest-audio-out/as370-audio-out",
      "sys/platform/14:01:12/pinecrest-nna/as370-nna",
      "sys/platform/14:01:a/as370-sdhci/sdhci/sdmmc/sdmmc-sdio/sdmmc-sdio-1/wifi/nxpfmac_sdio-wlanphy",
      "sys/platform/14:01:1/as370-gpio/gpio-11/pinecrest-buttons/hid-buttons/hidbus_function/hid-device/InputReport",
  };

  ASSERT_NO_FATAL_FAILURE(TestRunner(kDevicePaths, std::size(kDevicePaths)));
  EXPECT_EQ(zx_system_get_num_cpus(), 4);
}

}  // namespace
