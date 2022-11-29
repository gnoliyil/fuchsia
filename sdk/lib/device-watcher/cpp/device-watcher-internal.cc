// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/zx/clock.h>
#include <string.h>

#include <fbl/unique_fd.h>

#include "device-watcher-deprecated.h"
#include "device-watcher.h"

namespace device_watcher {

namespace {

constexpr char kDevPath[] = "/dev/";

}

__EXPORT
zx_status_t DirWatcher::Create(fbl::unique_fd dir_fd,
                               std::unique_ptr<DirWatcher>* out_dir_watcher) {
  return Create(dir_fd.get(), out_dir_watcher);
}

__EXPORT
zx_status_t IterateDirectory(const fbl::unique_fd& fd, FileCallback callback) {
  return IterateDirectory(fd.get(), std::move(callback));
}

// Waits for |file| to appear in |dir|, and opens it when it does.  Times out if
// the deadline passes.
__EXPORT
zx_status_t WaitForFile(const fbl::unique_fd& dir, const char* file, fbl::unique_fd* out) {
  auto watch_func = [](int dirfd, int event, const char* fn, void* cookie) -> zx_status_t {
    const std::string_view& file = *static_cast<std::string_view*>(cookie);
    if (event != WATCH_EVENT_ADD_FILE) {
      return ZX_OK;
    }
    if (std::string_view{fn} == file) {
      return ZX_ERR_STOP;
    }
    return ZX_OK;
  };

  std::string_view file_view{file};
  zx_status_t status = fdio_watch_directory(dir.get(), watch_func, ZX_TIME_INFINITE, &file_view);
  if (status != ZX_ERR_STOP) {
    return status;
  }
  out->reset(openat(dir.get(), file, O_RDWR));
  if (!out->is_valid()) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

namespace {

// This variant of WaitForFile opens the file specified relative to the rootdir,
// using the full_path. This is a workaround to deal with the fact that devhosts
// do not implement open_at.
zx_status_t WaitForFile2(const fbl::unique_fd& rootdir, const fbl::unique_fd& dir,
                         const char* full_path, const char* file, bool last, bool readonly,
                         fbl::unique_fd* out) {
  auto watch_func = [](int dirfd, int event, const char* fn, void* cookie) -> zx_status_t {
    const std::string_view& file = *static_cast<std::string_view*>(cookie);
    if (event != WATCH_EVENT_ADD_FILE) {
      return ZX_OK;
    }
    if (std::string_view{fn} == file) {
      return ZX_ERR_STOP;
    }
    return ZX_OK;
  };

  std::string_view file_view{file};
  zx_status_t status = fdio_watch_directory(dir.get(), watch_func, ZX_TIME_INFINITE, &file_view);
  if (status != ZX_ERR_STOP) {
    return status;
  }
  int flags = O_RDWR;
  if (readonly) {
    flags = O_RDONLY;
  }
  if (!last) {
    flags = O_RDONLY | O_DIRECTORY;
  }
  out->reset(openat(rootdir.get(), full_path, flags));
  if (!out->is_valid()) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

// Version of RecursiveWaitForFile that can mutate its path
zx_status_t RecursiveWaitForFileHelper(const fbl::unique_fd& rootdir, const fbl::unique_fd& dir,
                                       const char* full_path, char* path, bool readonly,
                                       fbl::unique_fd* out) {
  char* first_slash = strchr(path, '/');
  if (first_slash == nullptr) {
    // If there's no first slash, then we're just waiting for the file
    // itself to appear.
    return WaitForFile2(rootdir, dir, full_path, path, true, readonly, out);
  }
  *first_slash = 0;

  fbl::unique_fd next_dir;
  zx_status_t status = WaitForFile2(rootdir, dir, full_path, path, false, readonly, &next_dir);
  if (status != ZX_OK) {
    return status;
  }
  *first_slash = '/';
  return RecursiveWaitForFileHelper(rootdir, next_dir, full_path, first_slash + 1, readonly, out);
}

}  // namespace

// Waits for the relative |path| starting in |dir| to appear, and opens it.
__EXPORT
zx_status_t RecursiveWaitForFile(const fbl::unique_fd& dir, const char* path, fbl::unique_fd* out) {
  char path_copy[PATH_MAX];
  if (strlen(path) >= sizeof(path_copy)) {
    return ZX_ERR_INVALID_ARGS;
  }
  strcpy(path_copy, path);
  return RecursiveWaitForFileHelper(dir, dir, path_copy, path_copy, false, out);
}

__EXPORT
zx_status_t RecursiveWaitForFile(const char* path, fbl::unique_fd* out) {
  if (strncmp(kDevPath, path, strlen(kDevPath) - 1) != 0) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  fbl::unique_fd dev(open(kDevPath, O_RDONLY | O_DIRECTORY));
  return RecursiveWaitForFile(dev, path + strlen(kDevPath), out);
}

__EXPORT
zx_status_t RecursiveWaitForFileReadOnly(const char* path, fbl::unique_fd* out) {
  if (strncmp(kDevPath, path, strlen(kDevPath) - 1) != 0) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  fbl::unique_fd dev(open(kDevPath, O_RDONLY | O_DIRECTORY));
  return RecursiveWaitForFileReadOnly(dev, path + strlen(kDevPath), out);
}

__EXPORT
zx_status_t RecursiveWaitForFileReadOnly(const fbl::unique_fd& dir, const char* path,
                                         fbl::unique_fd* out) {
  char path_copy[PATH_MAX];
  if (strlen(path) >= sizeof(path_copy)) {
    return ZX_ERR_INVALID_ARGS;
  }
  strcpy(path_copy, path);
  return RecursiveWaitForFileHelper(dir, dir, path_copy, path_copy, true, out);
}

}  // namespace device_watcher
