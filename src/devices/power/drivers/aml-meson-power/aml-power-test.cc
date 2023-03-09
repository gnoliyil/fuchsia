// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-power.h"

#include <fidl/fuchsia.hardware.vreg/cpp/wire_test_base.h>
#include <fuchsia/hardware/pwm/cpp/banjo-mock.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/default.h>
#include <lib/async_patterns/testing/cpp/dispatcher_bound.h>
#include <lib/ddk/platform-defs.h>

#include <memory>
#include <optional>

#include <soc/aml-common/aml-pwm-regs.h>
#include <zxtest/zxtest.h>

bool operator==(const pwm_config_t& lhs, const pwm_config_t& rhs) {
  return (lhs.polarity == rhs.polarity) && (lhs.period_ns == rhs.period_ns) &&
         (lhs.duty_cycle == rhs.duty_cycle) && (lhs.mode_config_size == rhs.mode_config_size) &&
         (reinterpret_cast<aml_pwm::mode_config*>(lhs.mode_config_buffer)->mode ==
          reinterpret_cast<aml_pwm::mode_config*>(rhs.mode_config_buffer)->mode);
}

namespace power {

const std::vector<aml_voltage_table_t> kTestVoltageTable = {
    {1'050'000, 0},  {1'040'000, 3}, {1'030'000, 6}, {1'020'000, 8}, {1'010'000, 11},
    {1'000'000, 14}, {990'000, 17},  {980'000, 20},  {970'000, 23},  {960'000, 26},
    {950'000, 29},   {940'000, 31},  {930'000, 34},  {920'000, 37},  {910'000, 40},
    {900'000, 43},   {890'000, 45},  {880'000, 48},  {870'000, 51},  {860'000, 54},
    {850'000, 56},   {840'000, 59},  {830'000, 62},  {820'000, 65},  {810'000, 68},
    {800'000, 70},   {790'000, 73},  {780'000, 76},  {770'000, 79},  {760'000, 81},
    {750'000, 84},   {740'000, 87},  {730'000, 89},  {720'000, 92},  {710'000, 95},
    {700'000, 98},   {690'000, 100},
};

constexpr voltage_pwm_period_ns_t kTestPwmPeriodNs = 1250;

class AmlPowerTestWrapper : public AmlPower {
 public:
  AmlPowerTestWrapper(ddk::MockPwm& mock_big_pwm, ddk::MockPwm& mock_little_pwm,
                      std::vector<aml_voltage_table_t> voltage_table,
                      voltage_pwm_period_ns_t pwm_period)
      : AmlPower(nullptr, mock_big_pwm.GetProto(), mock_little_pwm.GetProto(), voltage_table,
                 pwm_period) {}

  AmlPowerTestWrapper(fidl::ClientEnd<fuchsia_hardware_vreg::Vreg> mock_big_vreg,
                      ddk::MockPwm& mock_little_pwm, std::vector<aml_voltage_table_t> voltage_table,
                      voltage_pwm_period_ns_t pwm_period)
      : AmlPower(nullptr, std::move(mock_big_vreg), mock_little_pwm.GetProto(), voltage_table,
                 pwm_period) {}

  AmlPowerTestWrapper(ddk::MockPwm& mock_big_pwm, std::vector<aml_voltage_table_t> voltage_table,
                      voltage_pwm_period_ns_t pwm_period)
      : AmlPower(nullptr, mock_big_pwm.GetProto(), voltage_table, pwm_period) {}

  AmlPowerTestWrapper(fidl::ClientEnd<fuchsia_hardware_vreg::Vreg> mock_big_vreg,
                      fidl::ClientEnd<fuchsia_hardware_vreg::Vreg> mock_little_vreg)
      : AmlPower(nullptr, std::move(mock_big_vreg), std::move(mock_little_vreg)) {}

  static std::unique_ptr<AmlPowerTestWrapper> Create(ddk::MockPwm& mock_big_pwm,
                                                     std::vector<aml_voltage_table_t> voltage_table,
                                                     voltage_pwm_period_ns_t pwm_period) {
    auto result = std::make_unique<AmlPowerTestWrapper>(mock_big_pwm, voltage_table, pwm_period);
    return result;
  }

  static std::unique_ptr<AmlPowerTestWrapper> Create(
      fidl::ClientEnd<fuchsia_hardware_vreg::Vreg> mock_big_vreg, ddk::MockPwm& mock_little_pwm,
      std::vector<aml_voltage_table_t> voltage_table, voltage_pwm_period_ns_t pwm_period) {
    auto result = std::make_unique<AmlPowerTestWrapper>(std::move(mock_big_vreg), mock_little_pwm,
                                                        voltage_table, pwm_period);
    return result;
  }

  static std::unique_ptr<AmlPowerTestWrapper> Create(ddk::MockPwm& mock_big_pwm,
                                                     ddk::MockPwm& mock_little_pwm,
                                                     std::vector<aml_voltage_table_t> voltage_table,
                                                     voltage_pwm_period_ns_t pwm_period) {
    auto result = std::make_unique<AmlPowerTestWrapper>(mock_big_pwm, mock_little_pwm,
                                                        voltage_table, pwm_period);
    return result;
  }

  static std::unique_ptr<AmlPowerTestWrapper> Create(
      fidl::ClientEnd<fuchsia_hardware_vreg::Vreg> mock_big_vreg,
      fidl::ClientEnd<fuchsia_hardware_vreg::Vreg> mock_little_vreg) {
    auto result = std::make_unique<AmlPowerTestWrapper>(std::move(mock_big_vreg),
                                                        std::move(mock_little_vreg));
    return result;
  }

 private:
};

class FakeVregServer final : public fidl::testing::WireTestBase<fuchsia_hardware_vreg::Vreg> {
 public:
  void SetRegulatorParams(uint32_t min_uv, uint32_t step_size_uv, uint32_t num_steps) {
    min_uv_ = min_uv;
    step_size_uv_ = step_size_uv;
    num_steps_ = num_steps;
  }

  void GetRegulatorParams(GetRegulatorParamsCompleter::Sync& completer) override {
    completer.Reply(min_uv_, step_size_uv_, num_steps_);
  }

  void SetVoltageStep(::fuchsia_hardware_vreg::wire::VregSetVoltageStepRequest* request,
                      SetVoltageStepCompleter::Sync& completer) override {
    voltage_step_ = request->step;
    completer.Reply(fit::success());
  }

  void GetVoltageStep(GetVoltageStepCompleter::Sync& completer) override {
    completer.Reply(voltage_step_);
  }

  void NotImplemented_(const std::string& name, ::fidl::CompleterBase& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  uint32_t voltage_step() const { return voltage_step_; }

  fidl::ClientEnd<fuchsia_hardware_vreg::Vreg> BindServer() {
    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_vreg::Vreg>();
    EXPECT_TRUE(endpoints.is_ok());
    binding_ref_ =
        fidl::BindServer(async_get_default_dispatcher(), std::move(endpoints->server), this);
    return std::move(endpoints->client);
  }

 private:
  uint32_t min_uv_;
  uint32_t step_size_uv_;
  uint32_t num_steps_;
  uint32_t voltage_step_;
  std::optional<fidl::ServerBindingRef<fuchsia_hardware_vreg::Vreg>> binding_ref_;
};

class AmlPowerTest : public zxtest::Test {
 public:
  void TearDown() override {
    big_cluster_pwm_.VerifyAndClear();
    little_cluster_pwm_.VerifyAndClear();
    vreg_loop_.Shutdown();
  }

  zx_status_t Create(uint32_t pid, std::vector<aml_voltage_table_t> voltage_table,
                     voltage_pwm_period_ns_t pwm_period) {
    EXPECT_OK(vreg_loop_.StartThread("vreg-servers"));
    auto big_cluster_vreg_client = big_cluster_vreg_.SyncCall(&FakeVregServer::BindServer);
    auto little_cluster_vreg_client = little_cluster_vreg_.SyncCall(&FakeVregServer::BindServer);
    switch (pid) {
      case PDEV_PID_ASTRO: {
        aml_power_ = AmlPowerTestWrapper::Create(big_cluster_pwm_, voltage_table, pwm_period);
        return ZX_OK;
      }
      case PDEV_PID_SHERLOCK: {
        aml_power_ = AmlPowerTestWrapper::Create(big_cluster_pwm_, little_cluster_pwm_,
                                                 voltage_table, pwm_period);
        return ZX_OK;
      }
      case PDEV_PID_LUIS: {
        aml_power_ = AmlPowerTestWrapper::Create(std::move(big_cluster_vreg_client),
                                                 little_cluster_pwm_, voltage_table, pwm_period);
        return ZX_OK;
      }
      case PDEV_PID_AMLOGIC_A311D: {
        aml_power_ = AmlPowerTestWrapper::Create(std::move(big_cluster_vreg_client),
                                                 std::move(little_cluster_vreg_client));
        return ZX_OK;
      }
      default:
        return ZX_ERR_INTERNAL;
    }
  }

  zx_status_t Create(uint32_t pid) { return Create(pid, kTestVoltageTable, kTestPwmPeriodNs); }

 protected:
  std::unique_ptr<AmlPowerTestWrapper> aml_power_;
  async::Loop vreg_loop_{&kAsyncLoopConfigNoAttachToCurrentThread};

  // Mmio Regs and Regions
  ddk::MockPwm big_cluster_pwm_;
  ddk::MockPwm little_cluster_pwm_;
  async_patterns::TestDispatcherBound<FakeVregServer> big_cluster_vreg_{vreg_loop_.dispatcher(),
                                                                        std::in_place};
  async_patterns::TestDispatcherBound<FakeVregServer> little_cluster_vreg_{vreg_loop_.dispatcher(),
                                                                           std::in_place};
};

TEST_F(AmlPowerTest, SetVoltage) {
  EXPECT_OK(Create(PDEV_PID_ASTRO));
  zx_status_t st;
  constexpr uint32_t kTestVoltageInitial = 690'000;
  constexpr uint32_t kTestVoltageFinal = 1'040'000;

  aml_pwm::mode_config on = {aml_pwm::ON, {}};

  // Initialize to 0.69V
  pwm_config_t cfg = {false, 1250, 100, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);

  // Scale up to 1.05V
  cfg = {false, 1250, 92, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 84, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 76, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 68, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 59, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 51, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 43, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 34, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 26, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 17, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 8, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 3, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);

  uint32_t actual;
  st = aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain,
                                           kTestVoltageInitial, &actual);
  EXPECT_EQ(kTestVoltageInitial, actual);
  EXPECT_OK(st);

  st = aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain,
                                           kTestVoltageFinal, &actual);
  EXPECT_EQ(kTestVoltageFinal, actual);
  EXPECT_OK(st);
}

TEST_F(AmlPowerTest, ClusterIndexOutOfRange) {
  constexpr uint32_t kTestVoltage = 690'000;

  EXPECT_OK(Create(PDEV_PID_ASTRO));

  uint32_t actual;
  zx_status_t st = aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kLittleClusterDomain,
                                                       kTestVoltage, &actual);
  EXPECT_NOT_OK(st);
}

TEST_F(AmlPowerTest, GetVoltageUnset) {
  // Get the voltage before it's been set. Should return ZX_ERR_BAD_STATE.
  EXPECT_OK(Create(PDEV_PID_ASTRO));

  uint32_t voltage;
  zx_status_t st =
      aml_power_->PowerImplGetCurrentVoltage(AmlPowerTestWrapper::kBigClusterDomain, &voltage);
  EXPECT_NOT_OK(st);
}

TEST_F(AmlPowerTest, GetVoltage) {
  // Get the voltage before it's been set. Should return ZX_ERR_BAD_STATE.
  constexpr uint32_t kTestVoltage = 690'000;
  zx_status_t st;

  EXPECT_OK(Create(PDEV_PID_ASTRO));

  // Initialize to 0.69V
  aml_pwm::mode_config on = {aml_pwm::ON, {}};
  pwm_config_t cfg = {false, 1250, 100, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);

  uint32_t requested_voltage, actual_voltage;
  st = aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain, kTestVoltage,
                                           &requested_voltage);
  EXPECT_OK(st);
  EXPECT_EQ(requested_voltage, kTestVoltage);

  st = aml_power_->PowerImplGetCurrentVoltage(AmlPowerTestWrapper::kBigClusterDomain,
                                              &actual_voltage);
  EXPECT_OK(st);
  EXPECT_EQ(requested_voltage, actual_voltage);
}

TEST_F(AmlPowerTest, GetVoltageOutOfRange) {
  EXPECT_OK(Create(PDEV_PID_ASTRO));

  uint32_t voltage;
  zx_status_t st =
      aml_power_->PowerImplGetCurrentVoltage(AmlPowerTestWrapper::kLittleClusterDomain, &voltage);
  EXPECT_NOT_OK(st);
}

TEST_F(AmlPowerTest, SetVoltageRoundDown) {
  // Set a voltage that's not exactly supported and let the driver round down to the nearest
  // voltage.
  EXPECT_OK(Create(PDEV_PID_ASTRO));
  constexpr uint32_t kTestVoltageInitial = 830'000;

  // We expect the driver to give us the highest voltage that does not exceed the requested voltage.
  constexpr uint32_t kTestVoltageFinalRequest = 935'000;
  constexpr uint32_t kTestVoltageFinalActual = 930'000;

  aml_pwm::mode_config on = {aml_pwm::ON, {}};
  pwm_config_t cfg = {false, 1250, 62, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 54, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 45, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 37, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 34, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  big_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);

  uint32_t actual;
  zx_status_t st;

  st = aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain,
                                           kTestVoltageInitial, &actual);
  EXPECT_OK(st);
  EXPECT_EQ(actual, kTestVoltageInitial);

  st = aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain,
                                           kTestVoltageFinalRequest, &actual);
  EXPECT_OK(st);
  EXPECT_EQ(actual, kTestVoltageFinalActual);
}

TEST_F(AmlPowerTest, SetVoltageLittleCluster) {
  // Set a voltage that's not exactly supported and let the driver round down to the nearest
  // voltage.
  EXPECT_OK(Create(PDEV_PID_SHERLOCK));
  constexpr uint32_t kTestVoltageInitial = 730'000;
  constexpr uint32_t kTestVoltageFinal = 930'000;

  aml_pwm::mode_config on = {aml_pwm::ON, {}};
  pwm_config_t cfg = {false, 1250, 89, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  little_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 81, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  little_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 73, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  little_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 65, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  little_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 56, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  little_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 48, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  little_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 40, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  little_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);
  cfg = {false, 1250, 34, reinterpret_cast<uint8_t*>(&on), sizeof(on)};
  little_cluster_pwm_.ExpectSetConfig(ZX_OK, cfg);

  uint32_t actual;
  zx_status_t st;

  st = aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kLittleClusterDomain,
                                           kTestVoltageInitial, &actual);
  EXPECT_OK(st);
  EXPECT_EQ(actual, kTestVoltageInitial);

