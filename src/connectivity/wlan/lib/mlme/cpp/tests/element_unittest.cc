// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/wlan/ieee80211/c/banjo.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include <gtest/gtest.h>
#include <wlan/common/buffer_writer.h>
#include <wlan/common/write_element.h>
#include <wlan/mlme/mac_frame.h>
#include <wlan/mlme/wlan.h>

namespace wlan {
namespace {

template <typename T>
T AvoidReferenceBindingToMisalignedAddress(T t) {
  return t;
}

class Elements : public ::testing::Test {
 protected:
  Elements() { buf_offset_ = buf_; }

  template <typename T>
  void add_to_buf(const T& value) {
    memcpy(buf_offset_, &value, sizeof(value));
    buf_offset_ += sizeof(value);
  }
  uint8_t* buf_offset_;
  uint8_t buf_[1024] = {};
  size_t actual_ = 0;
};

TEST_F(Elements, Tspec) {
  // Values are chosen randomly.
  constexpr uint8_t ts_info[3] = {97, 54, 13};
  constexpr uint16_t nominal_msdu_size = 1068;
  constexpr uint16_t max_msdu_size = 17223;
  constexpr uint32_t min_svc_interval = 3463625064;
  constexpr uint32_t max_svc_interval = 1348743544;
  constexpr uint32_t inactivity_interval = 3254177988;
  constexpr uint32_t suspension_interval = 3114872601;
  constexpr uint32_t svc_start_time = 1977490251;
  constexpr uint32_t min_data_rate = 2288957164;
  constexpr uint32_t mean_data_rate = 3691476893;
  constexpr uint32_t peak_data_rate = 3115603983;
  constexpr uint32_t burst_size = 2196032537;
  constexpr uint32_t delay_bound = 4120916503;
  constexpr uint32_t min_phy_rate = 4071757759;
  constexpr uint32_t surplus_bw_allowance = 12936;
  constexpr uint32_t medium_time = 2196;

  add_to_buf(ts_info);
  add_to_buf<uint16_t>(nominal_msdu_size);
  add_to_buf<uint16_t>(max_msdu_size);
  add_to_buf<uint32_t>(min_svc_interval);
  add_to_buf<uint32_t>(max_svc_interval);
  add_to_buf<uint32_t>(inactivity_interval);
  add_to_buf<uint32_t>(suspension_interval);
  add_to_buf<uint32_t>(svc_start_time);
  add_to_buf<uint32_t>(min_data_rate);
  add_to_buf<uint32_t>(mean_data_rate);
  add_to_buf<uint32_t>(peak_data_rate);
  add_to_buf<uint32_t>(burst_size);
  add_to_buf<uint32_t>(delay_bound);
  add_to_buf<uint32_t>(min_phy_rate);
  add_to_buf<uint16_t>(surplus_bw_allowance);
  add_to_buf<uint16_t>(medium_time);

  auto element = FromBytes<Tspec>(buf_, sizeof(buf_));
  ASSERT_NE(nullptr, element);
  EXPECT_EQ(element->nominal_msdu_size.size(), nominal_msdu_size);
  EXPECT_EQ(element->nominal_msdu_size.fixed(), 0);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->max_msdu_size), max_msdu_size);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->min_service_interval),
            min_svc_interval);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->max_service_interval),
            max_svc_interval);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->inactivity_interval),
            inactivity_interval);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->suspension_interval),
            suspension_interval);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->service_start_time), svc_start_time);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->min_data_rate), min_data_rate);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->mean_data_rate), mean_data_rate);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->peak_data_rate), peak_data_rate);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->burst_size), burst_size);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->delay_bound), delay_bound);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->min_phy_rate), min_phy_rate);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->surplus_bw_allowance),
            surplus_bw_allowance);
  EXPECT_EQ(AvoidReferenceBindingToMisalignedAddress(element->medium_time), medium_time);
}

TEST_F(Elements, TsInfoAggregation) {
  TsInfo ts_info;
  ts_info.p1.set_access_policy(TsAccessPolicy::kHccaSpca);
  EXPECT_TRUE(ts_info.IsValidAggregation());
  EXPECT_TRUE(ts_info.IsScheduleReserved());

  ts_info.p1.set_access_policy(TsAccessPolicy::kEdca);
  EXPECT_FALSE(ts_info.IsValidAggregation());
  EXPECT_FALSE(ts_info.IsScheduleReserved());

  ts_info.p2.set_schedule(1);
  EXPECT_TRUE(ts_info.IsValidAggregation());
}

