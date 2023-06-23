// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.cpu.profiler/cpp/fidl.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/socket.h>
#include <lib/zx/thread.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>

#include <sstream>
#include <thread>

#include <gtest/gtest.h>

#include "src/lib/fsl/socket/strings.h"

void MakeWork() {
  for (;;) {
  }
  zx_thread_exit();
}

TEST(ProfilerIntegrationTest, EndToEnd) {
  zx::result client_end = component::Connect<fuchsia_cpu_profiler::Session>();
  ASSERT_TRUE(client_end.is_ok());
  const fidl::SyncClient client{std::move(*client_end)};

  zx::socket in_socket;
  zx::socket outgoing_socket;

  ASSERT_EQ(zx::socket::create(0u, &in_socket, &outgoing_socket), ZX_OK);
  zx::process self;
  zx::process::self()->duplicate(ZX_RIGHT_SAME_RIGHTS, &self);

  zx::thread child;
  zx_status_t res = zx::thread::create(self, "TestChild", 9, 0, &child);

  static uint8_t stack[1024] __ALIGNED(16);
  res = child.start(reinterpret_cast<uintptr_t>(MakeWork),
                    reinterpret_cast<uintptr_t>(stack + sizeof(stack)), 0, 0);
  ASSERT_EQ(ZX_OK, res);

  res = child.wait_one(ZX_THREAD_RUNNING, zx::deadline_after(zx::sec(1)), nullptr);
  ASSERT_EQ(ZX_OK, res);

  zx_info_handle_basic_t info;
  res = child.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  ASSERT_EQ(ZX_OK, res);

  fuchsia_cpu_profiler::SamplingConfig sampling_config{{
      .period = 1000000,
      .timebase = fuchsia_cpu_profiler::Counter::WithPlatformIndependent(
          fuchsia_cpu_profiler::CounterId::kNanoseconds),
      .sample = fuchsia_cpu_profiler::Sample{{
          .callgraph =
              fuchsia_cpu_profiler::CallgraphConfig{
                  {.strategy = fuchsia_cpu_profiler::CallgraphStrategy::kFramePointer}},
          .counters = std::vector<fuchsia_cpu_profiler::Counter>{},
      }},
  }};

  fuchsia_cpu_profiler::TargetConfig target_config{{
      .task = fuchsia_cpu_profiler::Task::WithThread(info.koid),
  }};

  fuchsia_cpu_profiler::Config config{{
      .configs = std::vector{sampling_config},
      .targets = std::vector{target_config},
  }};

  auto config_response = client->Configure({{
      .output = std::move(outgoing_socket),
      .config = config,
  }});
  ASSERT_TRUE(config_response.is_ok());

  auto start_response = client->Start({{.buffer_results = true}});
  ASSERT_TRUE(start_response.is_ok());
  // Get Some samples
  sleep(1);

  auto stop_response = client->Stop();
  ASSERT_TRUE(stop_response.is_ok());
  ASSERT_TRUE(stop_response.value().samples_collected().has_value());
  EXPECT_GT(stop_response.value().samples_collected().value(), size_t{10});

  auto reset_response = client->Reset();
  ASSERT_TRUE(reset_response.is_ok());
}

// Monitor ourself and check that if we start new threads after the profiling session starts, that
// one or more of them show up in the samples we take.
TEST(ProfilerIntegrationTest, NewThreads) {
  zx::result client_end = component::Connect<fuchsia_cpu_profiler::Session>();
  ASSERT_TRUE(client_end.is_ok());
  const fidl::SyncClient client{std::move(*client_end)};

  zx::socket in_socket;
  zx::socket outgoing_socket;

  ASSERT_EQ(zx::socket::create(0u, &in_socket, &outgoing_socket), ZX_OK);
  zx::unowned_process self = zx::process::self();

  zx_info_handle_basic_t info;
  zx_status_t info_result =
      self->get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  ASSERT_EQ(ZX_OK, info_result);

  fuchsia_cpu_profiler::SamplingConfig sampling_config{{
      .period = 1000000,
      .timebase = fuchsia_cpu_profiler::Counter::WithPlatformIndependent(
          fuchsia_cpu_profiler::CounterId::kNanoseconds),
      .sample = fuchsia_cpu_profiler::Sample{{
          .callgraph =
              fuchsia_cpu_profiler::CallgraphConfig{
                  {.strategy = fuchsia_cpu_profiler::CallgraphStrategy::kFramePointer}},
          .counters = std::vector<fuchsia_cpu_profiler::Counter>{},
      }},
  }};

  // We'll sample ourself.
  fuchsia_cpu_profiler::TargetConfig target_config{{
      .task = fuchsia_cpu_profiler::Task::WithProcess(info.koid),
  }};

  fuchsia_cpu_profiler::Config config{{
      .configs = std::vector{sampling_config},
      .targets = std::vector{target_config},
  }};

  auto config_response = client->Configure({{
      .output = std::move(outgoing_socket),
      .config = config,
  }});
  ASSERT_TRUE(config_response.is_ok());

  auto start_response = client->Start({{.buffer_results = true}});
  ASSERT_TRUE(start_response.is_ok());
  // Start some threads;
  std::thread t1{MakeWork};
  std::thread t2{MakeWork};
  std::thread t3{MakeWork};
  t1.detach();
  t2.detach();
  t3.detach();
  // Get Some samples
  sleep(1);

  auto stop_response = client->Stop();
  ASSERT_TRUE(stop_response.is_ok());
  ASSERT_TRUE(stop_response.value().samples_collected().has_value());
  EXPECT_GT(stop_response.value().samples_collected().value(), size_t{10});

  std::string contents;
  fsl::BlockingCopyToString(std::move(in_socket), &contents);

  std::stringstream ss;
  ss << contents;
  std::set<zx_koid_t> pids;
  std::set<zx_koid_t> tids;
  // The socket data looks like:
  // <pid>\n
  // <tid>\n
  // {{{bt1}}}\n
  // {{{bt2}}}\n
  // ...
  // <pid>\n
  // <tid>\n
  // {{{bt1}}}\n
  // {{{bt2}}}\n
  // ...
  for (std::string pid_string; std::getline(ss, pid_string);) {
    if (pid_string.empty() || !isdigit(pid_string[0])) {
      continue;
    }
    std::string tid_string;
    std::getline(ss, tid_string);
    pids.insert(strtoll(pid_string.data(), nullptr, 0));
    tids.insert(strtoll(tid_string.data(), nullptr, 0));
  }
  auto reset_response = client->Reset();
  ASSERT_TRUE(reset_response.is_ok());

  // We should only have one pid
  EXPECT_EQ(size_t{1}, pids.size());

  // We should only have more than one thread
  EXPECT_GT(tids.size(), size_t{1});
}

