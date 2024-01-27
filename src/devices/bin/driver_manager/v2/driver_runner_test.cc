// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_manager/v2/driver_runner.h"

#include <fuchsia/component/cpp/fidl_test_base.h>
#include <fuchsia/component/decl/cpp/fidl.h>
#include <fuchsia/driver/host/cpp/fidl_test_base.h>
#include <fuchsia/io/cpp/fidl_test_base.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fit/defer.h>
#include <lib/inspect/cpp/reader.h>
#include <lib/inspect/testing/cpp/inspect.h>
#include <lib/sys/cpp/testing/component_context_provider.h>

#include <list>

#include "src/devices/bin/driver_manager/tests/fake_driver_index.h"
#include "src/lib/testing/loop_fixture/test_loop_fixture.h"

namespace fdata = fuchsia_data;
namespace fdf {
using namespace fuchsia::driver::framework;
}
namespace fdh = fuchsia::driver::host;
namespace fio = fuchsia::io;
namespace fprocess = fuchsia_process;
namespace frunner = fuchsia_component_runner;
namespace fcomponent = fuchsia::component;
namespace fdecl = fuchsia::component::decl;
namespace fcd = fuchsia_component_decl;

using namespace testing;
using namespace inspect::testing;
using namespace dfv2;

struct NodeChecker {
  std::vector<std::string> node_name;
  std::vector<std::string> child_names;
  std::map<std::string, std::string> str_properties;
};

void CheckNode(const inspect::Hierarchy& hierarchy, const NodeChecker& checker) {
  auto node = hierarchy.GetByPath(checker.node_name);
  ASSERT_NE(nullptr, node);

  if (node->children().size() != checker.child_names.size()) {
    printf("Mismatched children\n");
    for (size_t i = 0; i < node->children().size(); i++) {
      printf("Child %ld : %s\n", i, node->children()[i].name().c_str());
    }
    ASSERT_EQ(node->children().size(), checker.child_names.size());
  }

  for (auto& child : checker.child_names) {
    auto ptr = node->GetByPath({child});
    if (!ptr) {
      printf("Failed to find child %s\n", child.c_str());
    }
    ASSERT_NE(nullptr, ptr);
  }

  for (auto& property : checker.str_properties) {
    auto prop = node->node().get_property<inspect::StringPropertyValue>(property.first);
    if (!prop) {
      printf("Failed to find property %s\n", property.first.c_str());
    }
    ASSERT_EQ(property.second, prop->value());
  }
}

zx::result<fidl::ClientEnd<fuchsia_ldsvc::Loader>> LoaderFactory() {
  auto endpoints = fidl::CreateEndpoints<fuchsia_ldsvc::Loader>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }
  return zx::ok(std::move(endpoints->client));
}

fdecl::ChildRef CreateChildRef(std::string name, std::string collection) {
  return fdecl::ChildRef{.name = std::move(name), .collection = std::move(collection)};
}

class FakeContext : public fpromise::context {
 public:
  fpromise::executor* executor() const override {
    EXPECT_TRUE(false);
    return nullptr;
  }

  fpromise::suspended_task suspend_task() override {
    EXPECT_TRUE(false);
    return fpromise::suspended_task();
  }
};

fidl::AnyTeardownObserver TeardownWatcher(size_t index, std::vector<size_t>& indices) {
  return fidl::ObserveTeardown([&indices = indices, index] { indices.emplace_back(index); });
}

class TestRealm : public fcomponent::testing::Realm_TestBase {
 public:
  using CreateChildHandler = fit::function<void(fdecl::CollectionRef collection, fdecl::Child decl,
                                                std::vector<fdecl::Offer> offers)>;
  using OpenExposedDirHandler = fit::function<void(
      fdecl::ChildRef child, fidl::InterfaceRequest<fio::Directory> exposed_dir)>;

  void SetCreateChildHandler(CreateChildHandler create_child_handler) {
    create_child_handler_ = std::move(create_child_handler);
  }

  void SetOpenExposedDirHandler(OpenExposedDirHandler open_exposed_dir_handler) {
    open_exposed_dir_handler_ = std::move(open_exposed_dir_handler);
  }

  fidl::VectorView<fprocess::wire::HandleInfo> GetHandles() {
    return fidl::VectorView<fprocess::wire::HandleInfo>::FromExternal(handles_);
  }

  void AssertDestroyedChildren(std::vector<fdecl::ChildRef> expected) {
    auto destroyed_children = destroyed_children_;
    for (const auto& child : expected) {
      auto it =
          std::find_if(destroyed_children.begin(), destroyed_children.end(),
                       [&child](const fdecl::ChildRef& other) {
                         return child.name == other.name && *child.collection == *other.collection;
                       });
      ASSERT_NE(it, destroyed_children.end());
      destroyed_children.erase(it);
    }
    ASSERT_EQ(destroyed_children.size(), 0ul);
  }

 private:
  void CreateChild(fdecl::CollectionRef collection, fdecl::Child decl,
                   fcomponent::CreateChildArgs args, CreateChildCallback callback) override {
    handles_.clear();
    for (auto& info : *args.mutable_numbered_handles()) {
      handles_.push_back(fprocess::wire::HandleInfo{
          .handle = std::move(info.handle),
          .id = info.id,
      });
    }
    create_child_handler_(std::move(collection), std::move(decl),
                          std::move(*args.mutable_dynamic_offers()));
    callback(fcomponent::Realm_CreateChild_Result(fpromise::ok()));
  }

  void DestroyChild(fdecl::ChildRef child, DestroyChildCallback callback) override {
    destroyed_children_.push_back(std::move(child));
    callback(fcomponent::Realm_DestroyChild_Result(fpromise::ok()));
  }

  void OpenExposedDir(fdecl::ChildRef child, fidl::InterfaceRequest<fio::Directory> exposed_dir,
                      OpenExposedDirCallback callback) override {
    open_exposed_dir_handler_(std::move(child), std::move(exposed_dir));
    callback(fcomponent::Realm_OpenExposedDir_Result(fpromise::ok()));
  }

  void NotImplemented_(const std::string& name) override {
    printf("Not implemented: Realm::%s\n", name.data());
  }

  CreateChildHandler create_child_handler_;
  OpenExposedDirHandler open_exposed_dir_handler_;
  std::vector<fprocess::wire::HandleInfo> handles_;
  std::vector<fdecl::ChildRef> destroyed_children_;
};

class TestDirectory : public fio::testing::Directory_TestBase {
 public:
  using OpenHandler =
      fit::function<void(std::string path, fidl::InterfaceRequest<fio::Node> object)>;

  TestDirectory(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {}

  void Bind(fidl::InterfaceRequest<fio::Directory> request) {
    bindings_.AddBinding(this, std::move(request), dispatcher_);
  }

  void SetOpenHandler(OpenHandler open_handler) { open_handler_ = std::move(open_handler); }

 private:
  void Clone(fuchsia::io::OpenFlags flags, fidl::InterfaceRequest<fio::Node> object) override {
    EXPECT_EQ(fuchsia::io::OpenFlags::CLONE_SAME_RIGHTS, flags);
    fidl::InterfaceRequest<fio::Directory> dir(object.TakeChannel());
    Bind(std::move(dir));
  }

  void Open(fuchsia::io::OpenFlags flags, fuchsia::io::ModeType mode, std::string path,
            fidl::InterfaceRequest<fio::Node> object) override {
    open_handler_(std::move(path), std::move(object));
  }

  void NotImplemented_(const std::string& name) override {
    printf("Not implemented: Directory::%s\n", name.data());
  }

  async_dispatcher_t* dispatcher_;
  fidl::BindingSet<fio::Directory> bindings_;
  OpenHandler open_handler_;
};

class TestDriver : public fdh::testing::Driver_TestBase {
 public:
  TestDriver(fdf::NodePtr node) : node_(std::move(node)) {}

  fdf::NodePtr& node() { return node_; }

  using StopHandler = fit::function<void()>;
  void SetStopHandler(StopHandler handler) { stop_handler_ = std::move(handler); }

  void set_close_bindings(fit::function<void()> close) { close_binding_ = std::move(close); }

  void close_binding() { close_binding_(); }

  void Stop() override { stop_handler_(); }

 private:
  fit::function<void()> close_binding_;
  StopHandler stop_handler_;
  fdf::NodePtr node_;

  void NotImplemented_(const std::string& name) override {
    printf("Not implemented: Driver::%s\n", name.data());
  }
};

class TestDriverHost : public fdh::testing::DriverHost_TestBase {
 public:
  using StartHandler = fit::function<void(fdf::DriverStartArgs start_args,
                                          fidl::InterfaceRequest<fdh::Driver> driver)>;

