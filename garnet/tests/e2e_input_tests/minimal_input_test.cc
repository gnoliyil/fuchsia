// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/ui/input/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/spawn.h>
#include <lib/fit/function.h>
#include <lib/gtest/real_loop_fixture.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/ui/base_view/cpp/base_view_transitional.h>
#include <lib/ui/input/cpp/formatting.h>
#include <lib/ui/scenic/cpp/session.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>
#include <lib/zx/clock.h>
#include <zircon/status.h>

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "src/lib/fxl/logging.h"

// NOTE WELL. Run each of these e2e tests in its own executable.  They each
// consume and maintain process-global context, so it's better to keep them
// separate.  Plus, separation means they start up components in a known good
// state, instead of reusing component state possibly dirtied by other tests.

namespace {

using fuchsia::ui::input::InputEvent;
using Phase = fuchsia::ui::input::PointerEventPhase;

// Shared context for all tests in this process.
// Set it up once, never delete it.
sys::ComponentContext* g_context = nullptr;

// Max timeout in failure cases.
// Set this as low as you can that still works across all test platforms.
constexpr zx::duration kTimeout = zx::min(5);

// A very small Scenic client. Puts up a fuchsia-colored rectangle, and stores
// input events for examination.
class MinimalClientView : public scenic::BaseViewTransitional {
 public:
  MinimalClientView(scenic::ViewContextTransitional context, async_dispatcher_t* dispatcher)
      : scenic::BaseViewTransitional(std::move(context), "MinimalClientView"),
        dispatcher_(dispatcher) {
    FXL_CHECK(dispatcher_);
  }

  void CreateScene(uint32_t width_in_px, uint32_t height_in_px) {
    float width = static_cast<float>(width_in_px);
    float height = static_cast<float>(height_in_px);

    scenic::ShapeNode background(session());

    scenic::Material material(session());
    material.SetColor(255, 0, 255, 255);  // Fuchsia
    background.SetMaterial(material);

    scenic::Rectangle rectangle(session(), width, height);
    background.SetShape(rectangle);
    background.SetTranslation(width / 2, height / 2, -10.f);

    root_node().AddChild(background);
  }

  // |scenic::BaseView|
  void OnInputEvent(InputEvent event) override {
    // Simple termination condition: Last event of first gesture.
    if (event.is_pointer() && event.pointer().phase == Phase::REMOVE) {
      async::PostTask(dispatcher_, [this] {
        FXL_CHECK(on_terminate_) << "on_terminate_ was not set!";
        on_terminate_(observed_);
      });
    }

    // Store inputs for checking later.
    observed_.push_back(std::move(event));
  }

  void SetOnTerminateCallback(fit::function<void(const std::vector<InputEvent>&)> on_terminate) {
    on_terminate_ = std::move(on_terminate);
  }

 private:
  // |scenic::SessionListener|
  void OnScenicError(std::string error) override { FXL_LOG(FATAL) << error; }

  async_dispatcher_t* dispatcher_ = nullptr;
  std::vector<InputEvent> observed_;
  fit::function<void(const std::vector<InputEvent>&)> on_terminate_;
};

class MinimalInputTest : public gtest::RealLoopFixture {
 protected:
  // Mildly complex ctor, but we don't throw and we don't call virtual methods.
  MinimalInputTest() {
    // This fixture constructor may run multiple times, but we want the context
    // to be set up just once per process.
    if (g_context == nullptr) {
      g_context = sys::ComponentContext::Create().release();
    }

    auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();

    // Connect to Scenic, create a View.
    scenic_ = g_context->svc()->Connect<fuchsia::ui::scenic::Scenic>();
    scenic_.set_error_handler([](zx_status_t status) {
      FXL_LOG(FATAL) << "Lost connection to Scenic: " << zx_status_get_string(status);
    });
    scenic::ViewContextTransitional view_context = {
        .session_and_listener_request =
            scenic::CreateScenicSessionPtrAndListenerRequest(scenic_.get()),
        .view_token = std::move(view_token),
        .incoming_services = nullptr,
        .outgoing_services = nullptr,
        .component_context = g_context,
    };
    view_ = std::make_unique<MinimalClientView>(std::move(view_context), dispatcher());

    // Connect to RootPresenter, create a ViewHolder.
    root_presenter_ = g_context->svc()->Connect<fuchsia::ui::policy::Presenter>();
    root_presenter_.set_error_handler([](zx_status_t status) {
      FXL_LOG(FATAL) << "Lost connection to RootPresenter: " << zx_status_get_string(status);
    });
    root_presenter_->PresentView(std::move(view_holder_token), nullptr);

    // When display is available, create content and drive input to touchscreen.
    scenic_->GetDisplayInfo([this](fuchsia::ui::gfx::DisplayInfo display_info) {
      display_width_ = display_info.width_in_px;
      display_height_ = display_info.height_in_px;

      FXL_CHECK(display_width_ > 0 && display_height_ > 0)
          << "Display size unsuitable for this test: (" << display_width_ << ", " << display_height_
          << ").";

      view_->CreateScene(display_width_, display_height_);
      view_->session()->Present(zx_clock_get_monotonic(),
                                [this](fuchsia::images::PresentationInfo info) {
                                  inject_input_();  // Display up, content ready. Send in input.
                                  test_was_run_ = true;  // Actually did work for this test.
                                });
    });

    // Post a "just in case" quit task, if the test hangs.
    async::PostDelayedTask(
        dispatcher(),
        [] { FXL_LOG(FATAL) << "\n\n>> Test did not complete in time, terminating. <<\n\n"; },
        kTimeout);
  }

