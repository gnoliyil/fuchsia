// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/cobalt/bin/app/system_data_updater_impl.h"

#include <lib/fidl/cpp/binding_set.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/inspect/testing/cpp/inspect.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/syslog/cpp/macros.h>

#include <fstream>

#include "fuchsia/cobalt/cpp/fidl.h"
#include "src/lib/testing/loop_fixture/test_loop_fixture.h"

namespace cobalt {

using encoder::SystemData;
using fidl::VectorPtr;
using fuchsia::cobalt::SoftwareDistributionInfo;
using FuchsiaStatus = fuchsia::cobalt::Status;
using fuchsia::cobalt::SystemDataUpdaterPtr;
using inspect::testing::ChildrenMatch;
using inspect::testing::IntIs;
using inspect::testing::NameMatches;
using inspect::testing::NodeMatches;
using inspect::testing::PropertyList;
using inspect::testing::StringIs;
using ::testing::UnorderedElementsAre;

class CobaltAppForTest {
 public:
  CobaltAppForTest(std::unique_ptr<sys::ComponentContext> context)
      : system_data_("test", "test", ReleaseStage::DEBUG), context_(std::move(context)) {
    system_data_updater_impl_.reset(new SystemDataUpdaterImpl(
        inspector_.GetRoot().CreateChild("system_data"), &system_data_, "/tmp/test_"));

    context_->outgoing()->AddPublicService(
        system_data_updater_bindings_.GetHandler(system_data_updater_impl_.get()));
  }

  void ClearData() { system_data_updater_impl_->ClearData(); }

  const SystemData& system_make_data() { return system_data_; }

  const inspect::Inspector& inspector() { return inspector_; }

 private:
  inspect::Inspector inspector_;
  SystemData system_data_;

  std::unique_ptr<sys::ComponentContext> context_;

  std::unique_ptr<SystemDataUpdaterImpl> system_data_updater_impl_;
  fidl::BindingSet<fuchsia::cobalt::SystemDataUpdater> system_data_updater_bindings_;
};

class SystemDataUpdaterImplTests : public gtest::TestLoopFixture {
 public:
  void SetUp() override {
    TestLoopFixture::SetUp();
    cobalt_app_.reset(new CobaltAppForTest(context_provider_.TakeContext()));
  }

  void TearDown() override {
    cobalt_app_->ClearData();
    cobalt_app_.reset();
    TestLoopFixture::TearDown();
  }

 protected:
  SystemDataUpdaterPtr GetSystemDataUpdater() {
    SystemDataUpdaterPtr system_data_updater;
    context_provider_.ConnectToPublicService(system_data_updater.NewRequest());
    return system_data_updater;
  }

  const std::string& channel() {
    return cobalt_app_->system_make_data().system_profile().channel();
  }

  inspect::Hierarchy InspectHierarchy() {
    fpromise::result<inspect::Hierarchy> result =
        inspect::ReadFromVmo(cobalt_app_->inspector().DuplicateVmo());
    return result.take_value();
  }

 private:
  sys::testing::ComponentContextProvider context_provider_;
  std::unique_ptr<CobaltAppForTest> cobalt_app_;
};

TEST_F(SystemDataUpdaterImplTests, SetSoftwareDistributionInfo) {
  SystemDataUpdaterPtr system_data_updater = GetSystemDataUpdater();

  EXPECT_EQ(channel(), "<unset>");
  EXPECT_THAT(InspectHierarchy(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(NodeMatches(AllOf(
                        NameMatches("system_data"),
                        PropertyList(UnorderedElementsAre(IntIs("fidl_calls", 0),
                                                          StringIs("channel", "<unset>"),
                                                          StringIs("realm", "<unset>")))))))));

  SoftwareDistributionInfo info = SoftwareDistributionInfo();
  system_data_updater->SetSoftwareDistributionInfo(std::move(info), [](FuchsiaStatus s) {});
  RunLoopUntilIdle();

