// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <fuchsia/ui/input/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/test/input/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/syslog/global.h>
#include <lib/ui/scenic/cpp/resources.h>
#include <lib/ui/scenic/cpp/session.h>
#include <lib/zx/eventpair.h>
#include <zircon/status.h>

#include <array>
#include <memory>

#include <src/lib/fostr/fidl/fuchsia/ui/gfx/formatting.h>
#include <src/lib/fostr/fidl/fuchsia/ui/input/formatting.h>

namespace touch_gfx_client {

namespace {

std::array<float, 2> ViewportToViewCoordinates(
    std::array<float, 2> viewport_coordinates,
    const std::array<float, 9>& viewport_to_view_transform) {
  // The transform matrix is a FIDL array with matrix data in column-major
  // order. For a matrix with data [a b c d e f g h i], and with the viewport
  // coordinates expressed as homogeneous coordinates, the logical view
  // coordinates are obtained with the following formula:
  //   |a d g|   |x|   |x'|
  //   |b e h| * |y| = |y'|
  //   |c f i|   |1|   |w'|
  // which we then normalize based on the w component:
  //   if w' not zero: (x'/w', y'/w')
  //   else (x', y')
  const auto& M = viewport_to_view_transform;
  const float x = viewport_coordinates[0];
  const float y = viewport_coordinates[1];
  const float xp = M[0] * x + M[3] * y + M[6];
  const float yp = M[1] * x + M[4] * y + M[7];
  const float wp = M[2] * x + M[5] * y + M[8];
  if (wp != 0) {
    return {xp / wp, yp / wp};
  } else {
    return {xp, yp};
  }
}

}  // namespace

using fuchsia::ui::pointer::EventPhase;
using fuchsia::ui::pointer::TouchEvent;
using fuchsia::ui::pointer::TouchResponse;
using fuchsia::ui::pointer::TouchResponseType;

// Implementation of a very simple Scenic client.
class TouchGfxClient : public fuchsia::ui::app::ViewProvider {
 public:
  TouchGfxClient(async::Loop* loop) : loop_(loop), view_provider_binding_(this) {
    FX_CHECK(loop_);

    context_ = sys::ComponentContext::CreateAndServeOutgoingDirectory();
    context_->outgoing()->AddPublicService<fuchsia::ui::app::ViewProvider>(
        [this](fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider> request) {
          view_provider_binding_.Bind(std::move(request));
        });

    touch_input_listener_ =
        context_->svc()->Connect<fuchsia::ui::test::input::TouchInputListener>();
    touch_input_listener_.set_error_handler([](zx_status_t status) {
      FX_LOGS(WARNING) << "JFYI. Test response listener disconnected, status: "
                       << zx_status_get_string(status);
      // Don't quit, because we should be able to run this client outside of a test.
    });

    auto scenic = context_->svc()->Connect<fuchsia::ui::scenic::Scenic>();
    scenic.set_error_handler([this](zx_status_t status) {
      FX_LOGS(ERROR) << "Quitting. Scenic disconnected, status: " << zx_status_get_string(status);
      loop_->Quit();
    });

    {
      fuchsia::ui::scenic::SessionEndpoints endpoints;
      endpoints.set_touch_source(touch_source_.NewRequest());
      session_ = std::make_unique<scenic::Session>(scenic.get(), std::move(endpoints));
    }
    session_->set_error_handler([this](zx_status_t status) {
      FX_LOGS(ERROR) << "Quitting. Scenic session disconnected, status: "
                     << zx_status_get_string(status);
      loop_->Quit();
    });
    session_->set_event_handler(fit::bind_member(this, &TouchGfxClient::OnEvents));
    session_->set_on_frame_presented_handler([](auto frame_presented_info) {});

    root_node_ = std::make_unique<scenic::EntityNode>(session_.get());
    root_node_->SetEventMask(fuchsia::ui::gfx::kMetricsEventMask);

    session_->Present2(/*when*/ zx_clock_get_monotonic(), /*span*/ 0, [](auto) {});
    touch_source_->Watch({}, [this](auto events) { OnTouchEvents(std::move(events)); });
  }

 private:
  static constexpr std::array<std::array<uint8_t, 4>, 6> kColorsRgba = {
      {{255, 0, 0, 255},      // red
       {255, 128, 0, 255},    // orange
       {255, 255, 0, 255},    // yellow
       {0, 255, 0, 255},      // green
       {0, 0, 255, 255},      // blue
       {128, 0, 255, 255}}};  // purple

  // |fuchsia::ui::app::ViewProvider|
  void CreateViewWithViewRef(zx::eventpair token,
                             fuchsia::ui::views::ViewRefControl view_ref_control,
                             fuchsia::ui::views::ViewRef view_ref) override {
    FX_LOGS(INFO) << "CreateViewWithViewRef called.";

    view_ = std::make_unique<scenic::View>(
        session_.get(), fuchsia::ui::views::ViewToken{.value = std::move(token)},
        std::move(view_ref_control), std::move(view_ref), "cpp-gfx-client view");
    view_->AddChild(*root_node_);

    session_->Present2(/*when*/ zx_clock_get_monotonic(), /*span*/ 0, [](auto) {});
  }

