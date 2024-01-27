// Copyright 2015 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/bin/root_presenter/presentation.h"

#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>
#include <zircon/status.h>

#include <cmath>

#include "src/ui/bin/root_presenter/displays/display_configuration.h"
#include "src/ui/bin/root_presenter/inspect.h"
#include "src/ui/bin/root_presenter/safe_presenter.h"

#include <glm/glm.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra-semi"
#include <glm/ext.hpp>
#pragma GCC diagnostic pop

namespace root_presenter {
namespace {

// TODO(fxbug.dev/24474): Don't hardcode Z bounds in multiple locations.
constexpr float kDefaultRootViewDepth = 1000;

fuchsia::ui::gfx::ViewProperties ViewProperties(float min_x, float min_y, float min_z, float max_x,
                                                float max_y, float max_z, float in_min_x,
                                                float in_min_y, float in_min_z, float in_max_x,
                                                float in_max_y, float in_max_z, bool is_focusable) {
  fuchsia::ui::gfx::ViewProperties props;
  props.bounding_box.min = scenic::NewVector3({min_x, min_y, min_z});
  props.bounding_box.max = scenic::NewVector3({max_x, max_y, max_z});
  props.inset_from_min = scenic::NewVector3({in_min_x, in_min_y, in_min_z});
  props.inset_from_max = scenic::NewVector3({in_max_x, in_max_y, in_max_z});
  props.focus_change = is_focusable;

  return props;
}

}  // namespace

Presentation::Presentation(inspect::Node inspect_node, sys::ComponentContext* component_context,
                           fuchsia::ui::scenic::Scenic* scenic,
                           std::unique_ptr<scenic::Session> session,
                           fuchsia::ui::views::FocuserPtr focuser,
                           int32_t display_startup_rotation_adjustment)
    : inspect_node_(std::move(inspect_node)),
      root_session_(std::move(session)),
      compositor_(root_session_.get()),
      layer_stack_(root_session_.get()),
      layer_(root_session_.get()),
      renderer_(root_session_.get()),
      scene_(root_session_.get()),
      camera_(scene_),
      injector_session_(scenic),
      proxy_session_(scenic),
      display_startup_rotation_adjustment_(display_startup_rotation_adjustment),
      presentation_binding_(this),
      safe_presenter_root_(root_session_.get()),
      safe_presenter_injector_(&injector_session_),
      safe_presenter_proxy_(&proxy_session_),
      view_focuser_(std::move(focuser)) {
  FX_DCHECK(component_context);
  component_context->outgoing()->AddPublicService(presenter_bindings_.GetHandler(this));
  compositor_.SetLayerStack(layer_stack_);
  layer_stack_.AddLayer(layer_);
  renderer_.SetCamera(camera_);
  layer_.SetRenderer(renderer_);

  // Create the root view's scene.
  // TODO(fxbug.dev/24456): we add a directional light and a point light, expecting
  // only one of them to be active at a time.  This logic is implicit in
  // EngineRenderer, since no shadow-mode supports both directional and point
  // lights (either one or the other).  When directional light support is added
  // to PaperRenderer, the code here will result in over-brightening, and will
  // need to be adjusted at that time.
  scenic::AmbientLight ambient_light(root_session_.get());
  scenic::DirectionalLight directional_light(root_session_.get());
  scenic::PointLight point_light(root_session_.get());
  scene_.AddLight(ambient_light);
  scene_.AddLight(directional_light);
  scene_.AddLight(point_light);
  directional_light.SetDirection(1.f, 1.f, 2.f);
  point_light.SetPosition(300.f, 300.f, -2000.f);
  point_light.SetFalloff(0.f);

  // Explicitly set "UNSHADOWED" as the default shadow type. In addition to
  // setting the param, this sets appropriate light intensities.
  {
    // When no shadows, ambient light needs to be full brightness.  Otherwise,
    // ambient needs to be dimmed so that other lights don't "overbrighten".
    ambient_light.SetColor(1.f, 1.f, 1.f);
    directional_light.SetColor(0.f, 0.f, 0.f);
    point_light.SetColor(0.f, 0.f, 0.f);
    fuchsia::ui::gfx::RendererParam param;
    param.set_shadow_technique(fuchsia::ui::gfx::ShadowTechnique::UNSHADOWED);
    renderer_.SetParam(std::move(param));
  }

  SetScenicDisplayRotation();
  {
    fuchsia::ui::views::ViewRef root_view_ref, injector_view_ref;
    {    // Set up views and view holders.
      {  // Set up the root view.
        auto [internal_view_token, internal_view_holder_token] = scenic::ViewTokenPair::New();
        auto [control_ref, view_ref] = scenic::ViewRefPair::New();
        fidl::Clone(view_ref, &root_view_ref);
        root_view_holder_.emplace(root_session_.get(), std::move(internal_view_holder_token),
                                  "Root View Holder");
        root_view_.emplace(root_session_.get(), std::move(internal_view_token),
                           std::move(control_ref), std::move(view_ref), "Root View");
      }
      {  // Set up the injector view.
        auto [internal_view_token, internal_view_holder_token] = scenic::ViewTokenPair::New();
        auto [control_ref, view_ref] = scenic::ViewRefPair::New();
        fidl::Clone(view_ref, &injector_view_ref);
        injector_view_holder_.emplace(root_session_.get(), std::move(internal_view_holder_token),
                                      "Injector View Holder");
        injector_view_.emplace(&injector_session_, std::move(internal_view_token),
                               std::move(control_ref), std::move(view_ref), "Injector View");
      }
      {  // Set up the "proxy view"
        auto [internal_view_token, internal_view_holder_token] = scenic::ViewTokenPair::New();
        auto [control_ref, view_ref] = scenic::ViewRefPair::New();
        proxy_view_holder_.emplace(&injector_session_, std::move(internal_view_holder_token),
                                   "Proxy View Holder");
        proxy_view_.emplace(&proxy_session_, std::move(internal_view_token), std::move(control_ref),
                            std::move(view_ref), "Proxy View");
      }

      // Connect it all up.
      scene_.AddChild(root_view_holder_.value());
      root_view_->AddChild(injector_view_holder_.value());
      injector_view_->AddChild(proxy_view_holder_.value());

      safe_presenter_root_.QueuePresent([this] { UpdateGraphState({.root_view_attached = true}); });
      safe_presenter_injector_.QueuePresent(
          [this] { UpdateGraphState({.injector_view_attached = true}); });
      safe_presenter_proxy_.QueuePresent([] {});
    }

    injector_.emplace(component_context,
                      /*context=*/fidl::Clone(root_view_ref),
                      /*target=*/fidl::Clone(injector_view_ref),
                      fuchsia::ui::pointerinjector::DispatchPolicy::TOP_HIT_AND_ANCESTORS_IN_TARGET,
                      inspect_node_.CreateChild("Injector"));

    // Sets up InjectorConfigSetup for input pipeline to receive view refs and viewport updates.
    injector_config_setup_.emplace(component_context, /*context*/ std::move(root_view_ref),
                                   /*target*/ std::move(injector_view_ref));
  }

  scenic->GetDisplayInfo([this](fuchsia::ui::gfx::DisplayInfo display_info) mutable {
    InitializeDisplayModel(std::move(display_info));
    safe_presenter_root_.QueuePresent([] {});
    safe_presenter_injector_.QueuePresent([] {});
    safe_presenter_proxy_.QueuePresent([] {});
  });

  proxy_session_.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Proxy session closed unexpectedly with status: "
                   << zx_status_get_string(status);
  });
  injector_session_.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Injector session closed unexpectedly with status: "
                   << zx_status_get_string(status);
  });

  proxy_session_.set_event_handler([this](std::vector<fuchsia::ui::scenic::Event> events) {
    for (const auto& event : events) {
      if (event.Which() != fuchsia::ui::scenic::Event::Tag::kGfx)
        continue;

      const auto& gfx_event = event.gfx();
      if (gfx_event.Which() == fuchsia::ui::gfx::Event::Tag::kViewConnected) {
        UpdateGraphState({.client_view_attached = true});
      } else if (gfx_event.Which() == fuchsia::ui::gfx::Event::Tag::kViewDisconnected) {
        FX_LOGS(WARNING) << "Client View disconnected. Closing channel.";
        proxy_view_->DetachChild(client_view_holder_.value());
        client_view_holder_.reset();
        safe_presenter_proxy_.QueuePresent([] {});
        UpdateGraphState({.client_view_attached = false});
        presentation_binding_.Unbind();
      } else if (gfx_event.Which() == fuchsia::ui::gfx::Event::Tag::kViewAttachedToScene) {
        UpdateGraphState({.proxy_view_attached = true});
      } else if (gfx_event.Which() == fuchsia::ui::gfx::Event::Tag::kViewDetachedFromScene) {
        UpdateGraphState({.proxy_view_attached = false});
      }
    }
  });

  {
    // TODO(fxbug.dev/68206) Remove this and enable client-side FIDL errors.
    fidl::internal::TransitoryProxyControllerClientSideErrorDisabler client_side_error_disabler_;
  }

  FX_DCHECK(root_view_holder_);
  FX_DCHECK(root_view_);
  FX_DCHECK(injector_view_holder_);
  FX_DCHECK(injector_view_);
  FX_DCHECK(proxy_view_holder_);
  FX_DCHECK(proxy_view_);
  FX_DCHECK(injector_);
}