  EXPECT_EQ(channel(), "<unset>");
  EXPECT_THAT(InspectHierarchy(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(NodeMatches(AllOf(
                        NameMatches("system_data"),
                        PropertyList(UnorderedElementsAre(IntIs("fidl_calls", 1),
                                                          StringIs("channel", "<unset>"),
                                                          StringIs("realm", "<unset>")))))))));

  info = SoftwareDistributionInfo();
  info.set_current_channel("fishfood_release");
  system_data_updater->SetSoftwareDistributionInfo(std::move(info), [](FuchsiaStatus s) {});
  RunLoopUntilIdle();

  EXPECT_EQ(channel(), "fishfood_release");
  EXPECT_THAT(InspectHierarchy(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(NodeMatches(AllOf(
                        NameMatches("system_data"),
                        PropertyList(UnorderedElementsAre(IntIs("fidl_calls", 2),
                                                          StringIs("channel", "fishfood_release"),
                                                          StringIs("realm", "<unset>")))))))));

  // Set one software distribution field without overriding the other.
  info = SoftwareDistributionInfo();
  info.set_current_channel("test_channel");
  system_data_updater->SetSoftwareDistributionInfo(std::move(info), [](FuchsiaStatus s) {});
  RunLoopUntilIdle();

  EXPECT_EQ(channel(), "test_channel");
  EXPECT_THAT(InspectHierarchy(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(NodeMatches(AllOf(
                        NameMatches("system_data"),
                        PropertyList(UnorderedElementsAre(IntIs("fidl_calls", 3),
                                                          StringIs("channel", "test_channel"),
                                                          StringIs("realm", "<unset>")))))))));
}

namespace {

std::unique_ptr<SystemData> make_data() {
  return std::make_unique<SystemData>("test", "test", ReleaseStage::DEBUG);
}

std::unique_ptr<SystemDataUpdaterImpl> make_updater(inspect::Node node, SystemData* data) {
  return std::make_unique<SystemDataUpdaterImpl>(std::move(node), data, "/tmp/test_");
}

}  // namespace

TEST(SystemDataUpdaterImpl, TestSoftwareDistributionInfoPersistence) {
  inspect::Inspector inspector;
  auto system_data = make_data();
  auto updater = make_updater(inspector.GetRoot().CreateChild("system_data"), system_data.get());

  EXPECT_EQ(system_data->system_profile().channel(), "<unset>");
  EXPECT_THAT(inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(NodeMatches(AllOf(
                        NameMatches("system_data"),
                        PropertyList(UnorderedElementsAre(IntIs("fidl_calls", 0),
                                                          StringIs("channel", "<unset>"),
                                                          StringIs("realm", "<unset>")))))))));

  SoftwareDistributionInfo info = SoftwareDistributionInfo();
  info.set_current_channel("fishfood_release");
  updater->SetSoftwareDistributionInfo(std::move(info), [](FuchsiaStatus s) {});
  EXPECT_EQ(system_data->system_profile().channel(), "fishfood_release");
  EXPECT_THAT(inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(NodeMatches(AllOf(
                        NameMatches("system_data"),
                        PropertyList(UnorderedElementsAre(IntIs("fidl_calls", 1),
                                                          StringIs("channel", "fishfood_release"),
                                                          StringIs("realm", "<unset>")))))))));

  // Test restoring data.
  inspector = inspect::Inspector();
  system_data = make_data();
  updater = make_updater(inspector.GetRoot().CreateChild("system_data"), system_data.get());
  EXPECT_EQ(system_data->system_profile().channel(), "fishfood_release");
  EXPECT_THAT(inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(NodeMatches(AllOf(
                        NameMatches("system_data"),
                        PropertyList(UnorderedElementsAre(IntIs("fidl_calls", 0),
                                                          StringIs("channel", "fishfood_release"),
                                                          StringIs("realm", "<unset>")))))))));

  // Test default behavior with no data.
  updater->ClearData();
  inspector = inspect::Inspector();
  system_data = make_data();
  updater = make_updater(inspector.GetRoot().CreateChild("system_data"), system_data.get());
  EXPECT_EQ(system_data->system_profile().channel(), "<unset>");
  EXPECT_THAT(inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(NodeMatches(AllOf(
                        NameMatches("system_data"),
                        PropertyList(UnorderedElementsAre(IntIs("fidl_calls", 0),
                                                          StringIs("channel", "<unset>"),
                                                          StringIs("realm", "<unset>")))))))));
}

}  // namespace cobalt
