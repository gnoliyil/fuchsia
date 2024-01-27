// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.nodegroup.test/cpp/wire.h>
#include <lib/driver/compat/cpp/compat.h>
#include <lib/driver/component/cpp/driver_cpp.h>
#include <lib/driver/component/cpp/node_group.h>

#include <bind/fuchsia/nodegroupbind/test/cpp/bind.h>

namespace fdf {
using namespace fuchsia_driver_framework;
}  // namespace fdf

namespace ft = fuchsia_nodegroup_test;
namespace bindlib = bind_fuchsia_nodegroupbind_test;

namespace {

// Name these differently than what the child expects, so we test that
// FDF renames these correctly.
const std::string_view kLeftName = "left-node";
const std::string_view kRightName = "right-node";
const std::string_view kOptionalName = "optional-node";

// Group 1 is created before creating both the left and right nodes.
fdf::NodeGroup NodeGroupOne() {
  auto bind_rules_left = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_ONE_LEFT),
  };

  auto properties_left = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_LEFT),
  };

  auto bind_rules_right = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_ONE_RIGHT),
  };

  auto properties_right = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_RIGHT),
  };

  auto nodes = std::vector{
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_left,
          .properties = properties_left,
      }},
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_right,
          .properties = properties_right,
      }},
  };

  return {{.name = "test_group_1", .nodes = nodes}};
}

// Group 2 is created after creating the right node, but before creating the left node.
fdf::NodeGroup NodeGroupTwo() {
  auto bind_rules_left = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_TWO_LEFT),
  };

  auto properties_left = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_LEFT),
  };

  auto bind_rules_right = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_TWO_RIGHT),
  };

  auto properties_right = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_RIGHT),
  };

  auto nodes = std::vector{
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_left,
          .properties = properties_left,
      }},
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_right,
          .properties = properties_right,
      }},
  };

  return {{.name = "test_group_2", .nodes = nodes}};
}

// Group 3 is created after creating both the left and right nodes.
fdf::NodeGroup NodeGroupThree() {
  auto bind_rules_left = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_THREE_LEFT),
  };

  auto properties_left = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_LEFT),
  };

  auto bind_rules_right = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_THREE_RIGHT),
  };

  auto properties_right = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_RIGHT),
  };

  auto nodes = std::vector{
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_left,
          .properties = properties_left,
      }},
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_right,
          .properties = properties_right,
      }},
  };

  return {{.name = "test_group_3", .nodes = nodes}};
}

// Group 4 is created before creating the left, optional, and right nodes.
fdf::NodeGroup NodeGroupFour() {
  auto bind_rules_left = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_FOUR_LEFT),
  };

  auto properties_left = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_LEFT),
  };

  auto bind_rules_right = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_FOUR_RIGHT),
  };

  auto properties_right = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_RIGHT),
  };

  auto bind_rules_optional = std::vector{
      fdf::MakeAcceptBindRule(bindlib::TEST_BIND_PROPERTY,
                              bindlib::TEST_BIND_PROPERTY_FOUR_OPTIONAL),
  };

  auto properties_optional = std::vector{
      fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, bindlib::TEST_BIND_PROPERTY_DRIVER_OPTIONAL),
  };

  auto nodes = std::vector{
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_left,
          .properties = properties_left,
      }},
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_right,
          .properties = properties_right,
      }},
      fdf::NodeRepresentation{{
          .bind_rules = bind_rules_optional,
          .properties = properties_optional,
      }},
  };

  return {{.name = "test_group_4", .nodes = nodes}};
}

class NumberServer : public fidl::WireServer<ft::Device> {
 public:
  explicit NumberServer(uint32_t number) : number_(number) {}

  void GetNumber(GetNumberCompleter::Sync& completer) override { completer.Reply(number_); }

 private:
  uint32_t number_;
};

