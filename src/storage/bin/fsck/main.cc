// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include "src/lib/storage/fs_management/cpp/admin.h"

namespace {
struct {
  const char* name;
  fs_management::DiskFormat df;
} kSupportedFilesystems[] = {{"blobfs", fs_management::kDiskFormatBlobfs},
                             {"minfs", fs_management::kDiskFormatMinfs},
                             {"fat", fs_management::kDiskFormatFat},
                             {"factoryfs", fs_management::kDiskFormatFactoryfs},
                             {"f2fs", fs_management::kDiskFormatF2fs},
                             {"fxfs", fs_management::kDiskFormatFxfs}};

int usage(void) {
  fprintf(stderr, "usage: fsck [ <option>* ] devicepath filesystem\n");
  fprintf(stderr, " -v  : Verbose mode\n");
  fprintf(stderr, " values for 'filesystem' include:\n");
  for (const auto& fs : kSupportedFilesystems) {
    fprintf(stderr, "  '%s'\n", fs.name);
  }
  return -1;
}

int parse_args(int argc, char** argv, fs_management::FsckOptions* options,
               fs_management::DiskFormat* df, char** devicepath) {
  *df = fs_management::kDiskFormatUnknown;
  while (argc > 1) {
    if (!strcmp(argv[1], "-v")) {
      options->verbose = true;
    } else {
      break;
    }
    argc--;
    argv++;
  }
  if (argc < 3) {
    return usage();
  }

  *devicepath = argv[1];
  for (const auto& fs : kSupportedFilesystems) {
    if (!strcmp(fs.name, argv[2])) {
      *df = fs.df;
      break;
    }
  }
  if (*df == fs_management::kDiskFormatUnknown) {
    fprintf(stderr, "fs_fsck: Cannot check a device with filesystem '%s'\n", argv[2]);
    return -1;
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  char* devicepath;
  fs_management::DiskFormat df;
  int r;
  fs_management::FsckOptions options;
  if ((r = parse_args(argc, argv, &options, &df, &devicepath))) {
    return r;
  }

  if (options.verbose) {
    printf("fs_fsck: Checking device [%s]\n", devicepath);
  }

  if ((r = fs_management::Fsck(devicepath, df, options, fs_management::LaunchStdioSync)) < 0) {
    fprintf(stderr, "fs_fsck: Failed to check device: %d\n", r);
  }
  return r;
}
