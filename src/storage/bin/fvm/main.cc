// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <lib/fit/defer.h>
#include <lib/stdcompat/span.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fbl/alloc_checker.h>
#include <safemath/checked_math.h>

#include "fbl/unique_fd.h"
#include "range/interval-tree.h"
#include "src/storage/blobfs/format.h"
#include "src/storage/fvm/sparse_reader.h"
#include "src/storage/minfs/format.h"
#include "src/storage/volume_image/adapter/commands.h"
#include "src/storage/volume_image/ftl/ftl_image.h"
#include "src/storage/volume_image/ftl/ftl_raw_nand_image_writer.h"
#include "src/storage/volume_image/ftl/options.h"
#include "src/storage/volume_image/ftl/raw_nand_image.h"
#include "src/storage/volume_image/ftl/raw_nand_image_utils.h"
#include "src/storage/volume_image/fvm/fvm_sparse_image.h"
#include "src/storage/volume_image/fvm/fvm_sparse_image_reader.h"
#include "src/storage/volume_image/fvm/fvm_unpack.h"
#include "src/storage/volume_image/options.h"
#include "src/storage/volume_image/utils/decompressor.h"
#include "src/storage/volume_image/utils/fd_reader.h"
#include "src/storage/volume_image/utils/fd_writer.h"

