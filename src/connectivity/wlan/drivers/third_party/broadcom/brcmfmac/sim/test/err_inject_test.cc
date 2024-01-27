// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include <fuchsia/hardware/wlanphyimpl/c/banjo.h>
#include <zircon/errors.h>

#include <array>
#include <vector>

#include <wifi/wifi-config.h>
#include <zxtest/zxtest.h>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil_types.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/test/sim_test.h"
#include "src/connectivity/wlan/lib/common/cpp/include/wlan/common/macaddr.h"

namespace wlan::brcmfmac {

class ErrInjTest : public SimTest {
 public:
  void RunCountryTest(const std::vector<uint8_t>& input,
                      const std::array<uint8_t, 2>& expected_output);

 protected:
  // This is the interface we will use for our single client interface
  SimInterface client_ifc_;
};

void ErrInjTest::RunCountryTest(const std::vector<uint8_t>& input,
                                const std::array<uint8_t, 2>& expected_output) {
  // Allocate our alternative injection data. It will be mapped to a brcmf_fil_country_le struct
  // by the driver. We will provide enough data to at least reach the start of the "ccode" field
  // of the structure. Anything beyond that is provided by the individual test.
  constexpr off_t ccode_offset = offsetof(brcmf_fil_country_le, ccode);
  size_t inj_data_size = ccode_offset + input.size();
  std::vector<uint8_t> alt_cc_data(inj_data_size, 0);
  for (size_t ndx = 0; ndx < input.size(); ndx++) {
    alt_cc_data[ccode_offset + ndx] = input[ndx];
  }

  // Set up our injector
  brcmf_simdev* sim = device_->GetSim();
  sim->sim_fw->err_inj_.AddErrInjIovar("country", ZX_OK, BCME_OK, std::nullopt, &alt_cc_data);

  // Get the results and verify that the country code matches the first two characters of our input
  auto result = client_.buffer(test_arena_)->GetCountry();
  ASSERT_TRUE(result.ok());
  ASSERT_FALSE(result->is_error());
  auto& actual_country = result->value();
  EXPECT_EQ(actual_country->alpha2().data()[0], expected_output[0]);
  EXPECT_EQ(actual_country->alpha2().data()[1], expected_output[1]);

  sim->sim_fw->err_inj_.DelErrInjIovar("country");
}

TEST_F(ErrInjTest, ErrInjectorReplacementValues) {
  ASSERT_EQ(Init(), ZX_OK);

  // Less data than needed - the rest should be filled with zeroes
  RunCountryTest({}, {0, 0});
  RunCountryTest({'A'}, {'A', 0});

  // Just enough data to fill the parts of the output we care about
  RunCountryTest({'A', 'B'}, {'A', 'B'});

  // More data than the structure can contain -- this is OK, the injector should only use as much
  // as it needs.
  RunCountryTest({'A', 'B', 'C', 'D', 'E'}, {'A', 'B'});
}

TEST_F(ErrInjTest, CheckIfErrInjCmdEnabledWorks) {
  ASSERT_EQ(Init(), ZX_OK);
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);

  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, client_ifc_.iface_id_);

  const auto expected_status = ZX_ERR_SHOULD_WAIT;
  const auto expected_fw_err = BCME_BUSY;
  sim->sim_fw->err_inj_.AddErrInjCmd(BRCMF_C_GET_RATE, expected_status, expected_fw_err);

  zx_status_t status;
  bcme_status_t fw_err;
  ASSERT_TRUE(sim->sim_fw->err_inj_.CheckIfErrInjCmdEnabled(BRCMF_C_GET_RATE, &status, &fw_err,
                                                            ifp->ifidx));
  EXPECT_EQ(status, expected_status);
  EXPECT_EQ(fw_err, expected_fw_err);
}

TEST_F(ErrInjTest, CheckIfErrInjIovarEnabledWorks) {
  ASSERT_EQ(Init(), ZX_OK);
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);

  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, client_ifc_.iface_id_);

  const auto expected_status = ZX_ERR_SHOULD_WAIT;
  const auto expected_fw_err = BCME_BUSY;
  const std::vector<uint8_t> expected_inj_data = {0};
  sim->sim_fw->err_inj_.AddErrInjIovar("mchan", expected_status, expected_fw_err, ifp->ifidx,
                                       &expected_inj_data);

  zx_status_t status;
  bcme_status_t fw_err;
  const std::vector<uint8_t>* inj_data;
  ASSERT_TRUE(sim->sim_fw->err_inj_.CheckIfErrInjIovarEnabled("mchan", &status, &fw_err, &inj_data,
                                                              ifp->ifidx));
  EXPECT_EQ(status, expected_status);
  EXPECT_EQ(fw_err, expected_fw_err);
  ASSERT_NOT_NULL(inj_data);
  EXPECT_EQ(*inj_data, expected_inj_data);
}