void Presentation::UpdateGraphState(GraphState updated_state) {
  // Replace anything that isn't std::nullopt.
  // No (easy) way to iterate over a struct or a tuple, so we're left with brute force updating.
  graph_state_.root_view_attached =
      updated_state.root_view_attached.value_or(graph_state_.root_view_attached.value());
  graph_state_.injector_view_attached =
      updated_state.injector_view_attached.value_or(graph_state_.injector_view_attached.value());
  graph_state_.proxy_view_attached =
      updated_state.proxy_view_attached.value_or(graph_state_.proxy_view_attached.value());
  graph_state_.client_view_attached =
      updated_state.client_view_attached.value_or(graph_state_.client_view_attached.value());

  if (IsValidSceneGraph()) {
    injector_->MarkSceneReady();
  }
}

void Presentation::InitializeDisplayModel(fuchsia::ui::gfx::DisplayInfo display_info) {
  FX_DCHECK(!display_model_initialized_);
  display_model_initialized_ = true;

  // Initialize display model.
  display_configuration::InitializeModelForDisplay(display_info.width_in_px,
                                                   display_info.height_in_px, &display_model_);

  display_metrics_ = display_model_.GetMetrics();
  display_configuration::LogDisplayMetrics(display_metrics_);

  // Today, a layer needs the display's physical dimensions to render correctly.
  layer_.SetSize(static_cast<float>(display_metrics_.width_in_px()),
                 static_cast<float>(display_metrics_.height_in_px()));

  SetViewHolderProperties(display_metrics_);
  UpdateViewport(display_metrics_);
}