enum DiskType {
  File = 0,
  Mtd = 1,
  BlockDevice = 2,
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc99-designator"
const char* kDiskTypeStr[] = {
    [DiskType::File] = "file",
    [DiskType::Mtd] = "mtd",
    [DiskType::BlockDevice] = "block_device",
};
#pragma GCC diagnostic pop

int usage(void) {
  fprintf(stderr, "usage: fvm [ output_path ] [ command ] [ <flags>* ] [ <input_paths>* ]\n");
  fprintf(stderr, "fvm performs host-side FVM and sparse file creation\n");
  fprintf(stderr, "Commands:\n");
  fprintf(stderr, " create : Creates an FVM partition\n");
  fprintf(stderr,
          " extend : Extends an FVM container to the specified size (length is"
          " required)\n");
  fprintf(stderr,
          " ftl-raw-nand: converts the input fvm.sparse.blk into a FTL Raw Nand Image (--sparse is "
          "required).\n");
  fprintf(stderr, " sparse : Creates a sparse file. One or more input paths are required.\n");
  fprintf(stderr, " pave : Creates an FVM container from a sparse file.\n");
  fprintf(stderr,
          " check : verifies that the |--sparse| image provided is valid. if |--max_disk_size| is "
          "provided check that the maximum disk size is set to such value in the sparse image.\n");
  fprintf(stderr,
          " size : Prints the minimum size required in order to pave a sparse file."
          " If the --disk flag is provided, instead checks that the paved sparse file"
          " will fit within a disk of this size. On success, no information is"
          " outputted\n");
  fprintf(stderr,
          " decompress : Decompresses a compressed sparse/raw file. --sparse/lz4/default input "
          "path is required. If option is set to --default, the tool will attempt to detect the "
          "input format\n");
  fprintf(stderr,
          " unpack : Unpacks an input raw fvm image where the output path is used as a prefix for "
          "output files for partitions by appending the partition name. Resulting output paths "
          "will have dashes replaced with underscores, and duplicate names will get an additional "
          "dash and numerical suffix.\n");
  fprintf(stderr, "Flags (neither or both of offset/length must be specified):\n");
  fprintf(stderr, " --slice [bytes] - specify slice size - only valid on container creation.\n");
  fprintf(stderr,
          " --max-disk-size [bytes] Used for preallocating metadata. Only valid for sparse image. "
          "(defaults to 0)\n");
  fprintf(stderr, " --offset [bytes] - offset at which container begins (fvm only)\n");
  fprintf(stderr, " --length [bytes] - length of container within file (fvm only)\n");
  fprintf(stderr,
          " --compress [type] - specify that file should be compressed (sparse and android sparse "
          "image only). Currently, the only supported type is \"lz4\".\n");
  fprintf(stderr, " --disk [bytes] - Size of target disk (valid for size command only)\n");
  fprintf(stderr, " --disk-type [%s, %s OR %s] - Type of target disk (pave only)\n",
          kDiskTypeStr[DiskType::File], kDiskTypeStr[DiskType::Mtd],
          kDiskTypeStr[DiskType::BlockDevice]);
  fprintf(stderr, " --max-bad-blocks [number] - Max bad blocks for FTL (pave on mtd only)\n");
  fprintf(stderr, "Input options:\n");
  fprintf(stderr, " --blob [path] [reserve options] - Add path as blob type (must be blobfs)\n");
  fprintf(stderr,
          " --data [path] [reserve options] - Add path as encrypted data type (must"
          " be minfs)\n");
  fprintf(stderr, " --data-unsafe [path] - Add path as unencrypted data type (must be minfs)\n");
  fprintf(stderr, " --system [path] - Add path as system type (must be minfs)\n");
  fprintf(stderr, " --default [path] - Add generic path\n");
  fprintf(stderr, " --sparse [path] - Path to compressed sparse file\n");
  fprintf(stderr, " --lz4 [path] - Path to lz4 compressed raw file\n");
  fprintf(stderr, " --raw [path] - Path to raw fvm image file\n");
  fprintf(stderr,
          " --resize-image-file-to-fit - When used with create/extend command, the output image "
          "file will "
          "be resized to just fit the metadata header and added partitions. Disk size specified in "
          "the header remains the same. It's useful for reducing the size of the image file for "
          "flashing\n");
  fprintf(stderr,
          " --android-sparse-format - When used with create command, the image will be converted "
          "to android sparse image.\n");
  fprintf(
      stderr,
      " --length-is-lowerbound - When used with extend command, if current disk size is already "
      "no smaller than the specified size, the command will be no-op. If the option is not "
      "specified, it will error out in this case.\n");
  fprintf(stderr, "reserve options:\n");
  fprintf(stderr,
          " These options, on success, reserve additional fvm slices for data/inodes.\n"
          " The number of bytes reserved may exceed the actual bytes needed due to\n"
          " rounding up to slice boundary.\n");
  fprintf(stderr,
          " --minimum-inodes inode_count - number of inodes to reserve\n"
          "                                Blobfs inode size is %" PRIu64
          "\n"
          "                                Minfs inode size is %" PRIu32 "\n",
          blobfs::kBlobfsInodeSize, minfs::kMinfsInodeSize);

  fprintf(stderr,
          " --minimum-data-bytes data_bytes - number of bytes to reserve for data\n"
          "                                   in the fs\n"
          "                                   Blobfs block size is %" PRIu64
          "\n"
          "                                   Minfs block size is %" PRIu32 "\n",
          blobfs::kBlobfsBlockSize, minfs::kMinfsBlockSize);
  fprintf(stderr,
          " --maximum-bytes bytes - Places an upper bound of <bytes> on the total\n"
          "                         number of bytes which may be used by the partition.\n"
          "                         Returns an error if more space is necessary to\n"
          "                         create the requested filesystem.\n");
  fprintf(stderr,
          " --with-empty-data    - Adds a placeholder partition that will be formatted on boot,\n"
          "                         to minfs/fxfs. The partition will be the 'data' partition.\n");
  fprintf(stderr,
          " --with-empty-account-partition - Adds a placeholder partition with a label of \n"
          "                                  'account'. This will be formatted on account \n"
          "                                  creation to minfs.\n");
  fprintf(
      stderr,
      "   --nand-page-size : Sets the hardware page size in bytes used by the targetted device.\n");
  fprintf(stderr,
          "   --nand-oob-size : Sets the hardware page oob size in bytes used by the targetted "
          "device.\n");
  fprintf(stderr,
          "   --nand-pages-per-block : Sets the number of pages per block in the device.\n");
  fprintf(stderr, "   --nand-block-count : Sets the number of blocks in the device.\n");
  exit(-1);
}

int parse_size(const char* size_str, size_t* out) {
  char* end;
  size_t size = strtoull(size_str, &end, 10);

  switch (end[0]) {
    case 'K':
    case 'k':
      size *= 1024;
      end++;
      break;
    case 'M':
    case 'm':
      size *= static_cast<size_t>(1024) * 1024;
      end++;
      break;
    case 'G':
    case 'g':
      size *= static_cast<size_t>(1024) * 1024 * 1024;
      end++;
      break;
  }

  if (end[0] || size == 0) {
    fprintf(stderr, "Bad size: %s\n", size_str);
    return -1;
  }

  *out = size;
  return 0;
}

class RawBlockImageWriter final : public storage::volume_image::Writer {
 public:
  explicit RawBlockImageWriter(storage::volume_image::Writer* writer) : writer_(writer) {}

