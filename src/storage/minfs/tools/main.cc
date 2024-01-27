// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zircon/errors.h"
#define _XOPEN_SOURCE

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <fbl/algorithm.h>

#include "minfs.h"
#include "src/storage/minfs/format.h"
#include "src/storage/minfs/fsck.h"
#include "src/storage/minfs/host.h"
#include "src/storage/minfs/minfs.h"
#include "src/storage/minfs/transaction_limits.h"

char kDot[2] = ".";
char kDotDot[3] = "..";

namespace {
template <class T>
uint32_t ToU32(T in) {
  if (in > std::numeric_limits<uint32_t>::max()) {
    fprintf(stderr, "out of range %" PRIuMAX "\n", static_cast<uintmax_t>(in));
    exit(-1);
  }
  return static_cast<uint32_t>(in);
}
}  // namespace

// Returns the string version of |mode|.
static const char* GetModeString(uint32_t mode) {
  switch (mode & S_IFMT) {
    case S_IFREG:
      return "-";
    case S_IFCHR:
      return "c";
    case S_IFBLK:
      return "b";
    case S_IFDIR:
      return "d";
    default:
      return "?";
  }
}

// Adds PATH_PREFIX to path if it isn't there
void GetHostPath(const char* path, char* out) {
  out[0] = 0;
  int remaining = PATH_MAX - 1;

  if (strncmp(path, PATH_PREFIX, PREFIX_SIZE)) {
    strncat(out, PATH_PREFIX, remaining);
    remaining -= strlen(PATH_PREFIX);
  }

  strncat(out, path, remaining);
}

// Checks if the given |path| is a directory, and return the answer in |*result|.
zx_status_t IsDirectory(const char* path, bool* result) {
  struct stat s;
  bool host = host_path(path);

  if (host) {
    if (stat(path, &s) < 0) {
      fprintf(stderr, "error: cannot stat path '%s': %s\n", path, strerror(errno));
      return ZX_ERR_IO;
    }
  } else {
    if (!emu_is_mounted()) {
      fprintf(stderr, "Cannot check directory of unmounted minfs partition\n");
      return ZX_ERR_INTERNAL;
    }

    if (emu_stat(path, &s) < 0) {
      fprintf(stderr, "error: cannot stat path '%s': %s\n", path, strerror(errno));
      return ZX_ERR_IO;
    }
  }

  if (S_ISDIR(s.st_mode)) {
    *result = true;
  } else {
    *result = false;
  }
  return ZX_OK;
}

// Copies a file to minfs from the host, or vice versa.
zx_status_t CopyFile(const char* src_path, const char* dst_path) {
  FileWrapper src;
  FileWrapper dst;

  if (FileWrapper::Open(src_path, O_RDONLY, 0, &src) < 0) {
    fprintf(stderr, "error: cannot open file '%s'\n", src_path);
    return ZX_ERR_IO;
  }

  if (FileWrapper::Open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644, &dst) < 0) {
    fprintf(stderr, "error: cannot open file '%s'\n", dst_path);
    return ZX_ERR_IO;
  }

  char buffer[256 * 1024];
  ssize_t r;
  for (;;) {
    if ((r = src.Read(buffer, sizeof(buffer))) < 0) {
      fprintf(stderr, "error: reading from '%s'\n", src_path);
      break;
    } else if (r == 0) {
      break;
    }
    void* ptr = buffer;
    ssize_t len = r;
    while (len > 0) {
      if ((r = dst.Write(ptr, len)) < 0) {
        fprintf(stderr, "error: writing to '%s'\n", dst_path);
        goto done;
      }
      ptr = (void*)((uintptr_t)ptr + r);
      len -= r;
    }
  }
done:
  return r == 0 ? ZX_OK : ZX_ERR_IO;
}

// Attempts to make the directory at |path|.
// Returns a positive result if creation was successful or the specified directory already exists.
zx_status_t MakeDirectory(const char* path) {
  if (DirWrapper::Make(path, 0777) && errno != EEXIST) {
    fprintf(stderr, "minfs: could not create directory %s: %s\n", path, strerror(errno));
    return ZX_ERR_IO;
  }

  return ZX_OK;
}

