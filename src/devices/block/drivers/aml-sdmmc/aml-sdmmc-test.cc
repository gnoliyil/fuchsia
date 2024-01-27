// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-sdmmc.h"

#include <lib/ddk/platform-defs.h>
#include <lib/fake-bti/bti.h>
#include <lib/inspect/testing/cpp/zxtest/inspect.h>
#include <lib/mmio-ptr/fake.h>
#include <lib/mmio/mmio-buffer.h>
#include <lib/sdio/hw.h>
#include <lib/sdmmc/hw.h>
#include <threads.h>

#include <memory>
#include <vector>

#include <soc/aml-s912/s912-hw.h>
#include <zxtest/zxtest.h>

#include "aml-sdmmc-regs.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace sdmmc {

class TestAmlSdmmc : public AmlSdmmc {
 public:
  TestAmlSdmmc(zx_device_t* parent, fdf::MmioBuffer mmio, zx::bti bti)
      // Pass BTI ownership to AmlSdmmc, but keep a copy of the handle so we can get a list of VMOs
      // that are pinned when a request is made.
      : AmlSdmmc(parent, zx::bti(bti.get()), std::move(mmio),
                 aml_sdmmc_config_t{
                     .min_freq = 400000,
                     .max_freq = 120000000,
                     .version_3 = true,
                     .prefs = 0,
                 },
                 zx::interrupt(ZX_HANDLE_INVALID), ddk::GpioProtocolClient()),
        bti_(bti.release()) {}

  using AmlSdmmc::Bind;

  const inspect::Hierarchy* GetInspectRoot(const std::string& suffix) {
    const zx::vmo inspect_vmo = GetInspectVmo();
    if (!inspect_vmo.is_valid()) {
      return nullptr;
    }

    inspector_.ReadInspect(inspect_vmo);
    return inspector_.hierarchy().GetByPath({"aml-sdmmc-port" + suffix});
  }

  void DdkRelease() { AmlSdmmc::DdkRelease(); }

  zx_status_t WaitForInterruptImpl() override {
    fake_bti_pinned_vmo_info_t pinned_vmos[2];
    size_t actual = 0;
    zx_status_t status =
        fake_bti_get_pinned_vmos(bti_->get(), pinned_vmos, std::size(pinned_vmos), &actual);
    // In the tuning case there are exactly two VMOs pinned: one to hold the DMA descriptors, and
    // one to hold the received tuning block. Write the expected tuning data to the second pinned
    // VMO so that the tuning check always passes.
    if (status == ZX_OK && actual == std::size(pinned_vmos) &&
        pinned_vmos[0].size >= sizeof(aml_sdmmc_tuning_blk_pattern_4bit)) {
      zx_vmo_write(pinned_vmos[1].vmo, aml_sdmmc_tuning_blk_pattern_4bit, pinned_vmos[1].offset,
                   sizeof(aml_sdmmc_tuning_blk_pattern_4bit));
    }
    for (size_t i = 0; i < std::min(actual, std::size(pinned_vmos)); i++) {
      zx_handle_close(pinned_vmos[i].vmo);
    }

    if (request_index_ < request_results_.size() && request_results_[request_index_] == 0) {
      // Indicate a receive CRC error.
      mmio_.Write32(1, kAmlSdmmcStatusOffset);

      successful_transfers_ = 0;
      request_index_++;
    } else if (interrupt_status_.has_value()) {
      mmio_.Write32(interrupt_status_.value(), kAmlSdmmcStatusOffset);
    } else {
      // Indicate that the request completed successfully.
      mmio_.Write32(1 << 13, kAmlSdmmcStatusOffset);

      // Each tuning transfer is attempted five times with a short-circuit if one fails.
      // Report every successful transfer five times to make the results arrays easier to
      // follow.
      if (++successful_transfers_ % AML_SDMMC_TUNING_TEST_ATTEMPTS == 0) {
        successful_transfers_ = 0;
        request_index_++;
      }
    }
    return ZX_OK;
  }

  void WaitForBus() const override { /* Do nothing, bus is always ready in tests */
  }

  void SetRequestResults(const char* request_results) {
    request_results_.clear();
    const size_t results_size = strlen(request_results);
    request_results_.reserve(results_size);

    for (size_t i = 0; i < results_size; i++) {
      ASSERT_TRUE((request_results[i] == '|') || (request_results[i] == '-'));
      request_results_.push_back(request_results[i] == '|' ? 1 : 0);
    }

    request_index_ = 0;
  }

  void SetRequestInterruptStatus(uint32_t status) { interrupt_status_ = status; }

  aml_sdmmc_desc_t* descs() { return AmlSdmmc::descs(); }

 private:
  std::vector<uint8_t> request_results_;
  size_t request_index_ = 0;
  uint32_t successful_transfers_ = 0;
  // The optional interrupt status to set after a request is completed.
  std::optional<uint32_t> interrupt_status_;
  inspect::InspectTestHelper inspector_;
  zx::unowned_bti bti_;
};

class AmlSdmmcTest : public zxtest::Test {
 public:
  AmlSdmmcTest() = default;

  void SetUp() override {
    root_ = MockDevice::FakeRootParent();
    registers_.reset(new uint8_t[S912_SD_EMMC_B_LENGTH]);
    memset(registers_.get(), 0, S912_SD_EMMC_B_LENGTH);

    fdf::MmioBuffer mmio_buffer = CreateMmioBufferAndUpdateView(S912_SD_EMMC_B_LENGTH);

    memset(bti_paddrs_, 0, sizeof(bti_paddrs_));
    // This is used by AmlSdmmc::Init() to create the descriptor buffer -- can be any nonzero paddr.
    bti_paddrs_[0] = zx_system_get_page_size();

    zx::bti bti;
    ASSERT_OK(fake_bti_create(bti.reset_and_get_address()));

    dut_ = new TestAmlSdmmc(root_.get(), std::move(mmio_buffer), std::move(bti));
    ASSERT_OK(dut_->Bind());
    ASSERT_EQ(1, root_->child_count());
    mock_dev_ = root_->GetLatestChild();

    dut_->set_board_config({
        .min_freq = 400000,
        .max_freq = 120000000,
        .version_3 = true,
        .prefs = 0,
    });

    mmio_->Write32(0xff, kAmlSdmmcDelay1Offset);
    mmio_->Write32(0xff, kAmlSdmmcDelay2Offset);
    mmio_->Write32(0xff, kAmlSdmmcAdjustOffset);

    dut_->SdmmcHwReset();

    EXPECT_EQ(mmio_->Read32(kAmlSdmmcDelay1Offset), 0);
    EXPECT_EQ(mmio_->Read32(kAmlSdmmcDelay2Offset), 0);
    EXPECT_EQ(mmio_->Read32(kAmlSdmmcAdjustOffset), 0);

    mmio_->Write32(1, kAmlSdmmcCfgOffset);  // Set bus width 4.
  }

 protected:
  fdf::MmioBuffer CreateMmioBufferAndUpdateView(size_t size) {
    zx::vmo mmio_vmo;
    std::optional<fdf::MmioBuffer> mmio;
    ZX_ASSERT(zx::vmo::create(size, 0, &mmio_vmo) == ZX_OK);
    ZX_ASSERT(fdf::MmioBuffer::Create(0, size, std::move(mmio_vmo), ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                      &mmio) == ZX_OK);
    mmio_.emplace(mmio->View(0));
    return std::move(*mmio);
  }