  // Scenic Session event handler, passed to |fuchsia::ui::scenic::SessionListener|.
  void OnEvents(std::vector<fuchsia::ui::scenic::Event> events) {
    for (const auto& event : events) {
      if (event.is_gfx()) {
        switch (event.gfx().Which()) {
          case fuchsia::ui::gfx::Event::Tag::kMetrics: {
            FX_LOGS(INFO) << "Metrics received.";
            metrics_ = event.gfx().metrics().metrics;
            break;
          }
          case fuchsia::ui::gfx::Event::Tag::kViewPropertiesChanged: {
            FX_LOGS(INFO) << "View properties received.";
            view_properties_ = event.gfx().view_properties_changed().properties;
            break;
          }
          default:
            break;  // nop
        }
      }
    }

    if (!scene_created_ && ViewSize().x > 0 && ViewSize().y > 0) {
      CreateScene();
      scene_created_ = true;
    }
  }

  // Listens for touch events and responds to them. Should be initially called in the callback to
  // TouchSource::Watch().
  void OnTouchEvents(std::vector<TouchEvent> events) {
    for (auto& event : events) {
      if (event.has_view_parameters()) {
        view_params_ = std::move(event.view_parameters());
      }
      if (event.has_pointer_sample()) {
        const auto& pointer = event.pointer_sample();
        const auto& phase = pointer.phase();
        if (phase == EventPhase::ADD && material_) {
          color_index_ = (color_index_ + 1) % kColorsRgba.size();
          std::array<uint8_t, 4> color = kColorsRgba[color_index_];
          material_->SetColor(color[0], color[1], color[2], color[3]);
          session_->Present2(/*when*/ zx_clock_get_monotonic(), /*span*/ 0, [](auto) {});
        }

        if (touch_input_listener_) {
          // Only report ADD and CHANGE events for minimality; add more if necessary.
          if (phase == EventPhase::ADD || phase == EventPhase::CHANGE) {
            // The raw pointer event's coordinates are in pips (logical pixels). The test
            // expects coordinates in physical pixels. The former is transformed into the latter
            // with the scale factor provided in the metrics event.
            const auto logical = ViewportToViewCoordinates(
                pointer.position_in_viewport(), view_params_->viewport_to_view_transform);
            fuchsia::ui::test::input::TouchInputListenerReportTouchInputRequest request;
            request.set_local_x(logical[0] * metrics_.scale_x)
                .set_local_y(logical[1] * metrics_.scale_y)
                .set_time_received(zx_clock_get_monotonic())
                .set_component_name("touch-gfx-client");
            touch_input_listener_->ReportTouchInput(std::move(request));
          }
        }
      }
    }

    // Create responses.
    std::vector<TouchResponse> responses;
    for (auto& event : events) {
      if (event.has_pointer_sample()) {
        TouchResponse response;
        response.set_response_type(TouchResponseType::YES);
        responses.emplace_back(std::move(response));
      } else {
        // Add empty response for non-pointer event.
        responses.emplace_back();
      }
    }

    touch_source_->Watch(std::move(responses),
                         [this](auto events) { OnTouchEvents(std::move(events)); });
  }

  // Calculates view size based on view properties and metrics event.
  fuchsia::ui::gfx::vec2 ViewSize() const {
    const fuchsia::ui::gfx::ViewProperties& p = view_properties_;
    float size_x = ((p.bounding_box.max.x - p.inset_from_max.x) -
                    (p.bounding_box.min.x + p.inset_from_min.x)) *
                   metrics_.scale_x;
    float size_y = ((p.bounding_box.max.y - p.inset_from_max.y) -
                    (p.bounding_box.min.y + p.inset_from_min.y)) *
                   metrics_.scale_y;
    return fuchsia::ui::gfx::vec2{.x = size_x, .y = size_y};
  }

  // Encapsulates scene setup.
  void CreateScene() {
    FX_CHECK(session_) << "precondition";
    FX_CHECK(root_node_) << "precondition";

    scenic::Session* session = session_.get();

    scenic::ShapeNode shape(session);
    scenic::Rectangle rec(session, ViewSize().x, ViewSize().y);
    shape.SetShape(rec);
    shape.SetTranslation(ViewSize().x / 2, ViewSize().y / 2, 0.f);
    material_ = std::make_unique<scenic::Material>(session);
    std::array<uint8_t, 4> color = kColorsRgba[color_index_];
    material_->SetColor(color[0], color[1], color[2], color[3]);
    shape.SetMaterial(*material_);
    root_node_->AddChild(shape);

    session_->Present2(/*when*/ zx_clock_get_monotonic(), /*span*/ 0, [](auto) {});
  }

  std::optional<fuchsia::ui::pointer::ViewParameters> view_params_;

  // The main thread's message loop.
  async::Loop* loop_ = nullptr;

  // This component's global context.
  std::unique_ptr<sys::ComponentContext> context_;

  // Protocols used by this component.
  fuchsia::ui::test::input::TouchInputListenerPtr touch_input_listener_;

  fuchsia::ui::pointer::TouchSourcePtr touch_source_;

  // Protocols vended by this component.
  fidl::Binding<fuchsia::ui::app::ViewProvider> view_provider_binding_;

  // Scene state.
  std::unique_ptr<scenic::Session> session_;
  std::unique_ptr<scenic::View> view_;
  std::unique_ptr<scenic::EntityNode> root_node_;
  std::unique_ptr<scenic::Material> material_;
  fuchsia::ui::gfx::ViewProperties view_properties_;
  fuchsia::ui::gfx::Metrics metrics_;
  bool scene_created_ = false;
  uint32_t color_index_ = 0;
};
}  // namespace touch_gfx_client

// Component entry point.
int main(int argc, char** argv) {
  FX_LOGS(INFO) << "Starting cpp-gfx-client.";

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  touch_gfx_client::TouchGfxClient client(&loop);

  return loop.Run();
}