zx_status_t MinfsCreator::Usage() {
  zx_status_t status = FsCreator::Usage();

  // Additional information about manifest format.
  fprintf(stderr, "\nThe format of the manifest must be as follows:\n");
  fprintf(stderr, "\t'dst/path=src/path'\n");
  fprintf(stderr, "with one dst/src pair on each line.\n");

  fprintf(stderr,
          "\nPrefix all minfs paths with '::' "
          "(unless they are included in a manifest).\n");
  return status;
}

bool MinfsCreator::IsCommandValid(Command command) {
  switch (command) {
    case Command::kMkfs:
    case Command::kFsck:
    case Command::kUsedDataSize:
    case Command::kUsedInodes:
    case Command::kUsedSize:
    case Command::kLs:
    case Command::kCp:
    case Command::kManifest:
    case Command::kMkdir:
    case Command::kAdd:
      return true;
    default:
      return false;
  }
}

bool MinfsCreator::IsOptionValid(Option option) {
  switch (option) {
    case Option::kDepfile:
    case Option::kReadonly:
    case Option::kOffset:
    case Option::kLength:
    case Option::kHelp:
      return true;
    default:
      return false;
  }
}

bool MinfsCreator::IsArgumentValid(Argument argument) {
  switch (argument) {
    case Argument::kManifest:
      return true;
    default:
      return false;
  }
}

zx_status_t MinfsCreator::ProcessManifestLine(FILE* manifest, const char* dir_path) {
  char src[PATH_MAX];
  src[0] = '\0';
  char dst[PATH_MAX];
  dst[0] = '\0';

  zx_status_t status;
  if ((status = ParseManifestLine(manifest, dir_path, src, dst)) != ZX_OK) {
    return status;
  }

  if (!strlen(src) || !strlen(dst)) {
    fprintf(stderr, "Manifest line must specify source and destination files\n");
    return ZX_ERR_INVALID_ARGS;
  }

  bool dir;
  if ((status = IsDirectory(src, &dir)) != ZX_OK) {
    return status;
  }

  if (dir) {
    fprintf(stderr, "Manifest cannot specify directory as source\n");
    return ZX_ERR_INVALID_ARGS;
  }

  // Make dst into minfs path.
  char emu_dst[PATH_MAX];
  GetHostPath(dst, emu_dst);

  // Process parent directories.
  if ((status = ProcessParentDirectories(emu_dst)) != ZX_OK) {
    return status;
  }

  // Copy src to dst.
  return ProcessFile(src, emu_dst);
}

zx_status_t MinfsCreator::ProcessCustom(int argc, char** argv, uint8_t* processed) {
  uint8_t required_args = 0;

  switch (GetCommand()) {
    case Command::kLs:
      __FALLTHROUGH;
    case Command::kMkdir: {
      required_args = 1;
      if (argc != required_args) {
        return ZX_ERR_INVALID_ARGS;
      }

      // A non-host path is required here since the ls and mkdir commands must be run on minfs.
      if (host_path(argv[0])) {
        fprintf(stderr, "Must specify path with prefix '::'");
        return ZX_ERR_INVALID_ARGS;
      }

      // ls and mkdir can only be run on one path at a time.
      if (!dir_list_.is_empty()) {
        fprintf(stderr, "Too many paths specified\n");
        return ZX_ERR_INVALID_ARGS;
      }

      if (GetCommand() == Command::kLs) {
        dir_list_.push_back(argv[0]);
        return ZX_OK;
      }

      zx_status_t status;
      if ((status = ProcessParentDirectories(argv[0])) != ZX_OK) {
        return status;
      }

      if ((status = ProcessDirectory(argv[0])) != ZX_OK) {
        return status;
      }
      break;
    }
    case Command::kCp: {
      required_args = 2;
      if (argc != required_args) {
        return ZX_ERR_INVALID_ARGS;
      }

      // If our source file is located within minfs, we need to mount the minfs image so that we
      // can process any child files/directories.
      zx_status_t status;
      if (!host_path(argv[0]) && (status = MountMinfs()) != ZX_OK) {
        return status;
      }

      char src[PATH_MAX];
      char dst[PATH_MAX];
      strncpy(src, argv[0], PATH_MAX);
      strncpy(dst, argv[1], PATH_MAX);

      if ((status = ProcessParentDirectories(dst)) != ZX_OK) {
        return status;
      }

      if ((status = ProcessEntityAndChildren(src, dst)) != ZX_OK) {
        return status;
      }
      break;
    }
    case Command::kManifest: {
      required_args = 1;
      if (argc != required_args) {
        return ZX_ERR_INVALID_ARGS;
      }
      zx_status_t status;
      if ((status = ProcessManifest(argv[0])) != ZX_OK) {
        return status;
      }
      break;
    }
    default:
      fprintf(stderr, "Arguments not supported\n");
      return ZX_ERR_INVALID_ARGS;
  }

  *processed = required_args;
  return ZX_OK;
}

