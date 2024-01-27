// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_BIN_ROOT_PRESENTER_PRESENTATION_H_
#define SRC_UI_BIN_ROOT_PRESENTER_PRESENTATION_H_

#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <fuchsia/ui/input/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/ui/scenic/cpp/id.h>
#include <lib/ui/scenic/cpp/resources.h>

#include <map>

#include "src/ui/bin/root_presenter/constants.h"
#include "src/ui/bin/root_presenter/displays/display_metrics.h"
#include "src/ui/bin/root_presenter/displays/display_model.h"
#include "src/ui/bin/root_presenter/inspect.h"
#include "src/ui/bin/root_presenter/safe_presenter.h"
#include "src/ui/input/lib/injector/injector.h"
#include "src/ui/input/lib/injector/injector_config_setup.h"

namespace root_presenter {

// This class handles Scenic interactions, including:
// - Sets up the Scenic scene
// - Wires up input dispatch
// - Displays client Views by implementing fuchsia::ui::Presenter.
//
/// Scene topology ///
// [1] = owned by root presenter, [2] = owned by client, [3] owned by a11y manager
//
// After construction
// [1] scene_
//       |
// [1] root_view_holder_
//       |
// [1] root_view_
//       |
// [1] injector_view_holder_
//       |
// [1] injector_view_
//       |
// [1] proxy_view_holder_
//       |
// [1] proxy_view_
//       |
// [1] client_view_holder_
//       |
// [2] client view
//
// [1] scene_
//       |
// [1] root_view_holder_
//       |
// [1] root_view_
//       |
// [1] injector_view_holder_
//       |
// [1] injector_view_
//       |
// [1] proxy_view_holder_
//       |
// [1] proxy_view_
//       |
// [1] client_view_holder_
//       |
// [2] client view
//
class Presentation : fuchsia::ui::policy::Presenter, fuchsia::ui::policy::Presentation {
 public:
  Presentation(inspect::Node inspect_node, sys::ComponentContext* component_context,
               fuchsia::ui::scenic::Scenic* scenic, std::unique_ptr<scenic::Session> session,
               fuchsia::ui::views::FocuserPtr focuser, int32_t display_startup_rotation_adjustment);
  ~Presentation() override = default;

  // |fuchsia.ui.policy.Presenter|
  void PresentView(
      fuchsia::ui::views::ViewHolderToken view_holder_token,
      fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> presentation_request) override;

  // |fuchsia.ui.policy.Presenter|
  void PresentOrReplaceView(
      fuchsia::ui::views::ViewHolderToken view_holder_token,
      fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> presentation_request) override;

  // |fuchsia.ui.policy.Presenter|
  void PresentOrReplaceView2(
      fuchsia::ui::views::ViewHolderToken view_holder_token, fuchsia::ui::views::ViewRef view_ref,
      fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> presentation_request) override;

  // For tests. Returns true if the display has been initialized and the scene is ready down to the
  // proxy view. Does not look at the a11y or client view.
  bool is_initialized() const {
    return display_model_initialized_ && graph_state_.root_view_attached.value() &&
           graph_state_.injector_view_attached.value() && graph_state_.proxy_view_attached.value();
  }
  // For tests. Returns true if everything is ready for input injection.
  bool ready_for_injection() const { return injector_->scene_ready(); }

  struct GraphState {
    std::optional<bool> root_view_attached;
    std::optional<bool> injector_view_attached;
    std::optional<bool> a11y_view_attached;
    std::optional<bool> proxy_view_attached;
    std::optional<bool> client_view_attached;
  };

 private:
  // Creates and attaches a new View using the passed in tokens.
  // Any currently attached client is detached and its ViewHolder destroyed.
  void AttachClient(fuchsia::ui::views::ViewHolderToken view_holder_token,
                    std::optional<fuchsia::ui::views::ViewRef> view_ref,
                    fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> presentation_request);

  // |fuchsia::ui::policy::Presentation|
  void CapturePointerEventsHACK(
      fidl::InterfaceHandle<fuchsia::ui::policy::PointerCaptureListenerHACK> listener) override {
    FX_LOGS(ERROR) << "CapturePointerEventsHACK is obsolete.";
  }

