// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_TOOLS_BLOBFS_COMPRESSION_BLOBFS_COMPRESSION_H_
#define SRC_STORAGE_TOOLS_BLOBFS_COMPRESSION_BLOBFS_COMPRESSION_H_

#include <lib/zx/result.h>

#include <optional>
#include <string>
#include <vector>

#include <fbl/unique_fd.h>

#include "src/lib/chunked-compression/chunked-compressor.h"
#include "src/storage/blobfs/delivery_blob.h"

namespace blobfs_compress {

struct CompressionCliOptionStruct {
  std::string source_file;
  std::string compressed_file;
  std::optional<blobfs::DeliveryBlobType> type = std::nullopt;
  bool disable_size_alignment = false;

  fbl::unique_fd source_file_fd;
  fbl::unique_fd compressed_file_fd;
};

zx_status_t ValidateCliOptions(const CompressionCliOptionStruct& options);

zx_status_t BlobfsCompress(const uint8_t* src, size_t src_sz, uint8_t* dest_write_buf,
                           size_t* out_compressed_size,
                           chunked_compression::CompressionParams params,
                           const CompressionCliOptionStruct& cli_options);

zx::result<std::vector<uint8_t>> GenerateDeliveryBlob(cpp20::span<const uint8_t> data,
                                                      blobfs::DeliveryBlobType type);

}  // namespace blobfs_compress

#endif  // SRC_STORAGE_TOOLS_BLOBFS_COMPRESSION_BLOBFS_COMPRESSION_H_