zx_status_t MinfsCreator::CalculateRequiredSize(off_t* out) {
  uint32_t dir_count = static_cast<uint32_t>(dir_list_.size() + 1);  // dir_list_ + root

  // This is a rough estimate of how many directory data blocks will be needed.
  // This is not super robust and will not hold up if directories start requiring indirect blocks,
  // but for our current purposes it should be sufficient.
  uint32_t dir_blocks = ToU32(dir_count + (dir_bytes_ / minfs::kMinfsBlockSize));

  minfs::Superblock info;
  info.flags = 0;
  info.block_size = minfs::kMinfsBlockSize;
  info.inode_count = minfs::kMinfsDefaultInodeCount;
  info.block_count = ToU32(data_blocks_ + dir_blocks);

  // Calculate number of blocks we will need for all minfs structures.
  uint32_t inode_bitmap_blocks =
      (info.inode_count + minfs::kMinfsBlockBits - 1) / minfs::kMinfsBlockBits;
  uint32_t block_bitmap_blocks =
      (info.block_count + minfs::kMinfsBlockBits - 1) / minfs::kMinfsBlockBits;
  uint32_t inode_table_blocks =
      (info.inode_count + minfs::kMinfsInodesPerBlock - 1) / minfs::kMinfsInodesPerBlock;

  info.ibm_block = 8;
  info.abm_block = info.ibm_block + fbl::round_up(inode_bitmap_blocks, 8u);
  info.ino_block = info.abm_block + fbl::round_up(block_bitmap_blocks, 8u);
  info.integrity_start_block = info.ino_block + inode_table_blocks;
  minfs::TransactionLimits limits(info);
  info.dat_block = info.integrity_start_block + limits.GetRecommendedIntegrityBlocks();

  *out = (info.dat_block + info.block_count) * info.BlockSize();
  return ZX_OK;
}

zx_status_t MinfsCreator::Mkfs() {
  // Create the bcache.
  auto bc_or = GenerateBcache();
  if (bc_or.is_error()) {
    return bc_or.error_value();
  }

  // Consume the bcache to mkfs.
  if (auto mkfs_or = minfs::Mkfs(bc_or.value().get()); mkfs_or.is_error()) {
    return mkfs_or.error_value();
  }

  // Add any directories/files that have been pre-processed.
  if (zx_status_t status = Add(); status != ZX_OK) {
    fprintf(stderr,
            "Warning: Adding files on create failed - required size may have been "
            "miscalculated\n");
    return status;
  }

  return ZX_OK;
}

zx_status_t MinfsCreator::Fsck() {
  auto bc_or = GenerateBcache();
  if (bc_or.is_error()) {
    return bc_or.error_value();
  }

  bc_or = minfs::Fsck(std::move(bc_or.value()), minfs::FsckOptions{.repair = true});
  return bc_or.status_value();
}