  // Initializes all views to match the display dimensions.
  // Must call Present() for all Sessions afterwards to apply the updates.
  void InitializeDisplayModel(fuchsia::ui::gfx::DisplayInfo display_info);

  // Updates the injection Viewport to match the currently visible display (i.e. accounting for
  // ClipSpaceTransform).
  void UpdateViewport(const DisplayMetrics& display_metrics);

  // Sets properties for all view holders.
  // Must call Present() for all Sessions afterwards to apply the updates.
  void SetViewHolderProperties(const DisplayMetrics& display_metrics);

  // Passes the display rotation in degrees down to the scenic compositor.
  void SetScenicDisplayRotation();

  // A valid scene graph is any that has root, injector, proxy and client views attached.
  bool IsValidSceneGraph() const {
    return graph_state_.root_view_attached.value() && graph_state_.injector_view_attached.value() &&
           graph_state_.proxy_view_attached.value() && graph_state_.client_view_attached.value();
  }

  // Updates |graph_state_| and performs any appropriate actions depending on the new state.
  // Every value in |updated_state| except for the one being updated should be std::nullopt.
  void UpdateGraphState(GraphState updated_state);

  inspect::Node inspect_node_;

  std::unique_ptr<scenic::Session> root_session_;

  scenic::DisplayCompositor compositor_;
  scenic::LayerStack layer_stack_;
  scenic::Layer layer_;
  scenic::Renderer renderer_;

  // TODO(fxbug.dev/23500): put camera before scene.
  scenic::Scene scene_;
  scenic::Camera camera_;
  std::optional<scenic::View> root_view_;
  std::optional<scenic::ViewHolder> root_view_holder_;

  // The injector view is used as a constant target when injecting events through
  // fuchsia::ui::pointerinjector. It is where scale, rotation and translation for all child views
  // are set.
  scenic::Session injector_session_;
  std::optional<scenic::View> injector_view_;
  std::optional<scenic::ViewHolder> injector_view_holder_;

  scenic::Session proxy_session_;
  std::optional<scenic::View> proxy_view_;
  std::optional<scenic::ViewHolder> proxy_view_holder_;

  // ViewHolder connected to the client View; std::nullopt until AttachClient() is called.
  std::optional<scenic::ViewHolder> client_view_holder_;

  fit::function<void()> create_a11y_view_holder_callback_ = nullptr;

  // Tracks the current state of the scene graph. Each boolean denotes whether a view is connected
  // to its parent.
  GraphState graph_state_ = {.root_view_attached = false,
                             .injector_view_attached = false,
                             .a11y_view_attached = false,
                             .proxy_view_attached = false,
                             .client_view_attached = false};

  std::optional<input::InjectorConfigSetup> injector_config_setup_;
  std::optional<input::Injector> injector_;

  bool display_model_initialized_ = false;

  DisplayModel display_model_;
  DisplayMetrics display_metrics_;

  // At startup, apply a rotation defined in 90 degree increments, just once.
  // Implies resizing of the presentation to adjust to rotated coordinates.
  // Valid values are ... -180, -90, 0, 90, 180, ...
  //
  // Used when the native display orientation is reported incorrectly.
  // TODO(fxbug.dev/24074) - Make this less of a hack.
  const int32_t display_startup_rotation_adjustment_;

  // Current ClipSpaceTransform. Used to set up a matching input Viewport.
  float clip_scale_ = 1;
  float clip_offset_x_ = 0;
  float clip_offset_y_ = 0;

  fidl::BindingSet<fuchsia::ui::policy::Presenter> presenter_bindings_;
  fidl::Binding<fuchsia::ui::policy::Presentation> presentation_binding_;

  // One SafePresenter for each Session.
  SafePresenter safe_presenter_root_;
  SafePresenter safe_presenter_injector_;
  SafePresenter safe_presenter_proxy_;

  // Scenic focuser used to request focus chain updates.
  fuchsia::ui::views::FocuserPtr view_focuser_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Presentation);
};

}  // namespace root_presenter

#endif  // SRC_UI_BIN_ROOT_PRESENTER_PRESENTATION_H_
