// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/symbolizer/analytics.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace symbolizer {

namespace {

using ::analytics::google_analytics_4::Value;
using ::testing::ContainerEq;

TEST(AnalyticsTest, SymbolizationAnalyticsBuilder) {
  SymbolizationAnalyticsBuilder builder;
  builder.TotalTimerStart();
  builder.DownloadTimerStart();
  builder.SetAtLeastOneInvalidInput();
  builder.SetNumberOfModules(2);
  builder.SetNumberOfModulesWithLocalSymbols(3);
  builder.SetNumberOfModulesWithCachedSymbols(4);
  builder.SetNumberOfModulesWithDownloadedSymbols(5);
  builder.SetNumberOfModulesWithDownloadingFailure(6);
  builder.IncreaseNumberOfFrames();
  builder.IncreaseNumberOfFramesSymbolized();
  builder.IncreaseNumberOfFramesInvalid();
  builder.SetRemoteSymbolLookupEnabledBit(false);
  builder.DownloadTimerStop();
  builder.TotalTimerStop();

  auto parameters_ua = builder.BuildUaHit().parameters();
  ASSERT_GE(std::stoll(parameters_ua["utt"]), std::stoll(parameters_ua["cm11"]));
  ASSERT_GE(std::stoll(parameters_ua["cm11"]), 0LL);
  parameters_ua["utt"] = "100";
  parameters_ua["cm11"] = "50";

  const std::map<std::string, std::string> expected_result_ua{
      {"t", "timing"}, {"utc", "symbolization"},
      {"utv", ""},     {"cm1", "1"},
      {"cm2", "2"},    {"cm3", "3"},
      {"cm4", "4"},    {"cm5", "5"},
      {"cm6", "6"},    {"cm7", "1"},
      {"cm8", "1"},    {"cm9", "1"},
      {"cm10", "0"},   {"cm11", "50"},
      {"utt", "100"}};
  EXPECT_THAT(parameters_ua, ContainerEq(expected_result_ua));

  auto event = builder.BuildGa4Event();
  EXPECT_EQ(event->name(), "symbolize");
  ASSERT_TRUE(event->parameters_opt().has_value());
  auto parameters = *(event->parameters_opt());
  ASSERT_GE(parameters["time_total_ms"], parameters["time_download_ms"]);
  ASSERT_GE(std::get<int64_t>(parameters["time_download_ms"]), 0LL);
  parameters["time_total_ms"] = 100;
  parameters["time_download_ms"] = 50;

  const std::map<std::string, Value> expected_result{{"has_invalid_input", true},
                                                     {"num_modules", 2},
                                                     {"num_modules_local", 3},
                                                     {"num_modules_cached", 4},
                                                     {"num_modules_downloaded", 5},
                                                     {"num_modules_download_fail", 6},
                                                     {"num_frames", 1},
                                                     {"num_frames_symbolized", 1},
                                                     {"num_frames_invalid", 1},
                                                     {"remote_symbol_enabled", false},
                                                     {"time_download_ms", 50},
                                                     {"time_total_ms", 100}};
  EXPECT_THAT(parameters, ContainerEq(expected_result));
}

}  // namespace

}  // namespace symbolizer