  static zx_koid_t GetVmoKoid(const zx::vmo& vmo) {
    zx_info_handle_basic_t info = {};
    size_t actual = 0;
    size_t available = 0;
    zx_status_t status =
        vmo.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), &actual, &available);
    if (status != ZX_OK || actual < 1) {
      return ZX_KOID_INVALID;
    }
    return info.koid;
  }

  void InitializeContiguousPaddrs(const size_t vmos) {
    ResetDutWithFakeBtiPaddrs();
    // Start at 1 because one paddr has already been read to create the DMA descriptor buffer.
    for (size_t i = 0; i < vmos; i++) {
      bti_paddrs_[i + 1] = (i << 24) | zx_system_get_page_size();
    }
  }

  void InitializeSingleVmoPaddrs(const size_t pages) {
    ResetDutWithFakeBtiPaddrs();
    // Start at 1 because one paddr has already been read to create the DMA descriptor buffer.
    for (size_t i = 0; i < pages; i++) {
      bti_paddrs_[i + 1] = zx_system_get_page_size() * (i + 1);
    }
  }

  void InitializeNonContiguousPaddrs(const size_t vmos) {
    ResetDutWithFakeBtiPaddrs();
    for (size_t i = 0; i < vmos; i++) {
      bti_paddrs_[i + 1] = zx_system_get_page_size() * (i + 1) * 2;
    }
  }

  void ResetDutWithFakeBtiPaddrs() {
    zx::bti bti;
    ASSERT_OK(fake_bti_create_with_paddrs(bti_paddrs_, std::size(bti_paddrs_),
                                          bti.reset_and_get_address()));

    dut_->DdkAsyncRemove();
    mock_dev_->WaitUntilAsyncRemoveCalled();
    mock_ddk::ReleaseFlaggedDevices(dut_->zxdev());
    dut_ = new TestAmlSdmmc(root_.get(), CreateMmioBufferAndUpdateView(S912_SD_EMMC_B_LENGTH),
                            std::move(bti));
    ASSERT_OK(dut_->Bind());
    ASSERT_EQ(1, root_->child_count());
    mock_dev_ = root_->GetLatestChild();
  }

  zx_paddr_t bti_paddrs_[64] = {};

  std::optional<fdf::MmioView> mmio_;
  std::shared_ptr<MockDevice> root_;
  TestAmlSdmmc* dut_;
  MockDevice* mock_dev_;

 private:
  std::unique_ptr<uint8_t[]> registers_;
};

TEST_F(AmlSdmmcTest, DdkLifecycle) {
  dut_->DdkAsyncRemove();
  mock_dev_->WaitUntilAsyncRemoveCalled();
  mock_ddk::ReleaseFlaggedDevices(dut_->zxdev());
}

TEST_F(AmlSdmmcTest, InitV3) {
  dut_->set_board_config({
      .min_freq = 400000,
      .max_freq = 120000000,
      .version_3 = true,
      .prefs = 0,
  });

  AmlSdmmcClock::Get().FromValue(0).WriteTo(&*mmio_);

  ASSERT_OK(dut_->Init({}));

  EXPECT_EQ(AmlSdmmcClock::Get().ReadFrom(&*mmio_).reg_value(), AmlSdmmcClockV3::Get()
                                                                    .FromValue(0)
                                                                    .set_cfg_div(60)
                                                                    .set_cfg_src(0)
                                                                    .set_cfg_co_phase(2)
                                                                    .set_cfg_tx_phase(0)
                                                                    .set_cfg_rx_phase(0)
                                                                    .set_cfg_always_on(1)
                                                                    .reg_value());
}

TEST_F(AmlSdmmcTest, InitV2) {
  dut_->set_board_config({
      .min_freq = 400000,
      .max_freq = 120000000,
      .version_3 = false,
      .prefs = 0,
  });

  AmlSdmmcClock::Get().FromValue(0).WriteTo(&*mmio_);

  ASSERT_OK(dut_->Init({}));

  EXPECT_EQ(AmlSdmmcClock::Get().ReadFrom(&*mmio_).reg_value(), AmlSdmmcClockV2::Get()
                                                                    .FromValue(0)
                                                                    .set_cfg_div(60)
                                                                    .set_cfg_src(0)
                                                                    .set_cfg_co_phase(2)
                                                                    .set_cfg_tx_phase(0)
                                                                    .set_cfg_rx_phase(0)
                                                                    .set_cfg_always_on(1)
                                                                    .reg_value());
}

TEST_F(AmlSdmmcTest, TuningV3) {
  ASSERT_OK(dut_->Init({}));

  AmlSdmmcClock::Get().FromValue(0).set_cfg_div(10).WriteTo(&*mmio_);
  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_bus_width(AmlSdmmcCfg::kBusWidth4Bit).WriteTo(&*mmio_);

  auto adjust = AmlSdmmcAdjust::Get().FromValue(0);
  auto adjust_v2 = AmlSdmmcAdjustV2::Get().FromValue(0);

  adjust.set_adj_fixed(0).set_adj_delay(0x3f).WriteTo(&*mmio_);
  adjust_v2.set_adj_fixed(0).set_adj_delay(0x3f).WriteTo(&*mmio_);

  EXPECT_OK(dut_->SdmmcPerformTuning(SD_SEND_TUNING_BLOCK));

  adjust.ReadFrom(&*mmio_);
  adjust_v2.ReadFrom(&*mmio_);

  EXPECT_EQ(adjust.adj_fixed(), 1);
  EXPECT_EQ(adjust.adj_delay(), 0);
}

TEST_F(AmlSdmmcTest, TuningV2) {
  dut_->set_board_config({
      .min_freq = 400000,
      .max_freq = 120000000,
      .version_3 = false,
      .prefs = 0,
  });

  ASSERT_OK(dut_->Init({}));

  AmlSdmmcClock::Get().FromValue(0).set_cfg_div(10).WriteTo(&*mmio_);
  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_bus_width(AmlSdmmcCfg::kBusWidth4Bit).WriteTo(&*mmio_);

  auto adjust = AmlSdmmcAdjust::Get().FromValue(0);
  auto adjust_v2 = AmlSdmmcAdjustV2::Get().FromValue(0);

  adjust.set_adj_fixed(0).set_adj_delay(0x3f).WriteTo(&*mmio_);
  adjust_v2.set_adj_fixed(0).set_adj_delay(0x3f).WriteTo(&*mmio_);

  EXPECT_OK(dut_->SdmmcPerformTuning(SD_SEND_TUNING_BLOCK));

  adjust.ReadFrom(&*mmio_);
  adjust_v2.ReadFrom(&*mmio_);

  EXPECT_EQ(adjust_v2.adj_fixed(), 1);
  EXPECT_EQ(adjust_v2.adj_delay(), 0);
}