  void SetStartHandler(StartHandler start_handler) { start_handler_ = std::move(start_handler); }

 private:
  void Start(fdf::DriverStartArgs start_args, fidl::InterfaceRequest<fdh::Driver> driver,
             StartCallback callback) override {
    start_handler_(std::move(start_args), std::move(driver));
    callback(fuchsia::driver::host::DriverHost_Start_Result::WithResponse({}));
  }
  void InstallLoader(fidl::InterfaceHandle<fuchsia::ldsvc::Loader> loader) override {}

  void NotImplemented_(const std::string& name) override {
    printf("Not implemented: DriverHost::%s\n", name.data());
  }

  StartHandler start_handler_;
};

class TestTransaction : public fidl::Transaction {
 public:
  TestTransaction(bool close) : close_(close) {}

 private:
  std::unique_ptr<Transaction> TakeOwnership() override {
    return std::make_unique<TestTransaction>(close_);
  }

  zx_status_t Reply(fidl::OutgoingMessage* message, fidl::WriteOptions write_options) override {
    EXPECT_TRUE(false);
    return ZX_OK;
  }

  void Close(zx_status_t epitaph) override {
    EXPECT_TRUE(close_) << "epitaph: " << zx_status_get_string(epitaph);
  }

  bool close_;
};

struct Driver {
  std::string url;
  std::string binary;
  bool colocate = false;
  bool close = false;
};

class DriverRunnerTest : public gtest::TestLoopFixture {
 public:
  void SetUp() override {
    TestLoopFixture::SetUp();
    fidl::InterfaceRequestHandler<fcomponent::Realm> handler = [this](auto request) {
      EXPECT_EQ(ZX_OK, realm_binding_.Bind(std::move(request), dispatcher()));
    };
    ASSERT_EQ(ZX_OK, provider_.context()->outgoing()->AddPublicService(std::move(handler)));
  }

 protected:
  inspect::Inspector& inspector() { return inspector_; }
  TestRealm& realm() { return realm_; }
  TestDirectory& driver_dir() { return driver_dir_; }
  TestDriverHost& driver_host() { return driver_host_; }

  fidl::ClientEnd<fuchsia_component::Realm> ConnectToRealm() {
    fcomponent::RealmPtr realm;
    provider_.ConnectToPublicService(realm.NewRequest(dispatcher()));
    return fidl::ClientEnd<fuchsia_component::Realm>(realm.Unbind().TakeChannel());
  }

  FakeDriverIndex CreateDriverIndex() {
    return FakeDriverIndex(dispatcher(), [](auto args) -> zx::result<FakeDriverIndex::MatchResult> {
      if (args.name().get() == "second") {
        return zx::ok(FakeDriverIndex::MatchResult{
            .url = "fuchsia-boot:///#meta/second-driver.cm",
        });
      } else if (args.name().get() == "dev-group-0") {
        return zx::ok(FakeDriverIndex::MatchResult{
            .spec = fuchsia_driver_index::MatchedCompositeNodeSpecInfo({
                .name = "test-group",
                .node_index = 0,
                .composite = fuchsia_driver_index::MatchedCompositeInfo({
                    .composite_name = "test-composite",
                    .driver_info = fuchsia_driver_index::MatchedDriverInfo({
                        .url = "fuchsia-boot:///#meta/composite-driver.cm",
                        .colocate = true,
                        .package_type = fuchsia_driver_index::DriverPackageType::kBoot,
                    }),
                }),
                .num_nodes = 2,
                .node_names = {{"node-0", "node-1"}},
                .primary_index = 1,
            })});
      } else if (args.name().get() == "dev-group-1") {
        return zx::ok(FakeDriverIndex::MatchResult{
            .spec = fuchsia_driver_index::MatchedCompositeNodeSpecInfo({
                .name = "test-group",
                .node_index = 1,
                .composite = fuchsia_driver_index::MatchedCompositeInfo({
                    .composite_name = "test-composite",
                    .driver_info = fuchsia_driver_index::MatchedDriverInfo({
                        .url = "fuchsia-boot:///#meta/composite-driver.cm",
                        .colocate = true,
                        .package_type = fuchsia_driver_index::DriverPackageType::kBoot,
                    }),
                }),
                .num_nodes = 2,
                .node_names = {{"node-0", "node-1"}},
                .primary_index = 1,
            })});
      } else {
        return zx::error(ZX_ERR_NOT_FOUND);
      }
    });
  }

  void StartDriverHost(std::string coll) {
    constexpr std::string_view kDriverHostName = "driver-host-";
    realm().SetCreateChildHandler(
        [coll, kDriverHostName](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {
          EXPECT_EQ(coll, collection.name);
          EXPECT_EQ(kDriverHostName, decl.name().substr(0, kDriverHostName.size()));
          EXPECT_EQ("fuchsia-boot:///driver_host2#meta/driver_host2.cm", decl.url());
        });
    realm().SetOpenExposedDirHandler(
        [this, coll, kDriverHostName](fdecl::ChildRef child, auto exposed_dir) {
          EXPECT_EQ(coll, child.collection.value_or(""));
          EXPECT_EQ(kDriverHostName, child.name.substr(0, kDriverHostName.size()));
          driver_host_dir_.Bind(std::move(exposed_dir));
        });
    driver_host_dir_.SetOpenHandler([this](std::string path, auto object) {
      EXPECT_EQ(fdh::DriverHost::Name_, path);
      EXPECT_EQ(ZX_OK, driver_host_binding_.Bind(object.TakeChannel(), dispatcher()));
    });
  }

  void StopDriverComponent(fidl::ClientEnd<frunner::ComponentController> component) {
    fidl::WireClient client(std::move(component), dispatcher());
    auto stop_result = client->Stop();
    ASSERT_EQ(ZX_OK, stop_result.status());
    RunLoopUntilIdle();
  }

  fidl::ClientEnd<frunner::ComponentController> StartDriver(DriverRunner& driver_runner,
                                                            Driver driver) {
    fidl::Arena arena;

    fidl::VectorView<fdata::wire::DictionaryEntry> program_entries(arena, 2);
    program_entries[0].key.Set(arena, "binary");
    program_entries[0].value = fdata::wire::DictionaryValue::WithStr(arena, driver.binary);
    program_entries[1].key.Set(arena, "colocate");
    program_entries[1].value =
        fdata::wire::DictionaryValue::WithStr(arena, driver.colocate ? "true" : "false");

    auto program_builder = fdata::wire::Dictionary::Builder(arena);
    program_builder.entries(std::move(program_entries));

    auto outgoing_endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    EXPECT_EQ(ZX_OK, outgoing_endpoints.status_value());

    auto start_info_builder = frunner::wire::ComponentStartInfo::Builder(arena);
    start_info_builder.resolved_url(driver.url)
        .program(program_builder.Build())
        .outgoing_dir(std::move(outgoing_endpoints->server))
        .ns({})
        .numbered_handles(realm().GetHandles());

    auto controller_endpoints = fidl::CreateEndpoints<frunner::ComponentController>();
    EXPECT_EQ(ZX_OK, controller_endpoints.status_value());
    TestTransaction transaction(driver.close);
    {
      fidl::WireServer<frunner::ComponentRunner>::StartCompleter::Sync completer(&transaction);
      fidl::WireRequest<frunner::ComponentRunner::Start> request{
          start_info_builder.Build(), std::move(controller_endpoints->server)};
      static_cast<fidl::WireServer<frunner::ComponentRunner>&>(driver_runner.runner_for_tests())
          .Start(&request, completer);
    }
    RunLoopUntilIdle();
    return std::move(controller_endpoints->client);
  }

  zx::result<fidl::ClientEnd<frunner::ComponentController>> StartRootDriver(
      std::string url, DriverRunner& driver_runner) {
    realm().SetCreateChildHandler(
        [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {
          EXPECT_EQ("boot-drivers", collection.name);
          EXPECT_EQ("dev", decl.name());
          EXPECT_EQ("fuchsia-boot:///#meta/root-driver.cm", decl.url());
        });
    realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
      EXPECT_EQ("boot-drivers", child.collection);
      EXPECT_EQ("dev", child.name);
      driver_dir_.Bind(std::move(exposed_dir));
    });
    auto start = driver_runner.StartRootDriver(url);
    if (start.is_error()) {
      return start.take_error();
    }
    EXPECT_TRUE(RunLoopUntilIdle());

    StartDriverHost("driver-hosts");
    auto controller = StartDriver(driver_runner, {
                                                     .url = "fuchsia-boot:///#meta/root-driver.cm",
                                                     .binary = "driver/root-driver.so",
                                                 });
    return zx::ok(std::move(controller));
  }

