// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/io/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <gtest/gtest.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/storage/f2fs/f2fs.h"
#include "src/storage/f2fs/vnode.h"
#include "unit_lib.h"

namespace f2fs {
namespace {

class AsyncTearDownVnode : public Dir {
 public:
  AsyncTearDownVnode(F2fs* fs, ino_t ino, sync_completion_t* completions)
      : Dir(fs, ino, 0), callback_(nullptr), completions_(completions) {}

  ~AsyncTearDownVnode() {
    // C) Tear down the Vnode.
    sync_completion_signal(&completions_[2]);
  }

 private:
  void Sync(fs::Vnode::SyncCallback callback) final {
    callback_ = std::move(callback);
    std::thread thrd(&AsyncTearDownVnode::SyncThread, this);
    thrd.detach();
  }

  static void SyncThread(AsyncTearDownVnode* arg) {
    fs::Vnode::SyncCallback callback;
    {
      fbl::RefPtr<AsyncTearDownVnode> async_vn = fbl::RefPtr(arg);
      // A) Identify when the sync has started being processed.
      sync_completion_signal(&async_vn->completions_[0]);
      // B) Wait until the connection has been closed.
      sync_completion_wait(&async_vn->completions_[1], ZX_TIME_INFINITE);
      callback = std::move(async_vn->callback_);
    }
    callback(ZX_OK);
  }

  fs::Vnode::SyncCallback callback_;
  sync_completion_t* completions_;
};

TEST(Teardown, ShutdownOnNoConnections) {
  std::unique_ptr<f2fs::Bcache> bc;
  FileTester::MkfsOnFakeDev(&bc);

  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);

  MountOptions options{};
  ASSERT_EQ(options.SetValue(options.GetNameView(kOptDiscard), 1), ZX_OK);
  auto vfs_or = Runner::CreateRunner(loop.dispatcher());
  ASSERT_TRUE(vfs_or.is_ok());
  auto fs_or = F2fs::Create(nullptr, std::move(bc), options, (*vfs_or).get());
  ASSERT_TRUE(fs_or.is_ok());
  auto fs = (*fs_or).get();
  auto on_unmount = []() { FX_LOGS(INFO) << "[f2fs] Shutdown complete"; };
  vfs_or->SetUnmountCallback(std::move(on_unmount));
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  sync_completion_t root_completions[3], child_completions[3];

  // Create root directory connection.
  nid_t root_nid;
  ASSERT_TRUE(fs->GetNodeManager().AllocNid(root_nid).is_ok());
  auto root_dir = fbl::AdoptRef(new AsyncTearDownVnode(fs, root_nid, root_completions));
  root_dir->SetMode(S_IFDIR);

  async::Loop clients_loop(&kAsyncLoopConfigAttachToCurrentThread);
  fuchsia::io::DirectoryPtr root_client;
  auto root_server = root_client.NewRequest();
  ASSERT_TRUE(root_client.is_bound());
  ASSERT_EQ(vfs_or->ServeDirectory(std::move(root_dir), fidl::ServerEnd<fuchsia_io::Directory>(
                                                            root_server.TakeChannel())),
            ZX_OK);

  // A) Wait for root directory sync to begin.
  root_client->Sync([](fuchsia::io::Node2_Sync_Result) {});
  sync_completion_wait(&root_completions[0], ZX_TIME_INFINITE);

  // Create child vnode connection.
  nid_t child_nid;
  ASSERT_TRUE(fs->GetNodeManager().AllocNid(child_nid).is_ok());
  auto child_dir = fbl::AdoptRef(new AsyncTearDownVnode(fs, child_nid, child_completions));
  child_dir->SetMode(S_IFDIR);

  fuchsia::io::DirectoryPtr child_client;
  child_client.set_error_handler([&clients_loop](zx_status_t status) {
    ASSERT_EQ(status, ZX_OK);
    clients_loop.Quit();
  });
  auto child_server = child_client.NewRequest();
  ASSERT_TRUE(child_client.is_bound());
  auto validated_options = child_dir->ValidateOptions(fs::VnodeConnectionOptions());
  ASSERT_TRUE(validated_options.is_ok());
  ASSERT_EQ(child_dir->Open(validated_options.value(), nullptr), ZX_OK);
  ASSERT_EQ(
      vfs_or->Serve(std::move(child_dir), child_server.TakeChannel(), validated_options.value()),
      ZX_OK);

  // A) Wait for child vnode sync to begin.
  child_client->Sync([](fuchsia::io::Node2_Sync_Result) {});
  sync_completion_wait(&child_completions[0], ZX_TIME_INFINITE);

  // Terminate root directory connection.
  root_client.Unbind();

  // B) Let complete sync.
  sync_completion_signal(&root_completions[1]);

  // C) Tear down root directory.
  sync_completion_wait(&root_completions[2], ZX_TIME_INFINITE);

  // Sleep for a while until filesystem shutdown completes.
  zx::nanosleep(zx::deadline_after(zx::sec(1)));
  ASSERT_FALSE(vfs_or->IsTerminating());

  // Terminate child vnode connection.
  child_client.Unbind();

  // B) Let complete sync.
  sync_completion_signal(&child_completions[1]);

  // C) Tear down child vnode.
  sync_completion_wait(&child_completions[2], ZX_TIME_INFINITE);

  // Sleep for a while until filesystem shutdown completes.
  zx::nanosleep(zx::deadline_after(zx::sec(1)));
  ASSERT_TRUE(vfs_or->IsTerminating());
  fs->PutSuper();
}

}  // namespace
}  // namespace f2fs