TEST_F(AmlSdmmcTest, DelayLineTuningAllPass) {
  dut_->set_board_config({
      .min_freq = 400000,
      .max_freq = 120000000,
      .version_3 = true,
      .prefs = 0,
  });
  ASSERT_OK(dut_->Init({}));

  AmlSdmmcClock::Get().FromValue(0).set_cfg_div(10).WriteTo(&*mmio_);
  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_bus_width(AmlSdmmcCfg::kBusWidth4Bit).WriteTo(&*mmio_);

  auto adjust = AmlSdmmcAdjust::Get().FromValue(0).set_adj_delay(0x3f).WriteTo(&*mmio_);
  auto delay1 = AmlSdmmcDelay1::Get().FromValue(0).WriteTo(&*mmio_);
  auto delay2 = AmlSdmmcDelay2::Get().FromValue(0).WriteTo(&*mmio_);

  EXPECT_OK(dut_->SdmmcPerformTuning(SD_SEND_TUNING_BLOCK));

  adjust.ReadFrom(&*mmio_);
  delay1.ReadFrom(&*mmio_);
  delay2.ReadFrom(&*mmio_);

  // No failing window was found, so the default settings should be used.
  EXPECT_EQ(adjust.adj_delay(), 0);
  EXPECT_EQ(delay1.dly_0(), 0);
  EXPECT_EQ(delay1.dly_1(), 0);
  EXPECT_EQ(delay1.dly_2(), 0);
  EXPECT_EQ(delay1.dly_3(), 0);
  EXPECT_EQ(delay1.dly_4(), 0);
  EXPECT_EQ(delay2.dly_5(), 0);
  EXPECT_EQ(delay2.dly_6(), 0);
  EXPECT_EQ(delay2.dly_7(), 0);
  EXPECT_EQ(delay2.dly_8(), 0);
  EXPECT_EQ(delay2.dly_9(), 0);

  const auto* root = dut_->GetInspectRoot("-unknown");
  ASSERT_NOT_NULL(root);

  const auto* adj_delay = root->node().get_property<inspect::UintPropertyValue>("adj_delay");
  ASSERT_NOT_NULL(adj_delay);
  EXPECT_EQ(adj_delay->value(), 0);

  const auto* delay_lines = root->node().get_property<inspect::UintPropertyValue>("delay_lines");
  ASSERT_NOT_NULL(delay_lines);
  EXPECT_EQ(delay_lines->value(), 0);

  const auto* tuning_results = root->GetByPath({"tuning_results_adj_delay_0"})
                                   ->node()
                                   .get_property<inspect::StringPropertyValue>("tuning_results");
  ASSERT_NOT_NULL(tuning_results);
  EXPECT_STREQ(tuning_results->value(),
               "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||");

  const auto* distance =
      root->node().get_property<inspect::UintPropertyValue>("distance_to_failing_point");
  ASSERT_NOT_NULL(distance);
  EXPECT_EQ(distance->value(), 63);
}