  void Unbind() {
    driver_host_binding_.Unbind();
    EXPECT_TRUE(RunLoopUntilIdle());
  }

  TestDriver* BindDriver(fidl::InterfaceRequest<fdh::Driver> request, fdf::NodePtr node) {
    auto driver = std::make_unique<TestDriver>(std::move(node));
    auto driver_ptr = driver.get();
    driver_bindings_.AddBinding(driver_ptr, std::move(request), dispatcher(),
                                [driver = std::move(driver)](auto) {});
    driver_ptr->set_close_bindings(
        [this, driver_ptr]() mutable { driver_bindings_.CloseBinding(driver_ptr, ZX_OK); });
    driver_ptr->SetStopHandler([driver_ptr]() { driver_ptr->close_binding(); });
    return driver_ptr;
  }

  inspect::Hierarchy Inspect(DriverRunner& driver_runner) {
    FakeContext context;
    auto inspector = driver_runner.Inspect()(context).take_value();
    return inspect::ReadFromInspector(inspector)(context).take_value();
  }

  void SetupDevfs(DriverRunner& runner) { runner.root_node()->SetupDevfsForRootNode(devfs_); }

 private:
  TestRealm realm_;
  TestDirectory driver_host_dir_{dispatcher()};
  TestDirectory driver_dir_{dispatcher()};
  TestDriverHost driver_host_;
  fidl::Binding<fcomponent::Realm> realm_binding_{&realm_};
  fidl::Binding<fdh::DriverHost> driver_host_binding_{&driver_host_};
  fidl::BindingSet<fdh::Driver> driver_bindings_;

  std::optional<Devfs> devfs_;
  inspect::Inspector inspector_;
  sys::testing::ComponentContextProvider provider_{dispatcher()};
};

// Start the root driver.
TEST_F(DriverRunnerTest, StartRootDriver) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);

  auto defer = fit::defer([this] { Unbind(); });

  driver_host().SetStartHandler([this](fdf::DriverStartArgs start_args, auto request) {
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("false", entries[1].value->str());

    fdf::NodePtr node;
    ASSERT_EQ(ZX_OK, node.Bind(start_args.mutable_node()->TakeChannel()));
    BindDriver(std::move(request), std::move(node));
  });

  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver. Make sure that the driver is stopped before the Component is exited.
TEST_F(DriverRunnerTest, StartRootDriver_DriverStopBeforeComponentExit) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);

  std::vector<size_t> event_order;

  auto defer = fit::defer([this] { Unbind(); });

  TestDriver* root_node = nullptr;
  driver_host().SetStartHandler(
      [this, &root_node, &event_order](fdf::DriverStartArgs start_args, auto request) {
        auto& entries = start_args.program().entries();
        EXPECT_EQ(2u, entries.size());
        EXPECT_EQ("binary", entries[0].key);
        EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
        EXPECT_EQ("colocate", entries[1].key);
        EXPECT_EQ("false", entries[1].value->str());

        fdf::NodePtr node;
        EXPECT_EQ(ZX_OK, node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        root_node = BindDriver(std::move(request), std::move(node));
        root_node->SetStopHandler([&event_order, &root_node]() {
          event_order.push_back(0);
          root_node->close_binding();
        });
      });

  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());
  fidl::WireSharedClient<frunner::ComponentController> root_client(
      std::move(*root_driver), dispatcher(), TeardownWatcher(1, event_order));

  root_node->node().Unbind();

  EXPECT_TRUE(RunLoopUntilIdle());
  // Make sure the driver was stopped before we told the component framework the driver was stopped.
  EXPECT_THAT(event_order, ElementsAre(0, 1));
}

// Start the root driver, and add a child node owned by the root driver.
TEST_F(DriverRunnerTest, StartRootDriver_AddOwnedChild) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  driver_host().SetStartHandler([this](fdf::DriverStartArgs start_args, auto request) {
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("false", entries[1].value->str());

    fdf::NodePtr root_node;
    EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
    fdf::NodeAddArgs args;
    args.set_name("second");
    fdf::NodeControllerPtr node_controller;
    fdf::NodePtr second_node;
    root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()),
                        second_node.NewRequest(dispatcher()),
                        [](auto result) { EXPECT_FALSE(result.is_err()); });
    BindDriver(std::move(request), std::move(root_node));
  });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, add a child node, then remove it.
TEST_F(DriverRunnerTest, StartRootDriver_RemoveOwnedChild) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;

  TestDriver* root_test_driver = nullptr;
  fdf::NodePtr second_node;
  driver_host().SetStartHandler([this, &root_test_driver, &node_controller, &second_node](
                                    fdf::DriverStartArgs start_args, auto request) {
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("false", entries[1].value->str());

    fdf::NodePtr root_node;
    EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));

    fdf::NodeAddArgs args;
    args.set_name("second");
    root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()),
                        second_node.NewRequest(dispatcher()),
                        [](auto result) { EXPECT_FALSE(result.is_err()); });
    root_test_driver = BindDriver(std::move(request), std::move(root_node));
  });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  node_controller->Remove();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_FALSE(second_node.is_bound());
  ASSERT_NE(nullptr, root_test_driver);
  EXPECT_TRUE(root_test_driver->node().is_bound());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, and add two child nodes with duplicate names.
TEST_F(DriverRunnerTest, StartRootDriver_AddOwnedChild_DuplicateNames) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  TestDriver* root_test_driver = nullptr;
  fdf::NodePtr second_node, invalid_node;
  driver_host().SetStartHandler([this, &root_test_driver, &second_node, &invalid_node](
                                    fdf::DriverStartArgs start_args, auto request) {
    fdf::NodePtr root_node;
    EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));

    fdf::NodeAddArgs args;
    args.set_name("second");
    fdf::NodeControllerPtr node_controller;
    root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()),
                        second_node.NewRequest(dispatcher()),
                        [](auto result) { EXPECT_FALSE(result.is_err()); });
    args.set_name("second");
    root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()),
                        invalid_node.NewRequest(dispatcher()),
                        [](auto result) { EXPECT_TRUE(result.is_err()); });
    root_test_driver = BindDriver(std::move(request), std::move(root_node));
  });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  EXPECT_FALSE(invalid_node.is_bound());
  EXPECT_TRUE(second_node.is_bound());
  ASSERT_NE(nullptr, root_test_driver);
  EXPECT_TRUE(root_test_driver->node().is_bound());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, and add a child node with an offer that is missing a
