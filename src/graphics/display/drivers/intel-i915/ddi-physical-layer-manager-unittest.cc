// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/intel-i915/ddi-physical-layer-manager.h"

#include <lib/mmio/mmio-buffer.h>

#include <fake-mmio-reg/fake-mmio-reg.h>
#include <gtest/gtest.h>

#include "src/graphics/display/drivers/intel-i915/ddi-physical-layer.h"
#include "src/graphics/display/drivers/intel-i915/hardware-common.h"
#include "src/graphics/display/drivers/intel-i915/igd.h"
#include "src/graphics/display/drivers/intel-i915/mock-mmio-range.h"

namespace i915_tgl {

namespace {

const std::unordered_map<PowerWellId, PowerWellInfo> kPowerWellInfoTestDevice = {};

// A fake power well implementation used only for tests.
class TestPower : public Power {
 public:
  explicit TestPower(fdf::MmioBuffer* mmio_space) : Power(mmio_space, &kPowerWellInfoTestDevice) {}
  void Resume() override {}

  PowerWellRef GetCdClockPowerWellRef() override { return PowerWellRef(); }
  PowerWellRef GetPipePowerWellRef(PipeId pipe_id) override { return PowerWellRef(); }
  PowerWellRef GetDdiPowerWellRef(DdiId ddi_id) override { return PowerWellRef(); }

  bool GetDdiIoPowerState(DdiId ddi_id) override { return true; }
  void SetDdiIoPowerState(DdiId ddi_id, bool enable) override {}

  bool GetAuxIoPowerState(DdiId ddi_id) override { return true; }
  void SetAuxIoPowerState(DdiId ddi_id, bool enable) override {}

 private:
  void SetPowerWell(PowerWellId power_well, bool enable) override {}

  std::unordered_map<DdiId, bool> aux_state_;
};

// A fake DdiPhysicalLayer that tracks if a DDI has been enabled / disabled.
class TestDdi : public DdiPhysicalLayer {
 public:
  explicit TestDdi(DdiId ddi_id) : DdiPhysicalLayer(ddi_id) {}
  ~TestDdi() override = default;

  bool IsEnabled() const override { return enabled_; }
  bool IsHealthy() const override { return true; }
  bool Enable() override {
    enabled_ = enabled_ || can_enable_;
    return enabled_;
  }
  bool Disable() override {
    enabled_ = enabled_ && !can_disable_;
    return !enabled_;
  }
  PhysicalLayerInfo GetPhysicalLayerInfo() const override { return PhysicalLayerInfo{}; }
  void SetCanEnable(bool result) { can_enable_ = result; }
  void SetCanDisable(bool result) { can_disable_ = result; }

 private:
  bool enabled_ = false;
  bool can_enable_ = true;
  bool can_disable_ = true;
};

// An instance of DdiManager which only creates `TestDdi` for DDIs.
// Used to test interfaces of `DdiManager`.
class TestDdiManager : public DdiManager {
 public:
  TestDdiManager() = default;

  void AddDdi(DdiId ddi_id) { ddi_map()[ddi_id] = std::make_unique<TestDdi>(ddi_id); }