TEST_F(AmlSdmmcTest, DelayLineTuningFailingPoint) {
  dut_->SetRequestResults(
      "-----------|||||||||||||||||||||||||||||||||||||||||||||||||----"
      "-------------------------------|||||||||||||||||||||||||||||||||"
      "||||||||||-----------------------------------------|||||||||||||"
      "||||||||||||||||||||||||||||||----------------------------------"
      "||||||||||||||||||||||||||||||||||||||||||||||||||--------------"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "-----------|||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "-------------------------------|||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||-------------------------------|||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||------------------------");

  dut_->set_board_config({
      .min_freq = 400000,
      .max_freq = 120000000,
      .version_3 = true,
      .prefs = 0,
  });
  ASSERT_OK(dut_->Init({}));

  AmlSdmmcClock::Get().FromValue(0).set_cfg_div(10).WriteTo(&*mmio_);
  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_bus_width(AmlSdmmcCfg::kBusWidth4Bit).WriteTo(&*mmio_);

  auto adjust = AmlSdmmcAdjust::Get().FromValue(0).set_adj_delay(0x3f).WriteTo(&*mmio_);
  auto delay1 = AmlSdmmcDelay1::Get().FromValue(0).WriteTo(&*mmio_);
  auto delay2 = AmlSdmmcDelay2::Get().FromValue(0).WriteTo(&*mmio_);

  EXPECT_OK(dut_->SdmmcPerformTuning(SD_SEND_TUNING_BLOCK));

  adjust.ReadFrom(&*mmio_);
  delay1.ReadFrom(&*mmio_);
  delay2.ReadFrom(&*mmio_);

  EXPECT_EQ(adjust.adj_delay(), 7);
  EXPECT_EQ(delay1.dly_0(), 30);
  EXPECT_EQ(delay1.dly_1(), 30);
  EXPECT_EQ(delay1.dly_2(), 30);
  EXPECT_EQ(delay1.dly_3(), 30);
  EXPECT_EQ(delay1.dly_4(), 30);
  EXPECT_EQ(delay2.dly_5(), 30);
  EXPECT_EQ(delay2.dly_6(), 30);
  EXPECT_EQ(delay2.dly_7(), 30);
  EXPECT_EQ(delay2.dly_8(), 30);
  EXPECT_EQ(delay2.dly_9(), 30);

  const auto* root = dut_->GetInspectRoot("-unknown");
  ASSERT_NOT_NULL(root);

  const auto* adj_delay = root->node().get_property<inspect::UintPropertyValue>("adj_delay");
  ASSERT_NOT_NULL(adj_delay);
  EXPECT_EQ(adj_delay->value(), 7);

  const auto* delay_lines = root->node().get_property<inspect::UintPropertyValue>("delay_lines");
  ASSERT_NOT_NULL(delay_lines);
  EXPECT_EQ(delay_lines->value(), 30);

  const auto* tuning_results = root->GetByPath({"tuning_results_adj_delay_7"})
                                   ->node()
                                   .get_property<inspect::StringPropertyValue>("tuning_results");
  ASSERT_NOT_NULL(tuning_results);
  EXPECT_STREQ(tuning_results->value(),
               "-------------------------------|||||||||||||||||||||||||||||||||");

  const auto* distance =
      root->node().get_property<inspect::UintPropertyValue>("distance_to_failing_point");
  ASSERT_NOT_NULL(distance);
  EXPECT_EQ(distance->value(), 0);
}

TEST_F(AmlSdmmcTest, DelayLineTuningEvenDivider) {
  dut_->SetRequestResults(
      // Largest failing window: adj_delay 8, middle delay 25
      "||||||||||||||||||||||||||||||||||||||||||||||||||--------------"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "-|||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "---------------------|||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||-------------------------------|||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||-------------------------------|||");

  dut_->set_board_config({
      .min_freq = 400000,
      .max_freq = 120000000,
      .version_3 = true,
      .prefs = 0,
  });
  ASSERT_OK(dut_->Init({}));

  AmlSdmmcClock::Get().FromValue(0).set_cfg_div(10).WriteTo(&*mmio_);
  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_bus_width(AmlSdmmcCfg::kBusWidth4Bit).WriteTo(&*mmio_);

  auto adjust = AmlSdmmcAdjust::Get().FromValue(0).set_adj_delay(0x3f).WriteTo(&*mmio_);
  auto delay1 = AmlSdmmcDelay1::Get().FromValue(0).WriteTo(&*mmio_);
  auto delay2 = AmlSdmmcDelay2::Get().FromValue(0).WriteTo(&*mmio_);

  EXPECT_OK(dut_->SdmmcPerformTuning(SD_SEND_TUNING_BLOCK));

  adjust.ReadFrom(&*mmio_);
  delay1.ReadFrom(&*mmio_);
  delay2.ReadFrom(&*mmio_);

  EXPECT_EQ(adjust.adj_delay(), 3);
  EXPECT_EQ(delay1.dly_0(), 25);
  EXPECT_EQ(delay1.dly_1(), 25);
  EXPECT_EQ(delay1.dly_2(), 25);
  EXPECT_EQ(delay1.dly_3(), 25);
  EXPECT_EQ(delay1.dly_4(), 25);
  EXPECT_EQ(delay2.dly_5(), 25);
  EXPECT_EQ(delay2.dly_6(), 25);
  EXPECT_EQ(delay2.dly_7(), 25);
  EXPECT_EQ(delay2.dly_8(), 25);
  EXPECT_EQ(delay2.dly_9(), 25);

  const auto* root = dut_->GetInspectRoot("-unknown");
  ASSERT_NOT_NULL(root);

  const auto* adj_delay = root->node().get_property<inspect::UintPropertyValue>("adj_delay");
  ASSERT_NOT_NULL(adj_delay);
  EXPECT_EQ(adj_delay->value(), 3);

  const auto* delay_lines = root->node().get_property<inspect::UintPropertyValue>("delay_lines");
  ASSERT_NOT_NULL(delay_lines);
  EXPECT_EQ(delay_lines->value(), 25);

  const auto* tuning_results = root->GetByPath({"tuning_results_adj_delay_4"})
                                   ->node()
                                   .get_property<inspect::StringPropertyValue>("tuning_results");
  ASSERT_NOT_NULL(tuning_results);
  EXPECT_STREQ(tuning_results->value(),
               "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||");

  const auto* distance =
      root->node().get_property<inspect::UintPropertyValue>("distance_to_failing_point");
  ASSERT_NOT_NULL(distance);
  EXPECT_EQ(distance->value(), 63);
}

TEST_F(AmlSdmmcTest, DelayLineTuningOddDivider) {
  dut_->SetRequestResults(
      // Largest failing window: adj_delay 3, first delay 0
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "-----------|||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "-------------------------------|||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||-------------------------------|||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||------------------------"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||----"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||");

  dut_->set_board_config({
      .min_freq = 400000,
      .max_freq = 120000000,
      .version_3 = true,
      .prefs = 0,
  });
  ASSERT_OK(dut_->Init({}));

  AmlSdmmcClock::Get().FromValue(0).set_cfg_div(9).WriteTo(&*mmio_);
  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_bus_width(AmlSdmmcCfg::kBusWidth4Bit).WriteTo(&*mmio_);

  auto adjust = AmlSdmmcAdjust::Get().FromValue(0).set_adj_delay(0x3f).WriteTo(&*mmio_);
  auto delay1 = AmlSdmmcDelay1::Get().FromValue(0).WriteTo(&*mmio_);
  auto delay2 = AmlSdmmcDelay2::Get().FromValue(0).WriteTo(&*mmio_);

  EXPECT_OK(dut_->SdmmcPerformTuning(SD_SEND_TUNING_BLOCK));

  adjust.ReadFrom(&*mmio_);
  delay1.ReadFrom(&*mmio_);
  delay2.ReadFrom(&*mmio_);

  EXPECT_EQ(adjust.adj_delay(), 7);
  EXPECT_EQ(delay1.dly_0(), 0);
  EXPECT_EQ(delay1.dly_1(), 0);
  EXPECT_EQ(delay1.dly_2(), 0);
  EXPECT_EQ(delay1.dly_3(), 0);
  EXPECT_EQ(delay1.dly_4(), 0);
  EXPECT_EQ(delay2.dly_5(), 0);
  EXPECT_EQ(delay2.dly_6(), 0);
  EXPECT_EQ(delay2.dly_7(), 0);
  EXPECT_EQ(delay2.dly_8(), 0);
  EXPECT_EQ(delay2.dly_9(), 0);

  const auto* root = dut_->GetInspectRoot("-unknown");
  ASSERT_NOT_NULL(root);

  const auto* adj_delay = root->node().get_property<inspect::UintPropertyValue>("adj_delay");
  ASSERT_NOT_NULL(adj_delay);
  EXPECT_EQ(adj_delay->value(), 7);

  const auto* delay_lines = root->node().get_property<inspect::UintPropertyValue>("delay_lines");
  ASSERT_NOT_NULL(delay_lines);
  EXPECT_EQ(delay_lines->value(), 0);

  const auto* tuning_results = root->GetByPath({"tuning_results_adj_delay_7"})
                                   ->node()
                                   .get_property<inspect::StringPropertyValue>("tuning_results");
  ASSERT_NOT_NULL(tuning_results);
  EXPECT_STREQ(tuning_results->value(),
               "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||");

  const auto* distance =
      root->node().get_property<inspect::UintPropertyValue>("distance_to_failing_point");
  ASSERT_NOT_NULL(distance);
  EXPECT_EQ(distance->value(), 63);
}

TEST_F(AmlSdmmcTest, DelayLineTuningCorrectFailingWindowIfLastOne) {
  dut_->SetRequestResults(
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||----"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||"
      "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||");

  dut_->set_board_config({
      .min_freq = 400000,
      .max_freq = 120000000,
      .version_3 = true,
      .prefs = 0,
  });
  ASSERT_OK(dut_->Init({}));

  AmlSdmmcClock::Get().FromValue(0).set_cfg_div(5).WriteTo(&*mmio_);
  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_bus_width(AmlSdmmcCfg::kBusWidth4Bit).WriteTo(&*mmio_);

  auto adjust = AmlSdmmcAdjust::Get().FromValue(0).set_adj_delay(0x3f).WriteTo(&*mmio_);
  auto delay1 = AmlSdmmcDelay1::Get().FromValue(0).WriteTo(&*mmio_);
  auto delay2 = AmlSdmmcDelay2::Get().FromValue(0).WriteTo(&*mmio_);

  EXPECT_OK(dut_->SdmmcPerformTuning(SD_SEND_TUNING_BLOCK));

  adjust.ReadFrom(&*mmio_);
  delay1.ReadFrom(&*mmio_);
  delay2.ReadFrom(&*mmio_);

  EXPECT_EQ(adjust.adj_delay(), 2);
  EXPECT_EQ(delay1.dly_0(), 60);
  EXPECT_EQ(delay1.dly_1(), 60);
  EXPECT_EQ(delay1.dly_2(), 60);
  EXPECT_EQ(delay1.dly_3(), 60);
  EXPECT_EQ(delay1.dly_4(), 60);
  EXPECT_EQ(delay2.dly_5(), 60);
  EXPECT_EQ(delay2.dly_6(), 60);
  EXPECT_EQ(delay2.dly_7(), 60);
  EXPECT_EQ(delay2.dly_8(), 60);
  EXPECT_EQ(delay2.dly_9(), 60);

  const auto* root = dut_->GetInspectRoot("-unknown");
  ASSERT_NOT_NULL(root);

  const auto* adj_delay = root->node().get_property<inspect::UintPropertyValue>("adj_delay");
  ASSERT_NOT_NULL(adj_delay);
  EXPECT_EQ(adj_delay->value(), 2);

  const auto* delay_lines = root->node().get_property<inspect::UintPropertyValue>("delay_lines");
  ASSERT_NOT_NULL(delay_lines);
  EXPECT_EQ(delay_lines->value(), 60);
}

TEST_F(AmlSdmmcTest, SetBusFreq) {
  ASSERT_OK(dut_->Init({}));
  {
    const auto* root = dut_->GetInspectRoot("-unknown");
    ASSERT_NOT_NULL(root);

    const auto* bus_freq =
        root->node().get_property<inspect::UintPropertyValue>("bus_clock_frequency");
    ASSERT_NOT_NULL(bus_freq);
    EXPECT_EQ(bus_freq->value(), 400'000);
  }

  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_bus_width(AmlSdmmcCfg::kBusWidth4Bit).WriteTo(&*mmio_);

  auto clock = AmlSdmmcClock::Get().FromValue(0).WriteTo(&*mmio_);

  EXPECT_OK(dut_->SdmmcSetBusFreq(100'000'000));
  EXPECT_EQ(clock.ReadFrom(&*mmio_).cfg_div(), 10);
  EXPECT_EQ(clock.cfg_src(), 1);

  {
    const auto* root = dut_->GetInspectRoot("-unknown");
    ASSERT_NOT_NULL(root);

    const auto* bus_freq =
        root->node().get_property<inspect::UintPropertyValue>("bus_clock_frequency");
    ASSERT_NOT_NULL(bus_freq);
    EXPECT_EQ(bus_freq->value(), 100'000'000);
  }

  // Greater than the max, will be capped at 120 MHz.
  EXPECT_OK(dut_->SdmmcSetBusFreq(200'000'000));
  EXPECT_EQ(clock.ReadFrom(&*mmio_).cfg_div(), 9);
  EXPECT_EQ(clock.cfg_src(), 1);

  {
    const auto* root = dut_->GetInspectRoot("-unknown");
    ASSERT_NOT_NULL(root);

    const auto* bus_freq =
        root->node().get_property<inspect::UintPropertyValue>("bus_clock_frequency");
    ASSERT_NOT_NULL(bus_freq);
    EXPECT_EQ(bus_freq->value(), 111'111'111);
  }

  EXPECT_OK(dut_->SdmmcSetBusFreq(0));
  EXPECT_EQ(clock.ReadFrom(&*mmio_).cfg_div(), 0);

  {
    const auto* root = dut_->GetInspectRoot("-unknown");
    ASSERT_NOT_NULL(root);

    const auto* bus_freq =
        root->node().get_property<inspect::UintPropertyValue>("bus_clock_frequency");
    ASSERT_NOT_NULL(bus_freq);
    EXPECT_EQ(bus_freq->value(), 0);
  }

  EXPECT_OK(dut_->SdmmcSetBusFreq(54'000'000));
  EXPECT_EQ(clock.ReadFrom(&*mmio_).cfg_div(), 19);
  EXPECT_EQ(clock.cfg_src(), 1);

  {
    const auto* root = dut_->GetInspectRoot("-unknown");
    ASSERT_NOT_NULL(root);

    const auto* bus_freq =
        root->node().get_property<inspect::UintPropertyValue>("bus_clock_frequency");
    ASSERT_NOT_NULL(bus_freq);
    EXPECT_EQ(bus_freq->value(), 52'631'578);
  }

  EXPECT_OK(dut_->SdmmcSetBusFreq(400'000));
  EXPECT_EQ(clock.ReadFrom(&*mmio_).cfg_div(), 60);
  EXPECT_EQ(clock.cfg_src(), 0);

  {
    const auto* root = dut_->GetInspectRoot("-unknown");
    ASSERT_NOT_NULL(root);

    const auto* bus_freq =
        root->node().get_property<inspect::UintPropertyValue>("bus_clock_frequency");
    ASSERT_NOT_NULL(bus_freq);
    EXPECT_EQ(bus_freq->value(), 400'000);
  }
}

TEST_F(AmlSdmmcTest, ClearStatus) {
  ASSERT_OK(dut_->Init({}));

  // Set end_of_chain to indicate we're done and to have something to clear
  dut_->SetRequestInterruptStatus(1 << 13);
  sdmmc_req_t request;
  memset(&request, 0, sizeof(request));
  uint32_t unused_response[4];
  EXPECT_OK(dut_->SdmmcRequest(&request, unused_response));

  auto status = AmlSdmmcStatus::Get().FromValue(0);
  EXPECT_EQ(AmlSdmmcStatus::kClearStatus, status.ReadFrom(&*mmio_).reg_value());
}

TEST_F(AmlSdmmcTest, TxCrcError) {
  ASSERT_OK(dut_->Init({}));

  // Set TX CRC error bit (8) and desc_busy bit (30)
  dut_->SetRequestInterruptStatus(1 << 8 | 1 << 30);
  sdmmc_req_t request;
  memset(&request, 0, sizeof(request));
  uint32_t unused_response[4];
  EXPECT_EQ(ZX_ERR_IO_DATA_INTEGRITY, dut_->SdmmcRequest(&request, unused_response));

  auto start = AmlSdmmcStart::Get().FromValue(0);
  // The desc busy bit should now have been cleared because of the error
  EXPECT_EQ(0, start.ReadFrom(&*mmio_).desc_busy());
}

TEST_F(AmlSdmmcTest, RequestsFailAfterSuspend) {
  ASSERT_OK(dut_->Init({}));

  sdmmc_req_t request;
  memset(&request, 0, sizeof(request));
  uint32_t unused_response[4];
  EXPECT_OK(dut_->SdmmcRequest(&request, unused_response));

  ddk::SuspendTxn txn(dut_->zxdev(), 0, false, 0);
  dut_->DdkSuspend(std::move(txn));
  mock_dev_->WaitUntilSuspendReplyCalled();

  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, unused_response));
}

TEST_F(AmlSdmmcTest, UnownedVmosBlockMode) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmos[10] = {};
  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < std::size(vmos); i++) {
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmos[i]));
    buffers[i] = {
        .buffer =
            {
                .vmo = vmos[i].get(),
            },
        .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
        .offset = i * 16,
        .size = 32 * (i + 2),
    };
  }

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(2)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size());
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < std::size(vmos); i++) {
    expected_desc_cfg.set_len(i + 2).set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == std::size(vmos) - 1) {
      expected_desc_cfg.set_end_of_chain(1);
    }
    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, (i << 24) | (zx_system_get_page_size() + (i * 16)));
    EXPECT_EQ(descs[i].resp_addr, 0);
  }
}

