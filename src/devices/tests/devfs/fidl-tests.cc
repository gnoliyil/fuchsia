// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire_test_base.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/namespace.h>
#include <lib/zx/channel.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <utility>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

#include "src/lib/testing/predicates/status.h"

namespace {

namespace fio = fuchsia_io;

void FidlOpenValidator(const fidl::ClientEnd<fio::Directory>& directory, const char* path,
                       zx::result<fio::wire::NodeInfoDeprecated::Tag> expected) {
  zx::result endpoints = fidl::CreateEndpoints<fio::Node>();
  ASSERT_OK(endpoints.status_value());
  const fidl::Status result = fidl::WireCall(directory)->Open(
      fio::wire::OpenFlags::kRightReadable | fio::wire::OpenFlags::kDescribe, 0,
      fidl::StringView::FromExternal(path), std::move(endpoints->server));
  ASSERT_OK(result.status());

  class EventHandler : public fidl::testing::WireSyncEventHandlerTestBase<fio::Node> {
   public:
    std::optional<zx_status_t> status() const { return status_; }
    std::optional<fio::wire::NodeInfoDeprecated::Tag> tag() const { return tag_; }

    void OnOpen(fidl::WireEvent<fio::Node::OnOpen>* event) override {
      status_ = event->s;
      if (event->info.has_value()) {
        tag_ = event->info->Which();
      }
    }

    void NotImplemented_(const std::string& name) override {
      FAIL() << "Unexpected " << name.c_str();
    }

   private:
    std::optional<zx_status_t> status_;
    std::optional<fio::wire::NodeInfoDeprecated::Tag> tag_;
  };

  EventHandler event_handler;
  ASSERT_OK(event_handler.HandleOneEvent(endpoints->client).status());
  ASSERT_TRUE(event_handler.status().has_value());
  if (expected.is_ok()) {
    ASSERT_OK(event_handler.status().value());
    ASSERT_TRUE(event_handler.tag().has_value());
    ASSERT_EQ(event_handler.tag().value(), expected.value());
  }
  if (expected.is_error()) {
    ASSERT_STATUS(event_handler.status().value(), expected.status_value());
    ASSERT_EQ(event_handler.tag(), std::nullopt);
  }
}

// Ensure that our hand-rolled FIDL messages within devfs and memfs are acting correctly
// for open event messages (on both success and error).
TEST(FidlTestCase, OpenDev) {
  zx::result endpoints = fidl::CreateEndpoints<fio::Directory>();
  ASSERT_OK(endpoints.status_value());
  ASSERT_OK(fdio_open("/dev", static_cast<uint32_t>(fio::OpenFlags::kRightReadable),
                      endpoints->server.channel().release()));

  FidlOpenValidator(endpoints->client, "zero", zx::ok(fio::wire::NodeInfoDeprecated::Tag::kFile));
  FidlOpenValidator(endpoints->client, "this-path-better-not-actually-exist",
                    zx::error(ZX_ERR_NOT_FOUND));
  FidlOpenValidator(endpoints->client, "zero/this-path-better-not-actually-exist",
                    zx::error(ZX_ERR_NOT_SUPPORTED));
}

TEST(FidlTestCase, OpenPkg) {
  zx::result endpoints = fidl::CreateEndpoints<fio::Directory>();
  ASSERT_OK(endpoints.status_value());
  ASSERT_OK(fdio_open("/pkg", static_cast<uint32_t>(fio::OpenFlags::kRightReadable),
                      endpoints->server.channel().release()));

  FidlOpenValidator(endpoints->client, "bin",
                    zx::ok(fio::wire::NodeInfoDeprecated::Tag::kDirectory));
  FidlOpenValidator(endpoints->client, "this-path-better-not-actually-exist",
                    zx::error(ZX_ERR_NOT_FOUND));
}

TEST(FidlTestCase, BasicDevClass) {
  zx::result endpoints = fidl::CreateEndpoints<fio::Node>();
  ASSERT_OK(endpoints.status_value());
  ASSERT_OK(fdio_service_connect("/dev/class", endpoints->server.channel().release()));
  const fidl::WireResult result = fidl::WireCall(endpoints->client)->Query();
  ASSERT_OK(result.status());
  const auto& response = result.value();
  const cpp20::span data = response.protocol.get();
  const std::string_view protocol{reinterpret_cast<const char*>(data.data()), data.size_bytes()};
  ASSERT_EQ(protocol, fio::wire::kDirectoryProtocolName);
}

TEST(FidlTestCase, BasicDevZero) {
  zx::result endpoints = fidl::CreateEndpoints<fio::Node>();
  ASSERT_OK(endpoints.status_value());
  ASSERT_OK(fdio_service_connect("/dev/zero", endpoints->server.channel().release()));
  const fidl::WireResult result = fidl::WireCall(endpoints->client)->Query();
  ASSERT_OK(result.status());
  const auto& response = result.value();
  const cpp20::span data = response.protocol.get();
  const std::string_view protocol{reinterpret_cast<const char*>(data.data()), data.size_bytes()};
  ASSERT_EQ(protocol, fio::wire::kFileProtocolName);
}

using watch_buffer_t = struct {
  // Buffer containing cached messages
  uint8_t buf[fio::wire::kMaxBuf];
  uint8_t name_buf[fio::wire::kMaxFilename + 1];
  // Offset into 'buf' of next message
  uint8_t* ptr;
  // Maximum size of buffer
  size_t size;
};

void CheckLocalEvent(watch_buffer_t* wb, const char** name, fio::WatchEvent* event) {
  ASSERT_NE(wb->ptr, nullptr);

  // Used a cached event
  *event = static_cast<fio::WatchEvent>(wb->ptr[0]);
  ASSERT_LT(static_cast<size_t>(wb->ptr[1]), sizeof(wb->name_buf));
  memcpy(wb->name_buf, wb->ptr + 2, wb->ptr[1]);
  wb->name_buf[wb->ptr[1]] = 0;
  *name = reinterpret_cast<const char*>(wb->name_buf);
  wb->ptr += wb->ptr[1] + 2;
  ASSERT_LE((uintptr_t)wb->ptr, (uintptr_t)wb->buf + wb->size);
  if (wb->ptr == wb->buf + wb->size) {
    wb->ptr = nullptr;
  }
}

// Read the next event off the channel.  Storage for |*name| will be reused
// between calls.
void ReadEvent(watch_buffer_t* wb, const fidl::ClientEnd<fio::DirectoryWatcher>& client_end,
               const char** name, fio::WatchEvent* event) {
  if (wb->ptr == nullptr) {
    zx_signals_t observed;
    ASSERT_OK(client_end.channel().wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), &observed));
    ASSERT_EQ(observed & ZX_CHANNEL_READABLE, ZX_CHANNEL_READABLE);
    uint32_t actual;
    ASSERT_OK(client_end.channel().read(0, wb->buf, nullptr, sizeof(wb->buf), 0, &actual, nullptr));
    wb->size = actual;
    wb->ptr = wb->buf;
  }
  CheckLocalEvent(wb, name, event);
}

