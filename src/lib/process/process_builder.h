// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_PROCESS_PROCESS_BUILDER_H_
#define SRC_LIB_PROCESS_PROCESS_BUILDER_H_

#include <fuchsia/process/cpp/fidl.h>
#include <lib/zx/vmo.h>

#include <string>
#include <vector>

#include "lib/sys/cpp/service_directory.h"

namespace process {

// Creates a process step-by-step.
//
// For most use cases, |fdio_spawn| and |fdio_spawn_etc| are better choices for
// creating processes. However, if you need to manipulate the process after it
// has been created but before it has been started, you might want to use this
// class.
class ProcessBuilder {
 public:
  // Creates a process builder that uses the |fuchsia.process.Launcher| service.
  //
  // The process is created in zx_job_default().
  explicit ProcessBuilder(std::shared_ptr<sys::ServiceDirectory> services);

  // Creates a process builder that will build a process in the given |job|.
  ProcessBuilder(zx::job job, std::shared_ptr<sys::ServiceDirectory> services);
  ~ProcessBuilder();

  ProcessBuilder(const ProcessBuilder&) = delete;
  ProcessBuilder& operator=(const ProcessBuilder&) = delete;

  // Use |executable| as the executable for the process. The calling code is
  // responsible for setting the correct loader service via AddHandle().
  void LoadVMO(zx::vmo executable);

  // Load the executable for the process from the given |path|.
  zx_status_t LoadPath(const std::string& path);

  // Append arguments to the argument list for the process.
  //
  // Safe to call multiple times.
  zx_status_t AddArgs(const std::vector<std::string>& argv);

  // Adds the given handle to the handle list for the process.
  //
  // Safe to call multiple times.
  void AddHandle(uint32_t id, zx::handle handle);

  // Adds the given handles to the handle list for the process.
  //
  // Safe to call multiple times.
  void AddHandles(std::vector<fuchsia::process::HandleInfo> handles);

  // Provide |job| to the process as PA_JOB_DEFAULT.
  //
  // By default, the created process will use this job when creating more
  // processes.
  //
  // Does not affect in which job the process is created.
  void SetDefaultJob(zx::job job);

  // Set a name for the process.
  //
  // The name purely descriptive and used in process listing and other
  // diagnostic tools. The name doesn't need to correspond to the path.
  //
  // If |AddArgs| is called with a non-empty vector, the name for the process
  // will be taken from |argv[0]| the first time |AddArgs| is called. You can
  // call this function either before or after |AddArgs| to override the name.
  //
  // If you never call |AddArgs| with a non-empty vector, you will need to call
  // this function to set a name for the process.
  void SetName(std::string name);

  // Passes the job in which the process will be created as the |PA_JOB_DEFAULT|
  // for the created process.
  //
  // Defaults to zx_job_default() unless you passed a job explicitly when
  // constructing this object.
  zx_status_t CloneJob();

  // Clone the FDIO namespace for this process as the namespace for the created
  // process.
  zx_status_t CloneNamespace();

  // Clone the STDIO for this process as the namespace for the created process.
  //
  // If any of stdin, stdout, or stderr are closed (or otherwise not clonable),
  // they are ignored.
  void CloneStdio();

  // Clone the environ for this process as the environ for the created process.
  zx_status_t CloneEnvironment();

  // Calls |CloneJob|, |CloneLdsvc|, |CloneNamespace|, |CloneStdio|, and
  // |CloneEnvironment|.
  zx_status_t CloneAll();

  // Clone the local file descriptor |local_fd| as the file descriptor
  // |target_fd| in the created process.
  zx_status_t CloneFileDescriptor(int local_fd, int target_fd);

  // Create the process.
  //
  // Upon success, the process is created but not started. At this point, the
  // process can be inspected and manipulated using |data|.
  //
  // |error_message| is optional.
  zx_status_t Prepare(std::string* error_message);

  // Actually start the process.
  //
  // Only valid after |Prepare| has been called successfully.
  zx_status_t Start(zx::process* process_out);

  // Information about the process prior to start.
  //
  // Valid only between |Prepare| and |Start|.
  const fuchsia::process::ProcessStartData& data() const { return data_; }

 private:
  std::shared_ptr<sys::ServiceDirectory> services_;
  fuchsia::process::LauncherSyncPtr launcher_;
  fuchsia::process::LaunchInfo launch_info_;
  fuchsia::process::ProcessStartData data_;
  std::vector<fuchsia::process::HandleInfo> handles_;
};

}  // namespace process

#endif  // SRC_LIB_PROCESS_PROCESS_BUILDER_H_
