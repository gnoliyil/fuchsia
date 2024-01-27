// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire_test_base.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <lib/zx/channel.h>

#include <string>

#include <fbl/unique_fd.h>

#include "src/storage/fs_test/fs_test_fixture.h"
#include "src/storage/fs_test/misc.h"

namespace fs_test {
namespace {

namespace fio = fuchsia_io;

using OpenTest = FilesystemTest;

fidl::ClientEnd<fio::Directory> CreateDirectory(fio::wire::OpenFlags dir_flags,
                                                const std::string& path) {
  EXPECT_EQ(mkdir(path.c_str(), 0755), 0);

  auto endpoints = fidl::CreateEndpoints<fio::Directory>();
  EXPECT_EQ(endpoints.status_value(), ZX_OK);
  EXPECT_EQ(
      fdio_open(path.c_str(), static_cast<uint32_t>(dir_flags | fio::wire::OpenFlags::kDirectory),
                endpoints->server.TakeChannel().release()),
      ZX_OK);

  return std::move(endpoints->client);
}

zx_status_t OpenFileWithCreate(const fidl::ClientEnd<fio::Directory>& dir,
                               const std::string& path) {
  fio::wire::OpenFlags child_flags =
      fio::wire::OpenFlags::kCreate | fio::wire::OpenFlags::kRightReadable |
      fio::wire::OpenFlags::kNotDirectory | fio::wire::OpenFlags::kDescribe;
  auto child_endpoints = fidl::CreateEndpoints<fio::Node>();
  EXPECT_EQ(child_endpoints.status_value(), ZX_OK);
  auto open_res = fidl::WireCall(dir)->Open(child_flags, {}, fidl::StringView::FromExternal(path),
                                            std::move(child_endpoints->server));
  EXPECT_EQ(open_res.status(), ZX_OK);
  fidl::WireSyncClient child{std::move(child_endpoints->client)};

  class EventHandler : public fidl::testing::WireSyncEventHandlerTestBase<fio::Node> {
   public:
    EventHandler() = default;
    zx_status_t status() const { return status_; }

    void OnOpen(fidl::WireEvent<fio::Node::OnOpen>* event) override { status_ = event->s; }

    void NotImplemented_(const std::string& name) override { FAIL() << "Unexpected " << name; }

   private:
    zx_status_t status_ = ZX_OK;
  };

  EventHandler event_handler;
  auto handle_res = child.HandleOneEvent(event_handler);
  EXPECT_EQ(handle_res.status(), ZX_OK);

  return event_handler.status();
}

TEST_P(OpenTest, OpenFileWithCreateCreatesInReadWriteDir) {
  fio::wire::OpenFlags flags =
      fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kRightWritable;
  auto parent = CreateDirectory(flags, GetPath("a"));
  EXPECT_EQ(OpenFileWithCreate(parent, "b"), ZX_OK);
}

TEST_P(OpenTest, OpenFileWithCreateFailsInReadOnlyDir) {
  fio::wire::OpenFlags flags = fio::wire::OpenFlags::kRightReadable;
  auto parent = CreateDirectory(flags, GetPath("a"));
  EXPECT_EQ(OpenFileWithCreate(parent, "b"), ZX_ERR_ACCESS_DENIED);
}

TEST_P(OpenTest, OpenFileWithCreateCreatesInReadWriteDirPosixOpen) {
  // kOpenFlagPosixWritable expand the rights of the directory connection to include write rights if
  // the parent connection has them.
  fio::wire::OpenFlags flags =
      fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kRightWritable;
  auto parent = CreateDirectory(flags, GetPath("a"));

  flags = fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kPosixWritable |
          fio::wire::OpenFlags::kDirectory;
  std::string path = ".";
  auto clone_endpoints = fidl::CreateEndpoints<fio::Node>();
  ASSERT_EQ(clone_endpoints.status_value(), ZX_OK);
  auto clone_res = fidl::WireCall(parent)->Open(flags, {}, fidl::StringView::FromExternal(path),
                                                std::move(clone_endpoints->server));
  ASSERT_EQ(clone_res.status(), ZX_OK);
  fidl::ClientEnd<fio::Directory> clone_dir(clone_endpoints->client.TakeChannel());

  EXPECT_EQ(OpenFileWithCreate(clone_dir, "b"), ZX_OK);
}

TEST_P(OpenTest, OpenFileWithCreateFailsInReadOnlyDirPosixOpen) {
  fio::wire::OpenFlags flags = fio::wire::OpenFlags::kRightReadable;
  auto parent = CreateDirectory(flags, GetPath("a"));

  flags = fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kPosixWritable |
          fio::wire::OpenFlags::kDirectory;
  std::string path = ".";
  auto clone_endpoints = fidl::CreateEndpoints<fio::Node>();
  ASSERT_EQ(clone_endpoints.status_value(), ZX_OK);
  auto clone_res = fidl::WireCall(parent)->Open(flags, {}, fidl::StringView::FromExternal(path),
                                                std::move(clone_endpoints->server));
  ASSERT_EQ(clone_res.status(), ZX_OK);
  fidl::ClientEnd<fio::Directory> clone_dir(clone_endpoints->client.TakeChannel());

  EXPECT_EQ(OpenFileWithCreate(clone_dir, "b"), ZX_ERR_ACCESS_DENIED);
}

TEST_P(OpenTest, OpenFileWithCreateFailsInReadWriteDirPosixClone) {
  // kOpenFlagPosixWritable only does the rights expansion with the open call though.
  fio::wire::OpenFlags flags =
      fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kRightWritable;
  auto parent = CreateDirectory(flags, GetPath("a"));

  flags = fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kPosixWritable |
          fio::wire::OpenFlags::kDirectory;
  auto clone_endpoints = fidl::CreateEndpoints<fio::Node>();
  ASSERT_EQ(clone_endpoints.status_value(), ZX_OK);
  auto clone_res = fidl::WireCall(parent)->Clone(flags, std::move(clone_endpoints->server));
  ASSERT_EQ(clone_res.status(), ZX_OK);
  fidl::ClientEnd<fio::Directory> clone_dir(clone_endpoints->client.TakeChannel());

  EXPECT_EQ(OpenFileWithCreate(clone_dir, "b"), ZX_ERR_ACCESS_DENIED);
}

TEST_P(OpenTest, OpenFileWithCreateFailsInReadOnlyDirPosixClone) {
  fio::wire::OpenFlags flags = fio::wire::OpenFlags::kRightReadable;
  auto parent = CreateDirectory(flags, GetPath("a"));

  flags = fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kPosixWritable |
          fio::wire::OpenFlags::kDirectory;
  auto clone_endpoints = fidl::CreateEndpoints<fio::Node>();
  ASSERT_EQ(clone_endpoints.status_value(), ZX_OK);
  auto clone_res = fidl::WireCall(parent)->Clone(flags, std::move(clone_endpoints->server));
  ASSERT_EQ(clone_res.status(), ZX_OK);
  fidl::ClientEnd<fio::Directory> clone_dir(clone_endpoints->client.TakeChannel());

  EXPECT_EQ(OpenFileWithCreate(clone_dir, "b"), ZX_ERR_ACCESS_DENIED);
}

INSTANTIATE_TEST_SUITE_P(/*no prefix*/, OpenTest, testing::ValuesIn(AllTestFilesystems()),
                         testing::PrintToStringParamName());

}  // namespace
}  // namespace fs_test