TEST_F(AmlSdmmcTest, UnownedVmosNotBlockSizeMultiple) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmos[10] = {};
  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < std::size(vmos); i++) {
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmos[i]));
    buffers[i] = {
        .buffer =
            {
                .vmo = vmos[i].get(),
            },
        .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
        .offset = 0,
        .size = 32 * (i + 2),
    };
  }

  buffers[5].size = 25;

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, UnownedVmosByteMode) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmos[10] = {};
  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < std::size(vmos); i++) {
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmos[i]));
    buffers[i] = {
        .buffer =
            {
                .vmo = vmos[i].get(),
            },
        .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
        .offset = i * 4,
        .size = 50,
    };
  }

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 50,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(50)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size());
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < std::size(vmos); i++) {
    expected_desc_cfg.set_len(50).set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == std::size(vmos) - 1) {
      expected_desc_cfg.set_end_of_chain(1);
    }
    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, (i << 24) | (zx_system_get_page_size() + (i * 4)));
    EXPECT_EQ(descs[i].resp_addr, 0);
  }
}

TEST_F(AmlSdmmcTest, UnownedVmoByteModeMultiBlock) {
  InitializeContiguousPaddrs(1);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo = vmo.get(),
          },
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 0,
      .size = 400,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 100,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(100)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size());
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < 4; i++) {
    expected_desc_cfg.set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == 3) {
      expected_desc_cfg.set_end_of_chain(1);
    }
    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, zx_system_get_page_size() + (i * 100));
    EXPECT_EQ(descs[i].resp_addr, 0);
  }
}

TEST_F(AmlSdmmcTest, UnownedVmoOffsetNotAligned) {
  InitializeContiguousPaddrs(1);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo = vmo.get(),
          },
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 3,
      .size = 64,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, UnownedVmoSingleBufferMultipleDescriptors) {
  const size_t pages = ((32 * 514) / zx_system_get_page_size()) + 1;
  InitializeSingleVmoPaddrs(pages);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(pages * zx_system_get_page_size(), 0, &vmo));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo = vmo.get(),
          },
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 16,
      .size = 32 * 513,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(511)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size() + 16);
  EXPECT_EQ(descs[0].resp_addr, 0);

  expected_desc_cfg.set_len(2)
      .set_end_of_chain(1)
      .set_no_resp(1)
      .set_no_cmd(1)
      .set_resp_num(0)
      .set_cmd_idx(0);

  EXPECT_EQ(descs[1].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[1].cmd_arg, 0);
  EXPECT_EQ(descs[1].data_addr, zx_system_get_page_size() + (511 * 32) + 16);
  EXPECT_EQ(descs[1].resp_addr, 0);
}

TEST_F(AmlSdmmcTest, UnownedVmoSingleBufferNotPageAligned) {
  const size_t pages = ((32 * 514) / zx_system_get_page_size()) + 1;
  InitializeNonContiguousPaddrs(pages);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(pages * zx_system_get_page_size(), 0, &vmo));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo = vmo.get(),
          },
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 16,
      .size = 32 * 513,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, UnownedVmoSingleBufferPageAligned) {
  const size_t pages = ((32 * 514) / zx_system_get_page_size()) + 1;
  InitializeNonContiguousPaddrs(pages);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(pages * zx_system_get_page_size(), 0, &vmo));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo = vmo.get(),
          },
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 32,
      .size = 32 * 513,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(127)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, (zx_system_get_page_size() * 2) + 32);
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < 5; i++) {
    expected_desc_cfg.set_len(128).set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == 4) {
      expected_desc_cfg.set_len(2).set_end_of_chain(1);
    }

    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, zx_system_get_page_size() * (i + 1) * 2);
    EXPECT_EQ(descs[i].resp_addr, 0);
  }
}