  fpromise::result<void, std::string> Write(uint64_t offset,
                                            cpp20::span<const uint8_t> buffer) final {
    ranges_.insert(range::Range<>(offset, offset + buffer.size()));
    return writer_->Write(offset, buffer);
  }

  fpromise::result<void, std::string> VisitGaps(fit::function<fpromise::result<void, std::string>(
                                                    uint64_t start, uint64_t end, Writer* writer)>
                                                    visitor) {
    uint64_t last_gap_end = 0;
    for (const auto& range : ranges_) {
      if (range.second.Start() > last_gap_end) {
        auto visit_result = visitor(last_gap_end, range.second.Start(), writer_);
        if (visit_result.is_error()) {
          return visit_result.take_error_result();
        }
      }
      last_gap_end = range.second.End();
    }
    return fpromise::ok();
  }

 private:
  // Keep track of written ranges.
  range::IntervalTree<range::Range<>> ranges_;

  storage::volume_image::Writer* writer_ = nullptr;
};

size_t get_disk_size(const char* path, size_t offset) {
  fbl::unique_fd fd(open(path, O_RDONLY, 0644));

  if (fd) {
    struct stat s;
    if (fstat(fd.get(), &s) < 0) {
      fprintf(stderr, "Failed to stat %s\n", path);
      exit(-1);
    }

    return s.st_size - offset;
  }

  return 0;
}

zx_status_t ParseDiskType(const char* type_str, DiskType* out) {
  if (!strcmp(type_str, kDiskTypeStr[DiskType::File])) {
    *out = DiskType::File;
    return ZX_OK;
  } else if (!strcmp(type_str, kDiskTypeStr[DiskType::Mtd])) {
    *out = DiskType::Mtd;
    return ZX_OK;
  } else if (!strcmp(type_str, kDiskTypeStr[DiskType::BlockDevice])) {
    *out = DiskType::BlockDevice;
    return ZX_OK;
  }

  fprintf(stderr, "Unknown disk type: '%s'.\n", type_str);
  return ZX_ERR_INVALID_ARGS;
}

zx_status_t CopyFile(const char* dst, const char* src) {
  constexpr size_t kBufferLength{static_cast<size_t>(1024) * 1024};
  fbl::unique_fd fd_src(open(src, O_RDONLY, 0644));
  if (!fd_src) {
    fprintf(stderr, "Unable to open source file %s\n", src);
    return ZX_ERR_IO;
  }

  fbl::unique_fd fd_dst(open(dst, O_RDWR | O_CREAT, 0644));
  if (!fd_dst) {
    fprintf(stderr, "Unable to create output file %s\n", dst);
    return ZX_ERR_IO;
  }

  std::vector<uint8_t> buffer(kBufferLength);
  while (true) {
    ssize_t read_bytes = read(fd_src.get(), buffer.data(), kBufferLength);
    if (read_bytes < 0) {
      fprintf(stderr, "Failed to read data from image file\n");
      return ZX_ERR_IO;
    } else if (read_bytes == 0) {
      break;
    }

    if (write(fd_dst.get(), buffer.data(), read_bytes) != read_bytes) {
      fprintf(stderr, "BlockReader: failed to write to output\n");
      return ZX_ERR_IO;
    }
  }
  return ZX_OK;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return EXIT_FAILURE;
  }

