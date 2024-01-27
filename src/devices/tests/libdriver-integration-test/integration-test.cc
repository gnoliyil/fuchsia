// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "integration-test.h"

#include <fcntl.h>
#include <lib/async/cpp/executor.h>
#include <lib/async/cpp/wait.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/unsafe.h>
#include <lib/fpromise/bridge.h>
#include <zircon/boot/image.h>
#include <zircon/status.h>

#include <sstream>

namespace libdriver_integration_test {

async::Loop IntegrationTest::loop_{&kAsyncLoopConfigNoAttachToCurrentThread};
IntegrationTest::IsolatedDevmgr IntegrationTest::devmgr_;

void IntegrationTest::SetUpTestSuite() { DoSetup(); }

void IntegrationTest::DoSetup() {
  // Set up the isolated devmgr instance for this test suite.  Note that we
  // only do this once for the whole suite, because it is currently an
  // expensive process.  Ideally we'd do this between every test.
  auto args = IsolatedDevmgr::DefaultArgs();

  zx::result devmgr = IsolatedDevmgr::Create(std::move(args), loop_.dispatcher());
  ASSERT_TRUE(devmgr.is_ok()) << devmgr.status_string();
  IntegrationTest::devmgr_ = std::move(devmgr.value());
}

void IntegrationTest::TearDownTestSuite() { IntegrationTest::devmgr_.reset(); }

IntegrationTest::IntegrationTest() = default;

void IntegrationTest::SetUp() {
  // We do this in SetUp() rather than the ctor, since gtest cannot assert in
  // ctors.
  zx::channel channel;
  ASSERT_EQ(
      fdio_fd_clone(IntegrationTest::devmgr_.devfs_root().get(), channel.reset_and_get_address()),
      ZX_OK);
  ASSERT_EQ(devfs_.Bind(std::move(channel), IntegrationTest::loop_.dispatcher()), ZX_OK);
}

IntegrationTest::~IntegrationTest() {
  IntegrationTest::loop_.Quit();
  IntegrationTest::loop_.ResetQuit();
}

void IntegrationTest::RunPromise(Promise<void> promise) {
  async::Executor executor(IntegrationTest::loop_.dispatcher());

  auto new_promise = promise.then([&](Promise<void>::result_type& result) {
    if (result.is_error()) {
      ADD_FAILURE() << result.error();
    }
    IntegrationTest::loop_.Quit();
    return result;
  });

  executor.schedule_task(std::move(new_promise));

  zx_status_t status = IntegrationTest::loop_.Run();
  ASSERT_EQ(status, ZX_ERR_CANCELED);
}

IntegrationTest::Promise<void> IntegrationTest::CreateFirstChild(
    std::unique_ptr<RootMockDevice>* root_mock_device, std::unique_ptr<MockDevice>* child_device) {
  return ExpectBind(root_mock_device, [root_mock_device, child_device](HookInvocation record,
                                                                       Completer<void> completer) {
    ActionList actions;
    actions.AppendAddMockDevice(IntegrationTest::loop_.dispatcher(), (*root_mock_device)->path(),
                                "first_child", std::vector<zx_device_prop_t>{}, ZX_OK,
                                std::move(completer), child_device);
    actions.AppendReturnStatus(ZX_OK);
    return actions;
  });
}

IntegrationTest::Promise<void> IntegrationTest::ExpectUnbindThenRelease(
    const std::unique_ptr<MockDevice>& device) {
  fpromise::bridge<void, Error> bridge;
  auto unbind = ExpectUnbind(device, [unbind_reply_completer = std::move(bridge.completer)](
                                         HookInvocation record, Completer<void> completer) mutable {
    completer.complete_ok();
    ActionList actions;
    actions.AppendUnbindReply(std::move(unbind_reply_completer));
    return actions;
  });
  auto reply_done =
      bridge.consumer.promise_or(::fpromise::error("unbind_reply_completer abandoned"));
  return unbind.and_then(JoinPromises(std::move(reply_done), ExpectRelease(device)));
}

IntegrationTest::Promise<void> IntegrationTest::ExpectBind(
    std::unique_ptr<RootMockDevice>* root_mock_device, BindOnce::Callback actions_callback) {
  fpromise::bridge<void, Error> bridge;
  auto bind_hook =
      std::make_unique<BindOnce>(std::move(bridge.completer), std::move(actions_callback));
  zx_status_t status = RootMockDevice::Create(devmgr_, IntegrationTest::loop_.dispatcher(),
                                              std::move(bind_hook), root_mock_device);
  PROMISE_ASSERT(ASSERT_EQ(status, ZX_OK));
  return bridge.consumer.promise_or(::fpromise::error("bind abandoned"));
}

IntegrationTest::Promise<void> IntegrationTest::ExpectUnbind(
    const std::unique_ptr<MockDevice>& device, UnbindOnce::Callback actions_callback) {
  fpromise::bridge<void, Error> bridge;
  auto unbind_hook =
      std::make_unique<UnbindOnce>(std::move(bridge.completer), std::move(actions_callback));
  // Wrap the body in a promise, since we want to defer the evaluation of
  // device->set_hooks.
  return fpromise::make_promise([consumer = std::move(bridge.consumer), &device,
                                 unbind_hook = std::move(unbind_hook)]() mutable {
    device->set_hooks(std::move(unbind_hook));
    return consumer.promise_or(::fpromise::error("unbind abandoned"));
  });
}

IntegrationTest::Promise<void> IntegrationTest::ExpectRelease(
    const std::unique_ptr<MockDevice>& device) {
  // Wrap the body in a promise, since we want to defer the evaluation of
  // device->set_hooks.
  return fpromise::make_promise([&device]() {
    fpromise::bridge<void, Error> bridge;
    ReleaseOnce::Callback func = [](HookInvocation record, Completer<void> completer) {
      completer.complete_ok();
    };
    auto release_hook = std::make_unique<ReleaseOnce>(std::move(bridge.completer), std::move(func));
    device->set_hooks(std::move(release_hook));
    return bridge.consumer.promise_or(::fpromise::error("release abandoned"));
  });
}

IntegrationTest::Promise<void> IntegrationTest::DoOpen(
    const std::string& path, fidl::InterfacePtr<fuchsia::io::Node>* client) {
  fidl::InterfaceRequest<fuchsia::io::Node> server(
      client->NewRequest(IntegrationTest::IntegrationTest::loop_.dispatcher()));
  PROMISE_ASSERT(ASSERT_TRUE(server.is_valid()));

  PROMISE_ASSERT(ASSERT_EQ(client->events().OnOpen, nullptr));
  fpromise::bridge<void, Error> bridge;
  client->events().OnOpen = [client, completer = std::move(bridge.completer)](
                                zx_status_t status,
                                std::unique_ptr<fuchsia::io::NodeInfoDeprecated> info) mutable {
    if (status != ZX_OK) {
      std::string error("failed to open node: ");
      error.append(zx_status_get_string(status));
      completer.complete_error(std::move(error));
      client->events().OnOpen = nullptr;
      return;
    }
    completer.complete_ok();
    client->events().OnOpen = nullptr;
  };
  devfs_->Open(fuchsia::io::OpenFlags::RIGHT_READABLE | fuchsia::io::OpenFlags::DESCRIBE, {}, path,
               std::move(server));
  return bridge.consumer.promise_or(::fpromise::error("devfs open abandoned"));
}

namespace {

class AsyncWatcher {
 public:
  AsyncWatcher(std::string path, fidl::InterfaceHandle<fuchsia::io::DirectoryWatcher> watcher)
      : path_(std::move(path)),
        watcher_(std::move(watcher)),
        wait_(this, watcher_.channel().get(), ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, 0) {}

