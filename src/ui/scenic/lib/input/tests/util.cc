// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/input/tests/util.h"

#include <lib/fidl/cpp/clone.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>

#include <unordered_set>

#include <hid/hid.h>
#include <src/lib/fostr/fidl/fuchsia/ui/input/formatting.h>

#include "src/lib/testing/loop_fixture/test_loop_fixture.h"
#include "src/ui/scenic/lib/scheduling/constant_frame_predictor.h"
#include "src/ui/scenic/lib/scheduling/default_frame_scheduler.h"
#include "src/ui/scenic/lib/scheduling/id.h"

namespace lib_ui_input_tests {

// Used to compare whether two values are nearly equal.
// 1000 times machine limits to account for scaling from [0,1] to viewing volume [0,1000].
constexpr float kEpsilon = std::numeric_limits<float>::epsilon() * 1000;

using InputCommand = fuchsia::ui::input::Command;
using ScenicEvent = fuchsia::ui::scenic::Event;
using fuchsia::ui::input::InputEvent;
using fuchsia::ui::input::PointerEvent;
using fuchsia::ui::input::PointerEventPhase;
using fuchsia::ui::input::PointerEventType;
using fuchsia::ui::input::SendPointerInputCmd;
using fuchsia::ui::scenic::SessionListener;
using scenic_impl::GlobalId;
using scenic_impl::ResourceId;
using scenic_impl::Scenic;
using scenic_impl::display::Display;
using scenic_impl::gfx::Engine;
using scenic_impl::gfx::GfxSystem;
using scenic_impl::input::InputSystem;
using scenic_impl::test::ScenicTest;
using scheduling::ConstantFramePredictor;
using scheduling::DefaultFrameScheduler;

SessionWrapper::SessionWrapper(Scenic* scenic) {
  fuchsia::ui::scenic::SessionPtr session_ptr;
  fidl::InterfaceHandle<SessionListener> listener_handle;
  fidl::InterfaceRequest<SessionListener> listener_request = listener_handle.NewRequest();
  scenic->CreateSession(session_ptr.NewRequest(), std::move(listener_handle));
  session_ = std::make_unique<scenic::Session>(std::move(session_ptr), std::move(listener_request));
  session_->set_event_handler(fit::bind_member(this, &SessionWrapper::OnEvent));
}

SessionWrapper::SessionWrapper(SessionWrapper&& original) {
  session_ = std::move(original.session_);
  session_->set_event_handler(fit::bind_member(this, &SessionWrapper::OnEvent));
  view_ref_ = std::move(original.view_ref_);
  view_ = std::move(original.view_);
  events_ = std::move(original.events_);
}

SessionWrapper::~SessionWrapper() {
  if (session_) {
    session_->Flush();  // Ensure Scenic receives all release commands.
  }
}

void SessionWrapper::OnEvent(std::vector<ScenicEvent> events) {
  for (ScenicEvent& event : events) {
    if (event.is_input()) {
      events_.push_back(std::move(event.input()));
    }
    // Ignore other event types for these tests.
  }
}

GfxResourceGraph::GfxResourceGraph(scenic::Session* session)
    : scene(session),
      camera(scene),
      renderer(session),
      layer(session),
      layer_stack(session),
      compositor(session) {
  renderer.SetCamera(camera);
  layer.SetRenderer(renderer);
  layer_stack.AddLayer(layer);
  compositor.SetLayerStack(layer_stack);
}

void InputSystemTest::RequestToPresent(scenic::Session* session) {
  session->Present(/*presentation time*/ 0, [](auto) {});
  RunLoopFor(zx::msec(20));  // Run until the next frame should have been scheduled.
}

std::pair<SessionWrapper, GfxResourceGraph> InputSystemTest::CreateScene() {
  SessionWrapper root_session(scenic());
  GfxResourceGraph root_resources(root_session.session());
  root_resources.layer.SetSize(static_cast<float>(test_display_width_px()),
                               static_cast<float>(test_display_height_px()));
  {
    auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
    scenic::ViewHolder view_holder(root_session.session(), std::move(view_holder_token),
                                   "View Holder");

    auto pair = scenic::ViewRefPair::New();
    fuchsia::ui::views::ViewRef clone;
    pair.view_ref.Clone(&clone);
    root_session.SetViewRef(std::move(clone));

    // Make view really big to avoid unnecessary collisions.
    view_holder.SetViewProperties({.bounding_box = {.max = {1000, 1000, 1000}}});
    root_session.SetView(std::make_unique<scenic::View>(
        root_session.session(), std::move(view_token), std::move(pair.control_ref),
        std::move(pair.view_ref), "root_view"));
    root_resources.scene.AddChild(view_holder);
  }
  return {std::move(root_session), std::move(root_resources)};
}

GfxResourceGraphWithTargetView::GfxResourceGraphWithTargetView(scenic_impl::Scenic* scenic,
                                                               const float display_width,
                                                               const float display_height)
    : root_session(scenic),
      scene(root_session.session()),
      camera(scene),
      renderer(root_session.session()),
      layer(root_session.session()),
      layer_stack(root_session.session()),
      compositor(root_session.session()),
      injection_target_session(scenic) {
  renderer.SetCamera(camera);
  layer.SetRenderer(renderer);
  layer.SetSize(display_width, display_height);
  layer_stack.AddLayer(layer);
  compositor.SetLayerStack(layer_stack);

  {  // Create root view.
    auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
    auto root_view_holder = scenic::ViewHolder(root_session.session(), std::move(view_holder_token),
                                               "root_view_holder");
    // Make view really big to avoid unnecessary collisions.
    root_view_holder.SetViewProperties({.bounding_box = {.max = {1000, 1000, 1000}}});
    scene.AddChild(root_view_holder);

    auto [control_ref, view_ref] = scenic::ViewRefPair::New();
    fuchsia::ui::views::ViewRef clone;
    view_ref.Clone(&clone);
    root_session.SetViewRef(std::move(clone));
    root_session.SetView(
        std::make_unique<scenic::View>(root_session.session(), std::move(view_token),
                                       std::move(control_ref), std::move(view_ref), "root_view"));
  }

  {  // Create injection target view.
    auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
    auto injector_target_view_holder = scenic::ViewHolder(
        root_session.session(), std::move(view_holder_token), "root_view_holder");
    // Make view really big to avoid unnecessary collisions.
    injector_target_view_holder.SetViewProperties({.bounding_box = {.max = {1000, 1000, 1000}}});
    root_session.view()->AddChild(injector_target_view_holder);

    auto [control_ref, view_ref] = scenic::ViewRefPair::New();
    fuchsia::ui::views::ViewRef clone;
    view_ref.Clone(&clone);
    injection_target_session.SetViewRef(std::move(clone));
    injection_target_session.SetView(std::make_unique<scenic::View>(
        injection_target_session.session(), std::move(view_token), std::move(control_ref),
        std::move(view_ref), "injector_target_view"));
  }
}

GfxResourceGraphWithTargetView InputSystemTest::CreateScene2() {
  auto graph = GfxResourceGraphWithTargetView(scenic(), static_cast<float>(test_display_width_px()),
                                              static_cast<float>(test_display_height_px()));
  RequestToPresent(graph.root_session.session());
  RequestToPresent(graph.injection_target_session.session());
  return graph;
}

void InputSystemTest::SetUpTestView(scenic::View* view) {
  scenic::Session* const session = view->session();

  scenic::ShapeNode shape(session);
  shape.SetTranslation(2.5f, 2.5f, 0);  // Center the shape within the View.
  view->AddChild(shape);

  scenic::Rectangle rec(session, 5, 5);  // Simple; no real GPU work.
  shape.SetShape(rec);

  scenic::Material material(session);
  shape.SetMaterial(material);

  RequestToPresent(session);
}

SessionWrapper InputSystemTest::CreateClient(const std::string& name,
                                             fuchsia::ui::views::ViewToken view_token) {
  SessionWrapper session_wrapper(scenic());
  auto pair = scenic::ViewRefPair::New();
  fuchsia::ui::views::ViewRef clone;
  pair.view_ref.Clone(&clone);
  session_wrapper.SetViewRef(std::move(clone));
  session_wrapper.SetView(
      std::make_unique<scenic::View>(session_wrapper.session(), std::move(view_token),
                                     std::move(pair.control_ref), std::move(pair.view_ref), name));
  SetUpTestView(session_wrapper.view());

  return session_wrapper;
}

void InputSystemTest::InitializeScenic(std::shared_ptr<Scenic> scenic) {
  display_ = std::make_unique<Display>(
      /*id*/ 0, test_display_width_px(), test_display_height_px());
  engine_ = std::make_shared<Engine>(escher::EscherWeakPtr());

  std::function<void(zx_koid_t)> request_focus = [this, use_auto_focus =
                                                            auto_focus_behavior()](zx_koid_t koid) {
    if (!use_auto_focus)
      return;

    const auto& focus_chain = focus_manager_.focus_chain();
    if (!focus_chain.empty()) {
      const zx_koid_t requestor = focus_chain[0];
      const zx_koid_t request = koid != ZX_KOID_INVALID ? koid : requestor;
      focus_manager_.RequestFocus(requestor, request);
    }
  };

  scenic->RegisterSystem<GfxSystem>(engine_.get(),
                                    /* sysmem */ nullptr,
                                    /* display_manager */ nullptr,
                                    /*image_pipe_updater*/ nullptr);

  // TODO(fxbug.dev/72919): There's a bunch of logic copied from app.cc here. This will be removed
  // when moving out the integration tests from this folder.
  input_system_ =
      std::make_unique<InputSystem>(context_.get(), *scenic_->inspect_node(), request_focus);

  {
    std::vector<view_tree::SubtreeSnapshotGenerator> subtrees;
    subtrees.emplace_back(
        [engine = engine_]() { return engine->scene_graph()->view_tree().Snapshot(); });
    std::vector<view_tree::ViewTreeSnapshotter::Subscriber> subscribers;
    subscribers.push_back(
        {.on_new_view_tree =
             [this](auto snapshot) { input_system_->OnNewViewTreeSnapshot(std::move(snapshot)); },
         .dispatcher = async_get_default_dispatcher()});
    subscribers.push_back(
        {.on_new_view_tree =
             [this](auto snapshot) { focus_manager_.OnNewViewTreeSnapshot(std::move(snapshot)); },
         .dispatcher = async_get_default_dispatcher()});

    view_tree_snapshotter_ = std::make_shared<view_tree::ViewTreeSnapshotter>(
        std::move(subtrees), std::move(subscribers));
  }

  frame_scheduler_->Initialize(
      std::make_shared<scheduling::VsyncTiming>(),
      /*update_sessions*/
      [scenic, this](auto& sessions_to_update, auto trace_id, auto fences_from_previous_presents) {
        view_tree_snapshotter_->UpdateSessions(sessions_to_update, trace_id);
        auto results = scenic->UpdateSessions(sessions_to_update, trace_id);
        engine_->SignalFencesWhenPreviousRendersAreDone(std::move(fences_from_previous_presents));
        return results;
      },
      /*on_cpu_work_done*/
      [scenic, this] {
        view_tree_snapshotter_->OnCpuWorkDone();
        scenic->OnCpuWorkDone();
      },
      /*on_frame_presented*/
      [scenic, this](auto latched_times, auto present_times) {
        view_tree_snapshotter_->OnFramePresented(latched_times, present_times);
        scenic->OnFramePresented(latched_times, present_times);
      },
      /*render_scheduled_frame*/
      [this](auto frame_number, auto presentation_time, auto callback) {
        engine_->RenderScheduledFrame(frame_number, presentation_time, std::move(callback));
      });
}

void InputSystemTest::Inject(float x, float y, fuchsia::ui::pointerinjector::EventPhase phase) {
  FX_CHECK(injector_);
  std::vector<fuchsia::ui::pointerinjector::Event> events;
  {
    fuchsia::ui::pointerinjector::Event event;
    event.set_timestamp(0);
    fuchsia::ui::pointerinjector::PointerSample pointer_sample;
    pointer_sample.set_pointer_id(1);
    pointer_sample.set_phase(phase);
    pointer_sample.set_position_in_viewport({x, y});
    fuchsia::ui::pointerinjector::Data data;
    data.set_pointer_sample(std::move(pointer_sample));
    event.set_data(std::move(data));
    events.emplace_back(std::move(event));
  }

  bool inject_callback_fired = false;
  injector_->Inject(std::move(events), [&inject_callback_fired] { inject_callback_fired = true; });
  RunLoopUntilIdle();
  ASSERT_TRUE(inject_callback_fired);
}

void InputSystemTest::RegisterInjector(fuchsia::ui::views::ViewRef context_view_ref,
                                       fuchsia::ui::views::ViewRef target_view_ref,
                                       fuchsia::ui::pointerinjector::DispatchPolicy dispatch_policy,
                                       fuchsia::ui::pointerinjector::DeviceType type,
                                       std::array<std::array<float, 2>, 2> extents,
                                       std::array<float, 9> viewport_matrix) {
  fuchsia::ui::pointerinjector::Config config;
  config.set_device_id(1);
  config.set_device_type(type);
  config.set_dispatch_policy(dispatch_policy);
  {
    fuchsia::ui::pointerinjector::Viewport viewport;
    viewport.set_extents(extents);
    viewport.set_viewport_to_context_transform(viewport_matrix);
    config.set_viewport(std::move(viewport));
  }
  {
    fuchsia::ui::pointerinjector::Context context;
    context.set_view(std::move(context_view_ref));
    config.set_context(std::move(context));
  }
  {
    fuchsia::ui::pointerinjector::Target target;
    target.set_view(std::move(target_view_ref));
    config.set_target(std::move(target));
  }

  bool error_callback_fired = false;
  injector_.set_error_handler([&error_callback_fired](zx_status_t) {
    FX_LOGS(ERROR) << "Channel closed.";
    error_callback_fired = true;
  });
  bool register_callback_fired = false;
  input_system()->RegisterPointerinjector(
      std::move(config), injector_.NewRequest(),
      [&register_callback_fired] { register_callback_fired = true; });
  RunLoopUntilIdle();
  ASSERT_TRUE(register_callback_fired);
  ASSERT_FALSE(error_callback_fired);
}

void InputSystemTest::TearDown() {
  // A clean teardown sequence is a little involved but possible.
  // 0. All resources are released (i.e. test scope closure, ~ResourceGraph).
  // 1. Sessions |Flush| their last resource-release cmds (i.e. test scope closure,
  //    ~SessionWrapper).
  // 2. Scenic runs the last resource-release cmds.
  RunLoopUntilIdle();
  // 3. Destroy Scenic before destroying the command buffer sequencer (CBS).
  //    This ensures no CBS listeners are active by the time CBS is destroyed.
  ScenicTest::TearDown();
  engine_.reset();
  display_.reset();
  injector_ = {};
}

bool PointerMatches(const PointerEvent& event, uint32_t pointer_id, PointerEventPhase phase,
                    float x, float y, fuchsia::ui::input::PointerEventType type, uint32_t buttons) {
  using fuchsia::ui::input::operator<<;

  bool result = true;
  if (event.type != type) {
    FX_LOGS(ERROR) << "  Actual type: " << event.type;
    FX_LOGS(ERROR) << "Expected type: " << type;
    result = false;
  }
  if (event.buttons != buttons) {
    FX_LOGS(ERROR) << "  Actual buttons: " << event.buttons;
    FX_LOGS(ERROR) << "Expected buttons: " << buttons;
    result = false;
  }
  if (event.pointer_id != pointer_id) {
    FX_LOGS(ERROR) << "  Actual id: " << event.pointer_id;
    FX_LOGS(ERROR) << "Expected id: " << pointer_id;
    result = false;
  }
  if (event.phase != phase) {
    FX_LOGS(ERROR) << "  Actual phase: " << event.phase;
    FX_LOGS(ERROR) << "Expected phase: " << phase;
    result = false;
  }
  if (fabs(event.x - x) > kEpsilon) {
    FX_LOGS(ERROR) << "  Actual x: " << event.x;
    FX_LOGS(ERROR) << "Expected x: " << x;
    result = false;
  }
  if (fabs(event.y - y) > kEpsilon) {
    FX_LOGS(ERROR) << "  Actual y: " << event.y;
    FX_LOGS(ERROR) << "Expected y: " << y;
    result = false;
  }
  return result;
}

}  // namespace lib_ui_input_tests
