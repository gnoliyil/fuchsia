// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/wlanphyimpl/c/banjo.h>
#include <zircon/errors.h>

#include <zxtest/zxtest.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-device/device.h"
#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/common.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/test/sim_test.h"

namespace wlan::brcmfmac {

class CountryCodeTest : public SimTest {
 public:
  CountryCodeTest() = default;
  void Init();
  void CreateInterface();
  void DeleteInterface();
  zx_status_t SetCountryCode(const fuchsia_wlan_phyimpl::wire::WlanPhyCountry* country);
  void GetCountryCodeFromFirmware(brcmf_fil_country_le* ccode);
  zx_status_t SetCountryCodeInFirmware(const wlan_phy_country_t* country);
  zx_status_t ClearCountryCode();
  uint32_t DeviceCountByProtocolId(uint32_t proto_id);

 private:
  SimInterface client_ifc_;
};

void CountryCodeTest::Init() { ASSERT_EQ(SimTest::Init(), ZX_OK); }

void CountryCodeTest::CreateInterface() {
  zx_status_t status;

  status = StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  ASSERT_EQ(status, ZX_OK);
}

void CountryCodeTest::DeleteInterface() {
  EXPECT_EQ(SimTest::DeleteInterface(&client_ifc_), ZX_OK);
}

uint32_t CountryCodeTest::DeviceCountByProtocolId(uint32_t proto_id) {
  return dev_mgr_->DeviceCountByProtocolId(proto_id);
}

zx_status_t CountryCodeTest::SetCountryCode(
    const fuchsia_wlan_phyimpl::wire::WlanPhyCountry* country) {
  auto result = client_.sync().buffer(test_arena_)->SetCountry(*country);
  EXPECT_TRUE(result.ok());
  if (result->is_error()) {
    return result->error_value();
  }
  return ZX_OK;
}

zx_status_t CountryCodeTest::ClearCountryCode() {
  auto result = client_.sync().buffer(test_arena_)->ClearCountry();
  EXPECT_TRUE(result.ok());
  if (result->is_error()) {
    return result->error_value();
  }
  return ZX_OK;
}

// Note that this function is meant for SIM only. It retrieves the internal
// state of the country code setting by bypassing the interfaces.
void CountryCodeTest::GetCountryCodeFromFirmware(brcmf_fil_country_le* ccode) {
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, client_ifc_.iface_id_);
  zx_status_t status =
      brcmf_fil_iovar_data_get(ifp, "country", ccode, sizeof(brcmf_fil_country_le), nullptr);
  EXPECT_EQ(status, ZX_OK);
}

zx_status_t CountryCodeTest::SetCountryCodeInFirmware(const wlan_phy_country_t* country) {
  EXPECT_NE(country, nullptr);
  brcmf_simdev* sim = device_->GetSim();
  return brcmf_set_country(sim->drvr, country);
}

TEST_F(CountryCodeTest, SetDefault) {
  Init();
  CreateInterface();
  DeleteInterface();
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
}

TEST_F(CountryCodeTest, SetCCode) {
  const auto valid_country = fuchsia_wlan_phyimpl::wire::WlanPhyCountry::WithAlpha2({'U', 'S'});
  const auto invalid_country = fuchsia_wlan_phyimpl::wire::WlanPhyCountry::WithAlpha2({'X', 'X'});
  struct brcmf_fil_country_le country_code;
  zx_status_t status;
  uint8_t code;

  Init();
  CreateInterface();
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);

  // Get the country code and verify that it is set to WW.
  GetCountryCodeFromFirmware(&country_code);
  code = memcmp(country_code.ccode, "WW", WLANPHY_ALPHA2_LEN);
  ASSERT_EQ(code, 0);

  // Set an invalid CC and verify it fails
  status = SetCountryCode(&invalid_country);
  ASSERT_NE(status, ZX_OK);

  // Verify that it stays with the default
  GetCountryCodeFromFirmware(&country_code);
  code = memcmp(country_code.ccode, "WW", WLANPHY_ALPHA2_LEN);
  ASSERT_EQ(code, 0);
  // Set a valid CC and verify it succeeds
  status = SetCountryCode(&valid_country);
  ASSERT_EQ(status, ZX_OK);
  GetCountryCodeFromFirmware(&country_code);
  code = memcmp(&valid_country.alpha2(), country_code.ccode, WLANPHY_ALPHA2_LEN);
  ASSERT_EQ(code, 0);
}

TEST_F(CountryCodeTest, GetCCode) {
  Init();
  CreateInterface();

  {
    const wlan_phy_country_t country = {{'W', 'W'}};
    ASSERT_EQ(ZX_OK, SetCountryCodeInFirmware(&country));
    auto result = client_.sync().buffer(test_arena_)->GetCountry();
    EXPECT_TRUE(result.ok());
    ASSERT_FALSE(result->is_error());
    auto& get_country_result = result->value();
    EXPECT_EQ(get_country_result->alpha2().data()[0], 'W');
    EXPECT_EQ(get_country_result->alpha2().data()[1], 'W');
  }

  // Try again, just in case the first one was a default value.
  {
    const wlan_phy_country_t country = {{'U', 'S'}};
    ASSERT_EQ(ZX_OK, SetCountryCodeInFirmware(&country));
    auto result = client_.sync().buffer(test_arena_)->GetCountry();
    EXPECT_TRUE(result.ok());
    ASSERT_FALSE(result->is_error());
    auto& get_country_result = result->value();
    EXPECT_EQ(get_country_result->alpha2().data()[0], 'U');
    EXPECT_EQ(get_country_result->alpha2().data()[1], 'S');
  }
}

TEST_F(CountryCodeTest, ClearCCode) {
  const wlan_phy_country_t world_safe_country = {{'W', 'W'}};
  struct brcmf_fil_country_le country_code;
  zx_status_t status;
  uint8_t code;

  Init();
  CreateInterface();
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 1u);
  status = ClearCountryCode();
  ASSERT_EQ(status, ZX_OK);
  GetCountryCodeFromFirmware(&country_code);
  code = memcmp(world_safe_country.alpha2, country_code.ccode, WLANPHY_ALPHA2_LEN);
  ASSERT_EQ(code, 0);
}

}  // namespace wlan::brcmfmac