TEST_F(Elements, TsInfoScheduleSetting) {
  TsInfo ts_info;
  EXPECT_EQ(ts_info.GetScheduleSetting(), TsScheduleSetting::kNoSchedule);

  ts_info.p1.set_apsd(1);
  EXPECT_EQ(ts_info.GetScheduleSetting(), TsScheduleSetting::kUnschedledApsd);

  ts_info.p1.set_apsd(0);
  ts_info.p2.set_schedule(1);
  EXPECT_EQ(ts_info.GetScheduleSetting(), TsScheduleSetting::kScheduledPsmp_GcrSp);

  ts_info.p1.set_apsd(1);
  EXPECT_EQ(ts_info.GetScheduleSetting(), TsScheduleSetting::kScheduledApsd);
}

TEST(HtCapabilities, DdkConversion) {
  ht_capabilities_t ddk{
      .bytes =
          {
              0x6e, 0x01,  // HtCapabilityInfo
              0x17,        // AmpduParams
              0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff,
              0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SupportedMcsSet
              0x34, 0x12,                                      // HtExtCapabilities
              0x78, 0x56, 0x34, 0x12,                          // TxBeamformingCapabilities
              0xff,                                            // AselCapability
          },
  };

  auto ieee = HtCapabilities::FromDdk(ddk);
  EXPECT_EQ(0x016eU, ieee.ht_cap_info.as_uint16());
  EXPECT_EQ(0x17U, ieee.ampdu_params.val());
  std::array<uint8_t, 16> expected_mcs_set = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff,
                                              0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(expected_mcs_set, ieee.mcs_set.val());
  EXPECT_EQ(0x1234U, ieee.ht_ext_cap.as_uint16());
  EXPECT_EQ(0x12345678U, ieee.txbf_cap.as_uint32());
  EXPECT_EQ(0xffU, ieee.asel_cap.val());

  auto ddk2 = ieee.ToDdk();
  for (size_t i = 0; i < sizeof(ddk.bytes); i++) {
    EXPECT_EQ(ddk.bytes[i], ddk2.bytes[i]);
  }
}

TEST(HtOperation, DdkConversion) {
  wlan_ht_op ddk{
      .primary_channel = 123,
      .head = 0x01020304,
      .tail = 0x05,
      .mcs_set = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00,
                  0x00, 0x00, 0x00},
  };

  auto ieee = HtOperation::FromDdk(ddk);
  EXPECT_EQ(123U, ieee.primary_channel);
  EXPECT_EQ(0x01020304U, ieee.head.val());
  EXPECT_EQ(0x05U, ieee.tail.val());
  std::array<uint8_t, 16> expected_mcs_set = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff,
                                              0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(expected_mcs_set, ieee.basic_mcs_set.val());

  auto ddk2 = ieee.ToDdk();
  EXPECT_EQ(ddk.primary_channel, ddk2.primary_channel);
  EXPECT_EQ(ddk.head, ddk2.head);
  EXPECT_EQ(ddk.tail, ddk2.tail);
  for (size_t i = 0; i < sizeof(ddk.mcs_set); i++) {
    EXPECT_EQ(ddk.mcs_set[i], ddk2.mcs_set[i]);
  }
}

TEST(VhtCapabilities, DdkConversion) {
  vht_capabilities_t ddk{
      .bytes =
          {
              0xdd, 0xcc, 0xbb, 0xaa,                         // VhtCapabilityInfo
              0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00  // SupportedVhtMcsAndNssSet
          },
  };

  auto ieee = VhtCapabilities::FromDdk(ddk);
  EXPECT_EQ(0xaabbccddU, ieee.vht_cap_info.as_uint32());
  EXPECT_EQ(0x0011223344556677U, ieee.vht_mcs_nss.as_uint64());

  auto ddk2 = ieee.ToDdk();
  for (size_t i = 0; i < sizeof(ddk.bytes); i++) {
    EXPECT_EQ(ddk.bytes[i], ddk2.bytes[i]);
  }
}

TEST(VhtOperation, DdkConversion) {
  wlan_vht_op ddk{
      .vht_cbw = 0x01,
      .center_freq_seg0 = 42,
      .center_freq_seg1 = 106,
      .basic_mcs = 0x1122,
  };

  auto ieee = VhtOperation::FromDdk(ddk);
  EXPECT_EQ(0x01U, ieee.vht_cbw);
  EXPECT_EQ(42U, ieee.center_freq_seg0);
  EXPECT_EQ(106U, ieee.center_freq_seg1);
  EXPECT_EQ(0x1122U, ieee.basic_mcs.val());

  auto ddk2 = ieee.ToDdk();
  EXPECT_EQ(ddk.vht_cbw, ddk2.vht_cbw);
  EXPECT_EQ(ddk.center_freq_seg0, ddk2.center_freq_seg0);
  EXPECT_EQ(ddk.center_freq_seg1, ddk2.center_freq_seg1);
  EXPECT_EQ(ddk.basic_mcs, ddk2.basic_mcs);
}