  std::vector<std::string_view> arguments;
  arguments.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    arguments.push_back(argv[i]);
  }

  int i = 1;
  const char* path = argv[i++];     // Output path
  const char* command = argv[i++];  // Command
  if (strcmp(path, "check") == 0) {
    command = path;
    i--;
  }

  size_t length = 0;
  size_t offset = 0;
  size_t disk_size = 0;

  size_t max_disk_size = 0;
  bool is_max_bad_blocks_set = false;
  DiskType disk_type = DiskType::File;

  size_t block_count = 0;
  storage::volume_image::RawNandOptions options;

  while (i < argc) {
    // Not all arguments are parsed here; some of them get parsed below and some are passed through
    // to the volume image library and get parsed there (e.g. the --sparse argument).
    if (!strcmp(argv[i], "--offset") && i + 1 < argc) {
      if (parse_size(argv[++i], &offset) < 0) {
        return EXIT_FAILURE;
      }
    } else if (!strcmp(argv[i], "--length") && i + 1 < argc) {
      if (parse_size(argv[++i], &length) < 0) {
        return EXIT_FAILURE;
      }
    } else if (!strcmp(argv[i], "--compress")) {
      if (!strcmp(argv[++i], "lz4")) {
        // This flag does nothing.
      } else {
        fprintf(stderr, "Invalid compression type\n");
        return EXIT_FAILURE;
      }
    } else if (!strcmp(argv[i], "--disk-type")) {
      if (ParseDiskType(argv[++i], &disk_type) != ZX_OK) {
        return EXIT_FAILURE;
      }
    } else if (!strcmp(argv[i], "--max-bad-blocks")) {
      is_max_bad_blocks_set = true;
    } else if (!strcmp(argv[i], "--disk")) {
      if (parse_size(argv[++i], &disk_size) < 0) {
        return EXIT_FAILURE;
      }
    } else if (!strcmp(argv[i], "--max-disk-size") && i + 1 < argc) {
      if (parse_size(argv[++i], &max_disk_size) < 0) {
        return EXIT_FAILURE;
      }
    } else if (!strcmp(argv[i], "--resize-image-file-to-fit")) {
      // This flag does nothing.
    } else if (!strcmp(argv[i], "--length-is-lowerbound")) {
      // This flag does nothing.
    } else if (!strcmp(argv[i], "--android-sparse-format")) {
      // This flag does not do anything.
    } else if (!strcmp(argv[i], "--nand-page-size")) {
      size_t page_size = 0;
      if (parse_size(argv[++i], &page_size) < 0) {
        return EXIT_FAILURE;
      }
      options.page_size = static_cast<uint64_t>(page_size);
    } else if (!strcmp(argv[i], "--nand-oob-size")) {
      size_t oob_bytes_size = 0;
      if (parse_size(argv[++i], &oob_bytes_size) < 0) {
        return EXIT_FAILURE;
      }
      if (oob_bytes_size > std::numeric_limits<uint8_t>::max()) {
        fprintf(stderr, "OOB Byte size must lower than 256 bytes.\n");
        return EXIT_FAILURE;
      }
      options.oob_bytes_size = static_cast<uint8_t>(oob_bytes_size);
    } else if (!strcmp(argv[i], "--nand-pages-per-block")) {
      size_t pages_per_block = 0;
      if (parse_size(argv[++i], &pages_per_block) < 0) {
        return EXIT_FAILURE;
      }
      if (pages_per_block > std::numeric_limits<uint32_t>::max()) {
        fprintf(stderr, "Pages Per Block must be lower than 4,294,967,296.\n");
        return EXIT_FAILURE;
      }
      options.pages_per_block = static_cast<uint32_t>(pages_per_block);
    } else if (!strcmp(argv[i], "--nand-block-count")) {
      if (parse_size(argv[++i], &block_count) < 0) {
        return EXIT_FAILURE;
      }
    } else {
      break;
    }

    ++i;
  }

  if (strcmp(command, "check") == 0) {
    const char* input_path;
    // For convenience, allow output path to be used as an input path.
    if (command == path) {
      if (argc != i + 2) {
        usage();
        return EXIT_FAILURE;
      }
      char* input_type = argv[i++];
      if (strcmp(input_type, "--sparse") != 0) {
        usage();
        return EXIT_FAILURE;
      }
      input_path = argv[i++];
    } else {
      if (argc != i) {
        usage();
        return EXIT_FAILURE;
      }
      input_path = path;
    }
    // Temporary usage of internal symbols.
    auto sparse_image_reader_or = storage::volume_image::FdReader::Create(input_path);
    if (sparse_image_reader_or.is_error()) {
      fprintf(stderr, "%s\n", sparse_image_reader_or.error().c_str());
      return EXIT_FAILURE;
    }

    auto header_or =
        storage::volume_image::fvm_sparse_internal::GetHeader(0, sparse_image_reader_or.value());
    if (header_or.is_error()) {
      fprintf(stderr, "Failed to parse sparse image header. %s\n", header_or.error().c_str());
      return EXIT_FAILURE;
    }
    auto header = header_or.take_value();

    if (max_disk_size != 0 && header.maximum_disk_size != max_disk_size) {
      fprintf(stderr, "Sparse image does not match max disk size. Found %lu, expected %lu.\n",
              static_cast<size_t>(header.maximum_disk_size), max_disk_size);
      return EXIT_FAILURE;
    }

    auto partitions_or = storage::volume_image::fvm_sparse_internal::GetPartitions(
        sizeof(header), sparse_image_reader_or.value(), header);
    if (partitions_or.is_error()) {
      fprintf(stderr, "Failed to parse sparse image partition metadata. %s\n",
              partitions_or.error().c_str());
      return EXIT_FAILURE;
    }
    auto partitions = partitions_or.take_value();

    uint64_t expected_data_length = 0;
    uint64_t total_size = sparse_image_reader_or.value().length();
    for (const auto& partition : partitions) {
      for (const auto& extent : partition.extents) {
        expected_data_length += extent.extent_length;
      }
    }

    auto compression_options =
        storage::volume_image::fvm_sparse_internal::GetCompressionOptions(header);
    // Decompress the image.
    if (compression_options.schema != storage::volume_image::CompressionSchema::kNone) {
      auto reader_or = storage::volume_image::FdReader::Create(input_path);
      std::string tmp_path = std::filesystem::temp_directory_path().generic_string() +
                             "/decompressed_sparse_fvm_XXXXXX";
      fbl::unique_fd created_file(mkstemp(tmp_path.data()));
      if (!created_file.is_valid()) {
        fprintf(stderr, "Failed to create temporary file for decompressing image. %s\n",
                strerror(errno));
        return EXIT_FAILURE;
      }
      auto cleanup = fit::defer([&]() { unlink(tmp_path.c_str()); });

      auto writer = storage::volume_image::FdWriter(std::move(created_file));
      auto decompress_or =
          storage::volume_image::FvmSparseDecompressImage(0, reader_or.value(), writer);
      if (decompress_or.is_error()) {
        std::cerr << decompress_or.error();
        return EXIT_FAILURE;
      }

      auto decompressed_reader_or = storage::volume_image::FdReader::Create(tmp_path);
      if (reader_or.is_error()) {
        fprintf(stderr, "%s\n", reader_or.error().c_str());
        return EXIT_FAILURE;
      }
      total_size = decompressed_reader_or.value().length() - header.header_length;
    }

    if (expected_data_length > total_size) {
      fprintf(stderr,
              "Extent accumulated length is %" PRIu64 ", uncompressed data is %" PRIu64 "\n",
              expected_data_length, total_size);
      return EXIT_FAILURE;
    }

    fprintf(stderr, "Sparse input file is a valid FVM Sparse Image.\n");
    return EXIT_SUCCESS;
  }

  if (!strcmp(command, "ftl-raw-nand")) {
    if (argc != i + 2) {
      fprintf(stderr, "Invalid arguments for 'ftl-raw-nand' command.\n");
      return EXIT_FAILURE;
    }

    char* input_type = argv[i];
    char* input_path = argv[i + 1];

    if (strcmp(input_type, "--sparse") != 0) {
      usage();
      return EXIT_FAILURE;
    }

    if (options.page_size == 0) {
      fprintf(stderr, "Raw Nand device page size must be greater than zero.\n");
      return EXIT_FAILURE;
    }

    if (options.oob_bytes_size == 0) {
      fprintf(stderr, "Raw Nand device page oob size must be greater than zero.\n");
      return EXIT_FAILURE;
    }

    if (options.pages_per_block == 0) {
      fprintf(stderr, "Raw Nand device pages per block must be greater than zero.\n");
      return EXIT_FAILURE;
    }

    if (block_count == 0) {
      fprintf(stderr, "Raw Nand device block count must be greater than zero.\n");
      return EXIT_FAILURE;
    }

    options.page_count = safemath::CheckMul<uint32_t>(safemath::checked_cast<uint32_t>(block_count),
                                                      options.pages_per_block)
                             .ValueOrDie();

    auto sparse_image_reader_or = storage::volume_image::FdReader::Create(input_path);
    if (sparse_image_reader_or.is_error()) {
      fprintf(stderr, "%s\n", sparse_image_reader_or.error().c_str());
      return EXIT_FAILURE;
    }

    // The FTL writer intentionally leaves existing content in place when
    // opening a file, so we need to delete the output file first so that any
    // existing file data doesn't carry over - in particular, if the existing
    // NAND image is larger than the one we're about to generate, the excess
    // data would be left in-place, corrupting the FTL metadata.
    {
      fbl::unique_fd ftl_output(open(path, O_CREAT | O_RDWR, 0644));
      if (!ftl_output.is_valid()) {
        fprintf(stderr, "Failed to create output path. Error %s.\n", strerror(errno));
        return EXIT_FAILURE;
      }
      if (ftruncate(ftl_output.get(), 0) != 0) {
        fprintf(stderr, "Failed to truncate output path. Error %s.\n", strerror(errno));
        return EXIT_FAILURE;
      }
    }

    auto sparse_image_reader = sparse_image_reader_or.take_value();
    auto ftl_image_writer_or = storage::volume_image::FdWriter::Create(path);
    if (ftl_image_writer_or.is_error()) {
      fprintf(stderr, "%s\n", ftl_image_writer_or.error().c_str());
      return EXIT_FAILURE;
    }
    auto ftl_image_writer = ftl_image_writer_or.take_value();
    RawBlockImageWriter raw_writer(&ftl_image_writer);

    std::optional<uint64_t> max_disk_size_opt = std::nullopt;
    if (max_disk_size != 0) {
      max_disk_size_opt = max_disk_size;
    }

    auto fvm_partition_or =
        storage::volume_image::OpenSparseImage(sparse_image_reader, max_disk_size_opt);
    if (fvm_partition_or.is_error()) {
      fprintf(stderr, "%s\n", fvm_partition_or.error().c_str());
      return EXIT_FAILURE;
    }

    std::vector<storage::volume_image::RawNandImageFlag> flags = {
        storage::volume_image::RawNandImageFlag::kRequireWipeBeforeFlash};
    auto raw_nand_image_writer_or = storage::volume_image::FtlRawNandImageWriter::Create(
        options, flags, storage::volume_image::ImageFormat::kRawImage, &raw_writer);
    if (raw_nand_image_writer_or.is_error()) {
      fprintf(stderr, "%s\n", raw_nand_image_writer_or.error().c_str());
      return EXIT_FAILURE;
    }

    auto [raw_nand_image_writer, ftl_options] = raw_nand_image_writer_or.take_value();
    auto ftl_image_write_result = storage::volume_image::FtlImageWrite(
        ftl_options, fvm_partition_or.value(), &raw_nand_image_writer);
    if (ftl_image_write_result.is_error()) {
      fprintf(stderr, "%s\n", ftl_image_write_result.error().c_str());
      return EXIT_FAILURE;
    }

    // Write in the gaps in the image with 0xFF or 'unwritten' bytes.
    // For a raw image there may not be any gaps.
    std::vector<uint8_t> filler(4 << 10, 0xFF);
    auto fill_result = raw_writer.VisitGaps(
        [&filler](uint64_t start, uint64_t end,
                  storage::volume_image::Writer* writer) -> fpromise::result<void, std::string> {
          uint64_t length = end - start;
          if (filler.size() < length) {
            filler.resize(length, 0xFF);
          }
          return writer->Write(start, cpp20::span<const uint8_t>(filler).subspan(0, length));
        });

    if (fill_result.is_error()) {
      fprintf(stderr, "%s\n", fill_result.error().c_str());
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  }

  // If length was not specified, use remainder of file after offset.
  // get_disk_size may return 0 for block devices' behavior with fstat.
  // This scenario is checked in the pave section below.
  if (length == 0 && disk_type == DiskType::File) {
    length = get_disk_size(path, offset);
  }

  if (disk_type == DiskType::Mtd || disk_type == DiskType::BlockDevice) {
    if (strcmp(command, "pave")) {
      fprintf(stderr, "Only the pave command is supported for disk type %s.\n",
              kDiskTypeStr[disk_type]);
      return EXIT_FAILURE;
    }

    if (!is_max_bad_blocks_set && disk_type == DiskType::Mtd) {
      fprintf(stderr, "--max-bad-blocks is required when paving to MTD.\n");
      return EXIT_FAILURE;
    }
  }

  if (!strcmp(command, "create")) {
    auto create_params_or = storage::volume_image::CreateParams::FromArguments(arguments);
    if (create_params_or.is_error()) {
      std::cerr << create_params_or.error() << std::endl;
      return EXIT_FAILURE;
    }

    if (auto result = storage::volume_image::Create(create_params_or.value()); result.is_error()) {
      std::cerr << result.error() << std::endl;
      return EXIT_FAILURE;
    }
  } else if (!strcmp(command, "extend")) {
    auto extend_params_or = storage::volume_image::ExtendParams::FromArguments(arguments);
    if (extend_params_or.is_error()) {
      std::cerr << extend_params_or.error() << std::endl;
      return EXIT_FAILURE;
    }

    if (auto extend_res = Extend(extend_params_or.value()); extend_res.is_error()) {
      std::cerr << extend_res.error() << std::endl;
      return EXIT_FAILURE;
    }
  } else if (!strcmp(command, "sparse")) {
    auto create_params_or = storage::volume_image::CreateParams::FromArguments(arguments);
    if (create_params_or.is_error()) {
      std::cerr << create_params_or.error() << std::endl;
      return EXIT_FAILURE;
    }

    if (auto result = storage::volume_image::Create(create_params_or.value()); result.is_error()) {
      std::cerr << result.error() << std::endl;
      return EXIT_FAILURE;
    }
  } else if (!strcmp(command, "decompress")) {
    if (argc - i != 2) {
      usage();
      return EXIT_FAILURE;
    }

    const char* input_type = argv[i];
    const char* input_path = argv[i + 1];

    if (!strcmp(input_type, "--default")) {
      // Look at the magic values and update input_type to match the right one.
      auto reader_or = storage::volume_image::FdReader::Create(input_path);
      if (reader_or.is_error()) {
        std::cerr << "Failed to read image. " << reader_or.error() << std::endl;
        return EXIT_FAILURE;
      }

      // Check the first 16 bytes of the file to try and figure out the input type.
      std::array<uint8_t, 16> magic_bytes = {};
      reader_or.value().Read(0, magic_bytes);
      constexpr uint32_t kLZ4Magic = 0x184D2204;

      if (memcmp(magic_bytes.data(), &fvm::kMagic, sizeof(fvm::kMagic)) == 0) {
        input_type = "--raw";
      } else if (memcmp(magic_bytes.data(), &kLZ4Magic, sizeof(kLZ4Magic)) == 0) {
        input_type = "--lz4";
      } else if (memcmp(magic_bytes.data(), &fvm::kSparseFormatMagic,
                        sizeof(fvm::kSparseFormatMagic)) == 0) {
        input_type = "--sparse";
      }
    }

    if (!strcmp(input_type, "--sparse")) {
      auto sparse_image_reader_or = storage::volume_image::FdReader::Create(input_path);
      if (sparse_image_reader_or.is_error()) {
        std::cerr << "Failed to read image. " << sparse_image_reader_or.error() << std::endl;
        return EXIT_FAILURE;
      }

      auto header_or =
          storage::volume_image::fvm_sparse_internal::GetHeader(0, sparse_image_reader_or.value());
      if (header_or.is_error()) {
        std::cerr << "Failed to parse sparse image header. " << header_or.error() << std::endl;
        return EXIT_FAILURE;
      }
      auto header = header_or.take_value();

      auto compression_options =
          storage::volume_image::fvm_sparse_internal::GetCompressionOptions(header);
      // Decompress the image.
      if (compression_options.schema != storage::volume_image::CompressionSchema::kNone) {
        auto reader_or = storage::volume_image::FdReader::Create(input_path);
        auto writer_or = storage::volume_image::FdWriter::Create(path);
        if (writer_or.is_error()) {
          std::cerr << writer_or.error() << std::endl;
          return EXIT_FAILURE;
        }

        auto decompress_or = storage::volume_image::FvmSparseDecompressImage(0, reader_or.value(),
                                                                             writer_or.value());
        if (decompress_or.is_error()) {
          std::cerr << decompress_or.error();
          return EXIT_FAILURE;
        }
      }
    } else if (!strcmp(input_type, "--lz4")) {
      if (fvm::SparseReader::DecompressLZ4File(input_path, path) != ZX_OK) {
        return EXIT_FAILURE;
      }
    } else if (!strcmp(input_type, "--raw")) {
      if (CopyFile(path, input_path) != ZX_OK) {
        return EXIT_FAILURE;
      }
    } else {
      usage();
      return EXIT_FAILURE;
    }
  } else if (!strcmp(command, "size")) {
    auto size_params_or = storage::volume_image::SizeParams::FromArguments(arguments);
    if (size_params_or.is_error()) {
      std::cerr << size_params_or.error() << std::endl;
      return EXIT_FAILURE;
    }

    auto size_or = storage::volume_image::Size(size_params_or.value());
    if (size_or.is_error()) {
      std::cerr << size_or.error() << std::endl;
      return EXIT_FAILURE;
    }

    // If we are not doing a check of whether this image fits in a specified size, print the minimum
    // allocated size.
    if (!size_params_or.value().length.has_value()) {
      std::cout << size_or.value() << std::endl;
    }
  } else if (!strcmp(command, "pave")) {
    auto pave_params_or = storage::volume_image::PaveParams::FromArguments(arguments);
    if (pave_params_or.is_error()) {
      std::cerr << "Failed to parse pave params. " << pave_params_or.error() << std::endl;
      return EXIT_FAILURE;
    }

    if (auto result = storage::volume_image::Pave(pave_params_or.value()); result.is_error()) {
      std::cerr << "Failed to pave. " << result.error() << std::endl;
      return EXIT_FAILURE;
    }
  } else if (!strcmp(command, "unpack")) {
    if (argc - i != 1) {
      usage();
      return EXIT_FAILURE;
    }
    auto reader_or = storage::volume_image::FdReader::Create(arguments[i]);
    if (reader_or.is_error()) {
      fprintf(stderr, "%s\n", reader_or.take_error().c_str());
      return EXIT_FAILURE;
    }
    const auto& reader = reader_or.take_value();

    if (auto result = storage::volume_image::UnpackRawFvm(reader, path); result.is_error()) {
      fprintf(stderr, "Failed to unpack: %s\n", result.take_error().c_str());
      return EXIT_FAILURE;
    }
  } else {
    fprintf(stderr, "Unrecognized command: \"%s\"\n", command);
    usage();
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