zx_status_t MinfsCreator::UsedDataSize() {
  auto bc_or = GenerateBcache();
  if (bc_or.is_error()) {
    return bc_or.error_value();
  }

  auto size_or = minfs::UsedDataSize(bc_or.value());
  if (size_or.is_error()) {
    return size_or.error_value();
  }

  printf("%" PRIu64 "\n", size_or.value());
  return ZX_OK;
}

zx_status_t MinfsCreator::UsedInodes() {
  auto bc_or = GenerateBcache();
  if (bc_or.is_error()) {
    return bc_or.error_value();
  }

  auto used_inodes_or = minfs::UsedInodes(bc_or.value());
  if (used_inodes_or.is_error()) {
    return used_inodes_or.error_value();
  }

  printf("%" PRIu64 "\n", used_inodes_or.value());
  return ZX_OK;
}

zx_status_t MinfsCreator::UsedSize() {
  auto bc_or = GenerateBcache();
  if (bc_or.is_error()) {
    return bc_or.error_value();
  }

  auto size_or = minfs::UsedSize(bc_or.value());
  if (size_or.is_error()) {
    return size_or.error_value();
  }

  printf("%" PRIu64 "\n", size_or.value());
  return ZX_OK;
}

zx_status_t MinfsCreator::Add() {
  zx_status_t status;
  // Mount the minfs.
  if ((status = MountMinfs()) != ZX_OK) {
    return status;
  }

  // Make all directories.
  for (size_t n = 0; n < dir_list_.size(); n++) {
    if ((status = MakeDirectory(dir_list_[n].c_str())) != ZX_OK) {
      return status;
    }
  }

  // Copy all files.
  for (size_t n = 0; n < file_list_.size(); n++) {
    if ((status = AppendDepfile(file_list_[n].first.c_str())) != ZX_OK) {
      return status;
    }
    if ((status = CopyFile(file_list_[n].first.c_str(), file_list_[n].second.c_str())) != ZX_OK) {
      return status;
    }
  }

  return ZX_OK;
}

zx_status_t MinfsCreator::Ls() {
  if (!file_list_.is_empty() || dir_list_.size() != 1) {
    fprintf(stderr, "ls requires one argument\n");
    return Usage();
  }

  zx_status_t status;
  if ((status = MountMinfs()) != ZX_OK) {
    return status;
  }

  const char* path = dir_list_[0].c_str();
  if (strncmp(path, PATH_PREFIX, PREFIX_SIZE)) {
    fprintf(stderr, "error: ls can only operate minfs paths (must start with %s)\n", PATH_PREFIX);
    return ZX_ERR_INVALID_ARGS;
  }

  DIR* d = emu_opendir(path);
  if (!d) {
    fprintf(stderr, "error: cannot open directory '%s'\n", path);
    return ZX_ERR_IO;
  }

  struct dirent* dir_entry;
  char tmp[2048];
  struct stat stats;
  while ((dir_entry = emu_readdir(d)) != nullptr) {
    if (strcmp(dir_entry->d_name, ".") && strcmp(dir_entry->d_name, "..")) {
      memset(&stats, 0, sizeof(struct stat));
      if ((strlen(dir_entry->d_name) + strlen(path) + 2) <= sizeof(tmp)) {
        snprintf(tmp, sizeof(tmp), "%s/%s", path, dir_entry->d_name);
        emu_stat(tmp, &stats);
      }
      fprintf(stdout, "%s %8jd %s\n", GetModeString(stats.st_mode),
              static_cast<intmax_t>(stats.st_size), dir_entry->d_name);
    }
  }
  emu_closedir(d);
  return ZX_OK;
}