// source.
TEST_F(DriverRunnerTest, StartRootDriver_AddUnownedChild_OfferMissingSource) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));

        fdf::NodeAddArgs args;
        args.set_name("second");
        args.mutable_offers()->emplace_back().set_protocol(
            std::move(fdecl::OfferProtocol().set_target_name("fuchsia.package.Renamed")));
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_TRUE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  ASSERT_FALSE(node_controller.is_bound());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, and add a child node with one offer that has a source
// and another that has a target.
TEST_F(DriverRunnerTest, StartRootDriver_AddUnownedChild_OfferHasRef) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));

        fdf::NodeAddArgs args;
        args.set_name("second");
        args.mutable_offers()->emplace_back().set_protocol(
            std::move(fdecl::OfferProtocol()
                          .set_source(std::move(fdecl::Ref().set_self(fdecl::SelfRef())))
                          .set_source_name("fuchsia.package.Protocol")));
        args.mutable_offers()->emplace_back().set_protocol(
            std::move(fdecl::OfferProtocol()
                          .set_target(std::move(fdecl::Ref().set_self(fdecl::SelfRef())))
                          .set_source_name("fuchsia.package.Protocol")));
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_TRUE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  ASSERT_FALSE(node_controller.is_bound());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, and add a child node with duplicate symbols. The child
// node is unowned, so if we did not have duplicate symbols, the second driver
// would bind to it.
TEST_F(DriverRunnerTest, StartRootDriver_AddUnownedChild_DuplicateSymbols) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));

        fdf::NodeAddArgs args;
        args.set_name("second");
        args.mutable_offers()->emplace_back().set_protocol(
            std::move(fdecl::OfferProtocol()
                          .set_source(std::move(fdecl::Ref().set_self(fdecl::SelfRef())))
                          .set_source_name("fuchsia.package.Protocol")));
        args.mutable_offers()->emplace_back().set_protocol(
            std::move(fdecl::OfferProtocol()
                          .set_target(std::move(fdecl::Ref().set_self(fdecl::SelfRef())))
                          .set_source_name("fuchsia.package.Protocol")));
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_TRUE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  ASSERT_FALSE(node_controller.is_bound());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, and add a child node that has a symbol without an
// address.
TEST_F(DriverRunnerTest, StartRootDriver_AddUnownedChild_SymbolMissingAddress) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));

        fdf::NodeAddArgs args;
        args.set_name("second");
        args.mutable_symbols()->emplace_back(std::move(fdf::NodeSymbol().set_name("sym")));
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_TRUE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  ASSERT_FALSE(node_controller.is_bound());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, and add a child node that has a symbol without a name.
TEST_F(DriverRunnerTest, StartRootDriver_AddUnownedChild_SymbolMissingName) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));

        fdf::NodeAddArgs args;
        args.set_name("second");
        args.mutable_symbols()->emplace_back(std::move(fdf::NodeSymbol().set_address(0xfeed)));
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_TRUE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  ASSERT_FALSE(node_controller.is_bound());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, and then start a second driver in a new driver host.
TEST_F(DriverRunnerTest, StartSecondDriver_NewDriverHost) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  bool did_bind = false;
  node_controller.events().OnBind = [&did_bind] { did_bind = true; };
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        auto& entries = start_args.program().entries();
        EXPECT_EQ(2u, entries.size());
        EXPECT_EQ("binary", entries[0].key);
        EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
        EXPECT_EQ("colocate", entries[1].key);
        EXPECT_EQ("false", entries[1].value->str());

        realm().SetCreateChildHandler([](fdecl::CollectionRef collection, fdecl::Child decl,
                                         std::vector<fdecl::Offer> offers) {
          EXPECT_EQ("boot-drivers", collection.name);
          EXPECT_EQ("dev.second", decl.name());
          EXPECT_EQ("fuchsia-boot:///#meta/second-driver.cm", decl.url());

          EXPECT_EQ(1u, offers.size());
          ASSERT_TRUE(offers[0].is_protocol());
          auto& protocol = offers[0].protocol();

          ASSERT_TRUE(protocol.has_source());
          ASSERT_TRUE(protocol.source().is_child());
          auto& source_ref = protocol.source().child();
          EXPECT_EQ("dev", source_ref.name);
          EXPECT_EQ("boot-drivers", source_ref.collection.value_or("missing"));

          ASSERT_TRUE(protocol.has_source_name());
          EXPECT_EQ("fuchsia.package.Protocol", protocol.source_name());

          ASSERT_TRUE(protocol.has_target_name());
          EXPECT_EQ("fuchsia.package.Renamed", protocol.target_name());
        });
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          EXPECT_EQ("boot-drivers", child.collection);
          EXPECT_EQ("dev.second", child.name);
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("second");
        args.mutable_offers()->emplace_back().set_protocol(
            std::move(fdecl::OfferProtocol()
                          .set_source_name("fuchsia.package.Protocol")
                          .set_target_name("fuchsia.package.Renamed")));
        args.mutable_symbols()->emplace_back(
            std::move(fdf::NodeSymbol().set_name("sym").set_address(0xfeed)));
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  EXPECT_TRUE(did_bind);

  fidl::InterfaceRequest<fdh::Driver> second_driver_request;
  fidl::InterfaceHandle<fdf::Node> second_node;
  driver_host().SetStartHandler([&](fdf::DriverStartArgs start_args, auto request) {
    EXPECT_FALSE(start_args.has_symbols());
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/second-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("false", entries[1].value->str());
    second_driver_request = std::move(request);
    second_node = std::move(*start_args.mutable_node());
  });
  StartDriverHost("driver-hosts");
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                 });

  second_driver_request = {};
  second_node = {};
  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren(
      {CreateChildRef("dev", "boot-drivers"), CreateChildRef("dev.second", "boot-drivers")});
}

// Start the root driver, and then start a second driver in the same driver
// host.
TEST_F(DriverRunnerTest, StartSecondDriver_SameDriverHost) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  bool did_bind = false;
  node_controller.events().OnBind = [&did_bind] { did_bind = true; };
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        auto& entries = start_args.program().entries();
        EXPECT_EQ(2u, entries.size());
        EXPECT_EQ("binary", entries[0].key);
        EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
        EXPECT_EQ("colocate", entries[1].key);
        EXPECT_EQ("false", entries[1].value->str());

        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {
              EXPECT_EQ("boot-drivers", collection.name);
              EXPECT_EQ("dev.second", decl.name());
              EXPECT_EQ("fuchsia-boot:///#meta/second-driver.cm", decl.url());
            });
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          EXPECT_EQ("boot-drivers", child.collection);
          EXPECT_EQ("dev.second", child.name);
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("second");
        args.mutable_offers()->emplace_back().set_protocol(
            std::move(fdecl::OfferProtocol()
                          .set_source_name("fuchsia.package.Protocol")
                          .set_target_name("fuchsia.package.Renamed")));
        args.mutable_symbols()->emplace_back(
            std::move(fdf::NodeSymbol().set_name("sym").set_address(0xfeed)));
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());
  EXPECT_TRUE(did_bind);

  fidl::InterfaceRequest<fdh::Driver> second_driver_request;
  fidl::InterfaceHandle<fdf::Node> second_node;
  driver_host().SetStartHandler([&](fdf::DriverStartArgs start_args, auto request) {
    auto& symbols = start_args.symbols();
    EXPECT_EQ(1u, symbols.size());
    EXPECT_EQ("sym", symbols[0].name());
    EXPECT_EQ(0xfeedu, symbols[0].address());
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/second-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("true", entries[1].value->str());
    second_driver_request = std::move(request);
    second_node = std::move(*start_args.mutable_node());
  });
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                     .colocate = true,
                                 });

  second_driver_request = {};
  second_node = {};
  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren(
      {CreateChildRef("dev", "boot-drivers"), CreateChildRef("dev.second", "boot-drivers")});
}

// Start the root driver, and then start a second driver that we match based on
// node properties.
TEST_F(DriverRunnerTest, StartSecondDriver_UseProperties) {
  FakeDriverIndex driver_index(
      dispatcher(), [](auto args) -> zx::result<FakeDriverIndex::MatchResult> {
        if (args.has_properties() && args.properties()[0].key.is_int_value() &&
            args.properties()[0].key.int_value() == 0x1985 &&
            args.properties()[0].value.is_int_value() &&
            args.properties()[0].value.int_value() == 0x2301

            && args.properties()[1].key.is_string_value() &&
            args.properties()[1].key.string_value().get() == "fuchsia.driver.framework.dfv2" &&
            args.properties()[1].value.is_bool_value() && args.properties()[1].value.bool_value()

        ) {
          return zx::ok(FakeDriverIndex::MatchResult{
              .url = "fuchsia-boot:///#meta/second-driver.cm",
          });
        } else {
          return zx::error(ZX_ERR_NOT_FOUND);
        }
      });
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        auto& entries = start_args.program().entries();
        EXPECT_EQ(2u, entries.size());
        EXPECT_EQ("binary", entries[0].key);
        EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
        EXPECT_EQ("colocate", entries[1].key);
        EXPECT_EQ("false", entries[1].value->str());

        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {
              EXPECT_EQ("boot-drivers", collection.name);
              EXPECT_EQ("dev.second", decl.name());
              EXPECT_EQ("fuchsia-boot:///#meta/second-driver.cm", decl.url());
            });
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          EXPECT_EQ("boot-drivers", child.collection);
          EXPECT_EQ("dev.second", child.name);
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("second");
        args.mutable_properties()->emplace_back(fdf::NodeProperty{
            .key = fdf::NodePropertyKey::WithIntValue(0x1985),
            .value = fdf::NodePropertyValue::WithIntValue(0x2301),
        });
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  fidl::InterfaceRequest<fdh::Driver> second_driver_request;
  fidl::InterfaceHandle<fdf::Node> second_node;
  driver_host().SetStartHandler([&](fdf::DriverStartArgs start_args, auto request) {
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/second-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("true", entries[1].value->str());
    second_driver_request = std::move(request);
    second_node = std::move(*start_args.mutable_node());
  });
  StartDriver(driver_runner, {
                                 .url = "fuchsia-boot:///#meta/second-driver.cm",
                                 .binary = "driver/second-driver.so",
                                 .colocate = true,
                             });

  second_driver_request = {};
  second_node = {};
  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren(
      {CreateChildRef("dev", "boot-drivers"), CreateChildRef("dev.second", "boot-drivers")});
}