void Presentation::SetViewHolderProperties(const DisplayMetrics& display_metrics) {
  const bool is_90_degree_rotation = abs(display_startup_rotation_adjustment_ % 180) == 90;

  // Layout size
  {
    // Set the root view to native resolution and orientation (i.e. no rotation) of the display.
    // This lets us delegate touch coordinate transformations to Scenic.
    const float raw_metrics_width = static_cast<float>(display_metrics.width_in_px());
    const float raw_metrics_height = static_cast<float>(display_metrics.height_in_px());
    FX_DCHECK(root_view_holder_);
    root_view_holder_->SetViewProperties(ViewProperties(0.f, 0.f, -kDefaultRootViewDepth,
                                                        raw_metrics_width, raw_metrics_height, 0.f,
                                                        0.f, 0.f, 0.f, 0.f, 0.f, 0.f, false));
  }

  {  // Set all other views' resolutions to pips.
    float metrics_width = static_cast<float>(display_metrics.width_in_pp());
    float metrics_height = static_cast<float>(display_metrics.height_in_pp());

    // Swap metrics on left/right tilt.
    if (is_90_degree_rotation) {
      std::swap(metrics_width, metrics_height);
    }

    // Injector, a11y, proxy, and client views should all have the same dimensions.
    FX_DCHECK(injector_view_holder_);
    injector_view_holder_->SetViewProperties(ViewProperties(0.f, 0.f, -kDefaultRootViewDepth,
                                                            metrics_width, metrics_height, 0.f, 0.f,
                                                            0.f, 0.f, 0.f, 0.f, 0.f, false));

    FX_DCHECK(proxy_view_holder_);
    proxy_view_holder_->SetViewProperties(ViewProperties(0.f, 0.f, -kDefaultRootViewDepth,
                                                         metrics_width, metrics_height, 0.f, 0.f,
                                                         0.f, 0.f, 0.f, 0.f, 0.f, false));

    if (client_view_holder_) {
      client_view_holder_->SetViewProperties(ViewProperties(0.f, 0.f, -kDefaultRootViewDepth,
                                                            metrics_width, metrics_height, 0.f, 0.f,
                                                            0.f, 0.f, 0.f, 0.f, 0.f, true));
    }

    FX_VLOGS(2) << "DisplayModel layout: " << metrics_width << ", " << metrics_height;
  }

  // Remaining transformations are only applied to the root view's child and automatically
  // propagated down to the client view through the scene graph. The injector view holder is always
  // the root's child.
  {  // Scale a11y view to full device size.
    float metrics_scale_x = display_metrics.x_scale_in_px_per_pp();
    float metrics_scale_y = display_metrics.y_scale_in_px_per_pp();
    // Swap metrics on left/right tilt.
    if (is_90_degree_rotation) {
      std::swap(metrics_scale_x, metrics_scale_y);
    }

    injector_view_holder_->SetScale(metrics_scale_x, metrics_scale_y, 1.f);
    FX_VLOGS(2) << "DisplayModel pixel scale: " << metrics_scale_x << ", " << metrics_scale_y;
  }

  {  // Rotate root's child view to match desired display orientation.
    const glm::quat display_rotation =
        glm::quat(glm::vec3(0, 0, glm::radians<float>(display_startup_rotation_adjustment_)));
    injector_view_holder_->SetRotation(display_rotation.x, display_rotation.y, display_rotation.z,
                                       display_rotation.w);
  }

  {  // Adjust a11y view position for rotation.
    const float metrics_w = display_metrics.width_in_px();
    const float metrics_h = display_metrics.height_in_px();

    float left_offset = 0;
    float top_offset = 0;
    uint32_t degrees_rotated = abs(display_startup_rotation_adjustment_ % 360);
    switch (degrees_rotated) {
      case 0:
        left_offset = 0;
        top_offset = 0;
        break;
      case 90:
        left_offset = metrics_w;
        top_offset = 0;
        break;
      case 180:
        left_offset = metrics_w;
        top_offset = metrics_h;
        break;
      case 270:
        left_offset = 0;
        top_offset = metrics_h;
        break;
      default:
        FX_LOGS(ERROR) << "Unsupported rotation";
        break;
    }
    injector_view_holder_->SetTranslation(left_offset, top_offset, 0.f);
    FX_VLOGS(2) << "DisplayModel translation: " << left_offset << ", " << top_offset;
  }
}

