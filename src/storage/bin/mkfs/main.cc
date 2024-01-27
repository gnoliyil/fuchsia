// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <getopt.h>
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

#include <safemath/checked_math.h>

#include "src/lib/storage/fs_management/cpp/admin.h"

namespace {

struct {
  const char* name;
  fs_management::DiskFormat df;
} FILESYSTEMS[] = {{"blobfs", fs_management::kDiskFormatBlobfs},
                   {"minfs", fs_management::kDiskFormatMinfs},
                   {"fat", fs_management::kDiskFormatFat},
                   {"factoryfs", fs_management::kDiskFormatFactoryfs},
                   {"f2fs", fs_management::kDiskFormatF2fs},
                   {"fxfs", fs_management::kDiskFormatFxfs}};

int usage(void) {
  fprintf(stderr, "usage: mkfs [ <option>* ] devicepath filesystem\n");
  fprintf(stderr, " -h|--help                     Print this message\n");
  fprintf(stderr, " -v|--verbose                  Verbose mode\n");
  fprintf(stderr,
          " -s|--fvm_data_slices SLICES   If block device is on top of a FVM,\n"
          "                               the filesystem will have at least SLICES slices\n"
          "                               allocated for data.\n");
  fprintf(stderr, " values for 'filesystem' include:\n");
  for (const auto& fs : FILESYSTEMS) {
    fprintf(stderr, "  '%s'\n", fs.name);
  }
  return -1;
}

int parse_args(int argc, char** argv, fs_management::MkfsOptions* options,
               fs_management::DiskFormat* df, char** devicepath) {
  static const struct option cmds[] = {
      {"help", no_argument, nullptr, 'h'},
      {"verbose", no_argument, nullptr, 'v'},
      {"fvm_data_slices", required_argument, nullptr, 's'},
      {0, 0, 0, 0},
  };

  int opt_index = -1;
  int c = -1;

  while ((c = getopt_long(argc, argv, "hvs:", cmds, &opt_index)) >= 0) {
    switch (c) {
      case 'v':
        options->verbose = true;
        break;
      case 's':
        options->fvm_data_slices = safemath::checked_cast<uint32_t>(strtoul(optarg, nullptr, 0));
        if (options->fvm_data_slices == 0) {
          fprintf(stderr, "Invalid Args: %s\n", strerror(errno));
          return usage();
        }
        break;
      case 'h':
        return usage();
      default:
        break;
    };
  };

  if (argc - optind < 1) {
    fprintf(stderr, "Invalid Args: Missing devicepath.\n");
    return usage();
  }

  if (argc - optind < 2) {
    fprintf(stderr, "Invalid Args: Missing filesystem.\n");
    return usage();
  }

  for (const auto& fs : FILESYSTEMS) {
    if (!strcmp(fs.name, argv[argc - 1])) {
      *df = fs.df;
      break;
    }
  }

  if (*df == fs_management::kDiskFormatUnknown) {
    fprintf(stderr, "fs_mkfs: Cannot format a device with filesystem '%s'\n", argv[2]);
    return usage();
  }

  size_t device_arg = argc - 2;
  *devicepath = argv[device_arg];

  return 0;
}

}  // namespace
int main(int argc, char** argv) {
  fs_management::MkfsOptions options;
  char* devicepath;
  fs_management::DiskFormat df;
  int r;
  if ((r = parse_args(argc, argv, &options, &df, &devicepath))) {
    return r;
  }
  if (options.verbose) {
    printf("fs_mkfs: Formatting device [%s]\n", devicepath);
  }
  if ((r = fs_management::Mkfs(devicepath, df, fs_management::LaunchStdioSync, options)) < 0) {
    fprintf(stderr, "fs_mkfs: Failed to format device: %d\n", r);
  }
  return r;
}