zx_status_t MinfsCreator::ProcessEntityAndChildren(char* src, char* dst) {
  bool dir;
  zx_status_t status;
  if ((status = IsDirectory(src, &dir)) != ZX_OK) {
    return status;
  }

  if (dir) {
    ProcessDirectory(dst);
  } else {
    return ProcessFile(src, dst);
  }

  DirWrapper current_dir;
  if (DirWrapper::Open(src, &current_dir)) {
    fprintf(stderr, "error: cannot open directory '%s'\n", src);
    return ZX_ERR_IO;
  }

  size_t src_len = strlen(src);
  size_t dst_len = strlen(dst);
  struct dirent* dir_entry;
  while ((dir_entry = current_dir.ReadDir()) != nullptr) {
    if (!strcmp(dir_entry->d_name, ".") || !strcmp(dir_entry->d_name, "..")) {
      continue;
    }
    size_t name_len = strlen(dir_entry->d_name);
    if (src_len + name_len + 1 > PATH_MAX - 1 || dst_len + name_len + 1 > PATH_MAX - 1) {
      fprintf(stderr, "Path size exceeds PATH_MAX\n");
      return ZX_ERR_INVALID_ARGS;
    }
    strncat(src, "/", 1);
    strncat(src, dir_entry->d_name, PATH_MAX - src_len - 2);
    strncat(dst, "/", 1);
    strncat(dst, dir_entry->d_name, PATH_MAX - dst_len - 2);

    zx_status_t status = ProcessEntityAndChildren(src, dst);
    if (status != ZX_OK) {
      return status;
    }

    src[src_len] = '\0';
    dst[dst_len] = '\0';
  }
  return 0;
}

zx_status_t MinfsCreator::ProcessParentDirectories(char* path) {
  // Create parent directories if they don't exist.
  char* slash = strchr(path, '/');
  while (slash != nullptr) {
    *slash = '\0';

    char emu_path[PATH_MAX];
    GetHostPath(path, emu_path);

    zx_status_t status;
    if ((status = ProcessDirectory(emu_path)) != ZX_OK) {
      return status;
    }

    *slash = '/';
    slash = strchr(slash + 1, '/');
  }

  return ZX_OK;
}

zx_status_t MinfsCreator::ProcessDirectory(char* path) {
  if (!strncmp(path, PATH_PREFIX, strlen(path))) {
    // If |path| is the minfs root directory ("::"), return without processing.
    return ZX_OK;
  }

  // Check to see if |path| has already been processed.
  bool found = false;
  for (size_t i = 0; i < dir_list_.size(); i++) {
    if (!strcmp(path, dir_list_[i].c_str())) {
      found = true;
      break;
    }
  }

  if (!found) {
    // Only process the directory if it has not already been processed.
    if (GetCommand() == Command::kMkfs && !host_path(path)) {
      // Only calculate required space if we are running mkfs, and this is a minfs path.
      ProcessDirectoryEntry(path);
      ProcessDirectoryEntry(kDot);
      ProcessDirectoryEntry(kDotDot);
    }

    dir_list_.push_back(path);
  }

  return ZX_OK;
}

zx_status_t MinfsCreator::ProcessFile(char* src, char* dst) {
  struct stat stats;
  int result = host_path(src) ? stat(src, &stats) : emu_stat(src, &stats);
  if (result < 0) {
    fprintf(stderr, "Failed to stat file %s\n", src);
    return ZX_ERR_IO;
  }

  // Check if the file already exists in file_list_.
  for (size_t n = 0; n < file_list_.size(); n++) {
    if (!strcmp(dst, file_list_[n].second.c_str())) {
      if (!strcmp(src, file_list_[n].first.c_str())) {
        return ZX_OK;
      }

      // If the file does exist but the source does not match, return an error.
      fprintf(stderr, "Error: Source %s does not match existing source %s for destination %s\n",
              src, file_list_[n].first.c_str(), dst);
      return ZX_ERR_INVALID_ARGS;
    }
  }

  if (GetCommand() == Command::kMkfs && !host_path(dst)) {
    // Only calculate required size if we are copying to minfs, and we are mkfsing.
    zx_status_t status;
    if ((status = ProcessBlocks(stats.st_size)) != ZX_OK) {
      return status;
    }

    ProcessDirectoryEntry(dst);
  }

  file_list_.push_back(std::make_pair(src, dst));
  return ZX_OK;
}