  st = aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kLittleClusterDomain,
                                           kTestVoltageFinal, &actual);
  EXPECT_OK(st);
  EXPECT_EQ(actual, kTestVoltageFinal);
}

TEST_F(AmlPowerTest, DomainEnableDisable) {
  EXPECT_OK(Create(PDEV_PID_SHERLOCK));

  // Enable.
  EXPECT_OK(aml_power_->PowerImplEnablePowerDomain(AmlPowerTestWrapper::kLittleClusterDomain));
  EXPECT_OK(aml_power_->PowerImplEnablePowerDomain(AmlPowerTestWrapper::kBigClusterDomain));

  // Disable.
  EXPECT_OK(aml_power_->PowerImplDisablePowerDomain(AmlPowerTestWrapper::kLittleClusterDomain));
  EXPECT_OK(aml_power_->PowerImplDisablePowerDomain(AmlPowerTestWrapper::kBigClusterDomain));

  // Out of bounds.
  EXPECT_NOT_OK(
      aml_power_->PowerImplDisablePowerDomain(AmlPowerTestWrapper::kLittleClusterDomain + 1));
  EXPECT_NOT_OK(
      aml_power_->PowerImplEnablePowerDomain(AmlPowerTestWrapper::kLittleClusterDomain + 1));
}

