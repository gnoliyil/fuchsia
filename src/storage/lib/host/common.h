// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_LIB_HOST_COMMON_H_
#define SRC_STORAGE_LIB_HOST_COMMON_H_

#include <fcntl.h>
#include <unistd.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>

#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <fbl/vector.h>

// The "manifest" command is only being retained here for backwards compatibility.
// TODO(planders): Once all clients have switched create/add with --manifest, remove this command.
enum class Command {
  kNone,
  kMkfs,
  kFsck,
  kUsedDataSize,
  kUsedInodes,
  kUsedSize,
  kLs,
  kAdd,
  kCp,
  kManifest,
  kMkdir,
};

enum class Option {
  kDepfile,
  kReadonly,
  kOffset,
  kLength,
  kCompress,
  kJsonOutput,
  kHelp,
};

enum class Argument {
  kManifest,
  kBlob,
};

enum class ArgType {
  kNone,
  kOne,
  kTwo,
  kMany,
  kOptional,
};

// An abstract class which defines an interface for processing and running commands for file system
// host-side tools. This includes parsing all command line options, pre-processing any files to be
// copied, and resizing the file system image as necessary. Child classes must implement any
// commands they wish to support, as well as providing their own space calculations for files to be
// added.
class FsCreator {
 public:
  DISALLOW_COPY_ASSIGN_AND_MOVE(FsCreator);

  explicit FsCreator(uint64_t data_blocks)
      : data_blocks_(data_blocks), depfile_(nullptr, [](FILE* fp) { fclose(fp); }) {}
  virtual ~FsCreator() = default;

  // Process the command line arguments and run the specified command.
  zx_status_t ProcessAndRun(int argc, char** argv);

  // If a depfile was requested, |str| will be appended (followed by a space)
  // to the depfile. |str| must be less than PATH_MAX.
  zx_status_t AppendDepfile(const char* str);

 protected:
  // Print usage information for all options, commands, and arguments valid for this fs.
  virtual zx_status_t Usage();

  // Returns the command name of the child fs.
  virtual const char* GetToolName() = 0;

  // Tells whether a given |command|, |option|, or |argument| are valid for this fs.
  virtual bool IsCommandValid(Command command) = 0;
  virtual bool IsOptionValid(Option option) = 0;
  virtual bool IsArgumentValid(Argument argument) = 0;

  // Processes the manifest at |manifest_path| and adds all relevant source/destination files to
  // the child's internal processing lists.
  zx_status_t ProcessManifest(char* manifest_path);

  // Parses the next line in the |manifest| file located at |dir_path|,
  // and returns the |dst| and |src| paths (if found).
  zx_status_t ParseManifestLine(FILE* manifest, const char* dir_path, char* src, char* dst);

  // Processes one line in |manifest|, storing files to copy and calculating total space required.
  // Returns "ZX_ERR_OUT_OF_RANGE" when manifest has reached EOF.
  virtual zx_status_t ProcessManifestLine(FILE* manifest, const char* dir_path) = 0;

  // Process custom arguments specific to the child fs. Returns the number of processed arguments
  // in |processed|.
  virtual zx_status_t ProcessCustom(int argc, char** argv, uint8_t* processed) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Calculates the minimum fs size required for all files processed up to this point.
  virtual zx_status_t CalculateRequiredSize(off_t* out) = 0;

  // Commands.
  // Creates the fs at fd_.
  virtual zx_status_t Mkfs() { return ZX_ERR_NOT_SUPPORTED; }
  // Runs fsck on the fs at fd_.
  virtual zx_status_t Fsck() { return ZX_ERR_NOT_SUPPORTED; }

  // On success, returns ZX_OK and prints to stdout the number of bytes used
  // by data within the fs.
  virtual zx_status_t UsedDataSize() { return ZX_ERR_NOT_SUPPORTED; }

  // On success, returns ZX_OK and prints to stdout the number allocated
  // inodes within the fs.
  virtual zx_status_t UsedInodes() { return ZX_ERR_NOT_SUPPORTED; }

  // On success, returns ZX_OK and prints to stdout the number of bytes used by data
  // and bytes reserved for superblock, bitmaps, inodes and journal within the fs.
  virtual zx_status_t UsedSize() { return ZX_ERR_NOT_SUPPORTED; }

  // Adds all files specified in manifests or other command line arguments to the fs.
  virtual zx_status_t Add() { return ZX_ERR_NOT_SUPPORTED; }
  // Runs ls on the fs at fd_, at the specified path (if any).
  virtual zx_status_t Ls() { return ZX_ERR_NOT_SUPPORTED; }

  Command GetCommand() const { return command_; }
  off_t GetOffset() const { return offset_; }
  off_t GetLength() const { return length_; }
  bool ShouldCompress() const { return compress_; }

  const std::optional<std::filesystem::path>& json_output_path() { return json_output_path_; }
  fbl::unique_fd fd_;

  uint64_t data_blocks_;

 private:
  // Process all options/arguments and open fd to device.
  zx_status_t ProcessArgs(int argc, char** argv);

  // Perform the specified command.
  zx_status_t RunCommand();

  // Parses the size specification (if any) from the |device| string, and returns the result in
  // |*out|. The size argument is only valid for the "create" command.
  zx_status_t ParseSize(char* device, size_t* out);

  // Resizes the file on "create" if a different size was specified, or the file is not as
  // large as it needs to be to contain all specified files (specifiles).
  zx_status_t ResizeFile(off_t requested_size, struct stat stats);

  Command command_{Command::kNone};
  off_t offset_{0};
  off_t length_{0};
  bool read_only_{false};
  bool compress_{false};
  std::mutex depfile_lock_;
  std::unique_ptr<FILE, void (*)(FILE*)> depfile_;
  std::optional<std::filesystem::path> json_output_path_;
};

#endif  // SRC_STORAGE_LIB_HOST_COMMON_H_