// The root driver adds a node that only binds after a RequestBind() call.
TEST_F(DriverRunnerTest, BindThroughRequest) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        auto& entries = start_args.program().entries();
        EXPECT_EQ(2u, entries.size());
        EXPECT_EQ("binary", entries[0].key);
        EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
        EXPECT_EQ("colocate", entries[1].key);
        EXPECT_EQ("false", entries[1].value->str());

        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {
              EXPECT_EQ("boot-drivers", collection.name);
              EXPECT_EQ("dev.child", decl.name());
              EXPECT_EQ("fuchsia-boot:///#meta/second-driver.cm", decl.url());
            });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));

        fdf::NodeAddArgs args;
        args.set_name("child");
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });

        BindDriver(std::move(request), std::move(root_node));
      });

  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  ASSERT_EQ(1u, driver_runner.NumOrphanedNodes());

  driver_index.set_match_callback([](auto args) -> zx::result<FakeDriverIndex::MatchResult> {
    return zx::ok(FakeDriverIndex::MatchResult{
        .url = "fuchsia-boot:///#meta/second-driver.cm",
    });
  });

  auto bind_request = fdf::NodeControllerRequestBindRequest();
  node_controller->RequestBind(std::move(bind_request), [](auto result) {});

  RunLoopUntilIdle();
  ASSERT_EQ(0u, driver_runner.NumOrphanedNodes());

  fidl::InterfaceRequest<fdh::Driver> second_driver_request;
  fidl::InterfaceHandle<fdf::Node> second_node;
  driver_host().SetStartHandler([&](fdf::DriverStartArgs start_args, auto request) {
    EXPECT_FALSE(start_args.has_symbols());
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/second-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("false", entries[1].value->str());
    second_driver_request = std::move(request);
    second_node = std::move(*start_args.mutable_node());
  });
  StartDriverHost("driver-hosts");
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                 });

  second_driver_request = {};
  second_node = {};
  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren(
      {CreateChildRef("dev", "boot-drivers"), CreateChildRef("dev.child", "boot-drivers")});
}

// Start the root driver, and then add a child node that does not bind to a
// second driver.
TEST_F(DriverRunnerTest, StartSecondDriver_UnknownNode) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  driver_host().SetStartHandler([this](fdf::DriverStartArgs start_args, auto request) {
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("false", entries[1].value->str());

    fdf::NodePtr root_node;
    EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
    fdf::NodeAddArgs args;
    args.set_name("unknown-node");
    fdf::NodeControllerPtr node_controller;
    root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                        [](auto result) { EXPECT_FALSE(result.is_err()); });
    BindDriver(std::move(request), std::move(root_node));
  });

  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  StartDriver(driver_runner, {.close = true});
  ASSERT_EQ(1u, driver_runner.NumOrphanedNodes());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the root driver, and then add a child node that only binds to a base driver.
TEST_F(DriverRunnerTest, StartSecondDriver_BindOrphanToBaseDriver) {
  bool base_drivers_loaded = false;
  FakeDriverIndex driver_index(
      dispatcher(), [&base_drivers_loaded](auto args) -> zx::result<FakeDriverIndex::MatchResult> {
        if (base_drivers_loaded) {
          if (args.name().get() == "second") {
            return zx::ok(FakeDriverIndex::MatchResult{
                .url = "fuchsia-boot:///#meta/second-driver.cm",
            });
          }
        }
        return zx::error(ZX_ERR_NOT_FOUND);
      });
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  driver_host().SetStartHandler([this](fdf::DriverStartArgs start_args, auto request) {
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/root-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("false", entries[1].value->str());

    fdf::NodePtr root_node;
    EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
    fdf::NodeAddArgs args;
    args.set_name("second");
    args.mutable_properties()->emplace_back(
        fdf::NodeProperty{.key = fdf::NodePropertyKey::WithStringValue("driver.prop-one"),
                          .value = fdf::NodePropertyValue::WithStringValue("value")});
    fdf::NodeControllerPtr node_controller;
    root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                        [](auto result) { EXPECT_FALSE(result.is_err()); });
    BindDriver(std::move(request), std::move(root_node));
  });

  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  // Make sure the node we added was orphaned.
  ASSERT_EQ(1u, driver_runner.NumOrphanedNodes());

  // Set the handlers for the new driver.
  realm().SetCreateChildHandler(
      [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {
        EXPECT_EQ("boot-drivers", collection.name);
        EXPECT_EQ("dev.second", decl.name());
        EXPECT_EQ("fuchsia-boot:///#meta/second-driver.cm", decl.url());
      });
  realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
    EXPECT_EQ("boot-drivers", child.collection);
    EXPECT_EQ("dev.second", child.name);
    driver_dir().Bind(std::move(exposed_dir));
  });

  // Tell driver index to return the second driver, and wait for base drivers to load.
  base_drivers_loaded = true;
  driver_runner.ScheduleBaseDriversBinding();
  ASSERT_TRUE(RunLoopUntilIdle());

  // See that we don't have an orphan anymore.
  ASSERT_EQ(0u, driver_runner.NumOrphanedNodes());

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

// Start the second driver, and then unbind its associated node.
TEST_F(DriverRunnerTest, StartSecondDriver_UnbindSecondNode) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("second");
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  TestDriver* second_test_driver = nullptr;
  driver_host().SetStartHandler(
      [this, &second_test_driver](fdf::DriverStartArgs start_args, auto request) {
        fdf::NodePtr second_node;
        EXPECT_EQ(ZX_OK, second_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        second_test_driver = BindDriver(std::move(request), std::move(second_node));
      });

  StartDriverHost("driver-hosts");
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                 });

  // Unbinding the second node stops the driver bound to it.
  second_test_driver->node().Unbind();
  EXPECT_TRUE(RunLoopUntilIdle());
  zx_signals_t signals = 0;
  ASSERT_EQ(ZX_OK, second_driver.channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(),
                                                    &signals));
  ASSERT_TRUE(signals & ZX_CHANNEL_PEER_CLOSED);

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren(
      {CreateChildRef("dev", "boot-drivers"), CreateChildRef("dev.second", "boot-drivers")});
}

// Start the second driver, and then close the associated Driver protocol
// channel.
TEST_F(DriverRunnerTest, StartSecondDriver_CloseSecondDriver) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("second");
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  fdf::NodePtr second_node;
  fidl::InterfaceRequest<fdh::Driver> second_request;
  driver_host().SetStartHandler(
      [this, &second_node, &second_request](fdf::DriverStartArgs start_args, auto request) {
        second_request = std::move(request);
        EXPECT_EQ(ZX_OK, second_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
      });

  StartDriverHost("driver-hosts");
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                 });

  // Closing the Driver protocol channel of the second driver causes the driver
  // to be stopped.
  second_request.TakeChannel();
  EXPECT_TRUE(RunLoopUntilIdle());
  zx_signals_t signals = 0;
  ASSERT_EQ(ZX_OK, second_driver.channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite(),
                                                    &signals));
  ASSERT_TRUE(signals & ZX_CHANNEL_PEER_CLOSED);

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren(
      {CreateChildRef("dev", "boot-drivers"), CreateChildRef("dev.second", "boot-drivers")});
}