  ~MinimalInputTest() override { FXL_CHECK(test_was_run_) << "Oops, didn't actually do anything."; }

  void InjectInput(std::vector<const char*> args) {
    // Start with process name, end with nullptr.
    args.insert(args.begin(), "input");
    args.push_back(nullptr);

    // Start the /bin/input process.
    zx_handle_t proc;
    zx_status_t status =
        fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, "/bin/input", args.data(), &proc);
    FXL_CHECK(status == ZX_OK) << "fdio_spawn: " << zx_status_get_string(status);

    // Wait for termination.
    status = zx_object_wait_one(proc, ZX_PROCESS_TERMINATED,
                                (zx::clock::get_monotonic() + kTimeout).get(), nullptr);
    FXL_CHECK(status == ZX_OK) << "zx_object_wait_one: " << zx_status_get_string(status);

    // Check termination status.
    zx_info_process_t info;
    status = zx_object_get_info(proc, ZX_INFO_PROCESS, &info, sizeof(info), nullptr, nullptr);
    FXL_CHECK(status == ZX_OK) << "zx_object_get_info: " << zx_status_get_string(status);
    FXL_CHECK(info.return_code == 0) << "info.return_code: " << info.return_code;
  }

  void SetInjectInputCallback(fit::function<void()> inject_input) {
    inject_input_ = std::move(inject_input);
  }

  fuchsia::ui::policy::PresenterPtr root_presenter_;
  fuchsia::ui::scenic::ScenicPtr scenic_;

  std::unique_ptr<MinimalClientView> view_;
  uint32_t display_width_ = 0;
  uint32_t display_height_ = 0;

  fit::function<void()> inject_input_;
  bool test_was_run_ = false;
};

TEST_F(MinimalInputTest, Tap) {
  // Set up inputs. Fires when display and content are available.
  SetInjectInputCallback([this] {
    InjectInput({"tap",  // Tap at the center of the display
                 std::to_string(display_width_ / 2).c_str(),
                 std::to_string(display_height_ / 2).c_str(), nullptr});
  });

  // Set up expectations. Fires when we see the "quit" message.
  view_->SetOnTerminateCallback([this](const std::vector<InputEvent>& observed) {
    if (FXL_VLOG_IS_ON(2)) {
      for (const auto& event : observed) {
        FXL_LOG(INFO) << "Input event observed: " << event;
      }
    }

    EXPECT_EQ(observed.size(), 5u);

    EXPECT_EQ(observed[0].pointer().phase, Phase::ADD);
    EXPECT_TRUE(observed[1].focus().focused);
    EXPECT_EQ(observed[2].pointer().phase, Phase::DOWN);
    EXPECT_EQ(observed[3].pointer().phase, Phase::UP);
    EXPECT_EQ(observed[4].pointer().phase, Phase::REMOVE);

    QuitLoop();
    // TODO(SCN-1449): Cleanly break the View/ViewHolder connection.
  });

  RunLoop();  // Go!
}

}  // namespace

// NOTE: We link in FXL's gtest_main to enable proper logging.