TEST_F(AmlSdmmcTest, OwnedVmosBlockMode) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < std::size(buffers); i++) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
    EXPECT_OK(dut_->SdmmcRegisterVmo(i, 0, std::move(vmo), i * 64, 512, SDMMC_VMO_RIGHT_WRITE));
    buffers[i] = {
        .buffer =
            {
                .vmo_id = i,
            },
        .type = SDMMC_BUFFER_TYPE_VMO_ID,
        .offset = i * 16,
        .size = 32 * (i + 2),
    };
  }

  zx::vmo vmo;
  EXPECT_NOT_OK(dut_->SdmmcUnregisterVmo(3, 1, &vmo));

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(2)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size());
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < std::size(buffers); i++) {
    expected_desc_cfg.set_len(i + 2).set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == std::size(buffers) - 1) {
      expected_desc_cfg.set_end_of_chain(1);
    }
    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, (i << 24) | (zx_system_get_page_size() + (i * 80)));
    EXPECT_EQ(descs[i].resp_addr, 0);
  }

  request.client_id = 7;
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));

  EXPECT_OK(dut_->SdmmcUnregisterVmo(3, 0, &vmo));
  EXPECT_NOT_OK(dut_->SdmmcRegisterVmo(2, 0, std::move(vmo), 0, 512, SDMMC_VMO_RIGHT_WRITE));

  request.client_id = 0;
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, OwnedVmosNotBlockSizeMultiple) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < std::size(buffers); i++) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
    EXPECT_OK(dut_->SdmmcRegisterVmo(i, 0, std::move(vmo), i * 64, 512, SDMMC_VMO_RIGHT_WRITE));
    buffers[i] = {
        .buffer =
            {
                .vmo_id = i,
            },
        .type = SDMMC_BUFFER_TYPE_VMO_ID,
        .offset = 0,
        .size = 32 * (i + 2),
    };
  }

  buffers[5].size = 25;

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, OwnedVmosByteMode) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < std::size(buffers); i++) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
    EXPECT_OK(dut_->SdmmcRegisterVmo(i, 0, std::move(vmo), i * 64, 512, SDMMC_VMO_RIGHT_WRITE));
    buffers[i] = {
        .buffer =
            {
                .vmo_id = i,
            },
        .type = SDMMC_BUFFER_TYPE_VMO_ID,
        .offset = i * 4,
        .size = 50,
    };
  }

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 50,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(50)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size());
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < std::size(buffers); i++) {
    expected_desc_cfg.set_len(50).set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == std::size(buffers) - 1) {
      expected_desc_cfg.set_end_of_chain(1);
    }
    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, (i << 24) | (zx_system_get_page_size() + (i * 68)));
    EXPECT_EQ(descs[i].resp_addr, 0);
  }
}

TEST_F(AmlSdmmcTest, OwnedVmoByteModeMultiBlock) {
  InitializeContiguousPaddrs(1);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
  EXPECT_OK(dut_->SdmmcRegisterVmo(1, 0, std::move(vmo), 0, 512, SDMMC_VMO_RIGHT_WRITE));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo_id = 1,
          },
      .type = SDMMC_BUFFER_TYPE_VMO_ID,
      .offset = 0,
      .size = 400,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 100,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(100)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size());
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < 4; i++) {
    expected_desc_cfg.set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == 3) {
      expected_desc_cfg.set_end_of_chain(1);
    }
    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, zx_system_get_page_size() + (i * 100));
    EXPECT_EQ(descs[i].resp_addr, 0);
  }
}

TEST_F(AmlSdmmcTest, OwnedVmoOffsetNotAligned) {
  InitializeContiguousPaddrs(1);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
  EXPECT_OK(dut_->SdmmcRegisterVmo(1, 0, std::move(vmo), 2, 512, SDMMC_VMO_RIGHT_WRITE));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo_id = 1,
          },
      .type = SDMMC_BUFFER_TYPE_VMO_ID,
      .offset = 32,
      .size = 64,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, OwnedVmoSingleBufferMultipleDescriptors) {
  const size_t pages = ((32 * 514) / zx_system_get_page_size()) + 1;
  InitializeSingleVmoPaddrs(pages);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(pages * zx_system_get_page_size(), 0, &vmo));
  EXPECT_OK(dut_->SdmmcRegisterVmo(1, 0, std::move(vmo), 8, (pages * zx_system_get_page_size()) - 8,
                                   SDMMC_VMO_RIGHT_WRITE));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo_id = 1,
          },
      .type = SDMMC_BUFFER_TYPE_VMO_ID,
      .offset = 8,
      .size = 32 * 513,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(511)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size() + 16);
  EXPECT_EQ(descs[0].resp_addr, 0);

  expected_desc_cfg.set_len(1)
      .set_len(2)
      .set_end_of_chain(1)
      .set_no_resp(1)
      .set_no_cmd(1)
      .set_resp_num(0)
      .set_cmd_idx(0);

  EXPECT_EQ(descs[1].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[1].cmd_arg, 0);
  EXPECT_EQ(descs[1].data_addr, zx_system_get_page_size() + (511 * 32) + 16);
  EXPECT_EQ(descs[1].resp_addr, 0);
}

TEST_F(AmlSdmmcTest, OwnedVmoSingleBufferNotPageAligned) {
  const size_t pages = ((32 * 514) / zx_system_get_page_size()) + 1;
  InitializeNonContiguousPaddrs(pages);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(pages * zx_system_get_page_size(), 0, &vmo));
  EXPECT_OK(dut_->SdmmcRegisterVmo(1, 0, std::move(vmo), 8, (pages * zx_system_get_page_size()) - 8,
                                   SDMMC_VMO_RIGHT_WRITE));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo = 1,
          },
      .type = SDMMC_BUFFER_TYPE_VMO_ID,
      .offset = 8,
      .size = 32 * 513,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, OwnedVmoSingleBufferPageAligned) {
  const size_t pages = ((32 * 514) / zx_system_get_page_size()) + 1;
  InitializeNonContiguousPaddrs(pages);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(pages * zx_system_get_page_size(), 0, &vmo));
  EXPECT_OK(dut_->SdmmcRegisterVmo(
      1, 0, std::move(vmo), 16, (pages * zx_system_get_page_size()) - 16, SDMMC_VMO_RIGHT_WRITE));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo = 1,
          },
      .type = SDMMC_BUFFER_TYPE_VMO_ID,
      .offset = 16,
      .size = 32 * 513,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(127)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, (zx_system_get_page_size() * 2) + 32);
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < 5; i++) {
    expected_desc_cfg.set_len(128).set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == 4) {
      expected_desc_cfg.set_len(2).set_end_of_chain(1);
    }

    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, zx_system_get_page_size() * (i + 1) * 2);
    EXPECT_EQ(descs[i].resp_addr, 0);
  }
}