TEST_F(AmlPowerTest, GetDomainStatus) {
  EXPECT_OK(Create(PDEV_PID_ASTRO));

  // Happy case.
  power_domain_status_t result;
  EXPECT_OK(
      aml_power_->PowerImplGetPowerDomainStatus(AmlPowerTestWrapper::kBigClusterDomain, &result));
  EXPECT_EQ(result, POWER_DOMAIN_STATUS_ENABLED);

  // Out of bounds.
  EXPECT_NOT_OK(aml_power_->PowerImplGetPowerDomainStatus(AmlPowerTestWrapper::kLittleClusterDomain,
                                                          &result));
}

TEST_F(AmlPowerTest, LuisSetBigCluster) {
  EXPECT_OK(Create(PDEV_PID_LUIS));

  big_cluster_vreg_.SyncCall(&FakeVregServer::SetRegulatorParams, 100, 10, 10);
  const uint32_t kTestVoltage = 155;
  uint32_t actual;
  EXPECT_OK(aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain,
                                                kTestVoltage, &actual));
  EXPECT_EQ(actual, 150);
  EXPECT_EQ(big_cluster_vreg_.SyncCall(&FakeVregServer::voltage_step), 5);

  // Voltage is too low.
  EXPECT_NOT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain, 99, &actual));

  // Set voltage to the threshold.
  EXPECT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain, 200, &actual));
  EXPECT_EQ(actual, 200);
  EXPECT_EQ(big_cluster_vreg_.SyncCall(&FakeVregServer::voltage_step), 10);

  // Set voltage beyond the threshold.
  EXPECT_NOT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain, 300, &actual));
}