TEST(SupportedRate, Create) {
  SupportedRate rate = {};
  ASSERT_EQ(rate.rate(), 0);
  ASSERT_EQ(rate.is_basic(), 0);

  // Create a rate with basic bit set.
  rate = SupportedRate(0xF9);
  ASSERT_EQ(rate.rate(), 0x79);
  ASSERT_EQ(rate.is_basic(), 1);

  // Create a rate with basic bit set but explicitly override basic setting.
  rate = SupportedRate(0xF9, false);
  ASSERT_EQ(rate.rate(), 0x79);
  ASSERT_EQ(rate.is_basic(), 0);

  // Create a rate with explicitly setting basic bit.
  rate = SupportedRate::basic(0x79);
  ASSERT_EQ(rate.rate(), 0x79);
  ASSERT_EQ(rate.is_basic(), 1);
}

TEST(SupportedRate, ToUint8) {
  SupportedRate rate = {};
  ASSERT_EQ(static_cast<uint8_t>(rate), 0);

  rate = SupportedRate(0xF9);
  ASSERT_EQ(static_cast<uint8_t>(rate), 0xF9);

  rate = SupportedRate::basic(0x79);
  ASSERT_EQ(static_cast<uint8_t>(rate), 0xF9);
}

TEST(SupportedRate, Compare) {
  // Ignore basic bit when comparing rates.
  SupportedRate rate1(0x79);
  SupportedRate rate2(0xF9);
  ASSERT_TRUE(rate1 == rate2);
  ASSERT_FALSE(rate1 != rate2);
  ASSERT_FALSE(rate1 < rate2);
  ASSERT_FALSE(rate1 > rate2);

  // Test smaller.
  rate1 = SupportedRate(0x78);
  rate2 = SupportedRate(0xF9);
  ASSERT_FALSE(rate1 == rate2);
  ASSERT_TRUE(rate1 != rate2);
  ASSERT_TRUE(rate1 < rate2);
  ASSERT_FALSE(rate1 > rate2);

  // Test larger.
  rate1 = SupportedRate(0x7A);
  rate2 = SupportedRate(0xF9);
  ASSERT_FALSE(rate1 == rate2);
  ASSERT_TRUE(rate1 != rate2);
  ASSERT_FALSE(rate1 < rate2);
  ASSERT_TRUE(rate1 > rate2);
}

struct RateVector {
  std::vector<SupportedRate> ap;
  std::vector<SupportedRate> client;
  std::vector<SupportedRate> want;
};

TEST(Intersector, IntersectRates) {
  // Rates are in 0.5Mbps increment: 12 -> 6 Mbps, 11 -> 5.5 Mbps, etc.
  std::vector<RateVector> list = {
      {{}, {}, {}},
      {{SupportedRate(12)}, {SupportedRate(12)}, {SupportedRate(12)}},
      {{SupportedRate::basic(12)}, {SupportedRate(12)}, {SupportedRate::basic(12)}},
      {{SupportedRate(12)}, {SupportedRate::basic(12)}, {SupportedRate(12)}},
      {{SupportedRate::basic(12)}, {}, {}},
      {{}, {SupportedRate::basic(12)}, {}},
      {{SupportedRate(12)}, {}, {}},
      {{}, {SupportedRate(12)}, {}},
      {{SupportedRate::basic(12), SupportedRate(24)},
       {SupportedRate::basic(24), SupportedRate(12)},
       {SupportedRate::basic(12), SupportedRate(24)}},
      {{SupportedRate(24), SupportedRate::basic(12)},
       {SupportedRate(12), SupportedRate::basic(24)},
       {SupportedRate::basic(12), SupportedRate(24)}},
      {{SupportedRate(72), SupportedRate::basic(108), SupportedRate::basic(96)},
       {SupportedRate(96)},
       {SupportedRate::basic(96)}},
      {{SupportedRate(72), SupportedRate::basic(108), SupportedRate::basic(96)},
       {SupportedRate::basic(72)},
       {SupportedRate(72)}},
  };

  for (auto vec : list) {
    auto got = IntersectRatesAp(vec.ap, vec.client);
    EXPECT_EQ(vec.want, got);
    for (size_t i = 0; i < got.size(); ++i) {
      EXPECT_EQ(vec.want[i].val(), got[i].val());
    }
  }
}

}  // namespace
}  // namespace wlan