// Monitor ourself via our job id
TEST(ProfilerIntegrationTest, OwnJobId) {
  zx::result client_end = component::Connect<fuchsia_cpu_profiler::Session>();
  ASSERT_TRUE(client_end.is_ok());
  const fidl::SyncClient client{std::move(*client_end)};

  zx::socket in_socket;
  zx::socket outgoing_socket;

  ASSERT_EQ(zx::socket::create(0u, &in_socket, &outgoing_socket), ZX_OK);
  zx::unowned_job self = zx::job::default_job();

  zx_info_handle_basic_t info;
  zx_status_t info_result =
      self->get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  ASSERT_EQ(ZX_OK, info_result);

  fuchsia_cpu_profiler::SamplingConfig sampling_config{{
      .period = 1000000,
      .timebase = fuchsia_cpu_profiler::Counter::WithPlatformIndependent(
          fuchsia_cpu_profiler::CounterId::kNanoseconds),
      .sample = fuchsia_cpu_profiler::Sample{{
          .callgraph =
              fuchsia_cpu_profiler::CallgraphConfig{
                  {.strategy = fuchsia_cpu_profiler::CallgraphStrategy::kFramePointer}},
          .counters = std::vector<fuchsia_cpu_profiler::Counter>{},
      }},
  }};

  // We'll sample ourself by our job id
  fuchsia_cpu_profiler::TargetConfig target_config{{
      .task = fuchsia_cpu_profiler::Task::WithJob(info.koid),
  }};

  fuchsia_cpu_profiler::Config config{{
      .configs = std::vector{sampling_config},
      .targets = std::vector{target_config},
  }};

  auto config_response = client->Configure({{
      .output = std::move(outgoing_socket),
      .config = config,
  }});
  ASSERT_TRUE(config_response.is_ok());

  auto start_response = client->Start({{.buffer_results = true}});

  std::thread t1{MakeWork};
  std::thread t2{MakeWork};
  std::thread t3{MakeWork};
  t1.detach();
  t2.detach();
  t3.detach();
  ASSERT_TRUE(start_response.is_ok());
  // Get Some samples
  sleep(1);

  auto stop_response = client->Stop();
  ASSERT_TRUE(stop_response.is_ok());
  ASSERT_TRUE(stop_response.value().samples_collected().has_value());
  ASSERT_GT(stop_response.value().samples_collected().value(), size_t{10});

  std::string contents;
  fsl::BlockingCopyToString(std::move(in_socket), &contents);

  std::stringstream ss;
  ss << contents;
  std::set<zx_koid_t> pids;
  std::set<zx_koid_t> tids;
  // The socket data looks like:
  // <pid>\n
  // <tid>\n
  // {{{bt1}}}\n
  // {{{bt2}}}\n
  // ...
  // <pid>\n
  // <tid>\n
  // {{{bt1}}}\n
  // {{{bt2}}}\n
  // ...
  for (std::string pid_string; std::getline(ss, pid_string);) {
    if (pid_string.empty() || !isdigit(pid_string[0])) {
      continue;
    }
    std::string tid_string;
    std::getline(ss, tid_string);
    pids.insert(strtoll(pid_string.data(), nullptr, 0));
    tids.insert(strtoll(tid_string.data(), nullptr, 0));
  }
  auto reset_response = client->Reset();
  ASSERT_TRUE(reset_response.is_ok());

  // We should only have one pid
  EXPECT_EQ(size_t{1}, pids.size());

  // And that pid should be us
  zx::unowned_process process_self = zx::process::self();
  zx_info_handle_basic_t process_info;
  ASSERT_EQ(ZX_OK, process_self->get_info(ZX_INFO_HANDLE_BASIC, &process_info, sizeof(process_info),
                                          nullptr, nullptr));
  EXPECT_EQ(*pids.begin(), process_info.koid);
}
