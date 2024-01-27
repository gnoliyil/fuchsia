// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/display/display_manager2.h"

#include <fuchsia/images2/cpp/fidl.h>
#include <fuchsia/ui/display/cpp/fidl.h>
#include <fuchsia/ui/display/cpp/fidl_test_base.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/syslog/cpp/macros.h>

#include <gtest/gtest.h>

#include "src/lib/fsl/handles/object_info.h"
#include "src/lib/testing/loop_fixture/test_loop_fixture.h"
#include "src/ui/scenic/lib/display/display_controller_listener.h"
#include "src/ui/scenic/lib/display/tests/mock_display_controller.h"

namespace scenic_impl {
namespace display {
namespace test {

using DisplayManagerTest = gtest::TestLoopFixture;

using OnDisplayAddedCallback = std::function<void(fuchsia::ui::display::Info)>;
using OnDisplayRemovedCallback = std::function<void(fuchsia::ui::display::DisplayRef)>;
using OnDisplayOwnershipChangedCallback = std::function<void(
    std::vector<fuchsia::ui::display::DisplayRef> displays, bool owned_by_display_coordinator)>;

class MockDisplayListener : public fuchsia::ui::display::testing::DisplayListener_TestBase {
 public:
  void NotImplemented_(const std::string& name) final {}

  fidl::InterfaceHandle<fuchsia::ui::display::DisplayListener> Bind() {
    fidl::InterfaceHandle<fuchsia::ui::display::DisplayListener> listener;
    bindings_.AddBinding(this, listener.NewRequest(), nullptr);
    return listener;
  }

  void set_on_display_added_callback(OnDisplayAddedCallback callback) {
    on_display_added_cb_ = callback;
  }

  void set_on_display_removed_callback(OnDisplayRemovedCallback callback) {
    on_display_removed_cb_ = callback;
  }

  void set_on_display_ownership_changed_callback(OnDisplayOwnershipChangedCallback callback) {
    on_display_ownership_changed_cb_ = callback;
  }

  void OnDisplayAdded(fuchsia::ui::display::Info display) {
    if (on_display_added_cb_) {
      on_display_added_cb_(std::move(display));
    }
  }

  void OnDisplayRemoved(fuchsia::ui::display::DisplayRef display) {
    if (on_display_removed_cb_) {
      on_display_removed_cb_(std::move(display));
    }
  }

  void OnDisplayOwnershipChanged(std::vector<fuchsia::ui::display::DisplayRef> displays,
                                 bool owned_by_display_coordinator) {
    if (on_display_ownership_changed_cb_) {
      on_display_ownership_changed_cb_(std::move(displays), owned_by_display_coordinator);
    }
  }

