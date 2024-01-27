// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FSHOST_CONSTANTS_H_
#define SRC_STORAGE_FSHOST_CONSTANTS_H_

#include <string_view>

namespace fshost {

// These need to match whatever our imaging tools do.
constexpr std::string_view kBlobfsPartitionLabel = "blobfs";
constexpr std::string_view kDataPartitionLabel = "data";
constexpr std::string_view kLegacyDataPartitionLabel = "minfs";

// Binaries for data partition filesystems are expected to be at well known locations.
constexpr char kMinfsPath[] = "/pkg/bin/minfs";
constexpr char kFxfsPath[] = "/pkg/bin/fxfs";
constexpr char kF2fsPath[] = "/pkg/bin/f2fs";
constexpr char kFactoryfsPath[] = "/pkg/bin/factoryfs";

// These are default sizes of data partition.
constexpr uint64_t kDefaultMinfsMaxBytes = 24ull * 1024ull * 1024ull;
constexpr uint64_t kDefaultF2fsMinBytes = 100ull * 1024ull * 1024ull;

constexpr std::string_view kBlockDeviceClassPrefix = "/dev/class/block";
constexpr std::string_view kNandDeviceClassPrefix = "/dev/class/nand";

constexpr char kFVMDriverPath[] = "fvm.cm";
constexpr char kGPTDriverPath[] = "gpt.cm";
constexpr char kMBRDriverPath[] = "mbr.cm";
constexpr char kZxcryptDriverPath[] = "zxcrypt.cm";
constexpr char kBootpartDriverPath[] = "bootpart.cm";
constexpr char kBlockVerityDriverPath[] = "block-verity.cm";
constexpr char kNandBrokerDriverPath[] = "nand-broker.cm";

}  // namespace fshost

#endif  // SRC_STORAGE_FSHOST_CONSTANTS_H_
