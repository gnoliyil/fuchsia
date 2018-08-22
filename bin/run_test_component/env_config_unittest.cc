// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/run_test_component/env_config.h"

#include <string>

#include "gtest/gtest.h"
#include "lib/fxl/files/directory.h"
#include "lib/fxl/files/file.h"
#include "lib/fxl/files/scoped_temp_dir.h"
#include "lib/fxl/strings/string_printf.h"
#include "lib/fxl/strings/substitute.h"

namespace run {
namespace {

class EnvironmentConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE("", tmp_dir_.path()) << "Cannot acccess /tmp";
  }

  std::string NewJSONFile(const std::string& json) {
    std::string json_file;
    if (!tmp_dir_.NewTempFileWithData(json, &json_file)) {
      return "";
    }
    return json_file;
  }

 private:
  files::ScopedTempDir tmp_dir_;
};

TEST_F(EnvironmentConfigTest, InvalidJson) {
  const std::string json = R"JSON({
  "root": ["url1", "url3", "url5"]
  "sys": ["url2", "url4"]
  })JSON";
  const std::string file = NewJSONFile(json);
  auto config = run::EnvironmentConfig::CreateFromFile(file);
  EXPECT_TRUE(config.has_error());
  ASSERT_EQ(1u, config.errors().size());
}

TEST_F(EnvironmentConfigTest, EmptyJson) {
  const std::string json = R"JSON({
  })JSON";
  const std::string file = NewJSONFile(json);
  auto config = run::EnvironmentConfig::CreateFromFile(file);
  EXPECT_TRUE(config.has_error());
  ASSERT_EQ(2u, config.errors().size());
  ASSERT_EQ(0u, config.url_map().size());
}

TEST_F(EnvironmentConfigTest, NoRootElement) {
  const std::string json = R"JSON({
  "sys": ["url2", "url4"]
  })JSON";
  const std::string file = NewJSONFile(json);
  auto config = run::EnvironmentConfig::CreateFromFile(file);
  EXPECT_TRUE(config.has_error());
  ASSERT_EQ(1u, config.errors().size());
  ASSERT_EQ(2u, config.url_map().size());
}

TEST_F(EnvironmentConfigTest, NoSysElement) {
  const std::string json = R"JSON({
  "root": ["url2", "url4"]
  })JSON";
  const std::string file = NewJSONFile(json);
  auto config = run::EnvironmentConfig::CreateFromFile(file);
  EXPECT_TRUE(config.has_error());
  ASSERT_EQ(1u, config.errors().size());
  ASSERT_EQ(2u, config.url_map().size());
}

TEST_F(EnvironmentConfigTest, ValidConfig) {
  const std::string json = R"JSON({
  "root": ["url1", "url3", "url5"],
  "sys": ["url2", "url4"]
  })JSON";
  const std::string file = NewJSONFile(json);
  auto config = run::EnvironmentConfig::CreateFromFile(file);
  EXPECT_FALSE(config.has_error());
  ASSERT_EQ(0u, config.errors().size()) << config.errors()[0];
  ASSERT_EQ(5u, config.url_map().size());
  std::vector<std::string> root_urls = {"url1", "url3", "url5"};
  for (auto& url : root_urls) {
    auto map_entry = config.url_map().find(url);
    ASSERT_NE(map_entry, config.url_map().end());
    EXPECT_EQ(map_entry->second, run::EnvironmentType::ROOT);
  }

  std::vector<std::string> sys_urls = {"url2", "url4"};
  for (auto& url : sys_urls) {
    auto map_entry = config.url_map().find(url);
    ASSERT_NE(map_entry, config.url_map().end());
    EXPECT_EQ(map_entry->second, run::EnvironmentType::SYS);
  }
}

}  // namespace
}  // namespace run