TEST_F(AmlPowerTest, LuisGetSupportedVoltageRange) {
  EXPECT_OK(Create(PDEV_PID_LUIS));

  big_cluster_vreg_.SyncCall(&FakeVregServer::SetRegulatorParams, 100, 10, 10);

  uint32_t max, min;
  EXPECT_OK(aml_power_->PowerImplGetSupportedVoltageRange(AmlPowerTestWrapper::kBigClusterDomain,
                                                          &min, &max));
  EXPECT_EQ(max, 200);
  EXPECT_EQ(min, 100);

  EXPECT_OK(aml_power_->PowerImplGetSupportedVoltageRange(AmlPowerTestWrapper::kLittleClusterDomain,
                                                          &min, &max));
  EXPECT_EQ(max, 1'050'000);
  EXPECT_EQ(min, 690'000);
}

TEST_F(AmlPowerTest, Vim3SetBigCluster) {
  EXPECT_OK(Create(PDEV_PID_AMLOGIC_A311D));

  big_cluster_vreg_.SyncCall(&FakeVregServer::SetRegulatorParams, 100, 10, 10);
  const uint32_t kTestVoltage = 155;
  uint32_t actual;
  EXPECT_OK(aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain,
                                                kTestVoltage, &actual));
  EXPECT_EQ(actual, 150);
  EXPECT_EQ(big_cluster_vreg_.SyncCall(&FakeVregServer::voltage_step), 5);

  // Voltage is too low.
  EXPECT_NOT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain, 99, &actual));

  // Set voltage to the threshold.
  EXPECT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain, 200, &actual));
  EXPECT_EQ(actual, 200);
  EXPECT_EQ(big_cluster_vreg_.SyncCall(&FakeVregServer::voltage_step), 10);

  // Set voltage beyond the threshold.
  EXPECT_NOT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kBigClusterDomain, 300, &actual));
}