TEST_F(AmlSdmmcTest, OwnedVmoWritePastEnd) {
  const size_t pages = ((32 * 514) / zx_system_get_page_size()) + 1;
  InitializeNonContiguousPaddrs(pages);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(pages * zx_system_get_page_size(), 0, &vmo));
  EXPECT_OK(dut_->SdmmcRegisterVmo(1, 0, std::move(vmo), 32, 32 * 384, SDMMC_VMO_RIGHT_WRITE));

  sdmmc_buffer_region_t buffer = {
      .buffer =
          {
              .vmo = 1,
          },
      .type = SDMMC_BUFFER_TYPE_VMO_ID,
      .offset = 32,
      .size = 32 * 383,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(126)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, (zx_system_get_page_size() * 2) + 64);
  EXPECT_EQ(descs[0].resp_addr, 0);

  for (uint32_t i = 1; i < 4; i++) {
    expected_desc_cfg.set_len(128).set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
    if (i == 3) {
      expected_desc_cfg.set_len(1).set_end_of_chain(1);
    }

    EXPECT_EQ(descs[i].cmd_info, expected_desc_cfg.reg_value());
    EXPECT_EQ(descs[i].cmd_arg, 0);
    EXPECT_EQ(descs[i].data_addr, zx_system_get_page_size() * (i + 1) * 2);
    EXPECT_EQ(descs[i].resp_addr, 0);
  }

  buffer.size = 32 * 384;
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, SeparateClientVmoSpaces) {
  ASSERT_OK(dut_->Init({}));

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
  const zx_koid_t vmo1_koid = GetVmoKoid(vmo);
  EXPECT_NE(vmo1_koid, ZX_KOID_INVALID);
  EXPECT_OK(dut_->SdmmcRegisterVmo(1, 0, std::move(vmo), 0, zx_system_get_page_size(),
                                   SDMMC_VMO_RIGHT_WRITE));

  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
  const zx_koid_t vmo2_koid = GetVmoKoid(vmo);
  EXPECT_NE(vmo2_koid, ZX_KOID_INVALID);
  EXPECT_OK(dut_->SdmmcRegisterVmo(2, 0, std::move(vmo), 0, zx_system_get_page_size(),
                                   SDMMC_VMO_RIGHT_WRITE));

  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
  EXPECT_NOT_OK(dut_->SdmmcRegisterVmo(1, 0, std::move(vmo), 0, zx_system_get_page_size(),
                                       SDMMC_VMO_RIGHT_WRITE));

  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
  EXPECT_NOT_OK(dut_->SdmmcRegisterVmo(1, 8, std::move(vmo), 0, zx_system_get_page_size(),
                                       SDMMC_VMO_RIGHT_WRITE));

  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
  const zx_koid_t vmo3_koid = GetVmoKoid(vmo);
  EXPECT_NE(vmo3_koid, ZX_KOID_INVALID);
  EXPECT_OK(dut_->SdmmcRegisterVmo(1, 1, std::move(vmo), 0, zx_system_get_page_size(),
                                   SDMMC_VMO_RIGHT_WRITE));

  EXPECT_OK(dut_->SdmmcUnregisterVmo(1, 0, &vmo));
  EXPECT_EQ(GetVmoKoid(vmo), vmo1_koid);

  EXPECT_OK(dut_->SdmmcUnregisterVmo(2, 0, &vmo));
  EXPECT_EQ(GetVmoKoid(vmo), vmo2_koid);

  EXPECT_OK(dut_->SdmmcUnregisterVmo(1, 1, &vmo));
  EXPECT_EQ(GetVmoKoid(vmo), vmo3_koid);

  EXPECT_NOT_OK(dut_->SdmmcUnregisterVmo(1, 0, &vmo));
  EXPECT_NOT_OK(dut_->SdmmcUnregisterVmo(2, 0, &vmo));
  EXPECT_NOT_OK(dut_->SdmmcUnregisterVmo(1, 1, &vmo));
}

TEST_F(AmlSdmmcTest, RequestWithOwnedAndUnownedVmos) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  zx::vmo vmos[5] = {};
  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < 5; i++) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmos[i]));

    EXPECT_OK(dut_->SdmmcRegisterVmo(i, 0, std::move(vmo), i * 64, 512, SDMMC_VMO_RIGHT_WRITE));
    buffers[i * 2] = {
        .buffer =
            {
                .vmo_id = i,
            },
        .type = SDMMC_BUFFER_TYPE_VMO_ID,
        .offset = i * 16,
        .size = 32 * (i + 2),
    };
    buffers[(i * 2) + 1] = {
        .buffer =
            {
                .vmo = vmos[i].get(),
            },
        .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
        .offset = i * 16,
        .size = 32 * (i + 2),
    };
  }

  zx::vmo vmo;
  EXPECT_NOT_OK(dut_->SdmmcUnregisterVmo(3, 1, &vmo));

  sdmmc_req_t request = {
      .cmd_idx = SDMMC_READ_MULTIPLE_BLOCK,
      .cmd_flags = SDMMC_READ_MULTIPLE_BLOCK_FLAGS,
      .arg = 0x1234abcd,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  AmlSdmmcCmdResp::Get().FromValue(0xfedc9876).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(response[0], 0xfedc9876);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(2)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDMMC_READ_MULTIPLE_BLOCK)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x1234abcd);
  EXPECT_EQ(descs[0].data_addr, zx_system_get_page_size());
  EXPECT_EQ(descs[0].resp_addr, 0);

  expected_desc_cfg.set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
  EXPECT_EQ(descs[1].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[1].cmd_arg, 0);
  EXPECT_EQ(descs[1].data_addr, (5 << 24) | zx_system_get_page_size());
  EXPECT_EQ(descs[1].resp_addr, 0);

  expected_desc_cfg.set_len(3);
  EXPECT_EQ(descs[2].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[2].cmd_arg, 0);
  EXPECT_EQ(descs[2].data_addr, (1 << 24) | (zx_system_get_page_size() + 64 + 16));
  EXPECT_EQ(descs[2].resp_addr, 0);

  EXPECT_EQ(descs[3].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[3].cmd_arg, 0);
  EXPECT_EQ(descs[3].data_addr, (6 << 24) | (zx_system_get_page_size() + 16));
  EXPECT_EQ(descs[3].resp_addr, 0);

  expected_desc_cfg.set_len(4);
  EXPECT_EQ(descs[4].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[4].cmd_arg, 0);
  EXPECT_EQ(descs[4].data_addr, (2 << 24) | (zx_system_get_page_size() + 128 + 32));
  EXPECT_EQ(descs[4].resp_addr, 0);

  EXPECT_EQ(descs[5].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[5].cmd_arg, 0);
  EXPECT_EQ(descs[5].data_addr, (7 << 24) | (zx_system_get_page_size() + 32));
  EXPECT_EQ(descs[5].resp_addr, 0);

  expected_desc_cfg.set_len(5);
  EXPECT_EQ(descs[6].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[6].cmd_arg, 0);
  EXPECT_EQ(descs[6].data_addr, (3 << 24) | (zx_system_get_page_size() + 192 + 48));
  EXPECT_EQ(descs[6].resp_addr, 0);

  EXPECT_EQ(descs[7].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[7].cmd_arg, 0);
  EXPECT_EQ(descs[7].data_addr, (8 << 24) | (zx_system_get_page_size() + 48));
  EXPECT_EQ(descs[7].resp_addr, 0);

  expected_desc_cfg.set_len(6);
  EXPECT_EQ(descs[8].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[8].cmd_arg, 0);
  EXPECT_EQ(descs[8].data_addr, (4 << 24) | (zx_system_get_page_size() + 256 + 64));
  EXPECT_EQ(descs[8].resp_addr, 0);

  expected_desc_cfg.set_end_of_chain(1);
  EXPECT_EQ(descs[9].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[9].cmd_arg, 0);
  EXPECT_EQ(descs[9].data_addr, (9 << 24) | (zx_system_get_page_size() + 64));
  EXPECT_EQ(descs[9].resp_addr, 0);
}

