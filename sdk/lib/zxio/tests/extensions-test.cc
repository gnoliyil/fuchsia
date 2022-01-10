// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/llcpp/server.h>
#include <lib/zxio/extensions.h>
#include <lib/zxio/zxio.h>

#include <optional>

#include <zxtest/zxtest.h>

// This declaration is copied over from zxio/private.h, and is C++ linkage.
// If it ever gets out of sync, that would be surfaced by a linker error.
void zxio_node_init(zxio_node_t* node, zx_handle_t control, const zxio_extension_ops_t* ops);

class TestServerBase : public fidl::WireServer<fuchsia_io::Node> {
 public:
  virtual ~TestServerBase() = default;

  // Exercised by |zxio_close|.
  void Close(CloseRequestView request, CloseCompleter::Sync& completer) override {
    num_close_.fetch_add(1);
    completer.Reply(ZX_OK);
    // After the reply, we should close the connection.
    completer.Close(ZX_OK);
  }

  // Exercised by |zxio_close|.
  void Close2(Close2RequestView request, Close2Completer::Sync& completer) override {
    num_close_.fetch_add(1);
    completer.ReplySuccess();
    // After the reply, we should close the connection.
    completer.Close(ZX_OK);
  }

  void Clone(CloneRequestView request, CloneCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Describe(DescribeRequestView request, DescribeCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Sync(SyncRequestView request, SyncCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Sync2(Sync2RequestView request, Sync2Completer::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void GetAttr(GetAttrRequestView request, GetAttrCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void SetAttr(SetAttrRequestView request, SetAttrCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void QueryFilesystem(QueryFilesystemRequestView request,
                       QueryFilesystemCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  uint32_t num_close() const { return num_close_.load(); }

 private:
  std::atomic<uint32_t> num_close_ = 0;
};

class ExtensionNode : public zxtest::Test {
 public:
  virtual ~ExtensionNode() { binding_->Unbind(); }
  void SetUp() final {
    auto server_end = fidl::CreateEndpoints(&client_end_);
    ASSERT_OK(server_end.status_value());
    server_end_ = std::move(server_end).value();
  }

  template <typename ServerImpl>
  ServerImpl* StartServer() {
    server_ = std::make_unique<ServerImpl>();
    loop_ = std::make_unique<async::Loop>(&kAsyncLoopConfigNoAttachToCurrentThread);
    zx_status_t status;
    EXPECT_OK(status = loop_->StartThread("fake-filesystem"));
    if (status != ZX_OK) {
      return nullptr;
    }
    auto binding = fidl::BindServer(loop_->dispatcher(), std::move(server_end_), server_.get());
    binding_ = std::make_unique<fidl::ServerBindingRef<fuchsia_io::Node>>(std::move(binding));
    return static_cast<ServerImpl*>(server_.get());
  }

 protected:
  fidl::ClientEnd<fuchsia_io::Node> client_end_;
  fidl::ServerEnd<fuchsia_io::Node> server_end_;
  std::unique_ptr<TestServerBase> server_;
  std::unique_ptr<fidl::ServerBindingRef<fuchsia_io::Node>> binding_;
  std::unique_ptr<async::Loop> loop_;
};

TEST_F(ExtensionNode, DefaultBehaviors) {
  zxio_node_t node;
  constexpr static zxio_extension_ops_t extension_ops = {};
  zxio_node_init(&node, client_end_.TakeChannel().release(), &extension_ops);

  TestServerBase* server;
  ASSERT_NO_FAILURES(server = StartServer<TestServerBase>());

  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_readv(&node.io, nullptr, 0, 0, nullptr));
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_writev(&node.io, nullptr, 0, 0, nullptr));

  ASSERT_EQ(0, server->num_close());
  ASSERT_OK(zxio_close(&node.io));
  ASSERT_EQ(1, server->num_close());
}

TEST_F(ExtensionNode, CloseError) {
  zxio_node_t node;
  constexpr static zxio_extension_ops_t extension_ops = {};
  zxio_node_init(&node, client_end_.TakeChannel().release(), &extension_ops);

  class TestServer : public TestServerBase {
   public:
    void Close(CloseRequestView request, CloseCompleter::Sync& completer) override {
      completer.Reply(ZX_ERR_IO);
    }
    void Close2(Close2RequestView request, Close2Completer::Sync& completer) override {
      completer.ReplyError(ZX_ERR_IO);
    }
  };
  ASSERT_NO_FAILURES(StartServer<TestServer>());

  ASSERT_STATUS(ZX_ERR_IO, zxio_close(&node.io));
}

TEST_F(ExtensionNode, SkipClose) {
  zxio_node_t node;
  constexpr static zxio_extension_ops_t extension_ops = {
      .close = nullptr,
      .skip_close_call = true,
      .readv = nullptr,
      .writev = nullptr,
  };
  zxio_node_init(&node, client_end_.TakeChannel().release(), &extension_ops);

  TestServerBase* server;
  ASSERT_NO_FAILURES(server = StartServer<TestServerBase>());

  ASSERT_EQ(0, server->num_close());
  ASSERT_OK(zxio_close(&node.io));
  ASSERT_EQ(0, server->num_close());
}

TEST_F(ExtensionNode, OverrideOperations) {
  class MyIo : private zxio_node_t {
   public:
    explicit MyIo(fidl::ClientEnd<fuchsia_io::Node> client) {
      constexpr static zxio_extension_ops_t kExtensionOps = {
          .close = nullptr,
          .skip_close_call = false,
          .readv =
              [](zxio_node_t* io, const zx_iovec_t* vector, size_t vector_count, zxio_flags_t flags,
                 size_t* out_actual) {
                static_cast<MyIo*>(io)->read_called_.store(true);
                return ZX_OK;
              },
          .writev =
              [](zxio_node_t* io, const zx_iovec_t* vector, size_t vector_count, zxio_flags_t flags,
                 size_t* out_actual) {
                static_cast<MyIo*>(io)->write_called_.store(true);
                return ZX_OK;
              }};
      zxio_node_init(this, client.TakeChannel().release(), &kExtensionOps);
    }

    ~MyIo() { ASSERT_OK(zxio_close(**this)); }

    zxio_t* operator*() { return &this->zxio_node_t::io; }

    bool read_called() const { return read_called_.load(); }
    bool write_called() const { return write_called_.load(); }

   private:
    std::atomic<bool> read_called_ = false;
    std::atomic<bool> write_called_ = false;
  };
  MyIo node(std::move(client_end_));

  ASSERT_NO_FAILURES(StartServer<TestServerBase>());

  ASSERT_FALSE(node.read_called());
  ASSERT_OK(zxio_readv(*node, nullptr, 0, 0, nullptr));
  ASSERT_TRUE(node.read_called());

  ASSERT_FALSE(node.write_called());
  ASSERT_OK(zxio_writev(*node, nullptr, 0, 0, nullptr));
  ASSERT_TRUE(node.write_called());
}

TEST_F(ExtensionNode, GetAttr) {
  zxio_node_t node;
  constexpr static zxio_extension_ops_t extension_ops = {};
  zxio_node_init(&node, client_end_.TakeChannel().release(), &extension_ops);

  constexpr static uint64_t kContentSize = 42;

  class TestServer : public TestServerBase {
   public:
    void GetAttr(GetAttrRequestView request, GetAttrCompleter::Sync& completer) override {
      ASSERT_FALSE(called());
      called_.store(true);
      fuchsia_io::wire::NodeAttributes attr = {};
      attr.content_size = kContentSize;
      completer.Reply(ZX_OK, attr);
    }
    bool called() const { return called_.load(); }

   private:
    std::atomic<bool> called_ = false;
  };
  TestServer* server;
  ASSERT_NO_FAILURES(server = StartServer<TestServer>());

  ASSERT_FALSE(server->called());
  zxio_node_attributes_t attr;
  ASSERT_OK(zxio_attr_get(&node.io, &attr));
  ASSERT_TRUE(server->called());
  ASSERT_TRUE(attr.has.content_size);
  ASSERT_EQ(kContentSize, attr.content_size);

  ASSERT_OK(zxio_close(&node.io));
}