TEST_F(AmlPowerTest, Vim3SetLittleCluster) {
  EXPECT_OK(Create(PDEV_PID_AMLOGIC_A311D));

  little_cluster_vreg_.SyncCall(&FakeVregServer::SetRegulatorParams, 100, 10, 10);
  const uint32_t kTestVoltage = 155;
  uint32_t actual;
  EXPECT_OK(aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kLittleClusterDomain,
                                                kTestVoltage, &actual));
  EXPECT_EQ(actual, 150);
  EXPECT_EQ(little_cluster_vreg_.SyncCall(&FakeVregServer::voltage_step), 5);

  // Voltage is too low.
  EXPECT_NOT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kLittleClusterDomain, 99, &actual));

  // Set voltage to the threshold.
  EXPECT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kLittleClusterDomain, 200, &actual));
  EXPECT_EQ(actual, 200);
  EXPECT_EQ(little_cluster_vreg_.SyncCall(&FakeVregServer::voltage_step), 10);

  // Set voltage beyond the threshold.
  EXPECT_NOT_OK(
      aml_power_->PowerImplRequestVoltage(AmlPowerTestWrapper::kLittleClusterDomain, 300, &actual));
}

TEST_F(AmlPowerTest, Vim3GetSupportedVoltageRange) {
  EXPECT_OK(Create(PDEV_PID_AMLOGIC_A311D));

  big_cluster_vreg_.SyncCall(&FakeVregServer::SetRegulatorParams, 100, 10, 10);

  uint32_t max, min;
  EXPECT_OK(aml_power_->PowerImplGetSupportedVoltageRange(AmlPowerTestWrapper::kBigClusterDomain,
                                                          &min, &max));
  EXPECT_EQ(max, 200);
  EXPECT_EQ(min, 100);

  little_cluster_vreg_.SyncCall(&FakeVregServer::SetRegulatorParams, 100, 20, 5);

  EXPECT_OK(aml_power_->PowerImplGetSupportedVoltageRange(AmlPowerTestWrapper::kLittleClusterDomain,
                                                          &min, &max));
  EXPECT_EQ(max, 200);
  EXPECT_EQ(min, 100);
}

}  // namespace power
