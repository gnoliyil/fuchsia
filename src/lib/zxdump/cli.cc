// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cli.h"

#include <fcntl.h>

#include <cstdlib>
#include <iostream>
#include <utility>

void CommandLineHelper::KoidArguments(int argc, char** argv, int i, bool allow_jobs) {
  live_ = live_ || dump_files_.empty();

  // Collect data from all the dumps.
  bool used_stdin = false;
  while (!dump_files_.empty()) {
    const char* filename = dump_files_.front();
    dump_files_.pop();
    fbl::unique_fd fd;
    if (std::string_view{filename} == "-") {
      if (used_stdin) {
        std::cerr << "cannot use stdin (-) as an input more than once" << std::endl;
        Ok(false);
        continue;
      }
      used_stdin = true;
      fd.reset(STDIN_FILENO);
    } else {
      fd.reset(open(filename, O_RDONLY));
      if (!fd) {
        perror(filename);
        Ok(false);
        continue;
      }
    }
    auto result = holder_.Insert(std::move(fd), read_memory_);
    if (result.is_error()) {
      if (result.error_value().status_ == ZX_ERR_IO) {
        perror(filename);
        Ok(false);
      } else {
        Ok(result, filename);
      }
    }
  }

  // Don't fetch the live root job until we need it, so we don't generate extra
  // errors if we're in a no-op or error path anyway.
  auto get_root_job = [this, need_live = live_]() mutable -> zxdump::Job& {
    if (std::exchange(need_live, false)) {
      if (auto root = zxdump::GetRootJob(); Ok(root, "cannot get root job")) {
        Ok(holder_.Insert(std::move(root).value()), "root job");
      }
    }
    return holder_.root_job();
  };

  if (use_root_job_) {
    zxdump::Job& root_job = get_root_job();
    if (root_job.koid() != ZX_KOID_INVALID) {
      tasks_.emplace(root_job);
    } else {
      // The dumps didn't form a single whole job tree, so the root job is not
      // a real task and shouldn't itself be dumped.  Instead act like all its
      // immediate children (jobs and processes) had been on the command line.
      if (auto result = root_job.children(); Ok(result, "root children")) {
        for (auto& [koid, child] : result.value().get()) {
          tasks_.emplace(child);
        }
      }
      if (auto result = root_job.processes(); Ok(result, "root processes")) {
        for (auto& [koid, process] : result.value().get()) {
          tasks_.emplace(process);
        }
      }
    }
  }

  while (i < argc) {
    const char* const arg = argv[i++];

    char* p;
    zx_koid_t pid = strtoul(arg, &p, 0);
    if (*p != '\0') {
      std::cerr << arg << ": not a PID or job KOID" << std::endl;
      Ok(false);
      continue;
    }

    auto result = get_root_job().find(pid);
    if (Ok(result, pid)) {
      zxdump::Task& task = result.value();
      switch (task.type()) {
        case ZX_OBJ_TYPE_PROCESS:
          break;

        case ZX_OBJ_TYPE_JOB:
          if (allow_jobs) {
            break;
          }
          [[fallthrough]];

        default:
          std::cerr << pid << ": KOID is not a process" << (allow_jobs ? " or job" : "")
                    << std::endl;
          Ok(false);
          continue;
      }
      tasks_.emplace(task);
    }
  }
}

void CommandLineHelper::NeedRootResource(bool need) {
  if (need && live_) {
    auto result = zxdump::GetRootResource();
    if (Ok(result, "cannot get root resource")) {
      Ok(holder_.Insert(std::move(result).value()), "root resource");
    }
  }
}

void CommandLineHelper::NeedSystem(bool need) {
  if (need && live_) {
    Ok(holder_.InsertSystem(), "cannot get live system data");
  }
}
