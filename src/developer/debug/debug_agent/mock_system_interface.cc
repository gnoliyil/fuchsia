// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/mock_system_interface.h"

namespace debug_agent {

std::unique_ptr<JobHandle> MockSystemInterface::GetRootJob() const {
  return std::make_unique<MockJobHandle>(root_job_);
}

std::unique_ptr<BinaryLauncher> MockSystemInterface::GetLauncher() const {
  // Unimplemented in this mock.
  FX_NOTREACHED();
  return nullptr;
}

std::unique_ptr<MockSystemInterface> MockSystemInterface::CreateWithData() {
  // Job 121.
  MockProcessHandle job121_p1(19, "job121-p1");
  job121_p1.set_threads({MockThreadHandle(20, "initial-thread")});

  MockProcessHandle job121_p2(21, "job121-p2");
  job121_p2.set_threads({MockThreadHandle(22, "initial-thread"),
                         MockThreadHandle(23, "second-thread"),
                         MockThreadHandle(24, "third-thread")});

  MockJobHandle job121(18, "job121");
  job121.set_child_processes({job121_p1, job121_p2});

  // Job 12.
  MockJobHandle job12(17, "job12");
  job12.set_child_jobs({job121});

  // Job 11.
  MockProcessHandle job11_p1(14, "job11-p1");
  job11_p1.set_threads(
      {MockThreadHandle(15, "initial-thread"), MockThreadHandle(16, "second-thread")});

  MockJobHandle job11(13, "job11");
  job11.set_child_processes({job11_p1});

  // Job 1
  MockProcessHandle job1_p1(9, "job1-p1");
  job1_p1.set_threads({MockThreadHandle(10, "initial-thread")});

  MockProcessHandle job1_p2(11, "job1-p2");
  job1_p2.set_threads({MockThreadHandle(12, "initial-thread")});

  MockJobHandle job1(8, "job1");
  job1.set_child_processes({job1_p1, job1_p2});
  job1.set_child_jobs({job11, job12});

  // Job 2
  MockProcessHandle job2_p1(26, "job2-p1");
  job2_p1.set_threads({MockThreadHandle(27, "initial-thread")});

  MockJobHandle job2(25, "job2");
  job2.set_child_processes({job2_p1});

  // Root.
  MockProcessHandle root_p1(2, "root-p1");
  root_p1.set_threads({MockThreadHandle(3, "initial-thread")});

  MockProcessHandle root_p2(4, "root-p2");
  root_p2.set_threads({MockThreadHandle(5, "initial-thread")});

  MockProcessHandle root_p3(6, "root-p3");
  root_p3.set_threads({MockThreadHandle(7, "initial-thread")});

  MockJobHandle root(1, "root");
  root.set_child_processes({root_p1, root_p2, root_p3});
  root.set_child_jobs({job1, job2});

  auto system_interface = std::make_unique<MockSystemInterface>(std::move(root));

  system_interface->mock_component_manager().component_info().emplace(
      job1.GetKoid(),
      debug_ipc::ComponentInfo{.moniker = "/moniker",
                               .url = "fuchsia-pkg://devhost/package#meta/component.cm"});

  system_interface->mock_component_manager().component_info().emplace(
      job2.GetKoid(),
      debug_ipc::ComponentInfo{.moniker = "/a/long/generated_to_here/fixed/moniker",
                               .url = "fuchsia-pkg://devhost/test_package#meta/component2.cm"});

  return system_interface;
}

}  // namespace debug_agent
