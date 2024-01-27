// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/cobalt/bin/app/system_data_updater_impl.h"

#include <lib/syslog/cpp/macros.h>

#include <cstdio>
#include <fstream>

namespace cobalt {

using FuchsiaStatus = fuchsia::cobalt::Status;

constexpr char kChannelCacheFilenameSuffix[] = "last_reported_channel";

SystemDataUpdaterImpl::SystemDataUpdaterImpl(inspect::Node inspect_node,
                                             encoder::SystemDataInterface* system_data,
                                             const std::string& cache_file_name_prefix)
    : inspect_node_(std::move(inspect_node)),
      system_data_(system_data),
      cache_file_name_prefix_(cache_file_name_prefix) {
  num_calls_ = inspect_node_.CreateInt("fidl_calls", 0);
  channel_ = inspect_node_.CreateString("channel", system_data_->channel());
  RestoreData();
}

void SystemDataUpdaterImpl::RestoreData() {
  std::string d = Restore(kChannelCacheFilenameSuffix);
  if (!d.empty()) {
    system_data_->SetChannel(d);
    channel_.Set(system_data_->channel());
  }
}

void SystemDataUpdaterImpl::ClearData() { DeleteData(kChannelCacheFilenameSuffix); }

std::string SystemDataUpdaterImpl::Restore(const std::string& suffix) {
  std::ifstream file(cache_file_name_prefix_ + suffix);
  if (!file) {
    return "";
  }
  std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  FX_LOGS(INFO) << "Restored `" << str << "` from `" << cache_file_name_prefix_ << suffix << "`";
  return str;
}

void SystemDataUpdaterImpl::Persist(const std::string& suffix, const std::string& value) {
  if (value.empty()) {
    return;
  }
  FX_LOGS(INFO) << "Writing `" << value << "` to `" << cache_file_name_prefix_ << suffix << "`";
  std::ofstream c(cache_file_name_prefix_ + suffix);
  c << value;
  c.close();
}

void SystemDataUpdaterImpl::DeleteData(const std::string& suffix) {
  if (std::remove((cache_file_name_prefix_ + suffix).c_str()) == 0) {
    FX_LOGS(INFO) << "Successfully deleted `" << cache_file_name_prefix_ << suffix << "`";
  }
}

void SystemDataUpdaterImpl::SetSoftwareDistributionInfo(
    fuchsia::cobalt::SoftwareDistributionInfo current_info,
    SetSoftwareDistributionInfoCallback callback) {
  num_calls_.Add(1);
  system_data::SoftwareDistributionInfo info;

  if (current_info.has_current_channel()) {
    const std::string& channel = current_info.current_channel();
    Persist(kChannelCacheFilenameSuffix, channel);
    FX_LOGS(INFO) << "Setting channel to `" << channel << "`";
    info.channel = channel;
  }

  system_data_->SetSoftwareDistributionInfo(info);
  channel_.Set(system_data_->channel());
  callback(FuchsiaStatus::OK);
}  // namespace cobalt

}  // namespace cobalt
