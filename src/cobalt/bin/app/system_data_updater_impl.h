// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_COBALT_BIN_APP_SYSTEM_DATA_UPDATER_IMPL_H_
#define SRC_COBALT_BIN_APP_SYSTEM_DATA_UPDATER_IMPL_H_

#include <fuchsia/cobalt/cpp/fidl.h>
#include <lib/inspect/cpp/inspect.h>
#include <stdlib.h>

#include "src/lib/fxl/macros.h"
#include "third_party/cobalt/src/system_data/system_data.h"

namespace cobalt {

class SystemDataUpdaterImpl : public fuchsia::cobalt::SystemDataUpdater {
 public:
  SystemDataUpdaterImpl(inspect::Node inspect_node, encoder::SystemDataInterface* system_data,
                        const std::string& cache_file_name_prefix);

  void SetSoftwareDistributionInfo(fuchsia::cobalt::SoftwareDistributionInfo current_info,
                                   SetSoftwareDistributionInfoCallback callback) override;

 private:
  // Persist stores the specified |value| to disk
  //
  // |suffix| Used with cache_file_name_prefix_ to construct the filename that
  // is written to.
  //
  // |value| The value to write to the file.
  void Persist(const std::string& suffix, const std::string& value);

  // Restore reads a value from disk
  //
  // |suffix| Used with cache_file_name_prefix_ to construct the filename that
  // is read from.
  //
  // Returns:
  //   - If the file exists and is readable, return the contents of the file
  //   - If not return the empty string
  std::string Restore(const std::string& suffix);

  // Deletes the specified data file from the disk.
  //
  // |suffix| Used with cache_file_name_prefix_ to construct the filename that
  // is to be deleted.
  void DeleteData(const std::string& suffix);

  // RestoreData restores all the SystemData values that were persisted to disk.
  void RestoreData();

 public:
  // ClearData deletes all the SystemData values that were persisted to disk.
  void ClearData();

 private:
  inspect::Node inspect_node_;
  inspect::IntProperty num_calls_;
  inspect::StringProperty channel_;

  encoder::SystemDataInterface* system_data_;  // Not owned.
  std::string cache_file_name_prefix_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SystemDataUpdaterImpl);
};

}  // namespace cobalt

#endif  // SRC_COBALT_BIN_APP_SYSTEM_DATA_UPDATER_IMPL_H_