TEST_F(ErrInjTest, CmdFirmwareErrorLifecycle) {
  const common::MacAddr kDefaultBssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc});
  constexpr uint16_t kDefaultChanspec = 53397;

  ASSERT_EQ(Init(), ZX_OK);
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);

  // Initialize variables
  zx_status_t status = ZX_OK;
  bcme_status_t fw_err = BCME_OK;
  struct brcmf_join_params join_params;
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, client_ifc_.iface_id_);

  // Initialize parameter for BRCMF_C_SET_SSID, here we use kDefaultSoftApSsid as the fake
  // association target, the content doesn't really affect the result.
  memcpy(&join_params.ssid_le.SSID, SimInterface::kDefaultSoftApSsid.data,
         SimInterface::kDefaultSoftApSsid.len);
  join_params.ssid_le.SSID_len = SimInterface::kDefaultSoftApSsid.len;

  kDefaultBssid.CopyTo(join_params.params_le.bssid);
  join_params.params_le.chanspec_num = 1;
  join_params.params_le.chanspec_list[0] = kDefaultChanspec;

  // Inject firmware error.
  sim->sim_fw->err_inj_.AddErrInjCmd(BRCMF_C_SET_SSID, ZX_OK, BCME_BADARG);

  status =
      brcmf_fil_cmd_data_set(ifp, BRCMF_C_SET_SSID, &join_params, sizeof(join_params), &fw_err);
  // status code will be adjusted to ZX_ERR_IO_REFUSED even when no error was injected to it.
  EXPECT_EQ(status, ZX_ERR_IO_REFUSED);
  EXPECT_EQ(fw_err, BCME_BADARG);

  // Inject a different firmware error.
  sim->sim_fw->err_inj_.AddErrInjCmd(BRCMF_C_SET_SSID, ZX_OK, BCME_BUSY);

  status =
      brcmf_fil_cmd_data_set(ifp, BRCMF_C_SET_SSID, &join_params, sizeof(join_params), &fw_err);
  // Firmware error BCME_BUSY will cause status code will be adjusted to ZX_ERR_SHOULD_WAIT.
  EXPECT_EQ(status, ZX_ERR_SHOULD_WAIT);
  EXPECT_EQ(fw_err, BCME_BUSY);

  // Delete the error injections to verify the deletion logic.
  sim->sim_fw->err_inj_.DelErrInjCmd(BRCMF_C_SET_SSID);

  status =
      brcmf_fil_cmd_data_set(ifp, BRCMF_C_SET_SSID, &join_params, sizeof(join_params), &fw_err);
  EXPECT_EQ(status, ZX_OK);
  EXPECT_EQ(fw_err, BCME_OK);
}

TEST_F(ErrInjTest, IovarFirmwareErrorLifecycle) {
  const common::MacAddr kDefaultBssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc});

  ASSERT_EQ(Init(), ZX_OK);
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);

  // Initialize variables
  zx_status_t status = ZX_OK;
  bcme_status_t fw_err = BCME_OK;
  struct brcmf_fil_country_le ccreq;
  brcmf_simdev* sim = device_->GetSim();
  struct brcmf_if* ifp = brcmf_get_ifp(sim->drvr, client_ifc_.iface_id_);

  // Initialize parameter for "country" iovar.
  ccreq.ccode[0] = 'W';
  ccreq.ccode[1] = 'W';
  ccreq.ccode[2] = 0;
  ccreq.country_abbrev[0] = 'W';
  ccreq.country_abbrev[1] = 'W';
  ccreq.country_abbrev[2] = 0;

  // Inject firmware error.
  sim->sim_fw->err_inj_.AddErrInjIovar("country", ZX_OK, BCME_ERROR);

  status = brcmf_fil_iovar_data_set(ifp, "country", &ccreq, sizeof(ccreq), &fw_err);
  // status code will be adjusted to ZX_ERR_IO_REFUSED even when no error was injected to it.
  EXPECT_EQ(status, ZX_ERR_IO_REFUSED);
  EXPECT_EQ(fw_err, BCME_ERROR);

  // Inject a different firmware error.
  sim->sim_fw->err_inj_.AddErrInjIovar("country", ZX_OK, BCME_BUSY);

  status = brcmf_fil_iovar_data_set(ifp, "country", &ccreq, sizeof(ccreq), &fw_err);
  // Firmware error BCME_BUSY will cause status code to be adjusted to ZX_ERR_SHOULD_WAIT.
  EXPECT_EQ(status, ZX_ERR_SHOULD_WAIT);
  EXPECT_EQ(fw_err, BCME_BUSY);

  // Delete the error injection to verify the deletion logic.
  sim->sim_fw->err_inj_.DelErrInjIovar("country");

  status = brcmf_fil_iovar_data_set(ifp, "country", &ccreq, sizeof(ccreq), &fw_err);
  EXPECT_EQ(status, ZX_OK);
  EXPECT_EQ(fw_err, BCME_OK);
}

}  // namespace wlan::brcmfmac