// Start a chain of drivers, and then unbind the second driver's node.
TEST_F(DriverRunnerTest, StartDriverChain_UnbindSecondNode) {
  FakeDriverIndex driver_index(dispatcher(),
                               [](auto args) -> zx::result<FakeDriverIndex::MatchResult> {
                                 std::string name(args.name().get());
                                 return zx::ok(FakeDriverIndex::MatchResult{
                                     .url = "fuchsia-boot:///#meta/" + name + "-driver.cm",
                                 });
                               });
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("node-0");
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  constexpr size_t kMaxNodes = 10;
  fdf::NodePtr second_node;
  std::vector<fidl::ClientEnd<frunner::ComponentController>> drivers;
  for (size_t i = 1; i <= kMaxNodes; i++) {
    driver_host().SetStartHandler(
        [this, &second_node, &node_controller, i](fdf::DriverStartArgs start_args, auto request) {
          realm().SetCreateChildHandler(
              [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
          realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
            driver_dir().Bind(std::move(exposed_dir));
          });

          fdf::NodePtr node;
          EXPECT_EQ(ZX_OK, node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
          // Only add a node that a driver will be bound to.
          if (i != kMaxNodes) {
            fdf::NodeAddArgs args;
            args.set_name("node-" + std::to_string(i));
            node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                           [](auto result) { EXPECT_FALSE(result.is_err()); });
          }
          auto driver = BindDriver(std::move(request), std::move(node));
          if (!second_node.is_bound()) {
            second_node = std::move(driver->node());
          }
        });

    StartDriverHost("driver-hosts");
    drivers.emplace_back(StartDriver(driver_runner, {
                                                        .url = "fuchsia-boot:///#meta/node-" +
                                                               std::to_string(i - 1) + "-driver.cm",
                                                        .binary = "driver/driver.so",
                                                    }));
  }

  // Unbinding the second node stops all drivers bound in the sub-tree, in a
  // depth-first order.
  std::vector<size_t> indices;
  std::vector<fidl::WireSharedClient<frunner::ComponentController>> clients;
  for (auto& driver : drivers) {
    clients.emplace_back(std::move(driver), dispatcher(),
                         TeardownWatcher(clients.size() + 1, indices));
  }
  second_node.Unbind();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_THAT(indices, ElementsAre(10, 9, 8, 7, 6, 5, 4, 3, 2, 1));

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren(
      {CreateChildRef("dev", "boot-drivers"), CreateChildRef("dev.node-0", "boot-drivers"),
       CreateChildRef("dev.node-0.node-1", "boot-drivers"),
       CreateChildRef("dev.node-0.node-1.node-2", "boot-drivers"),
       CreateChildRef("dev.node-0.node-1.node-2.node-3", "boot-drivers"),
       CreateChildRef("dev.node-0.node-1.node-2.node-3.node-4", "boot-drivers"),
       CreateChildRef("dev.node-0.node-1.node-2.node-3.node-4.node-5", "boot-drivers"),
       CreateChildRef("dev.node-0.node-1.node-2.node-3.node-4.node-5.node-6", "boot-drivers"),
       CreateChildRef("dev.node-0.node-1.node-2.node-3.node-4.node-5.node-6.node-7",
                      "boot-drivers"),
       CreateChildRef("dev.node-0.node-1.node-2.node-3.node-4.node-5.node-6.node-7.node-8",
                      "boot-drivers"),
       CreateChildRef("dev.node-0.node-1.node-2.node-3.node-4.node-5.node-6.node-7.node-8.node-9",
                      "boot-drivers")});
}

// Start the second driver, and then unbind the root node.
TEST_F(DriverRunnerTest, StartSecondDriver_UnbindRootNode) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  fdf::NodePtr root_node;
  driver_host().SetStartHandler(
      [this, &node_controller, &root_node](fdf::DriverStartArgs start_args, auto request) {
        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr node;
        EXPECT_EQ(ZX_OK, node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("second");
        node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                       [](auto result) { EXPECT_FALSE(result.is_err()); });
        auto driver = BindDriver(std::move(request), std::move(node));
        root_node = std::move(driver->node());
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  driver_host().SetStartHandler([this](fdf::DriverStartArgs start_args, auto request) {
    fdf::NodePtr second_node;
    EXPECT_EQ(ZX_OK, second_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
    BindDriver(std::move(request), std::move(second_node));
  });

  StartDriverHost("driver-hosts");
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                 });

  // Unbinding the root node stops all drivers.
  std::vector<size_t> indices;
  fidl::WireSharedClient<frunner::ComponentController> root_client(
      std::move(*root_driver), dispatcher(), TeardownWatcher(0, indices));
  fidl::WireSharedClient<frunner::ComponentController> second_client(
      std::move(second_driver), dispatcher(), TeardownWatcher(1, indices));
  root_node.Unbind();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_THAT(indices, ElementsAre(1, 0));
}

// Start the second driver, and then Stop the root node.
TEST_F(DriverRunnerTest, StartSecondDriver_StopRootNode) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;

  // These represent the order that Driver::Stop is called
  std::vector<size_t> driver_stop_indices;

  driver_host().SetStartHandler([this, &node_controller, &driver_stop_indices](
                                    fdf::DriverStartArgs start_args, auto request) {
    realm().SetCreateChildHandler(
        [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
    realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
      driver_dir().Bind(std::move(exposed_dir));
    });

    fdf::NodePtr node;
    EXPECT_EQ(ZX_OK, node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
    fdf::NodeAddArgs args;
    args.set_name("second");
    node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                   [](auto result) { EXPECT_FALSE(result.is_err()); });
    auto driver = BindDriver(std::move(request), std::move(node));
    driver->SetStopHandler([&driver_stop_indices, driver]() {
      driver_stop_indices.push_back(0);
      driver->close_binding();
    });
  });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  driver_host().SetStartHandler(
      [this, &driver_stop_indices](fdf::DriverStartArgs start_args, auto request) {
        fdf::NodePtr second_node;
        EXPECT_EQ(ZX_OK, second_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        auto driver = BindDriver(std::move(request), std::move(second_node));
        driver->SetStopHandler([&driver_stop_indices, driver]() {
          driver_stop_indices.push_back(1);
          driver->close_binding();
        });
      });

  StartDriverHost("driver-hosts");
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                 });

  std::vector<size_t> indices;
  fidl::WireSharedClient<frunner::ComponentController> root_client(
      std::move(*root_driver), dispatcher(), TeardownWatcher(0, indices));
  fidl::WireSharedClient<frunner::ComponentController> second_client(
      std::move(second_driver), dispatcher(), TeardownWatcher(1, indices));

  // Simulate the Component Framework calling Stop on the root driver.
  [[maybe_unused]] auto result = root_client->Stop();

  EXPECT_TRUE(RunLoopUntilIdle());
  // Check that the driver components were shut down in order.
  EXPECT_THAT(indices, ElementsAre(1, 0));
  // Check that Driver::Stop was called in order.
  EXPECT_THAT(driver_stop_indices, ElementsAre(1, 0));
}

// Start the second driver, and then stop the root driver.
TEST_F(DriverRunnerTest, StartSecondDriver_StopRootDriver) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("second");
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  driver_host().SetStartHandler([this](fdf::DriverStartArgs start_args, auto request) {
    fdf::NodePtr node;
    EXPECT_EQ(ZX_OK, node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
    BindDriver(std::move(request), std::move(node));
  });

  StartDriverHost("driver-hosts");
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                 });

  // Stopping the root driver stops all drivers.
  std::vector<size_t> indices;
  fidl::WireSharedClient<frunner::ComponentController> root_client(
      std::move(*root_driver), dispatcher(), TeardownWatcher(0, indices));
  fidl::WireSharedClient<frunner::ComponentController> second_client(
      std::move(second_driver), dispatcher(), TeardownWatcher(1, indices));
  [[maybe_unused]] auto result = root_client->Stop();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_THAT(indices, ElementsAre(1, 0));
}

// Start the second driver, stop the root driver, and block while waiting on the
// second driver to shut down.
TEST_F(DriverRunnerTest, StartSecondDriver_BlockOnSecondDriver) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("second");
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  TestDriver* second_test_driver = nullptr;
  driver_host().SetStartHandler(
      [this, &second_test_driver](fdf::DriverStartArgs start_args, auto request) {
        fdf::NodePtr node;
        EXPECT_EQ(ZX_OK, node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        second_test_driver = BindDriver(std::move(request), std::move(node));
      });

  StartDriverHost("driver-hosts");
  auto second_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/second-driver.cm",
                                     .binary = "driver/second-driver.so",
                                 });
  // When the second driver gets asked to stop, don't drop the binding,
  // which means DriverRunner will wait for the binding to drop.
  second_test_driver->SetStopHandler([]() {});

  // Stopping the root driver stops all drivers, but is blocked waiting on the
  // second driver to stop.
  std::vector<size_t> indices;
  fidl::WireSharedClient<frunner::ComponentController> root_client(
      std::move(*root_driver), dispatcher(), TeardownWatcher(0, indices));
  fidl::WireSharedClient<frunner::ComponentController> second_client(
      std::move(second_driver), dispatcher(), TeardownWatcher(1, indices));
  [[maybe_unused]] auto result = root_client->Stop();
  EXPECT_TRUE(RunLoopUntilIdle());
  // Nothing has shut down yet, since we are waiting.
  EXPECT_THAT(indices, ElementsAre());

  // Attempt to add a child node to a removed node.
  bool is_error = false;
  second_test_driver->node()->AddChild({}, node_controller.NewRequest(dispatcher()), {},
                                       [&is_error](auto result) { is_error = result.is_err(); });
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_TRUE(is_error);

  // Unbind the second node, indicating the second driver has stopped, thereby
  // continuing the stop sequence.
  second_test_driver->close_binding();
  EXPECT_TRUE(RunLoopUntilIdle());
  EXPECT_THAT(indices, ElementsAre(1, 0));
}

