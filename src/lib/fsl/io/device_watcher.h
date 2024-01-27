// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_FSL_IO_DEVICE_WATCHER_H_
#define SRC_LIB_FSL_IO_DEVICE_WATCHER_H_

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async/cpp/wait.h>
#include <lib/fit/function.h>

#include <memory>
#include <string>

#include "src/lib/fxl/fxl_export.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace fsl {

// Watches for devices to be registered in devfs.
//
// TODO(jeffbrown): Generalize to watching arbitrary directories or dealing
// with removal when fdio has a protocol for it.
class FXL_EXPORT DeviceWatcher {
 public:
  // Callback function which is invoked whenever a device is found.
  // |dir| is the directory handle provided at construction.
  // |filename| is the name of the file relative to the directory.
  using ExistsCallback = fit::function<void(const fidl::ClientEnd<fuchsia_io::Directory>& dir,
                                            const std::string& filename)>;
  // Callback function which is invoked after the existing files have been
  // reported via ExistsCallback, and before newly-arriving files are delivered
  // via ExistsCallback.
  using IdleCallback = fit::function<void()>;

  ~DeviceWatcher() = default;

  // Creates a device watcher associated with the current message loop.
  //
  // Asynchronously invokes |exists_callback| for all existing devices within
  // the specified directory as well as any subsequently added devices until
  // the device watcher is destroyed. Ignores the "." directory.
  //
  // Equivalent to:
  // CreateWithIdleCallback(directory_path, exists_callback, []{});
  //
  // |directory_path| is the directory to watch (without a trailing slash).
  //
  // |exists_callback| gets called with each existing or new filename, or with
  // an empty string after existing files if empty_after_existing is true.
  static std::unique_ptr<DeviceWatcher> Create(const std::string& directory_path,
                                               ExistsCallback exists_callback,
                                               async_dispatcher_t* dispatcher = nullptr);

  // Creates a device watcher associated with the current message loop.
  //
  // Asynchronously invokes |exists_callback| for all existing devices within
  // the specified directory as well as any subsequently added devices until
  // the device watcher is destroyed. Ignores the "." directory.
  //
  // The |idle_callback| is invoked once immediately after all pre-existing
  // devices have been reported via |exists_callback| shortly after creation.
  // After |idle_callback| returns, any newly-arriving devices are reported via
  // |exists_callback|.
  //
  // |directory_path| is the directory to watch (without a trailing slash).
  //
  // |exists_callback| gets called with each existing or new filename, or with
  // an empty string after existing files if empty_after_existing is true.
  //
  // |idle_callback| gets called after |exists_callback| has returned for all
  // pre-existing devices, and returns before |exists_callback| is called for
  // any subsequently-added devices.
  // |idle_callback| will be deleted after it is called, so captured context
  // is guaranteed to not be retained.
  static std::unique_ptr<DeviceWatcher> CreateWithIdleCallback(
      const std::string& directory_path, ExistsCallback exists_callback, IdleCallback idle_callback,
      async_dispatcher_t* dispatcher = nullptr);

  static std::unique_ptr<DeviceWatcher> CreateWithIdleCallback(
      fidl::ClientEnd<fuchsia_io::Directory> dir, ExistsCallback exists_callback,
      IdleCallback idle_callback, async_dispatcher_t* dispatcher = nullptr);

 private:
  DeviceWatcher(async_dispatcher_t* dispatcher, fidl::ClientEnd<fuchsia_io::Directory> dir,
                fidl::Endpoints<fuchsia_io::DirectoryWatcher> dir_watcher,
                ExistsCallback exists_callback, IdleCallback idle_callback);

  void Handler(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
               const zx_packet_signal* signal);

  fidl::ClientEnd<fuchsia_io::Directory> dir_;
  fidl::ClientEnd<fuchsia_io::DirectoryWatcher> dir_watcher_;
  ExistsCallback exists_callback_;
  IdleCallback idle_callback_;
  async::WaitMethod<DeviceWatcher, &DeviceWatcher::Handler> wait_;
  fxl::WeakPtrFactory<DeviceWatcher> weak_ptr_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DeviceWatcher);
};

}  // namespace fsl

#endif  // SRC_LIB_FSL_IO_DEVICE_WATCHER_H_