void Presentation::PresentView(
    fuchsia::ui::views::ViewHolderToken view_holder_token,
    fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> presentation_request) {
  if (presentation_binding_.is_bound()) {
    FX_LOGS(ERROR) << "Support for multiple simultaneous presentations has been removed. To "
                      "replace a view, use PresentOrReplaceView";
    // Reject the request.
    presentation_request.Close(ZX_ERR_ALREADY_BOUND);
    return;
  }

  AttachClient(std::move(view_holder_token), std::nullopt, std::move(presentation_request));
}

void Presentation::PresentOrReplaceView(
    fuchsia::ui::views::ViewHolderToken view_holder_token,
    fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> presentation_request) {
  AttachClient(std::move(view_holder_token), std::nullopt, std::move(presentation_request));
}

void Presentation::PresentOrReplaceView2(
    fuchsia::ui::views::ViewHolderToken view_holder_token, fuchsia::ui::views::ViewRef view_ref,
    fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> presentation_request) {
  AttachClient(std::move(view_holder_token), std::move(view_ref), std::move(presentation_request));
}

void Presentation::AttachClient(
    fuchsia::ui::views::ViewHolderToken view_holder_token,
    std::optional<fuchsia::ui::views::ViewRef> view_ref,
    fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> presentation_request) {
  if (client_view_holder_) {
    proxy_view_->DetachChild(client_view_holder_.value());
    UpdateGraphState({.client_view_attached = false});
  }

  client_view_holder_.emplace(&proxy_session_, std::move(view_holder_token), "Client View Holder");
  proxy_view_->AddChild(client_view_holder_.value());

  // This is necessary so that the geometry observer API will give the
  // correct `scale` value for this view. See fxbug.dev/106725.
  client_view_holder_->SetEventMask(fuchsia::ui::gfx::kMetricsEventMask);

  if (display_model_initialized_) {
    SetViewHolderProperties(display_metrics_);
  }

  if (view_ref) {
    fuchsia::ui::views::FocuserSetAutoFocusRequest request;
    request.set_view_ref(std::move(*view_ref));
    view_focuser_->SetAutoFocus(std::move(request), [](auto result) {
      if (result.is_err()) {
        FX_LOGS(WARNING) << "Failed to set auto-focus for client root view";
      }
    });
  }

  presentation_binding_.Bind(std::move(presentation_request));
  safe_presenter_proxy_.QueuePresent([] {});
}