TEST_F(DriverRunnerTest, CreateAndBindCompositeNodeSpec) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);

  auto defer = fit::defer([this] { Unbind(); });

  // Add a match for the composite node spec that we are creating.
  std::string name("test-group");
  const fuchsia_driver_index::MatchedCompositeNodeSpecInfo match({
      .composite = fuchsia_driver_index::MatchedCompositeInfo({
          .composite_name = "test-composite",
          .driver_info = fuchsia_driver_index::MatchedDriverInfo({
              .url = "fuchsia-boot:///#meta/composite-driver.cm",
              .colocate = true,
              .package_type = fuchsia_driver_index::DriverPackageType::kBoot,
          }),
      }),
      .node_names = {{"node-0", "node-1"}},
      .primary_index = 1,
  });
  driver_index.AddCompositeNodeSpecMatch(name, match);

  const fuchsia_driver_framework::CompositeNodeSpec fidl_spec(
      {.name = name,
       .parents = std::vector<fuchsia_driver_framework::ParentSpec>{
           fuchsia_driver_framework::ParentSpec({
               .bind_rules = std::vector<fuchsia_driver_framework::BindRule>(),
               .properties = std::vector<fuchsia_driver_framework::NodeProperty>(),
           }),
           fuchsia_driver_framework::ParentSpec({
               .bind_rules = std::vector<fuchsia_driver_framework::BindRule>(),
               .properties = std::vector<fuchsia_driver_framework::NodeProperty>(),
           })}});

  auto spec = std::make_unique<CompositeNodeSpecV2>(
      CompositeNodeSpecCreateInfo{
          .name = name,
          .size = 2,
      },
      dispatcher(), &driver_runner);
  fidl::Arena<> arena;
  auto added = driver_runner.composite_node_spec_manager().AddSpec(fidl::ToWire(arena, fidl_spec),
                                                                   std::move(spec));
  ASSERT_TRUE(added.is_ok());

  RunLoopUntilIdle();

  ASSERT_EQ(2u,
            driver_runner.composite_node_spec_manager().specs().at(name)->parent_specs().size());

  ASSERT_FALSE(driver_runner.composite_node_spec_manager().specs().at(name)->parent_specs().at(0));

  ASSERT_FALSE(driver_runner.composite_node_spec_manager().specs().at(name)->parent_specs().at(1));

  fdf::NodeControllerPtr node_controller;
  driver_host().SetStartHandler(
      [this, &node_controller](fdf::DriverStartArgs start_args, auto request) {
        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr root_node;
        EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        fdf::NodeAddArgs args;
        args.set_name("dev-group-0");
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        args.set_name("dev-group-1");
        root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                            [](auto result) { EXPECT_FALSE(result.is_err()); });
        BindDriver(std::move(request), std::move(root_node));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  ASSERT_TRUE(driver_runner.composite_node_spec_manager().specs().at(name)->parent_specs().at(0));

  ASSERT_TRUE(driver_runner.composite_node_spec_manager().specs().at(name)->parent_specs().at(1));

  driver_host().SetStartHandler([this](fdf::DriverStartArgs start_args, auto request) {
    auto& entries = start_args.program().entries();
    EXPECT_EQ(2u, entries.size());
    EXPECT_EQ("binary", entries[0].key);
    EXPECT_EQ("driver/composite-driver.so", entries[0].value->str());
    EXPECT_EQ("colocate", entries[1].key);
    EXPECT_EQ("true", entries[1].value->str());

    fdf::NodePtr node;
    ASSERT_EQ(ZX_OK, node.Bind(start_args.mutable_node()->TakeChannel()));
    BindDriver(std::move(request), std::move(node));
  });
  auto composite_driver =
      StartDriver(driver_runner, {
                                     .url = "fuchsia-boot:///#meta/composite-driver.cm",
                                     .binary = "driver/composite-driver.so",
                                     .colocate = true,
                                 });

  auto hierarchy = Inspect(driver_runner);
  ASSERT_NO_FATAL_FAILURE(CheckNode(hierarchy, {
                                                   .node_name = {"node_topology"},
                                                   .child_names = {"dev"},
                                               }));

  ASSERT_NO_FATAL_FAILURE(
      CheckNode(hierarchy, {.node_name = {"node_topology", "dev"},
                            .child_names = {"dev-group-0", "dev-group-1"},
                            .str_properties = {
                                {"driver", "fuchsia-boot:///#meta/root-driver.cm"},
                            }}));

  ASSERT_NO_FATAL_FAILURE(CheckNode(
      hierarchy,
      {.node_name = {"node_topology", "dev", "dev-group-0"}, .child_names = {"test-group"}}));

  ASSERT_NO_FATAL_FAILURE(CheckNode(
      hierarchy,
      {.node_name = {"node_topology", "dev", "dev-group-1"}, .child_names = {"test-group"}}));

  ASSERT_NO_FATAL_FAILURE(
      CheckNode(hierarchy, {.node_name = {"node_topology", "dev", "dev-group-0", "test-group"},
                            .str_properties = {
                                {"driver", "fuchsia-boot:///#meta/composite-driver.cm"},
                            }}));

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers"),
                                   CreateChildRef("dev.dev-group-1.test-group", "boot-drivers")});
}

// Start a driver and inspect the driver runner.
TEST_F(DriverRunnerTest, StartAndInspect) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  driver_host().SetStartHandler([this](fdf::DriverStartArgs start_args, auto request) {
    realm().SetCreateChildHandler(
        [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
    realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
      driver_dir().Bind(std::move(exposed_dir));
    });

    fdf::NodePtr root_node;
    EXPECT_EQ(ZX_OK, root_node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
    fdf::NodeAddArgs args;
    args.set_name("second");
    args.mutable_offers()->emplace_back().set_protocol(
        std::move(fdecl::OfferProtocol()
                      .set_source_name("fuchsia.package.ProtocolA")
                      .set_target_name("fuchsia.package.RenamedA")));
    args.mutable_offers()->emplace_back().set_protocol(
        std::move(fdecl::OfferProtocol()
                      .set_source_name("fuchsia.package.ProtocolB")
                      .set_target_name("fuchsia.package.RenamedB")));
    args.mutable_symbols()->emplace_back(
        std::move(fdf::NodeSymbol().set_name("symbol-A").set_address(0x2301)));
    args.mutable_symbols()->emplace_back(
        std::move(fdf::NodeSymbol().set_name("symbol-B").set_address(0x1985)));
    fdf::NodeControllerPtr node_controller;
    root_node->AddChild(std::move(args), node_controller.NewRequest(dispatcher()), {},
                        [](auto result) { EXPECT_FALSE(result.is_err()); });
    BindDriver(std::move(request), std::move(root_node));
  });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());

  auto hierarchy = Inspect(driver_runner);
  ASSERT_EQ("root", hierarchy.node().name());
  ASSERT_EQ(3ul, hierarchy.children().size());

  ASSERT_NO_FATAL_FAILURE(CheckNode(hierarchy, {
                                                   .node_name = {"node_topology"},
                                                   .child_names = {"dev"},
                                               }));

  ASSERT_NO_FATAL_FAILURE(
      CheckNode(hierarchy, {.node_name = {"node_topology", "dev"},
                            .child_names = {"second"},
                            .str_properties = {
                                {"driver", "fuchsia-boot:///#meta/root-driver.cm"},
                            }}));

  ASSERT_NO_FATAL_FAILURE(
      CheckNode(hierarchy, {.node_name = {"node_topology", "dev", "second"},
                            .child_names = {},
                            .str_properties = {
                                {"offers", "fuchsia.package.RenamedA, fuchsia.package.RenamedB"},
                                {"symbols", "symbol-A, symbol-B"},
                                {"driver", "unbound"},
                            }}));

  ASSERT_NO_FATAL_FAILURE(CheckNode(hierarchy, {
                                                   .node_name = {"orphan_nodes"},
                                               }));

  ASSERT_NO_FATAL_FAILURE(CheckNode(hierarchy, {
                                                   .node_name = {"dfv1_composites"},
                                               }));

  StopDriverComponent(std::move(root_driver.value()));
  realm().AssertDestroyedChildren({CreateChildRef("dev", "boot-drivers")});
}