void MinfsCreator::ProcessDirectoryEntry(char* path) {
  if (path == nullptr) {
    return;
  }
  char* last_slash = strrchr(path, '/');
  last_slash = last_slash == nullptr ? path : last_slash + 1;
  size_t component_length = strlen(last_slash);
  if (component_length > std::numeric_limits<uint8_t>::max()) {
    fprintf(stderr, "component too long");
    exit(-1);
  }
  dir_bytes_ += minfs::DirentSize(static_cast<uint8_t>(component_length));
}

zx_status_t MinfsCreator::ProcessBlocks(off_t file_size) {
  uint64_t total_blocks = 0;
  uint32_t remaining = ToU32((file_size + minfs::kMinfsBlockSize - 1) / minfs::kMinfsBlockSize);

  // Add direct blocks to the total.
  uint32_t direct_blocks = std::min(remaining, minfs::kMinfsDirect);
  total_blocks += direct_blocks;
  remaining -= direct_blocks;

  if (remaining) {
    // If more blocks remain, calculate how many indirect blocks we need.
    uint32_t indirect_blocks =
        std::min((remaining + minfs::kMinfsDirectPerIndirect + 1) / minfs::kMinfsDirectPerIndirect,
                 minfs::kMinfsIndirect);

    direct_blocks = std::min(remaining, indirect_blocks * minfs::kMinfsDirectPerIndirect);

    // Add blocks to the total, and update remaining.
    total_blocks += indirect_blocks + direct_blocks;
    remaining -= direct_blocks;

    if (remaining) {
      // If blocks remain past the indirect block range, calculate required doubly indirect
      // and indirect blocks.
      uint32_t dindirect_blocks = std::min(
          (remaining + minfs::kMinfsDirectPerDindirect + 1) / minfs::kMinfsDirectPerDindirect,
          minfs::kMinfsDoublyIndirect);

      indirect_blocks = std::min(
          (remaining + minfs::kMinfsDirectPerIndirect + 1) / minfs::kMinfsDirectPerIndirect,
          minfs::kMinfsDirectPerIndirect * minfs::kMinfsDoublyIndirect);

      direct_blocks =
          std::min(remaining, minfs::kMinfsDoublyIndirect * minfs::kMinfsDirectPerDindirect);

      // Add blocks to the total, and update remaining.
      total_blocks += dindirect_blocks + indirect_blocks + direct_blocks;
      remaining -= direct_blocks;

      if (remaining) {
        // If we still have more blocks remaining at this point, the file is larger than
        // the current minfs max file size.
        fprintf(stderr, "Error: File too large for minfs @ %" PRIu64 " bytes\n", file_size);
        return ZX_ERR_INVALID_ARGS;
      }
    }
  }

  // Add calculated blocks to the total so far.
  data_blocks_ += total_blocks;
  return ZX_OK;
}

zx::result<std::unique_ptr<minfs::Bcache>> MinfsCreator::GenerateBcache() {
  uint32_t block_count = static_cast<uint32_t>(GetLength() / minfs::kMinfsBlockSize);

  // Duplicate the fd so that we can re-open the minfs partition if we need to.
  int dupfd = dup(fd_.get());

  auto bc_or = minfs::Bcache::Create(std::move(fd_), block_count);
  if (bc_or.is_error()) {
    fprintf(stderr, "error: cannot create block cache\n");
    return zx::error(ZX_ERR_IO);
  }

  if (auto status = bc_or->SetOffset(GetOffset()); status.is_error()) {
    return zx::error(ZX_ERR_BAD_STATE);
  }
  fd_.reset(dupfd);
  return bc_or;
}

zx_status_t MinfsCreator::MountMinfs() {
  if (emu_is_mounted()) {
    // If we have already mounted minfs, do nothing.
    return ZX_OK;
  }

  auto bc_or = GenerateBcache();
  if (bc_or.is_error()) {
    return bc_or.error_value();
  }

  return emu_mount_bcache(std::move(bc_or.value()));
}

int main(int argc, char** argv) {
  MinfsCreator minfs;

  if (minfs.ProcessAndRun(argc, argv) != ZX_OK) {
    return -1;
  }

  return 0;
}
