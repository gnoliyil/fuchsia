// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device-watcher.h"

#include <dirent.h>
#include <fcntl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/zx/clock.h>
#include <string.h>

namespace device_watcher {

namespace {

constexpr char kDevPath[] = "/dev/";

}

__EXPORT
zx_status_t DirWatcher::Create(int dir_fd, std::unique_ptr<DirWatcher>* out_dir_watcher) {
  zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::DirectoryWatcher>();
  if (endpoints.is_error()) {
    return endpoints.status_value();
  }

  fdio_t* io = fdio_unsafe_fd_to_io(dir_fd);
  zx::unowned_channel channel{fdio_unsafe_borrow_channel(io)};

  // Make a one-off call to fuchsia.io.Directory.Watch to open a channel from
  // which watch events can be read.
  auto result = fidl::WireCall(fidl::UnownedClientEnd<fuchsia_io::Directory>(channel))
                    ->Watch(fuchsia_io::wire::WatchMask::kRemoved, 0, std::move(endpoints->server));

  fdio_unsafe_release(io);

  if (!result.ok()) {
    return result.status();
  }
  *out_dir_watcher = std::make_unique<DirWatcher>(std::move(endpoints->client));

  return ZX_OK;
}

__EXPORT
zx_status_t DirWatcher::WaitForRemoval(std::string_view filename, zx::duration timeout) {
  auto deadline = zx::deadline_after(timeout);
  // Loop until we see the removal event, or wait_one fails due to timeout.
  for (;;) {
    zx_signals_t observed;
    zx_status_t status = client_.channel().wait_one(ZX_CHANNEL_READABLE, deadline, &observed);
    if (status != ZX_OK) {
      return status;
    }
    if (!(observed & ZX_CHANNEL_READABLE)) {
      return ZX_ERR_IO;
    }

    // Messages are of the form:
    //  uint8_t event
    //  uint8_t len
    //  char* name
    uint8_t buf[fuchsia_io::wire::kMaxBuf];
    uint32_t actual_len;
    status = client_.channel().read(0, buf, nullptr, sizeof(buf), 0, &actual_len, nullptr);
    if (status != ZX_OK) {
      return status;
    }
    if (static_cast<fuchsia_io::wire::WatchEvent>(buf[0]) !=
        fuchsia_io::wire::WatchEvent::kRemoved) {
      continue;
    }
    if (filename.length() == 0) {
      // Waiting on any file.
      return ZX_OK;
    }
    if ((buf[1] == filename.length()) &&
        (memcmp(buf + 2, filename.data(), filename.length()) == 0)) {
      return ZX_OK;
    }
  }
  return ZX_ERR_NOT_FOUND;
}

__EXPORT
zx_status_t IterateDirectory(const int fd, FileCallback callback) {
  struct dirent* entry;

  DIR* dir = fdopendir(fd);
  if (dir == nullptr) {
    return ZX_ERR_IO;
  }

  fdio_t* io = fdio_unsafe_fd_to_io(fd);
  zx::unowned_channel dir_channel{fdio_unsafe_borrow_channel(io)};

  zx_status_t status = ZX_OK;
  while ((entry = readdir(dir)) != nullptr) {
    std::string filename(entry->d_name);
    if (filename == "." || filename == "..") {
      continue;
    }

    auto endpoints = fidl::CreateEndpoints<fuchsia_io::Node>();
    if (endpoints.is_error()) {
      status = endpoints.status_value();
      goto finish;
    }

    // Open a channel to the file.
    status = fdio_open_at(dir_channel->get(), entry->d_name, 0,
                          endpoints->server.TakeChannel().release());
    if (status != ZX_OK) {
      goto finish;
    }

    // Invoke the user-provided callback.
    status = callback(filename, endpoints->client.TakeChannel());
    if (status != ZX_OK) {
      goto finish;
    }
  }

finish:
  fdio_unsafe_release(io);
  closedir(dir);
  return status;
}

__EXPORT
zx::result<zx::channel> WaitForFile(const int dir_fd, const char* file) {
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
  zx_status_t status = fdio_watch_directory(dir_fd, watch_func, ZX_TIME_INFINITE, &file_view);
  if (status != ZX_ERR_STOP) {
    return zx::error(status);
  }
  // Open a channel to the file.
  zx::channel chan;
  status = fdio_get_service_handle(openat(dir_fd, file, O_RDWR), chan.reset_and_get_address());
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(std::move(chan));
}

namespace {

// This variant of WaitForFile opens the file specified relative to the rootdir,
// using the full_path. This is a workaround to deal with the fact that devhosts
// do not implement open_at.
zx::result<zx::channel> WaitForFile2(const int rootdir_fd, const int dir_fd, const char* full_path,
                                     const char* file, bool last, bool readonly) {
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
  zx_status_t status = fdio_watch_directory(dir_fd, watch_func, ZX_TIME_INFINITE, &file_view);
  if (status != ZX_ERR_STOP) {
    return zx::error(status);
  }
  int flags = O_RDWR;
  if (readonly) {
    flags = O_RDONLY;
  }
  if (!last) {
    flags = O_RDONLY | O_DIRECTORY;
  }

  // Open a channel to the file.
  zx::channel chan;
  status =
      fdio_get_service_handle(openat(rootdir_fd, full_path, flags), chan.reset_and_get_address());
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(std::move(chan));
}

// Version of RecursiveWaitForFile that can mutate its path
zx::result<zx::channel> RecursiveWaitForFileHelper(const int rootdir_fd, const int dir_fd,
                                                   const char* full_path, char* path,
                                                   bool readonly) {
  char* first_slash = strchr(path, '/');
  if (first_slash == nullptr) {
    // If there's no first slash, then we're just waiting for the file
    // itself to appear.
    return WaitForFile2(rootdir_fd, dir_fd, full_path, path, true, readonly);
  }
  *first_slash = 0;

  auto result = WaitForFile2(rootdir_fd, dir_fd, full_path, path, false, readonly);
  if (!result.is_ok()) {
    return result.take_error();
  }
  int next_fd;
  zx_status_t status = fdio_fd_create(result.value().release(), &next_fd);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  auto close_action = fit::defer([next_fd]() { close(next_fd); });
  *first_slash = '/';
  return RecursiveWaitForFileHelper(rootdir_fd, next_fd, full_path, first_slash + 1, readonly);
}

}  // namespace

__EXPORT
zx::result<zx::channel> RecursiveWaitForFile(const int dir_fd, const char* path) {
  char path_copy[PATH_MAX];
  if (strlen(path) >= sizeof(path_copy)) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  strcpy(path_copy, path);
  return RecursiveWaitForFileHelper(dir_fd, dir_fd, path_copy, path_copy, false);
}

__EXPORT
zx::result<zx::channel> RecursiveWaitForFile(const char* path) {
  if (strncmp(kDevPath, path, strlen(kDevPath) - 1) != 0) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }
  const int dev_fd = open(kDevPath, O_RDONLY | O_DIRECTORY);
  return RecursiveWaitForFile(dev_fd, path + strlen(kDevPath));
}

__EXPORT
zx::result<zx::channel> RecursiveWaitForFileReadOnly(const char* path) {
  if (strncmp(kDevPath, path, strlen(kDevPath) - 1) != 0) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }
  const int dev_fd = open(kDevPath, O_RDONLY | O_DIRECTORY);
  return RecursiveWaitForFileReadOnly(dev_fd, path + strlen(kDevPath));
}

__EXPORT
zx::result<zx::channel> RecursiveWaitForFileReadOnly(const int dir_fd, const char* path) {
  char path_copy[PATH_MAX];
  if (strlen(path) >= sizeof(path_copy)) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  strcpy(path_copy, path);
  return RecursiveWaitForFileHelper(dir_fd, dir_fd, path_copy, path_copy, true);
}

}  // namespace device_watcher
