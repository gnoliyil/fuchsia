// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/test/integration/fdio_test.h"

#include <fidl/fuchsia.fs/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fuchsia/inspect/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/inspect/cpp/hierarchy.h>
#include <lib/inspect/service/cpp/reader.h>
#include <lib/zx/result.h>

#include "lib/fidl/cpp/wire/internal/transport_channel.h"
#include "src/lib/storage/fs_management/cpp/admin.h"
#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/storage/blobfs/mkfs.h"

namespace blobfs {

constexpr uint64_t kBlockSize = 512;
constexpr uint64_t kNumBlocks = 8192;

void FdioTest::SetUp() {
  loop_ = std::make_unique<async::Loop>(&kAsyncLoopConfigNoAttachToCurrentThread);

  auto device = std::make_unique<block_client::FakeBlockDevice>(kNumBlocks, kBlockSize);
  block_device_ = device.get();
  ASSERT_EQ(FormatFilesystem(block_device_,
                             FilesystemOptions{
                                 .blob_layout_format = GetBlobLayoutFormat(),
                                 .oldest_minor_version = GetOldestMinorVersion(),
                             }),
            ZX_OK);

  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_EQ(endpoints.status_value(), ZX_OK);
  auto [outgoing_dir_client, outgoing_dir_server] = *std::move(endpoints);

  runner_ = std::make_unique<ComponentRunner>(
      *loop_, ComponentOptions{.pager_threads = mount_options_.paging_threads});
  ASSERT_EQ(runner_->ServeRoot(std::move(outgoing_dir_server), {}, {}, std::move(vmex_resource_))
                .status_value(),
            ZX_OK);

  ASSERT_EQ(runner_->Configure(std::move(device), mount_options_).status_value(), ZX_OK);

  ASSERT_EQ(loop_->StartThread("blobfs test dispatcher"), ZX_OK);

  auto root_client_or = fs_management::FsRootHandle(outgoing_dir_client);
  ASSERT_EQ(root_client_or.status_value(), ZX_OK);

  // FDIO serving the root directory.
  ASSERT_EQ(
      fdio_fd_create(root_client_or->TakeChannel().release(), root_fd_.reset_and_get_address()),
      ZX_OK);
  ASSERT_TRUE(root_fd_.is_valid());
  ASSERT_EQ(fdio_fd_create(outgoing_dir_client.TakeChannel().release(),
                           outgoing_dir_fd_.reset_and_get_address()),
            ZX_OK);
  ASSERT_TRUE(outgoing_dir_fd_.is_valid());
}

void FdioTest::TearDown() {
  fdio_cpp::UnownedFdioCaller outgoing_dir(outgoing_dir_fd_);
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_EQ(endpoints.status_value(), ZX_OK);
  ASSERT_EQ(fidl::WireCall(outgoing_dir.directory())
                ->Open(fuchsia_io::OpenFlags(0), {}, "svc",
                       fidl::ServerEnd<fuchsia_io::Node>(endpoints->server.TakeChannel()))
                .status(),
            ZX_OK);
  auto admin_client = component::ConnectAt<fuchsia_fs::Admin>(endpoints->client);
  ASSERT_EQ(admin_client.status_value(), ZX_OK);
  ASSERT_EQ(fidl::WireCall(*admin_client)->Shutdown().status(), ZX_OK);
}

zx_handle_t FdioTest::outgoing_dir() {
  zx::channel outgoing_dir;
  fdio_fd_clone(outgoing_dir_fd_.get(), outgoing_dir.reset_and_get_address());
  return outgoing_dir.release();
}

fpromise::result<inspect::Hierarchy> FdioTest::TakeSnapshot() {
  async::Loop loop = async::Loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  loop.StartThread("metric-collection-thread");
  async::Executor executor(loop.dispatcher());

  fuchsia::inspect::TreePtr tree;
  async_dispatcher_t* dispatcher = executor.dispatcher();
  zx_status_t status = fdio_service_connect_at(outgoing_dir(), "diagnostics/fuchsia.inspect.Tree",
                                               tree.NewRequest(dispatcher).TakeChannel().release());
  if (status != ZX_OK) {
    return fpromise::error();
  }

  std::condition_variable cv;
  std::mutex m;
  bool done = false;
  fpromise::result<inspect::Hierarchy> hierarchy_or_error;

  auto promise = inspect::ReadFromTree(std::move(tree))
                     .then([&](fpromise::result<inspect::Hierarchy>& result) {
                       {
                         std::unique_lock<std::mutex> lock(m);
                         hierarchy_or_error = std::move(result);
                         done = true;
                       }
                       cv.notify_all();
                     });

  executor.schedule_task(std::move(promise));

  std::unique_lock<std::mutex> lock(m);
  cv.wait(lock, [&done]() { return done; });

  loop.Quit();
  loop.JoinThreads();

  return hierarchy_or_error;
}

void FdioTest::GetUintMetricFromHierarchy(const inspect::Hierarchy& hierarchy,
                                          const std::vector<std::string>& path,
                                          const std::string& property, uint64_t* value) {
  ASSERT_NE(value, nullptr);
  const inspect::Hierarchy* direct_parent = hierarchy.GetByPath(path);
  ASSERT_NE(direct_parent, nullptr);

  const inspect::UintPropertyValue* property_node =
      direct_parent->node().get_property<inspect::UintPropertyValue>(property);
  ASSERT_NE(property_node, nullptr);

  *value = property_node->value();
}

void FdioTest::GetUintMetric(const std::vector<std::string>& path, const std::string& property,
                             uint64_t* value) {
  fpromise::result<inspect::Hierarchy> hierarchy_or_error = TakeSnapshot();
  ASSERT_TRUE(hierarchy_or_error.is_ok());
  GetUintMetricFromHierarchy(hierarchy_or_error.value(), path, property, value);
}

}  // namespace blobfs