TEST_F(AmlSdmmcTest, ResetCmdInfoBits) {
  ResetDutWithFakeBtiPaddrs();

  ASSERT_OK(dut_->Init({}));

  bti_paddrs_[1] = 0x1897'7000;
  bti_paddrs_[2] = 0x1997'8000;
  bti_paddrs_[3] = 0x1997'e000;

  // Make sure the appropriate cmd_info bits get cleared.
  dut_->descs()[0].cmd_info = 0xffff'ffff;
  dut_->descs()[1].cmd_info = 0xffff'ffff;
  dut_->descs()[2].cmd_info = 0xffff'ffff;

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(zx_system_get_page_size() * 3, 0, &vmo));
  EXPECT_OK(dut_->SdmmcRegisterVmo(1, 2, std::move(vmo), 0, zx_system_get_page_size() * 3,
                                   SDMMC_VMO_RIGHT_WRITE));

  sdmmc_buffer_region_t buffer = {
      .buffer = {.vmo_id = 1},
      .type = SDMMC_BUFFER_TYPE_VMO_ID,
      .offset = 0,
      .size = 10752,
  };

  sdmmc_req_t request = {
      .cmd_idx = SDIO_IO_RW_DIRECT_EXTENDED,
      .cmd_flags = SDIO_IO_RW_DIRECT_EXTENDED_FLAGS | SDMMC_CMD_READ,
      .arg = 0x29000015,
      .blocksize = 512,
      .suppress_error_messages = false,
      .client_id = 2,
      .buffers_list = &buffer,
      .buffers_count = 1,
  };
  uint32_t response[4] = {};
  AmlSdmmcCfg::Get().ReadFrom(&*mmio_).set_blk_len(0).WriteTo(&*mmio_);
  EXPECT_OK(dut_->SdmmcRequest(&request, response));
  EXPECT_EQ(AmlSdmmcCfg::Get().ReadFrom(&*mmio_).blk_len(), 9);

  const aml_sdmmc_desc_t* descs = dut_->descs();
  auto expected_desc_cfg = AmlSdmmcCmdCfg::Get()
                               .FromValue(0)
                               .set_len(8)
                               .set_block_mode(1)
                               .set_timeout(AmlSdmmcCmdCfg::kDefaultCmdTimeout)
                               .set_data_io(1)
                               .set_data_wr(0)
                               .set_resp_num(1)
                               .set_cmd_idx(SDIO_IO_RW_DIRECT_EXTENDED)
                               .set_owner(1);

  EXPECT_EQ(descs[0].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[0].cmd_arg, 0x29000015);
  EXPECT_EQ(descs[0].data_addr, 0x1897'7000);
  EXPECT_EQ(descs[0].resp_addr, 0);

  expected_desc_cfg.set_no_resp(1).set_no_cmd(1).set_resp_num(0).set_cmd_idx(0);
  EXPECT_EQ(descs[1].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[1].cmd_arg, 0);
  EXPECT_EQ(descs[1].data_addr, 0x1997'8000);
  EXPECT_EQ(descs[1].resp_addr, 0);

  expected_desc_cfg.set_len(5).set_end_of_chain(1);
  EXPECT_EQ(descs[2].cmd_info, expected_desc_cfg.reg_value());
  EXPECT_EQ(descs[2].cmd_arg, 0);
  EXPECT_EQ(descs[2].data_addr, 0x1997'e000);
  EXPECT_EQ(descs[2].resp_addr, 0);
}

TEST_F(AmlSdmmcTest, WriteToReadOnlyVmo) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < std::size(buffers); i++) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
    const uint32_t vmo_rights = SDMMC_VMO_RIGHT_READ | (i == 5 ? 0 : SDMMC_VMO_RIGHT_WRITE);
    EXPECT_OK(dut_->SdmmcRegisterVmo(i, 0, std::move(vmo), i * 64, 512, vmo_rights));
    buffers[i] = {
        .buffer =
            {
                .vmo_id = i,
            },
        .type = SDMMC_BUFFER_TYPE_VMO_ID,
        .offset = 0,
        .size = 32 * (i + 2),
    };
  }

  sdmmc_req_t request = {
      .cmd_idx = SDIO_IO_RW_DIRECT_EXTENDED,
      .cmd_flags = SDIO_IO_RW_DIRECT_EXTENDED_FLAGS | SDMMC_CMD_READ,
      .arg = 0x29000015,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, ReadFromWriteOnlyVmo) {
  InitializeContiguousPaddrs(10);

  ASSERT_OK(dut_->Init({}));

  sdmmc_buffer_region_t buffers[10];
  for (uint32_t i = 0; i < std::size(buffers); i++) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));
    const uint32_t vmo_rights = SDMMC_VMO_RIGHT_WRITE | (i == 5 ? 0 : SDMMC_VMO_RIGHT_READ);
    EXPECT_OK(dut_->SdmmcRegisterVmo(i, 0, std::move(vmo), i * 64, 512, vmo_rights));
    buffers[i] = {
        .buffer =
            {
                .vmo_id = i,
            },
        .type = SDMMC_BUFFER_TYPE_VMO_ID,
        .offset = 0,
        .size = 32 * (i + 2),
    };
  }

  sdmmc_req_t request = {
      .cmd_idx = SDIO_IO_RW_DIRECT_EXTENDED,
      .cmd_flags = SDIO_IO_RW_DIRECT_EXTENDED_FLAGS,
      .arg = 0x29000015,
      .blocksize = 32,
      .suppress_error_messages = false,
      .client_id = 0,
      .buffers_list = buffers,
      .buffers_count = std::size(buffers),
  };
  uint32_t response[4] = {};
  EXPECT_NOT_OK(dut_->SdmmcRequest(&request, response));
}

TEST_F(AmlSdmmcTest, ConsecutiveErrorLogging) {
  ASSERT_OK(dut_->Init({}));

  // First data error.
  dut_->SetRequestInterruptStatus(1 << 8);
  sdmmc_req_t request;
  memset(&request, 0, sizeof(request));
  uint32_t unused_response[4];
  EXPECT_EQ(ZX_ERR_IO_DATA_INTEGRITY, dut_->SdmmcRequest(&request, unused_response));

  // First cmd error.
  dut_->SetRequestInterruptStatus(1 << 11);
  memset(&request, 0, sizeof(request));
  EXPECT_EQ(ZX_ERR_TIMED_OUT, dut_->SdmmcRequest(&request, unused_response));

  // Second data error.
  dut_->SetRequestInterruptStatus(1 << 7);
  memset(&request, 0, sizeof(request));
  EXPECT_EQ(ZX_ERR_IO_DATA_INTEGRITY, dut_->SdmmcRequest(&request, unused_response));

  // Second cmd error.
  dut_->SetRequestInterruptStatus(1 << 11);
  memset(&request, 0, sizeof(request));
  EXPECT_EQ(ZX_ERR_TIMED_OUT, dut_->SdmmcRequest(&request, unused_response));

  zx::vmo vmo;
  EXPECT_OK(zx::vmo::create(32, 0, &vmo));

  // cmd/data goes through.
  const sdmmc_buffer_region_t region{
      .buffer = {.vmo = vmo.get()},
      .type = SDMMC_BUFFER_TYPE_VMO_HANDLE,
      .offset = 0,
      .size = 32,
  };
  dut_->SetRequestInterruptStatus(1 << 13);
  memset(&request, 0, sizeof(request));
  request.cmd_flags = SDMMC_RESP_DATA_PRESENT;  // Must be set to clear the data error count.
  request.blocksize = 32;
  request.buffers_list = &region;
  request.buffers_count = 1;
  EXPECT_EQ(ZX_OK, dut_->SdmmcRequest(&request, unused_response));

  // Third data error.
  dut_->SetRequestInterruptStatus(1 << 7);
  memset(&request, 0, sizeof(request));
  EXPECT_EQ(ZX_ERR_IO_DATA_INTEGRITY, dut_->SdmmcRequest(&request, unused_response));

  // Third cmd error.
  dut_->SetRequestInterruptStatus(1 << 11);
  memset(&request, 0, sizeof(request));
  EXPECT_EQ(ZX_ERR_TIMED_OUT, dut_->SdmmcRequest(&request, unused_response));
}

}  // namespace sdmmc