void Presentation::UpdateViewport(const DisplayMetrics& display_metrics) {
  // Viewport should match the visible part of the display 1:1. To do this we need to match the
  // ClipSpaceTransform.
  //
  // Since the ClipSpaceTransform is defined in Vulkan NDC with scaling, and the Viewport is defined
  // in pixel coordinates, we need to be able to transform offsets to pixel coordinates. This is
  // done by multiplying by half the display length and inverting the scale.
  //
  // Because the ClipSpaceTransform is defined with its origin in the center, and the
  // Viewport with its origin in the top left corner, we need to add a center offset to compensate.
  // This turns out to be as simple as half the scaled display length minus half the ClipSpace
  // length, which equals scale - 1 in NDC.
  //
  // Finally, because the ClipSpaceTransform and the Viewport transform are defined in opposite
  // directions (camera to scene vs context to viewport), all the transforms should be inverted
  // for the Viewport transform. This means an inverted scale and negative clip offsets.
  //
  const float display_width = static_cast<float>(display_metrics.width_in_px());
  const float display_height = static_cast<float>(display_metrics.height_in_px());
  const float inverted_scale = 1.f / clip_scale_;
  const float ndc_to_pixel_x = inverted_scale * display_width * 0.5f;
  const float ndc_to_pixel_y = inverted_scale * display_height * 0.5f;
  const float center_offset_ndc = clip_scale_ - 1.f;

  injector_->SetViewport({
      .width = display_width,
      .height = display_height,
      .scale = inverted_scale,
      .x_offset = ndc_to_pixel_x * (center_offset_ndc - clip_offset_x_),
      .y_offset = ndc_to_pixel_y * (center_offset_ndc - clip_offset_y_),
  });
  injector_config_setup_->UpdateViewport(injector_->GetCurrentViewport());
}

void Presentation::SetScenicDisplayRotation() {
  fuchsia::ui::gfx::Command command;
  fuchsia::ui::gfx::SetDisplayRotationCmdHACK display_rotation_cmd;
  display_rotation_cmd.compositor_id = compositor_.id();
  display_rotation_cmd.rotation_degrees = display_startup_rotation_adjustment_;
  command.set_set_display_rotation(std::move(display_rotation_cmd));
  root_session_->Enqueue(std::move(command));
}

}  // namespace root_presenter
