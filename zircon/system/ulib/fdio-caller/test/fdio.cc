// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/fd.h>
#include <lib/sync/completion.h>
#include <unistd.h>

#include <utility>

#include <fbl/unique_fd.h>
#include <zxtest/zxtest.h>

#include "src/storage/memfs/memfs.h"
#include "src/storage/memfs/vnode_dir.h"

namespace fio = fuchsia_io;

namespace {

void TryFilesystemOperations(fidl::UnownedClientEnd<fio::File> client_end) {
  constexpr std::string_view payload("foobar");
  const fidl::WireResult write_result =
      fidl::WireCall(client_end)
          ->WriteAt(
              fidl::VectorView<uint8_t>::FromExternal(
                  reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data())), payload.size()),
              0);
  ASSERT_EQ(write_result.status(), ZX_OK);
  const fit::result write_response = write_result.value();
  ASSERT_TRUE(write_response.is_ok(), "%s", zx_status_get_string(write_response.error_value()));
  ASSERT_EQ(write_response.value()->actual_count, payload.size());

  const fidl::WireResult read_result = fidl::WireCall(client_end)->ReadAt(256, 0);
  ASSERT_OK(read_result.status());
  const fit::result read_response = read_result.value();
  ASSERT_TRUE(read_response.is_ok(), "%s", zx_status_get_string(read_response.error_value()));
  const fidl::VectorView data = read_result->value()->data;
  ASSERT_EQ(std::string_view(reinterpret_cast<const char*>(data.data()), data.count()), payload);
}

void TryFilesystemOperations(const zx::unowned_channel& channel) {
  TryFilesystemOperations(fidl::UnownedClientEnd<fio::File>(channel));
}

void TryFilesystemOperations(const zx::channel& channel) {
  TryFilesystemOperations(channel.borrow());
}

void TryFilesystemOperations(const fdio_cpp::FdioCaller& caller) {
  TryFilesystemOperations(caller.channel());
}

void TryFilesystemOperations(const fdio_cpp::UnownedFdioCaller& caller) {
  TryFilesystemOperations(caller.channel());
}

class Harness {
 public:
  Harness() = default;
  ~Harness() = default;

  void Setup() {
    ASSERT_EQ(loop_.StartThread(), ZX_OK);

    zx::result result = memfs::Memfs::Create(loop_.dispatcher(), "<tmp>");
    ASSERT_OK(result);
    auto& [memfs, root] = result.value();

    zx::result endpoints = fidl::CreateEndpoints<fio::Directory>();
    ASSERT_OK(endpoints);
    auto& [client, server] = endpoints.value();

    ASSERT_OK(memfs->ServeDirectory(std::move(root), std::move(server)));
    memfs_ = std::move(memfs);

    fbl::unique_fd dir;
    ASSERT_EQ(fdio_fd_create(client.TakeChannel().release(), dir.reset_and_get_address()), ZX_OK);
    fd_.reset(openat(dir.get(), "my-file", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR));
    ASSERT_TRUE(fd_);
  }

  fbl::unique_fd fd() { return std::move(fd_); }

 private:
  async::Loop loop_ = async::Loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  std::unique_ptr<memfs::Memfs> memfs_;  // Must be after the loop_ for proper tear-down.
  fbl::unique_fd fd_;
};

TEST(FdioCallTests, FdioCallerFile) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();

  // Try some filesystem operations.
  fdio_cpp::FdioCaller caller(std::move(fd));
  ASSERT_TRUE(caller);
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller));

  // Re-acquire the underlying fd.
  fd = caller.release();
  ASSERT_EQ(close(fd.release()), 0);
}

TEST(FdioCallTests, FdioCallerMoveAssignment) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();

  fdio_cpp::FdioCaller caller(std::move(fd));
  fdio_cpp::FdioCaller move_assignment_caller = std::move(caller);
  ASSERT_TRUE(move_assignment_caller);
  ASSERT_FALSE(caller);
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(move_assignment_caller));
}

TEST(FdioCallTests, FdioCallerMoveConstructor) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();

  fdio_cpp::FdioCaller caller(std::move(fd));
  fdio_cpp::FdioCaller move_ctor_caller(std::move(caller));
  ASSERT_TRUE(move_ctor_caller);
  ASSERT_FALSE(caller);
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(move_ctor_caller));
}

TEST(FdioCallTests, FdioCallerBorrow) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();

  fdio_cpp::FdioCaller caller(std::move(fd));
  zx::unowned_channel channel = caller.channel();
  ASSERT_TRUE(channel->is_valid());
  ASSERT_TRUE(caller);
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(channel));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.node().channel()));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.file().channel()));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.directory().channel()));
}

TEST(FdioCallTests, FdioCallerClone) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();

  fdio_cpp::FdioCaller caller(std::move(fd));
  auto channel = caller.clone_channel();
  ASSERT_OK(channel.status_value());
  ASSERT_TRUE(channel->is_valid());
  ASSERT_TRUE(caller);
  ASSERT_NE(caller.channel()->get(), channel->get());
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(channel->borrow()));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.clone_node()->channel()));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.clone_file()->channel()));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.clone_directory()->channel()));
}

TEST(FdioCallTests, FdioCallerTake) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();

  fdio_cpp::FdioCaller caller(std::move(fd));
  auto channel = caller.take_channel();
  ASSERT_OK(channel.status_value());
  ASSERT_TRUE(channel->is_valid());
  ASSERT_FALSE(caller);
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(channel->borrow()));
}

TEST(FdioCallTests, FdioCallerTakeAs) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();

  fdio_cpp::FdioCaller caller(std::move(fd));
  auto channel = caller.take_as<fio::File>();
  ASSERT_OK(channel.status_value());
  ASSERT_TRUE(channel->is_valid());
  ASSERT_FALSE(caller);
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(channel->borrow()));
}

TEST(FdioCallTests, UnownedFdioCaller) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();
  fdio_cpp::UnownedFdioCaller caller(fd);
  ASSERT_TRUE(caller);
  ASSERT_TRUE(fd);
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller));
}

TEST(FdioCallTests, UnownedFdioCallerBorrow) {
  Harness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Setup());
  auto fd = harness.fd();

  fdio_cpp::UnownedFdioCaller caller(fd);
  zx::unowned_channel channel = caller.channel();
  ASSERT_TRUE(channel->is_valid());
  ASSERT_TRUE(caller);
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(channel));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.node().channel()));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.file().channel()));
  ASSERT_NO_FATAL_FAILURE(TryFilesystemOperations(caller.directory().channel()));
}

}  // namespace