TEST(FidlTestCase, DirectoryWatcherExisting) {
  zx::result endpoints = fidl::CreateEndpoints<fio::Directory>();
  ASSERT_OK(endpoints.status_value());

  zx::result watcher_endpoints = fidl::CreateEndpoints<fio::DirectoryWatcher>();
  ASSERT_OK(watcher_endpoints.status_value());

  ASSERT_OK(fdio_service_connect("/dev/class", endpoints->server.channel().release()));

  const fidl::WireResult result =
      fidl::WireCall(endpoints->client)
          ->Watch(fio::wire::WatchMask::kMask, 0, std::move(watcher_endpoints->server));
  ASSERT_OK(result.status());
  const auto& response = result.value();
  ASSERT_OK(response.s);

  watch_buffer_t wb = {};
  // We should see nothing but EXISTING events until we see an IDLE event
  while (true) {
    const char* name = nullptr;
    fio::wire::WatchEvent event;
    ReadEvent(&wb, watcher_endpoints->client, &name, &event);
    if (event == fio::wire::WatchEvent::kIdle) {
      ASSERT_STREQ(name, "");
      break;
    }
    ASSERT_EQ(event, fio::wire::WatchEvent::kExisting);
    ASSERT_STRNE(name, "");
  }
}

TEST(FidlTestCase, DirectoryWatcherWithClosedHalf) {
  zx::result endpoints = fidl::CreateEndpoints<fio::Directory>();
  ASSERT_OK(endpoints.status_value());

  ASSERT_OK(fdio_service_connect("/dev/class", endpoints->server.channel().release()));

  {
    zx::result watcher_endpoints = fidl::CreateEndpoints<fio::DirectoryWatcher>();
    ASSERT_OK(watcher_endpoints.status_value());

    // Close our end of the watcher before devmgr gets its end.
    watcher_endpoints->client.reset();

    const fidl::WireResult result =
        fidl::WireCall(endpoints->client)
            ->Watch(fio::wire::WatchMask::kMask, 0, std::move(watcher_endpoints->server));
    ASSERT_OK(result.status());
    const auto& response = result.value();
    ASSERT_OK(response.s);
    // If we're here and usermode didn't crash, we didn't hit the bug.
  }

  {
    // Create a new watcher, and see if it's functional at all
    zx::result watcher_endpoints = fidl::CreateEndpoints<fio::DirectoryWatcher>();
    ASSERT_OK(watcher_endpoints.status_value());

    const fidl::WireResult result =
        fidl::WireCall(endpoints->client)
            ->Watch(fio::wire::WatchMask::kMask, 0, std::move(watcher_endpoints->server));
    ASSERT_OK(result.status());
    const auto& response = result.value();
    ASSERT_OK(response.s);

    watch_buffer_t wb = {};
    const char* name = nullptr;
    fio::wire::WatchEvent event;
    ReadEvent(&wb, watcher_endpoints->client, &name, &event);
    ASSERT_EQ(event, fio::wire::WatchEvent::kExisting);
  }
}

}  // namespace