class RootDriver : public fdf::DriverBase {
 public:
  RootDriver(fdf::DriverStartArgs start_args, fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : fdf::DriverBase("root", std::move(start_args), std::move(driver_dispatcher)) {}

  zx::result<> Start() override {
    node_client_.Bind(std::move(node()), dispatcher());
    // Add service "left".
    {
      auto device = [this](fidl::ServerEnd<ft::Device> server_end) mutable -> void {
        fidl::BindServer(dispatcher(), std::move(server_end), &this->left_server_);
      };
      ft::Service::InstanceHandler handler({.device = std::move(device)});
      zx::result<> status =
          context().outgoing()->AddService<ft::Service>(std::move(handler), kLeftName);
      if (status.is_error()) {
        FDF_LOG(ERROR, "Failed to add service %s", status.status_string());
      }
    }

    // Add service "right".
    {
      auto device = [this](fidl::ServerEnd<ft::Device> server_end) mutable -> void {
        fidl::BindServer(dispatcher(), std::move(server_end), &this->right_server_);
      };
      ft::Service::InstanceHandler handler({.device = std::move(device)});
      zx::result<> status =
          context().outgoing()->AddService<ft::Service>(std::move(handler), kRightName);
      if (status.is_error()) {
        FDF_LOG(ERROR, "Failed to add service %s", status.status_string());
      }
    }

    // Add service "optional".
    {
      auto device = [this](fidl::ServerEnd<ft::Device> server_end) mutable -> void {
        fidl::BindServer(dispatcher(), std::move(server_end), &this->optional_server_);
      };
      ft::Service::InstanceHandler handler({.device = std::move(device)});
      zx::result<> status =
          context().outgoing()->AddService<ft::Service>(std::move(handler), kOptionalName);
      if (status.is_error()) {
        FDF_LOG(ERROR, "Failed to add service %s", status.status_string());
      }
    }

    // Setup the node group manager client.
    auto dgm_client = context().incoming()->Connect<fdf::NodeGroupManager>();
    if (dgm_client.is_error()) {
      FDF_LOG(ERROR, "Failed to connect to NodeGroupManager: %s",
              zx_status_get_string(dgm_client.error_value()));
      DropNode();
      return dgm_client.take_error();
    }

    node_group_manager_.Bind(std::move(dgm_client.value()), dispatcher());

    TestGroupOne();
    TestGroupTwo();
    TestGroupThree();
    TestGroupFour();
    return zx::ok();
  }

 private:
  // Add node group
  // Add left
  // Add right
  void TestGroupOne() {
    fit::closure add_right = [this]() {
      auto right_result = AddChild(kRightName, 1, one_right_controller_,
                                   bindlib::TEST_BIND_PROPERTY_ONE_RIGHT, []() {});
      if (!right_result) {
        FDF_LOG(ERROR, "Failed to start right child.");
        DropNode();
      }
    };

    fit::closure add_left_then_right = [this, add_right = std::move(add_right)]() mutable {
      auto left_result = AddChild(kLeftName, 1, one_left_controller_,
                                  bindlib::TEST_BIND_PROPERTY_ONE_LEFT, std::move(add_right));
      if (!left_result) {
        FDF_LOG(ERROR, "Failed to start left child.");
        DropNode();
      }
    };

    AddNodeGroup(NodeGroupOne(), std::move(add_left_then_right));
  }

  // Add right
  // Add node group
  // Add left
  void TestGroupTwo() {
    fit::closure add_left = [this]() mutable {
      auto left_result = AddChild(kLeftName, 2, two_left_controller_,
                                  bindlib::TEST_BIND_PROPERTY_TWO_LEFT, []() {});
      if (!left_result) {
        FDF_LOG(ERROR, "Failed to start left child.");
        DropNode();
      }
    };

    fit::closure add_node_group_then_left = [this, add_left = std::move(add_left)]() mutable {
      AddNodeGroup(NodeGroupTwo(), std::move(add_left));
    };

    auto right_result =
        AddChild(kRightName, 2, two_right_controller_, bindlib::TEST_BIND_PROPERTY_TWO_RIGHT,
                 std::move(add_node_group_then_left));
    if (!right_result) {
      FDF_LOG(ERROR, "Failed to start right child.");
      DropNode();
    }
  }

  // Add left
  // Add right
  // Add node group
  void TestGroupThree() {
    fit::closure add_node_group = [this]() mutable { AddNodeGroup(NodeGroupThree(), []() {}); };

    fit::closure add_right_then_node_group = [this, add_node_group =
                                                        std::move(add_node_group)]() mutable {
      auto right_result =
          AddChild(kRightName, 3, three_right_controller_, bindlib::TEST_BIND_PROPERTY_THREE_RIGHT,
                   std::move(add_node_group));
      if (!right_result) {
        FDF_LOG(ERROR, "Failed to start right child.");
        DropNode();
      }
    };

    auto left_result =
        AddChild(kLeftName, 3, three_left_controller_, bindlib::TEST_BIND_PROPERTY_THREE_LEFT,
                 std::move(add_right_then_node_group));
    if (!left_result) {
      FDF_LOG(ERROR, "Failed to start left child.");
      DropNode();
    }
  }

  // Add node group
  // Add left
  // Add optional
  // Add right
  void TestGroupFour() {
    fit::closure add_right = [this]() {
      auto right_result = AddChild(kRightName, 4, four_right_controller_,
                                   bindlib::TEST_BIND_PROPERTY_FOUR_RIGHT, []() {});
      if (!right_result) {
        FDF_LOG(ERROR, "Failed to start right child.");
        DropNode();
      }
    };

    fit::closure add_optional_then_right = [this, add_right = std::move(add_right)]() mutable {
      auto optional_result =
          AddChild(kOptionalName, 4, four_optional_controller_,
                   bindlib::TEST_BIND_PROPERTY_FOUR_OPTIONAL, std::move(add_right));
      if (!optional_result) {
        FDF_LOG(ERROR, "Failed to start optional child.");
        DropNode();
      }
    };

    fit::closure add_left_then_optional = [this, add_optional =
                                                     std::move(add_optional_then_right)]() mutable {
      auto left_result = AddChild(kLeftName, 4, four_left_controller_,
                                  bindlib::TEST_BIND_PROPERTY_FOUR_LEFT, std::move(add_optional));
      if (!left_result) {
        FDF_LOG(ERROR, "Failed to start left child.");
        DropNode();
      }
    };

    AddNodeGroup(NodeGroupFour(), std::move(add_left_then_optional));
  }

  bool AddChild(std::string_view name, int group,
                fidl::SharedClient<fdf::NodeController>& controller, std::string_view property,
                fit::closure callback) {
    auto node_name = std::string(name) + "-" + std::to_string(group);
    // Set the properties of the node that a driver will bind to.
    fdf::NodeProperty node_property = fdf::MakeProperty(bindlib::TEST_BIND_PROPERTY, property);
    fdf::NodeAddArgs args({.name = node_name,
                           .offers = {{fdf::MakeOffer<ft::Service>(name)}},
                           .properties = {{node_property}}});

    // Create endpoints of the `NodeController` for the node.
    auto endpoints = fidl::CreateEndpoints<fdf::NodeController>();
    if (endpoints.is_error()) {
      return false;
    }

    auto add_callback = [this, &controller, node_name, callback = std::move(callback),
                         client = std::move(endpoints->client)](
                            fidl::Result<fdf::Node::AddChild>& result) mutable {
      if (result.is_error()) {
        FDF_LOG(ERROR, "Adding child failed: %s", result.error_value().FormatDescription().c_str());
        DropNode();
        return;
      }

      controller.Bind(std::move(client), dispatcher());
      FDF_LOG(INFO, "Successfully added child %s.", node_name.c_str());
      callback();
    };

    node_client_->AddChild({std::move(args), std::move(endpoints->server), {}})
        .Then(std::move(add_callback));
    return true;
  }

  void AddNodeGroup(fdf::NodeGroup dev_group, fit::closure callback) {
    auto dev_group_name = dev_group.name();
    node_group_manager_->AddNodeGroup(std::move(dev_group))
        .Then([this, dev_group_name, callback = std::move(callback)](
                  fidl::Result<fdf::NodeGroupManager::AddNodeGroup>& create_result) {
          if (create_result.is_error()) {
            FDF_LOG(ERROR, "AddNodeGroup failed: %s",
                    create_result.error_value().FormatDescription().c_str());
            DropNode();
            return;
          }

          auto name = dev_group_name.has_value() ? dev_group_name.value() : "";
          FDF_LOG(INFO, "Succeeded adding node group %s.", name.c_str());
          callback();
        });
  }

  void DropNode() { node_client_.AsyncTeardown(); }

  fidl::SharedClient<fdf::NodeController> one_left_controller_;
  fidl::SharedClient<fdf::NodeController> one_right_controller_;

  fidl::SharedClient<fdf::NodeController> two_left_controller_;
  fidl::SharedClient<fdf::NodeController> two_right_controller_;

  fidl::SharedClient<fdf::NodeController> three_left_controller_;
  fidl::SharedClient<fdf::NodeController> three_right_controller_;

  fidl::SharedClient<fdf::NodeController> four_left_controller_;
  fidl::SharedClient<fdf::NodeController> four_right_controller_;
  fidl::SharedClient<fdf::NodeController> four_optional_controller_;

  fidl::SharedClient<fdf::Node> node_client_;
  fidl::SharedClient<fdf::NodeGroupManager> node_group_manager_;

  NumberServer left_server_ = NumberServer(1);
  NumberServer right_server_ = NumberServer(2);
  NumberServer optional_server_ = NumberServer(3);
};

}  // namespace

FUCHSIA_DRIVER_RECORD_CPP_V3(fdf::Record<RootDriver>);