 private:
  fidl::BindingSet<fuchsia::ui::display::DisplayListener> bindings_;
  OnDisplayAddedCallback on_display_added_cb_;
  OnDisplayRemovedCallback on_display_removed_cb_;
  OnDisplayOwnershipChangedCallback on_display_ownership_changed_cb_;
};

static fuchsia::hardware::display::Info CreateFakeDisplayInfo(uint64_t display_id) {
  fuchsia::hardware::display::Mode mode = {
      .horizontal_resolution = 1024, .vertical_resolution = 800, .refresh_rate_e2 = 60, .flags = 0};
  fuchsia::hardware::display::Info display = {
      .id = display_id,
      .modes = {mode},
      .pixel_format = {fuchsia::images2::PixelFormat::BGRA32},
      .cursor_configs = {},
      .manufacturer_name = "fake_manufacturer_name",
      .monitor_name = "fake_monitor_name",
      .monitor_serial = "fake_monitor_serial"};
  return display;
}

TEST_F(DisplayManagerTest, RemoveInvalidDisplayCoordinator) {
  DisplayManager2 display_manager;
  DisplayCoordinatorObjects display_coordinator_objs = CreateMockDisplayCoordinator();
  display_manager.AddDisplayCoordinator(display_coordinator_objs.interface_ptr,
                                        std::move(display_coordinator_objs.listener));

  std::vector<fuchsia::ui::display::Info> displays_added;
  std::vector<fuchsia::ui::display::DisplayRef> displays_removed;

  MockDisplayListener displays_changed_listener;
  display_manager.AddDisplayListener(displays_changed_listener.Bind());
  displays_changed_listener.set_on_display_added_callback(
      [&displays_added](auto display_info) { displays_added.push_back(std::move(display_info)); });
  displays_changed_listener.set_on_display_removed_callback([&displays_removed](auto display_ref) {
    displays_removed.push_back(std::move(display_ref));
  });

  // Add display with id = 1
  display_coordinator_objs.mock->events().OnDisplaysChanged(
      /* added */ {CreateFakeDisplayInfo(/*display_id=*/1)},
      /* removed */ {});
  RunLoopUntilIdle();
  ASSERT_EQ(1u, displays_added.size());

  // Invalidate display coordinator.
  display_coordinator_objs.mock.reset();
  RunLoopUntilIdle();
  EXPECT_EQ(1u, displays_added.size());  // Unchanged.

  // Displays are marked as removed if their display coordinator is destroyed.
  EXPECT_EQ(1u, displays_removed.size());
}

TEST_F(DisplayManagerTest, OnDisplaysChanged) {
  DisplayCoordinatorObjects display_coordinator_objs = CreateMockDisplayCoordinator();
  {
    DisplayManager2 display_manager;
    display_manager.AddDisplayCoordinator(display_coordinator_objs.interface_ptr,
                                          std::move(display_coordinator_objs.listener));
    std::vector<fuchsia::ui::display::Info> displays_added;
    std::vector<fuchsia::ui::display::DisplayRef> displays_removed;

    MockDisplayListener displays_changed_listener;
    display_manager.AddDisplayListener(displays_changed_listener.Bind());
    displays_changed_listener.set_on_display_added_callback([&displays_added](auto display_info) {
      displays_added.push_back(std::move(display_info));
    });
    displays_changed_listener.set_on_display_removed_callback(
        [&displays_removed](auto display_ref) {
          displays_removed.push_back(std::move(display_ref));
        });

    // Add display with id = 1
    display_coordinator_objs.mock->events().OnDisplaysChanged(
        /* added */ {CreateFakeDisplayInfo(/*display_id=*/1)},
        /* removed */ {});
    RunLoopUntilIdle();
    ASSERT_EQ(1u, displays_added.size());
    EXPECT_EQ(0u, displays_removed.size());

    // Add another display with id = 1. Expect error.
    display_coordinator_objs.mock->events().OnDisplaysChanged(
        /* added */ {CreateFakeDisplayInfo(/*display_id=*/1)},
        /* removed */ {});
    RunLoopUntilIdle();
    EXPECT_EQ(display_manager.last_error(),
              "DisplayManager: Display added, but a display already exists with same id=1");
    EXPECT_EQ(1u, displays_added.size());
    EXPECT_EQ(0u, displays_removed.size());

    // Remove display that doesn't exist.
    display_coordinator_objs.mock->events().OnDisplaysChanged(/* added */ {}, /* removed */ {2u});
    RunLoopUntilIdle();
    EXPECT_EQ(display_manager.last_error(),
              "DisplayManager: Got a display removed event for invalid display=2");
    ASSERT_EQ(1u, displays_added.size());
    EXPECT_EQ(0u, displays_removed.size());

    // Remove display that exists.
    display_coordinator_objs.mock->events().OnDisplaysChanged(/* added */ {}, /* removed */ {1u});
    RunLoopUntilIdle();
    EXPECT_EQ(1u, displays_added.size());
    EXPECT_EQ(1u, displays_removed.size());

    // Add display with id = 2
    display_coordinator_objs.mock->events().OnDisplaysChanged(
        /* added */ {CreateFakeDisplayInfo(/*display_id=*/2)},
        /* removed */ {});
    RunLoopUntilIdle();
    ASSERT_EQ(2u, displays_added.size());
    EXPECT_EQ(1u, displays_removed.size());

    // The two displays are unique.
    EXPECT_NE(fsl::GetKoid(displays_added[0].display_ref().reference.get()),
              fsl::GetKoid(displays_added[1].display_ref().reference.get()));
  }

  // Expect no crashes during teardown.

  // Trigger display coordinator events after display manager is destroyed.
  display_coordinator_objs.mock->events().OnDisplaysChanged(
      /* added */ {CreateFakeDisplayInfo(/*display_id=*/3)},
      /* removed */ {});
  display_coordinator_objs.mock->events().OnClientOwnershipChange(true);

  // Invalidate display coordinator.
  display_coordinator_objs.mock.reset();
  RunLoopUntilIdle();
}

TEST_F(DisplayManagerTest, OnDisplaysChangedBeforeAdddingListener) {
  DisplayManager2 display_manager;
  DisplayCoordinatorObjects display_coordinator_objs = CreateMockDisplayCoordinator();
  display_manager.AddDisplayCoordinator(display_coordinator_objs.interface_ptr,
                                        std::move(display_coordinator_objs.listener));
  std::vector<fuchsia::ui::display::Info> displays_added;
  std::vector<fuchsia::ui::display::DisplayRef> displays_removed;
  std::vector<fuchsia::ui::display::DisplayRef> displays_ownership_changed;
  bool has_ownership = false;

  // Add displays with id = 1 and id = 2
  display_coordinator_objs.mock->events().OnDisplaysChanged(
      /* added */ {CreateFakeDisplayInfo(/*display_id=*/1),
                   CreateFakeDisplayInfo(/*display_id=*/2)},
      /* removed */ {});
  RunLoopUntilIdle();

  // Remove display with id = 1.
  display_coordinator_objs.mock->events().OnDisplaysChanged(/* added */ {}, /* removed */ {1u});
  RunLoopUntilIdle();

  display_coordinator_objs.mock->events().OnClientOwnershipChange(true);
  RunLoopUntilIdle();

  // Add a listener and expect it to receive an DisplayAdded with id = 1 and a display ownership
  // changed event.
  MockDisplayListener displays_changed_listener;
  display_manager.AddDisplayListener(displays_changed_listener.Bind());
  displays_changed_listener.set_on_display_added_callback(
      [&displays_added](auto display_info) { displays_added.push_back(std::move(display_info)); });
  displays_changed_listener.set_on_display_removed_callback([&displays_removed](auto display_ref) {
    displays_removed.push_back(std::move(display_ref));
  });
  displays_changed_listener.set_on_display_ownership_changed_callback(
      [&displays_ownership_changed, &has_ownership](
          std::vector<fuchsia::ui::display::DisplayRef> displays, bool owned) {
        for (auto& display : displays) {
          displays_ownership_changed.push_back(std::move(display));
        }
        has_ownership = owned;
      });
  RunLoopUntilIdle();

  EXPECT_EQ(1u, displays_added.size());
  EXPECT_EQ(0u, displays_removed.size());

  ASSERT_EQ(1u, displays_ownership_changed.size());
  EXPECT_EQ(fsl::GetKoid(displays_added[0].display_ref().reference.get()),
            fsl::GetKoid(displays_ownership_changed[0].reference.get()));
}

TEST_F(DisplayManagerTest, DisplayOwnershipChanged) {
  DisplayManager2 display_manager;

  DisplayCoordinatorObjects display_coordinator_objs1 = CreateMockDisplayCoordinator();
  display_manager.AddDisplayCoordinator(display_coordinator_objs1.interface_ptr,
                                        std::move(display_coordinator_objs1.listener));
  DisplayCoordinatorObjects display_coordinator_objs2 = CreateMockDisplayCoordinator();
  display_manager.AddDisplayCoordinator(display_coordinator_objs2.interface_ptr,
                                        std::move(display_coordinator_objs2.listener));

  std::vector<fuchsia::ui::display::Info> displays_added;
  std::vector<fuchsia::ui::display::DisplayRef> displays_ownership_changed;
  bool has_ownership = false;

  MockDisplayListener displays_changed_listener;
  display_manager.AddDisplayListener(displays_changed_listener.Bind());
  displays_changed_listener.set_on_display_added_callback(
      [&displays_added](auto display_info) { displays_added.push_back(std::move(display_info)); });
  displays_changed_listener.set_on_display_ownership_changed_callback(
      [&displays_ownership_changed, &has_ownership](
          std::vector<fuchsia::ui::display::DisplayRef> displays, bool owned) {
        for (auto& display : displays) {
          displays_ownership_changed.push_back(std::move(display));
        }
        has_ownership = owned;
      });

  // Add displays with ids 1...4 from two display coordinators.
  display_coordinator_objs1.mock->events().OnDisplaysChanged(
      /*added=*/{CreateFakeDisplayInfo(/*display_id=*/1)},
      /*removed=*/{});
  display_coordinator_objs1.mock->events().OnDisplaysChanged(
      /*added=*/{CreateFakeDisplayInfo(/*display_id=*/2)},
      /*removed=*/{});
  // Make sure the operations for the first display coordinator are finished first, because we rely
  // on the order displays are added below.
  RunLoopUntilIdle();
  display_coordinator_objs2.mock->events().OnDisplaysChanged(
      /*added=*/{CreateFakeDisplayInfo(/*display_id=*/3)},
      /*removed=*/{});
  display_coordinator_objs2.mock->events().OnDisplaysChanged(
      /*added=*/{CreateFakeDisplayInfo(/*display_id=*/4)},
      /*removed=*/{});

  display_coordinator_objs1.mock->events().OnClientOwnershipChange(true);
  RunLoopUntilIdle();
  EXPECT_EQ(4u, displays_added.size());
  EXPECT_TRUE(has_ownership);
  EXPECT_EQ(2u, displays_ownership_changed.size());
  EXPECT_EQ(fsl::GetKoid(displays_added[0].display_ref().reference.get()),
            fsl::GetKoid(displays_ownership_changed[0].reference.get()));
  EXPECT_EQ(fsl::GetKoid(displays_added[1].display_ref().reference.get()),
            fsl::GetKoid(displays_ownership_changed[1].reference.get()));
}

TEST_F(DisplayManagerTest, ClaimDisplay) {
  DisplayManager2 display_manager;
  DisplayCoordinatorObjects display_coordinator_objs = CreateMockDisplayCoordinator();
  display_manager.AddDisplayCoordinator(display_coordinator_objs.interface_ptr,
                                        std::move(display_coordinator_objs.listener));

  const uint64_t kTestDisplayId1 = 1u;
  const uint64_t kTestDisplayId2 = 2u;
  const uint64_t kTestDisplayId3 = 3u;
  const uint64_t kTestTimestamp = 111111u;
  const fuchsia::hardware::display::ConfigStamp kTestConfigStamp = {.value = 2u};

  display_coordinator_objs.mock->events().OnDisplaysChanged(
      /* added */ {CreateFakeDisplayInfo(kTestDisplayId1)},
      /* removed */ {});

  std::vector<fuchsia::ui::display::Info> displays_added;
  MockDisplayListener displays_changed_listener;
  display_manager.AddDisplayListener(displays_changed_listener.Bind());
  displays_changed_listener.set_on_display_added_callback(
      [&displays_added](auto display_info) { displays_added.push_back(std::move(display_info)); });

  RunLoopUntilIdle();
  EXPECT_EQ(1u, displays_added.size());

  {
    DisplayCoordinatorUniquePtr display_coordinator =
        display_manager.ClaimDisplay(fsl::GetKoid(displays_added[0].display_ref().reference.get()));
    EXPECT_TRUE(display_coordinator);
    EXPECT_EQ(1u, display_coordinator->displays()->size());
    EXPECT_EQ(kTestDisplayId1, display_coordinator->displays()->at(0).display_id());

    // Try claiming the display a second time.
    DisplayCoordinatorUniquePtr display_coordinator2 =
        display_manager.ClaimDisplay(fsl::GetKoid(displays_added[0].display_ref().reference.get()));
    EXPECT_FALSE(display_coordinator2);

    // Test display added/removed events.
    bool display_added_received = false;
    bool display_removed_received = false;
    display_coordinator->set_on_display_added_callback([&](Display2* display) {
      display_added_received = true;
      EXPECT_EQ(kTestDisplayId2, display->display_id());
    });
    display_coordinator->set_on_display_removed_callback([&](uint64_t display_id) {
      display_removed_received = true;
      EXPECT_EQ(kTestDisplayId1, display_id);
    });

    display_coordinator_objs.mock->events().OnDisplaysChanged(
        /* added */ {CreateFakeDisplayInfo(kTestDisplayId2)},
        /* removed */ {kTestDisplayId1});
    RunLoopUntilIdle();
    EXPECT_TRUE(display_added_received);
    EXPECT_TRUE(display_removed_received);
    EXPECT_EQ(1u, display_coordinator->displays()->size());
    EXPECT_EQ(kTestDisplayId2, display_coordinator->displays()->at(0).display_id());

    // Test vsync delivery.
    bool vsync_received = false;
    display_coordinator->displays()->at(0).set_on_vsync_callback(
        [&](zx::time timestamp, fuchsia::hardware::display::ConfigStamp stamp) {
          vsync_received = true;
          EXPECT_EQ(zx::time(kTestTimestamp), timestamp);
          EXPECT_EQ(kTestConfigStamp.value, stamp.value);
        });

    display_coordinator_objs.mock->events().OnVsync(kTestDisplayId2, kTestTimestamp,
                                                    kTestConfigStamp, 0);
    RunLoopUntilIdle();
    EXPECT_TRUE(vsync_received);
  }

  // The display is now unclaimed.

  // Trigger a few events to check that we don't need a claimed display.
  display_coordinator_objs.mock->events().OnDisplaysChanged(
      /* added */ {CreateFakeDisplayInfo(kTestDisplayId3)},
      /* removed */ {kTestDisplayId2});
  display_coordinator_objs.mock->events().OnVsync(kTestDisplayId3, kTestTimestamp, kTestConfigStamp,
                                                  0);
  RunLoopUntilIdle();

  // Claim the display again.
  const size_t kDisplaysAddedSize = 3;
  ASSERT_EQ(kDisplaysAddedSize, displays_added.size());
  DisplayCoordinatorUniquePtr display_coordinator = display_manager.ClaimDisplay(
      fsl::GetKoid(displays_added[kDisplaysAddedSize - 1].display_ref().reference.get()));
  EXPECT_TRUE(display_coordinator);
  EXPECT_EQ(1u, display_coordinator->displays()->size());
  EXPECT_EQ(kTestDisplayId3, display_coordinator->displays()->at(0).display_id());
}

}  // namespace test
}  // namespace display
}  // namespace scenic_impl
