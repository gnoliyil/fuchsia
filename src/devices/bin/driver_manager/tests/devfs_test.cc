// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_manager/devfs/devfs.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/ddk/driver.h>

#include <functional>

#include <zxtest/zxtest.h>

namespace {

namespace fio = fuchsia_io;

class Connecter : public fidl::WireServer<fuchsia_device_fs::Connector> {
 public:
 private:
  void Connect(ConnectRequestView request, ConnectCompleter::Sync& completer) override {
    ASSERT_EQ(channel_.get(), ZX_HANDLE_INVALID);
    channel_ = std::move(request->server);
  }

  zx::channel channel_;
};

std::optional<std::reference_wrapper<const Devnode>> lookup(const Devnode& parent,
                                                            std::string_view name) {
  {
    fbl::RefPtr<fs::Vnode> out;
    switch (const zx_status_t status = parent.children().Lookup(name, &out); status) {
      case ZX_OK:
        return std::reference_wrapper(fbl::RefPtr<Devnode::VnodeImpl>::Downcast(out)->holder_);
      case ZX_ERR_NOT_FOUND:
        break;
      default:
        ADD_FAILURE("%s", zx_status_get_string(status));
        return {};
    }
  }
  const auto it = parent.children().unpublished.find(name);
  if (it != parent.children().unpublished.end()) {
    return it->second.get();
  }
  return {};
}

TEST(Devfs, Export) {
  std::optional<Devnode> root_slot;
  const Devfs devfs(root_slot);
  ASSERT_TRUE(root_slot.has_value());
  Devnode& root_node = root_slot.value();
  std::vector<std::unique_ptr<Devnode>> out;

  ASSERT_OK(root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one/two", {}, out));

  std::optional node_one = lookup(root_node, "one");
  ASSERT_TRUE(node_one.has_value());
  EXPECT_EQ("one", node_one->get().name());
  std::optional node_two = lookup(node_one->get(), "two");
  ASSERT_TRUE(node_two.has_value());
  EXPECT_EQ("two", node_two->get().name());
}

TEST(Devfs, Export_ExcessSeparators) {
  std::optional<Devnode> root_slot;
  const Devfs devfs(root_slot);
  ASSERT_TRUE(root_slot.has_value());
  Devnode& root_node = root_slot.value();
  std::vector<std::unique_ptr<Devnode>> out;

  ASSERT_STATUS(root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one//two", {}, out),
                ZX_ERR_INVALID_ARGS);

  ASSERT_FALSE(lookup(root_node, "one").has_value());
  ASSERT_FALSE(lookup(root_node, "two").has_value());
}

TEST(Devfs, Export_OneByOne) {
  std::optional<Devnode> root_slot;
  const Devfs devfs(root_slot);
  ASSERT_TRUE(root_slot.has_value());
  Devnode& root_node = root_slot.value();
  std::vector<std::unique_ptr<Devnode>> out;

  ASSERT_OK(root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one", {}, out));
  std::optional node_one = lookup(root_node, "one");
  ASSERT_TRUE(node_one.has_value());
  EXPECT_EQ("one", node_one->get().name());

  ASSERT_OK(root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one/two", {}, out));
  std::optional node_two = lookup(node_one->get(), "two");
  ASSERT_TRUE(node_two.has_value());
  EXPECT_EQ("two", node_two->get().name());
}

TEST(Devfs, Export_InvalidPath) {
  std::optional<Devnode> root_slot;
  const Devfs devfs(root_slot);
  ASSERT_TRUE(root_slot.has_value());
  Devnode& root_node = root_slot.value();
  std::vector<std::unique_ptr<Devnode>> out;

  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                root_node.export_dir(Devnode::Target(Devnode::Connector{}), "", {}, out));
  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                root_node.export_dir(Devnode::Target(Devnode::Connector{}), "/one/two", {}, out));
  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one/two/", {}, out));
  ASSERT_STATUS(ZX_ERR_INVALID_ARGS,
                root_node.export_dir(Devnode::Target(Devnode::Connector{}), "/one/two/", {}, out));
}

TEST(Devfs, Export_WithProtocol) {
  std::optional<Devnode> root_slot;
  Devfs devfs(root_slot);
  ASSERT_TRUE(root_slot.has_value());
  Devnode& root_node = root_slot.value();

  std::optional proto_node = devfs.proto_node(ZX_PROTOCOL_BLOCK);
  ASSERT_TRUE(proto_node.has_value());
  EXPECT_EQ("block", proto_node.value().get().name());
  {
    fbl::RefPtr<fs::Vnode> node_000;
    EXPECT_STATUS(proto_node.value().get().children().Lookup("000", &node_000), ZX_ERR_NOT_FOUND);
    ASSERT_EQ(node_000, nullptr);
  }

  std::vector<std::unique_ptr<Devnode>> out;
  ASSERT_OK(root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one/two", "block", out));

  std::optional node_one = lookup(root_node, "one");
  ASSERT_TRUE(node_one.has_value());
  EXPECT_EQ("one", node_one->get().name());

  std::optional node_two = lookup(node_one->get(), "two");
  ASSERT_TRUE(node_two.has_value());
  EXPECT_EQ("two", node_two->get().name());

  fbl::RefPtr<fs::Vnode> node_000;
  ASSERT_OK(proto_node.value().get().children().Lookup("000", &node_000));
  ASSERT_NE(node_000, nullptr);
}

TEST(Devfs, Export_AlreadyExists) {
  std::optional<Devnode> root_slot;
  const Devfs devfs(root_slot);
  ASSERT_TRUE(root_slot.has_value());
  Devnode& root_node = root_slot.value();
  std::vector<std::unique_ptr<Devnode>> out;

  ASSERT_OK(root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one/two", {}, out));
  ASSERT_STATUS(ZX_ERR_ALREADY_EXISTS,
                root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one/two", {}, out));
}

TEST(Devfs, Export_DropDevfs) {
  std::optional<Devnode> root_slot;
  const Devfs devfs(root_slot);
  ASSERT_TRUE(root_slot.has_value());
  Devnode& root_node = root_slot.value();
  std::vector<std::unique_ptr<Devnode>> out;

  ASSERT_OK(root_node.export_dir(Devnode::Target(Devnode::Connector{}), "one/two", {}, out));

  {
    std::optional node_one = lookup(root_node, "one");
    ASSERT_TRUE(node_one.has_value());
    EXPECT_EQ("one", node_one->get().name());

    std::optional node_two = lookup(node_one->get(), "two");
    ASSERT_TRUE(node_two.has_value());
    EXPECT_EQ("two", node_two->get().name());
  }

  out.clear();

  ASSERT_FALSE(lookup(root_node, "one").has_value());
}

}  // namespace