TEST_F(DriverRunnerTest, TestTearDownNodeTreeWithManyChildren) {
  auto driver_index = CreateDriverIndex();
  auto driver_index_client = driver_index.Connect();
  ASSERT_EQ(ZX_OK, driver_index_client.status_value());
  DriverRunner driver_runner(ConnectToRealm(), std::move(*driver_index_client), inspector(),
                             &LoaderFactory, dispatcher());
  SetupDevfs(driver_runner);
  auto defer = fit::defer([this] { Unbind(); });

  std::vector<fdf::NodeControllerPtr> node_controller;
  std::vector<fdf::NodePtr> nodes;
  driver_host().SetStartHandler(
      [this, &node_controller, &nodes](fdf::DriverStartArgs start_args, auto request) {
        realm().SetCreateChildHandler(
            [](fdecl::CollectionRef collection, fdecl::Child decl, auto offers) {});
        realm().SetOpenExposedDirHandler([this](fdecl::ChildRef child, auto exposed_dir) {
          driver_dir().Bind(std::move(exposed_dir));
        });

        fdf::NodePtr node;
        EXPECT_EQ(ZX_OK, node.Bind(std::move(*start_args.mutable_node()), dispatcher()));
        for (size_t i = 0; i < 100; i++) {
          fdf::NodeAddArgs args;
          args.set_name(std::string("child") + std::to_string(i));
          fdf::NodeControllerPtr ptr;
          node->AddChild(std::move(args), ptr.NewRequest(dispatcher()), {},
                         [&node_controller, ptr = std::move(ptr)](auto result) mutable {
                           EXPECT_FALSE(result.is_err());
                           node_controller.emplace_back(std::move(ptr));
                         });
        }
        auto driver = BindDriver(std::move(request), std::move(node));
        nodes.emplace_back(std::move(driver->node()));
      });
  auto root_driver = StartRootDriver("fuchsia-boot:///#meta/root-driver.cm", driver_runner);
  ASSERT_EQ(ZX_OK, root_driver.status_value());
  Unbind();
}

TEST_F(DriverRunnerTest, TestBindResultTracker) {
  bool callback_called = false;
  bool* callback_called_ptr = &callback_called;

  auto callback = [callback_called_ptr](
                      fidl::VectorView<fuchsia_driver_development::wire::NodeBindingInfo> results) {
    ASSERT_EQ(std::string_view("node_name"), results[0].node_name().get());
    ASSERT_EQ(std::string_view("driver_url"), results[0].driver_url().get());
    ASSERT_EQ(1ul, results.count());
    *callback_called_ptr = true;
  };

  BindResultTracker tracker(3, std::move(callback));
  ASSERT_EQ(false, callback_called);
  tracker.ReportNoBind();
  ASSERT_EQ(false, callback_called);
  tracker.ReportSuccessfulBind(std::string_view("node_name"), std::string_view("driver_url"));
  ASSERT_EQ(false, callback_called);
  tracker.ReportNoBind();
  ASSERT_EQ(true, callback_called);

  callback_called = false;
  auto callback_two =
      [callback_called_ptr](
          fidl::VectorView<fuchsia_driver_development::wire::NodeBindingInfo> results) {
        ASSERT_EQ(0ul, results.count());
        *callback_called_ptr = true;
      };

  BindResultTracker tracker_two(3, std::move(callback_two));
  ASSERT_EQ(false, callback_called);
  tracker_two.ReportNoBind();
  ASSERT_EQ(false, callback_called);
  tracker_two.ReportNoBind();
  ASSERT_EQ(false, callback_called);
  tracker_two.ReportNoBind();
  ASSERT_EQ(true, callback_called);

  callback_called = false;
  auto callback_three =
      [callback_called_ptr](
          fidl::VectorView<fuchsia_driver_development::wire::NodeBindingInfo> results) {
        ASSERT_EQ(std::string_view("node_name"), results[0].node_name().get());
        ASSERT_EQ(std::string_view("driver_url"), results[0].driver_url().get());
        ASSERT_EQ(1ul, results.count());
        *callback_called_ptr = true;
      };

  BindResultTracker tracker_three(3, std::move(callback_three));
  ASSERT_EQ(false, callback_called);
  tracker_three.ReportNoBind();
  ASSERT_EQ(false, callback_called);
  tracker_three.ReportNoBind();
  ASSERT_EQ(false, callback_called);
  tracker_three.ReportSuccessfulBind(std::string_view("node_name"), std::string_view("driver_url"));
  ASSERT_EQ(true, callback_called);
}

TEST(CompositeServiceOfferTest, WorkingOffer) {
  const std::string_view kServiceName = "fuchsia.service";
  fidl::Arena<> arena;
  auto service = fcd::wire::OfferService::Builder(arena);
  service.source_name(arena, kServiceName);
  service.target_name(arena, kServiceName);

  fidl::VectorView<fcd::wire::NameMapping> mappings(arena, 2);
  mappings[0].source_name = fidl::StringView(arena, "instance-1");
  mappings[0].target_name = fidl::StringView(arena, "default");

  mappings[1].source_name = fidl::StringView(arena, "instance-1");
  mappings[1].target_name = fidl::StringView(arena, "instance-2");
  service.renamed_instances(mappings);

  fidl::VectorView<fidl::StringView> filters(arena, 2);
  filters[0] = fidl::StringView(arena, "default");
  filters[1] = fidl::StringView(arena, "instance-2");
  service.source_instance_filter(filters);

  auto offer = fcd::wire::Offer::WithService(arena, service.Build());
  auto new_offer = CreateCompositeServiceOffer(arena, offer, "parent_node", false);
  ASSERT_TRUE(new_offer);

  ASSERT_EQ(2ul, new_offer->service().renamed_instances().count());
  // Check that the default instance got renamed.
  ASSERT_EQ(std::string("instance-1"),
            std::string(new_offer->service().renamed_instances()[0].source_name.get()));
  ASSERT_EQ(std::string("parent_node"),
            std::string(new_offer->service().renamed_instances()[0].target_name.get()));

  // Check that a non-default instance stayed the same.
  ASSERT_EQ(std::string("instance-1"),
            std::string(new_offer->service().renamed_instances()[1].source_name.get()));
  ASSERT_EQ(std::string("instance-2"),
            std::string(new_offer->service().renamed_instances()[1].target_name.get()));

  ASSERT_EQ(2ul, new_offer->service().source_instance_filter().count());
  // Check that the default filter got renamed.
  ASSERT_EQ(std::string("parent_node"),
            std::string(new_offer->service().source_instance_filter()[0].get()));

  // Check that a non-default filter stayed the same.
  ASSERT_EQ(std::string("instance-2"),
            std::string(new_offer->service().source_instance_filter()[1].get()));
}

TEST(CompositeServiceOfferTest, WorkingOfferPrimary) {
  const std::string_view kServiceName = "fuchsia.service";
  fidl::Arena<> arena;
  auto service = fcd::wire::OfferService::Builder(arena);
  service.source_name(arena, kServiceName);
  service.target_name(arena, kServiceName);

  fidl::VectorView<fcd::wire::NameMapping> mappings(arena, 2);
  mappings[0].source_name = fidl::StringView(arena, "instance-1");
  mappings[0].target_name = fidl::StringView(arena, "default");

  mappings[1].source_name = fidl::StringView(arena, "instance-1");
  mappings[1].target_name = fidl::StringView(arena, "instance-2");
  service.renamed_instances(mappings);

  fidl::VectorView<fidl::StringView> filters(arena, 2);
  filters[0] = fidl::StringView(arena, "default");
  filters[1] = fidl::StringView(arena, "instance-2");
  service.source_instance_filter(filters);

  auto offer = fcd::wire::Offer::WithService(arena, service.Build());
  auto new_offer = CreateCompositeServiceOffer(arena, offer, "parent_node", true);
  ASSERT_TRUE(new_offer);

  ASSERT_EQ(3ul, new_offer->service().renamed_instances().count());
  // Check that the default instance stayed the same (because we're primary).
  ASSERT_EQ(std::string("instance-1"),
            std::string(new_offer->service().renamed_instances()[0].source_name.get()));
  ASSERT_EQ(std::string("default"),
            std::string(new_offer->service().renamed_instances()[0].target_name.get()));

  // Check that the default instance got renamed.
  ASSERT_EQ(std::string("instance-1"),
            std::string(new_offer->service().renamed_instances()[1].source_name.get()));
  ASSERT_EQ(std::string("parent_node"),
            std::string(new_offer->service().renamed_instances()[1].target_name.get()));

  // Check that a non-default instance stayed the same.
  ASSERT_EQ(std::string("instance-1"),
            std::string(new_offer->service().renamed_instances()[2].source_name.get()));
  ASSERT_EQ(std::string("instance-2"),
            std::string(new_offer->service().renamed_instances()[2].target_name.get()));

  ASSERT_EQ(3ul, new_offer->service().source_instance_filter().count());
  // Check that the default filter stayed the same (because we're primary).
  EXPECT_EQ(std::string("default"),
            std::string(new_offer->service().source_instance_filter()[0].get()));

  // Check that the default filter got renamed.
  EXPECT_EQ(std::string("parent_node"),
            std::string(new_offer->service().source_instance_filter()[1].get()));

  // Check that a non-default filter stayed the same.
  EXPECT_EQ(std::string("instance-2"),
            std::string(new_offer->service().source_instance_filter()[2].get()));
}
