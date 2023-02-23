// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.sysinfo/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>

#include "zircon/system/utest/device-enumeration/common.h"

namespace {

enum NucType {
  NUC7i5DNB,
};

std::string NucTypeToString(NucType nuc_type) {
  if (nuc_type == NucType::NUC7i5DNB) {
    return "NUC7i5DNB";
  }

  return "N/A";
}

std::variant<NucType, std::string> GetNucType() {
  zx::result sys_info = component::Connect<fuchsia_sysinfo::SysInfo>();
  EXPECT_EQ(ZX_OK, sys_info.status_value(), "Couldn't connect to SysInfo.");

  const fidl::WireResult result = fidl::WireCall(sys_info.value())->GetBoardName();
  EXPECT_EQ(ZX_OK, result.status(), "Couldn't call GetBoardName.");

  const fidl::WireResponse response = result.value();
  EXPECT_EQ(ZX_OK, response.status, "GetBoardName failed.");

  const std::string_view board_name = response.name.get();
  if (board_name == "NUC7i5DNB") {
    return NucType::NUC7i5DNB;
  }

  return std::string(board_name);
}

bool CheckTestMatch(NucType desired_nuc_type) {
  std::variant nuc_type = GetNucType();
  const std::string* unknown_nuc_type = std::get_if<std::string>(&nuc_type);
  if (unknown_nuc_type) {
    printf("Skipping unknown NUC type: %s", unknown_nuc_type->c_str());
    return false;
  }

  NucType nuc = std::get<NucType>(nuc_type);
  if (nuc != desired_nuc_type) {
    printf("Skipping NUC type: %s", NucTypeToString(nuc).c_str());
    return false;
  }

  return true;
}

TEST_F(DeviceEnumerationTest, Nuc7i5DNBTest) {
  if (!CheckTestMatch(NucType::NUC7i5DNB)) {
    return;
  }

  static const char* kDevicePaths[] = {
      "sys/platform/pt/PCI0/bus/00:02.0_/pci-00:02.0-fidl/intel_i915/intel-gpu-core",
      "sys/platform/pt/PCI0/bus/00:02.0_/pci-00:02.0-fidl/intel_i915/intel-display-controller/display-controller",
      "sys/platform/pt/PCI0/bus/00:14.0_/pci-00:14.0-fidl/xhci/usb-bus",
      "sys/platform/pt/PCI0/bus/00:15.0_/pci-00:15.0-fidl/i2c-bus-9d60",
      "sys/platform/pt/PCI0/bus/00:15.1_/pci-00:15.1-fidl/i2c-bus-9d61",
      "sys/platform/pt/PCI0/bus/00:17.0_/pci-00:17.0-fidl/ahci",
      // TODO(fxbug.dev/84037): Temporarily removed.
      // "pci-00:1f.3-fidl/intel-hda-000",
      // "pci-00:1f.3-fidl/intel-hda-controller",
      "sys/platform/pt/PCI0/bus/00:1f.6_/pci-00:1f.6-fidl/e1000",
  };

  ASSERT_NO_FATAL_FAILURE(TestRunner(kDevicePaths, std::size(kDevicePaths)));
}

}  // namespace