  void WatcherChanged(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                      const zx_packet_signal_t* signal) {
    if (status != ZX_OK) {
      std::ostringstream os;
      os << "watcher callback error: " << zx_status_get_string(status);
      cb_(fit::error(os.str()));
      return;
    }
    if (signal->observed & ZX_CHANNEL_READABLE) {
      char buf[fuchsia::io::MAX_BUF + 1];
      uint32_t bytes_read;
      zx_status_t status =
          watcher_.channel().read(0, buf, nullptr, sizeof(buf) - 1, 0, &bytes_read, nullptr);
      if (status != ZX_OK) {
        std::ostringstream os;
        os << "watcher read error: " << zx_status_get_string(status);
        cb_(fit::error(os.str()));
        return;
      }

      size_t bytes_processed = 0;
      while (bytes_read - bytes_processed > 2) {
        [[maybe_unused]] uint8_t event = buf[bytes_processed++];
        uint8_t name_length = buf[bytes_processed++];

        if (size_t remaining = bytes_read - bytes_processed; remaining < name_length) {
          std::ostringstream os;
          os << "watcher read error: name length (" << name_length << ") > remaining (" << remaining
             << ")";
          cb_(fit::error(os.str()));
          return;
        }

        const std::string_view filename{&buf[bytes_processed], name_length};
        if (filename == path_) {
          cb_(fit::ok());
          return;
        }
        bytes_processed += name_length;
      }

      wait->Begin(dispatcher);
    } else if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
      std::ostringstream os;
      os << "watcher read error: " << zx_status_get_string(status);
      cb_(fit::error(os.str()));
      return;
    }
  }