  TestDdi* GetDdi(DdiId ddi_id) {
    if (ddi_map().find(ddi_id) != ddi_map().end()) {
      return static_cast<TestDdi*>(ddi_map().at(ddi_id).get());
    }
    return nullptr;
  }
};

TEST(DdiManager, GetDdiReference_Success) {
  TestDdiManager ddi_manager;
  ddi_manager.AddDdi(DdiId::DDI_A);
  ddi_manager.AddDdi(DdiId::DDI_B);

  {
    EXPECT_FALSE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
    DdiReference ddi_a_reference = ddi_manager.GetDdiReference(DdiId::DDI_A);
    EXPECT_FALSE(ddi_a_reference.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
  }

  // Create and destroy multiple references to a single DDI
  {
    EXPECT_FALSE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
    DdiReference ddi_a_reference_1 = ddi_manager.GetDdiReference(DdiId::DDI_A);
    EXPECT_FALSE(ddi_a_reference_1.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());

    DdiReference ddi_a_reference_2 = ddi_manager.GetDdiReference(DdiId::DDI_A);
    EXPECT_FALSE(ddi_a_reference_2.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());

    ddi_a_reference_1 = {};
    EXPECT_TRUE(ddi_a_reference_1.IsNull());
    EXPECT_FALSE(ddi_a_reference_2.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());

    ddi_a_reference_2 = {};
    EXPECT_FALSE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
  }

  // DdiReference can be move-constructed
  {
    EXPECT_FALSE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
    DdiReference ddi_a_reference_1 = ddi_manager.GetDdiReference(DdiId::DDI_A);
    EXPECT_FALSE(ddi_a_reference_1.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());

    DdiReference ddi_a_reference_2(std::move(ddi_a_reference_1));
    EXPECT_FALSE(ddi_a_reference_2.IsNull());
    EXPECT_TRUE(ddi_a_reference_1.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
  }

  // DdiReference can be move-assigned
  {
    EXPECT_FALSE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
    DdiReference ddi_a_reference = ddi_manager.GetDdiReference(DdiId::DDI_A);
    EXPECT_FALSE(ddi_a_reference.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());

    EXPECT_FALSE(ddi_manager.GetDdi(DdiId::DDI_B)->IsEnabled());
    DdiReference ddi_b_reference = ddi_manager.GetDdiReference(DdiId::DDI_B);
    EXPECT_FALSE(ddi_b_reference.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_B)->IsEnabled());

    ddi_b_reference = std::move(ddi_a_reference);
    EXPECT_TRUE(ddi_a_reference.IsNull());
    EXPECT_FALSE(ddi_b_reference.IsNull());
    EXPECT_TRUE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
    EXPECT_FALSE(ddi_manager.GetDdi(DdiId::DDI_B)->IsEnabled());
  }
}

TEST(DdiManager, GetDdiReference_Failure_UnsupportedDdi) {
  TestDdiManager ddi_manager;
  ddi_manager.AddDdi(DdiId::DDI_A);
  ddi_manager.AddDdi(DdiId::DDI_B);

  EXPECT_DEATH(ddi_manager.GetDdiReference(DdiId::DDI_TC_1), "DDI .+ is not available");
}

TEST(DdiManager, GetDdiReference_Failure_DdiCannotEnable) {
  TestDdiManager ddi_manager;
  ddi_manager.AddDdi(DdiId::DDI_A);
  ddi_manager.AddDdi(DdiId::DDI_B);

  ddi_manager.GetDdi(DdiId::DDI_A)->SetCanEnable(false);
  auto ddi_reference = ddi_manager.GetDdiReference(DdiId::DDI_A);

  EXPECT_TRUE(ddi_reference.IsNull());
  EXPECT_FALSE(ddi_manager.GetDdi(DdiId::DDI_A)->IsEnabled());
}

// For testing purpose only.
// This exposes `ddi_map` from `DdiManagerTigerLake`, so that tests can access
// each `DdiPhysicalLayer` directly to check if it is created correctly.
class DdiManagerTigerLakeForTesting : public DdiManagerTigerLake {
 public:
  DdiManagerTigerLakeForTesting(Power* power, fdf::MmioBuffer* mmio_space,
                                const IgdOpRegion& igd_opregion)
      : DdiManagerTigerLake(power, mmio_space, igd_opregion) {}

  using DdiManagerTigerLake::ddi_map;
};

constexpr int kPhyMiscAOffset = 0x64c00;
constexpr int kPhyMiscBOffset = 0x64c04;

constexpr int kPortClDw5BOffset = 0x6c014;
constexpr int kPortCompDw0BOffset = 0x6c100;
constexpr int kPortCompDw1BOffset = 0x6c104;
constexpr int kPortCompDw3BOffset = 0x6c10c;
constexpr int kPortCompDw8BOffset = 0x6c120;
constexpr int kPortCompDw9BOffset = 0x6c124;
constexpr int kPortCompDw10BOffset = 0x6c128;
constexpr int kPortPcsDw1AuxBOffset = 0x6c304;
constexpr int kPortTxDw8AuxBOffset = 0x6c3a0;
constexpr int kPortPcsDw1Ln0BOffset = 0x6c804;
constexpr int kPortTxDw8Ln0BOffset = 0x6c8a0;
constexpr int kPortPcsDw1Ln1BOffset = 0x6c904;
constexpr int kPortTxDw8Ln1BOffset = 0x6c9a0;
constexpr int kPortPcsDw1Ln2BOffset = 0x6ca04;
constexpr int kPortTxDw8Ln2BOffset = 0x6caa0;
constexpr int kPortPcsDw1Ln3BOffset = 0x6cb04;
constexpr int kPortTxDw8Ln3BOffset = 0x6cba0;

constexpr int kPortClDw5AOffset = 0x162014;
constexpr int kPortCompDw0AOffset = 0x162100;
constexpr int kPortCompDw1AOffset = 0x162104;
constexpr int kPortCompDw3AOffset = 0x16210c;
constexpr int kPortCompDw8AOffset = 0x162120;
constexpr int kPortCompDw9AOffset = 0x162124;
constexpr int kPortCompDw10AOffset = 0x162128;
constexpr int kPortPcsDw1AuxAOffset = 0x162304;
constexpr int kPortTxDw8AuxAOffset = 0x1623a0;
constexpr int kPortPcsDw1Ln0AOffset = 0x162804;
constexpr int kPortTxDw8Ln0AOffset = 0x1628a0;
constexpr int kPortPcsDw1Ln1AOffset = 0x162904;
constexpr int kPortTxDw8Ln1AOffset = 0x1629a0;
constexpr int kPortPcsDw1Ln2AOffset = 0x162a04;
constexpr int kPortTxDw8Ln2AOffset = 0x162aa0;
constexpr int kPortPcsDw1Ln3AOffset = 0x162b04;
constexpr int kPortTxDw8Ln3AOffset = 0x162ba0;

TEST(DdiManagerTigerLake, ParseVbtTable_Dell5420) {
  // On Dell Latitude 5420, there are 4 DDIs:
  // - DDI_A: COMBO eDP port
  // - DDI_B: COMBO HDMI port
  // - DDI_TC_1: Type-C DDI with Type-C port
  // - DDI_TC_2:Type-C DDI with Type-C port

  constexpr static int kMmioRangeSize = 0x200000;
  MockMmioRange mmio_range{kMmioRangeSize, MockMmioRange::Size::k32};
  fdf::MmioBuffer mmio_buffer{mmio_range.GetMmioBuffer()};

  mmio_range.Expect(MockMmioRange::AccessList({
      // Access pattern from ComboDdiTigerLakeTest.InitializeDdiADell5420.
      {.address = kPortCompDw3AOffset, .value = 0xc0606b25},
      {.address = kPortCompDw1AOffset, .value = 0x81000400},
      {.address = kPortCompDw9AOffset, .value = 0x62ab67bb},
      {.address = kPortCompDw10AOffset, .value = 0x51914f96},
      {.address = kPortClDw5AOffset, .value = 0x1204047b},
      {.address = kPortTxDw8AuxAOffset, .value = 0x30037c9c},
      {.address = kPortPcsDw1AuxAOffset, .value = 0x1c300004},
      {.address = kPortTxDw8Ln0AOffset, .value = 0x300335dc},
      {.address = kPortPcsDw1Ln0AOffset, .value = 0x1c300004},
      {.address = kPortTxDw8Ln1AOffset, .value = 0x3003379c},
      {.address = kPortPcsDw1Ln1AOffset, .value = 0x1c300004},
      {.address = kPortTxDw8Ln2AOffset, .value = 0x3003501c},
      {.address = kPortPcsDw1Ln2AOffset, .value = 0x1c300004},
      {.address = kPortTxDw8Ln3AOffset, .value = 0x3003501c},
      {.address = kPortPcsDw1Ln3AOffset, .value = 0x1c300004},
      {.address = kPhyMiscAOffset, .value = 0x23000000},
      {.address = kPortCompDw8AOffset, .value = 0x010d0280},
      {.address = kPortCompDw0AOffset, .value = 0x80005f25},

      // Access pattern from ComboDdiTigerLakeTest.InitializeDdiBDell5420.
      {.address = kPortCompDw3BOffset, .value = 0xc0606b25},
      {.address = kPortCompDw1BOffset, .value = 0x81000400},
      {.address = kPortCompDw9BOffset, .value = 0x62ab67bb},
      {.address = kPortCompDw10BOffset, .value = 0x51914f96},
      {.address = kPortClDw5BOffset, .value = 0x12040478},
      {.address = kPortTxDw8AuxBOffset, .value = 0x3003501c},
      {.address = kPortPcsDw1AuxBOffset, .value = 0x1c300004},
      {.address = kPortTxDw8Ln0BOffset, .value = 0x3003501c},
      {.address = kPortPcsDw1Ln0BOffset, .value = 0x1c300004},
      {.address = kPortTxDw8Ln1BOffset, .value = 0x3003501c},
      {.address = kPortPcsDw1Ln1BOffset, .value = 0x1c300004},
      {.address = kPortTxDw8Ln2BOffset, .value = 0x3003501c},
      {.address = kPortPcsDw1Ln2BOffset, .value = 0x1c300004},
      {.address = kPortTxDw8Ln3BOffset, .value = 0x3003501c},
      {.address = kPortPcsDw1Ln3BOffset, .value = 0x1c300004},
      {.address = kPhyMiscBOffset, .value = 0x23000000},
      {.address = kPortCompDw8BOffset, .value = 0x000d0280},
      {.address = kPortCompDw0BOffset, .value = 0x80005f26},
  }));

  TestPower power(&mmio_buffer);

  IgdOpRegion test_igd_opregion;

  test_igd_opregion.SetIsEdpForTesting(DdiId::DDI_A, true);
  test_igd_opregion.SetSupportsDpForTesting(DdiId::DDI_A, true);
  test_igd_opregion.SetIsTypeCForTesting(DdiId::DDI_A, false);

  test_igd_opregion.SetIsEdpForTesting(DdiId::DDI_B, false);
  test_igd_opregion.SetSupportsDpForTesting(DdiId::DDI_B, false);
  test_igd_opregion.SetIsTypeCForTesting(DdiId::DDI_B, false);

  test_igd_opregion.SetIsEdpForTesting(DdiId::DDI_TC_1, false);
  test_igd_opregion.SetSupportsDpForTesting(DdiId::DDI_TC_1, true);
  test_igd_opregion.SetIsTypeCForTesting(DdiId::DDI_TC_1, true);

  test_igd_opregion.SetIsEdpForTesting(DdiId::DDI_TC_2, false);
  test_igd_opregion.SetSupportsDpForTesting(DdiId::DDI_TC_2, true);
  test_igd_opregion.SetIsTypeCForTesting(DdiId::DDI_TC_2, true);

  DdiManagerTigerLakeForTesting ddi_manager(&power, &mmio_buffer, test_igd_opregion);

  const auto& ddi_map = ddi_manager.ddi_map();

  // Verify if all DDIs are correctly created.
  EXPECT_EQ(ddi_map.size(), 4u);

  EXPECT_NE(ddi_map.find(DdiId::DDI_A), ddi_map.end());
  EXPECT_NE(ddi_map.find(DdiId::DDI_B), ddi_map.end());
  EXPECT_NE(ddi_map.find(DdiId::DDI_TC_1), ddi_map.end());
  EXPECT_NE(ddi_map.find(DdiId::DDI_TC_2), ddi_map.end());

  // Verify DDI physical layer information.
  auto ddi_a_info = ddi_map.at(DdiId::DDI_A)->GetPhysicalLayerInfo();
  EXPECT_EQ(ddi_a_info.ddi_type, DdiPhysicalLayer::DdiType::kCombo);
  EXPECT_EQ(ddi_a_info.connection_type, DdiPhysicalLayer::ConnectionType::kBuiltIn);

  auto ddi_b_info = ddi_map.at(DdiId::DDI_B)->GetPhysicalLayerInfo();
  EXPECT_EQ(ddi_b_info.ddi_type, DdiPhysicalLayer::DdiType::kCombo);
  EXPECT_EQ(ddi_b_info.connection_type, DdiPhysicalLayer::ConnectionType::kBuiltIn);

  auto ddi_tc_1_info = ddi_map.at(DdiId::DDI_TC_1)->GetPhysicalLayerInfo();
  EXPECT_EQ(ddi_tc_1_info.ddi_type, DdiPhysicalLayer::DdiType::kTypeC);
  EXPECT_NE(ddi_tc_1_info.connection_type, DdiPhysicalLayer::ConnectionType::kBuiltIn);

  auto ddi_tc_2_info = ddi_map.at(DdiId::DDI_TC_2)->GetPhysicalLayerInfo();
  EXPECT_EQ(ddi_tc_2_info.ddi_type, DdiPhysicalLayer::DdiType::kTypeC);
  EXPECT_NE(ddi_tc_2_info.connection_type, DdiPhysicalLayer::ConnectionType::kBuiltIn);
}

TEST(DdiManagerTigerLake, ParseVbtTable_NUC11PAHi5) {
  // On Intel NUC11PAHi5, there are 4 DDIs:
  // - DDI_TC_1: Type-C DDI with Type-C port
  // - DDI_TC_3: Type-C DDI with built-in HDMI Port
  // - DDI_TC_4: Type-C DDI with built-in DisplayPort
  // - DDI_TC_6: Type-C DDI with Type-C port

  constexpr static int kMmioRangeSize = 0x200000;
  MockMmioRange mmio_range{kMmioRangeSize, MockMmioRange::Size::k32};
  fdf::MmioBuffer mmio_buffer{mmio_range.GetMmioBuffer()};

  TestPower power(&mmio_buffer);

  IgdOpRegion test_igd_opregion;

  test_igd_opregion.SetIsEdpForTesting(DdiId::DDI_TC_1, false);
  test_igd_opregion.SetSupportsDpForTesting(DdiId::DDI_TC_1, true);
  test_igd_opregion.SetIsTypeCForTesting(DdiId::DDI_TC_1, true);

  test_igd_opregion.SetIsEdpForTesting(DdiId::DDI_TC_3, false);
  test_igd_opregion.SetSupportsDpForTesting(DdiId::DDI_TC_3, false);
  test_igd_opregion.SetIsTypeCForTesting(DdiId::DDI_TC_3, false);

  test_igd_opregion.SetIsEdpForTesting(DdiId::DDI_TC_4, false);
  test_igd_opregion.SetSupportsDpForTesting(DdiId::DDI_TC_4, true);
  test_igd_opregion.SetIsTypeCForTesting(DdiId::DDI_TC_4, false);

  test_igd_opregion.SetIsEdpForTesting(DdiId::DDI_TC_6, false);
  test_igd_opregion.SetSupportsDpForTesting(DdiId::DDI_TC_6, true);
  test_igd_opregion.SetIsTypeCForTesting(DdiId::DDI_TC_6, true);

  DdiManagerTigerLakeForTesting ddi_manager(&power, &mmio_buffer, test_igd_opregion);

  const auto& ddi_map = ddi_manager.ddi_map();

  // Verify if all DDIs are correctly created.
  EXPECT_EQ(ddi_map.size(), 4u);

  EXPECT_NE(ddi_map.find(DdiId::DDI_TC_1), ddi_map.end());
  EXPECT_NE(ddi_map.find(DdiId::DDI_TC_3), ddi_map.end());
  EXPECT_NE(ddi_map.find(DdiId::DDI_TC_4), ddi_map.end());
  EXPECT_NE(ddi_map.find(DdiId::DDI_TC_6), ddi_map.end());

  // Verify DDI physical layer information.
  auto ddi_tc_1_info = ddi_map.at(DdiId::DDI_TC_1)->GetPhysicalLayerInfo();
  EXPECT_EQ(ddi_tc_1_info.ddi_type, DdiPhysicalLayer::DdiType::kTypeC);
  EXPECT_NE(ddi_tc_1_info.connection_type, DdiPhysicalLayer::ConnectionType::kBuiltIn);

  auto ddi_tc_3_info = ddi_map.at(DdiId::DDI_TC_3)->GetPhysicalLayerInfo();
  EXPECT_EQ(ddi_tc_3_info.ddi_type, DdiPhysicalLayer::DdiType::kTypeC);
  EXPECT_EQ(ddi_tc_3_info.connection_type, DdiPhysicalLayer::ConnectionType::kBuiltIn);

  auto ddi_tc_4_info = ddi_map.at(DdiId::DDI_TC_4)->GetPhysicalLayerInfo();
  EXPECT_EQ(ddi_tc_4_info.ddi_type, DdiPhysicalLayer::DdiType::kTypeC);
  EXPECT_EQ(ddi_tc_4_info.connection_type, DdiPhysicalLayer::ConnectionType::kBuiltIn);

  auto ddi_tc_6_info = ddi_map.at(DdiId::DDI_TC_6)->GetPhysicalLayerInfo();
  EXPECT_EQ(ddi_tc_6_info.ddi_type, DdiPhysicalLayer::DdiType::kTypeC);
  EXPECT_NE(ddi_tc_6_info.connection_type, DdiPhysicalLayer::ConnectionType::kBuiltIn);
}

}  // namespace

}  // namespace i915_tgl