  void Begin(async_dispatcher_t* dispatcher,
             fit::callback<void(fit::result<IntegrationTest::Error>)> cb) {
    cb_ = std::move(cb);
    if (const zx_status_t status = wait_.Begin(dispatcher); status != ZX_OK) {
      std::ostringstream os;
      os << "watcher error: " << zx_status_get_string(status);
      cb_(fit::error(os.str()));
    }
  }

  const std::string& path() const { return path_; }

 private:
  const std::string path_;
  const fidl::InterfaceHandle<fuchsia::io::DirectoryWatcher> watcher_;

  fit::callback<void(fit::result<IntegrationTest::Error>)> cb_;
  async::WaitMethod<AsyncWatcher, &AsyncWatcher::WatcherChanged> wait_;
};

void WaitForPath(const fidl::InterfacePtr<fuchsia::io::Directory>& dir,
                 async_dispatcher_t* dispatcher, std::string path,
                 fit::callback<void(fit::result<IntegrationTest::Error>)> cb) {
  fidl::InterfaceHandle<fuchsia::io::DirectoryWatcher> watcher;
  fidl::InterfaceRequest<fuchsia::io::DirectoryWatcher> request = watcher.NewRequest();

  std::optional<std::string> next_path;
  const size_t slash = path.find('/');
  if (slash != std::string::npos) {
    next_path = path.substr(slash + 1);
    path = path.substr(0, slash);
  }

  dir->Watch(
      fuchsia::io::WatchMask::ADDED | fuchsia::io::WatchMask::EXISTING, 0, std::move(request),
      [dispatcher, path = std::move(path), watcher = std::move(watcher), &dir,
       next_path = std::move(next_path), cb = std::move(cb)](zx_status_t status) mutable {
        if (status != ZX_OK) {
          std::ostringstream os;
          os << "watcher error: " << zx_status_get_string(status);
          cb(fit::error(os.str()));
          return;
        }
        std::unique_ptr async_watcher =
            std::make_unique<AsyncWatcher>(std::move(path), std::move(watcher));
        async_watcher->Begin(
            dispatcher, [dispatcher, next_path = std::move(next_path), cb = std::move(cb),
                         watcher = std::move(async_watcher),
                         &dir](fit::result<IntegrationTest::Error> result) mutable {
              if (result.is_error()) {
                cb(result.take_error());
                return;
              }
              if (!next_path.has_value()) {
                cb(fit::ok());
                return;
              }
              fidl::InterfaceHandle<fuchsia::io::Node> node;
              fidl::InterfaceRequest<fuchsia::io::Node> request = node.NewRequest();
              fidl::InterfacePtr<fuchsia::io::Directory> subdir;
              if (const zx_status_t status = subdir.Bind(node.TakeChannel(), dispatcher);
                  status != ZX_OK) {
                std::ostringstream os;
                os << "bind: " << zx_status_get_string(status);
                cb(fit::error(os.str()));
                return;
              }
              std::string dirname = watcher->path();
              auto& events = subdir.events();
              events.OnOpen = [dispatcher, subdir = std::move(subdir),
                               path = std::move(next_path.value()), cb = std::move(cb), dirname](
                                  zx_status_t status,
                                  std::unique_ptr<fuchsia::io::NodeInfoDeprecated> info) mutable {
                if (status != ZX_OK) {
                  std::ostringstream os;
                  os << "Open(" << dirname << ").OnOpen: " << zx_status_get_string(status);
                  cb(fit::error(os.str()));
                  return;
                }
                std::unique_ptr pinned_subdir =
                    std::make_unique<fidl::InterfacePtr<fuchsia::io::Directory>>(std::move(subdir));
                WaitForPath(*pinned_subdir, dispatcher, std::move(path),
                            [pinned_subdir = std::move(pinned_subdir), cb = std::move(cb)](
                                fit::result<IntegrationTest::Error> result) mutable {
                              cb(std::move(result));
                            });
              };

              dir->Open(fuchsia::io::OpenFlags::DIRECTORY | fuchsia::io::OpenFlags::DESCRIBE |
                            fuchsia::io::OpenFlags::RIGHT_READABLE,
                        {}, std::move(dirname), std::move(request));
            });
      });
}

}  // namespace

IntegrationTest::Promise<void> IntegrationTest::DoWaitForPath(const std::string& path) {
  fpromise::bridge<void, Error> bridge;

  WaitForPath(devfs_, loop_.dispatcher(), path,
              [completer = std::move(bridge.completer)](
                  fit::result<IntegrationTest::Error> result) mutable {
                if (result.is_error()) {
                  completer.complete_error(result.error_value());
                } else {
                  completer.complete_ok();
                }
              });

  return bridge.consumer.promise_or(::fpromise::error("WaitForPath abandoned"));
}

}  // namespace libdriver_integration_test
