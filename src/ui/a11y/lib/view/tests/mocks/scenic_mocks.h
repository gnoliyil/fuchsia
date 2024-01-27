// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_VIEW_TESTS_MOCKS_SCENIC_MOCKS_H_
#define SRC_UI_A11Y_LIB_VIEW_TESTS_MOCKS_SCENIC_MOCKS_H_

#include <fuchsia/ui/pointer/augment/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl_test_base.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/syslog/cpp/macros.h>

#include <set>
#include <unordered_map>
#include <vector>

#include "lib/fidl/cpp/binding.h"

namespace accessibility_test {

// TODO(76754): Consolidate with other scenic mocks.

struct ViewHolderAttributes {
  // Session-specific id of the view holder resource.
  uint32_t id;
  fuchsia::ui::views::ViewHolderToken view_holder_token;
  uint32_t parent_id;
  fuchsia::ui::gfx::ViewProperties properties;
  bool operator==(const ViewHolderAttributes& rhs) const {
    return this->id == rhs.id && this->parent_id == rhs.parent_id;
  }
};

struct ViewAttributes {
  // Session-specific id of the view resource.
  uint32_t id;
  fuchsia::ui::views::ViewRef view_ref;
  std::set<uint32_t> children;
  bool operator==(const ViewAttributes& rhs) const {
    return this->id == rhs.id && this->children == rhs.children;
  }
};

struct EntityNodeAttributes {
  // Session-specific id of the entity node resource.
  uint32_t id;
  uint32_t parent_id;
  std::array<float, 3> scale_vector;
  std::array<float, 3> translation_vector;
  std::set<uint32_t> children;
  bool operator==(const EntityNodeAttributes& rhs) const {
    return this->id == rhs.id && this->parent_id == rhs.parent_id &&
           this->scale_vector == rhs.scale_vector &&
           this->translation_vector == rhs.translation_vector && this->children == rhs.children;
  }
};

struct RectangleNodeAttributes {
  // Session-specific id of the rectangle holder node resource.
  uint32_t id;
  uint32_t parent_id;
  uint32_t rectangle_id;
  uint32_t material_id;
  bool operator==(const RectangleNodeAttributes& rhs) const {
    return this->id == rhs.id && this->parent_id == rhs.parent_id &&
           this->rectangle_id == rhs.rectangle_id && this->material_id == rhs.material_id;
  }
};

struct RectangleAttributes {
  // Session-specific id of the rectangle shape resource.
  uint32_t id;
  uint32_t parent_id;
  float width;
  float height;
  float elevation;
  float center_x;
  float center_y;
  bool operator==(const RectangleAttributes& rhs) const {
    return this->id == rhs.id && this->parent_id == rhs.parent_id && this->width == rhs.width &&
           this->height == rhs.height && this->elevation == rhs.elevation &&
           this->center_x == rhs.center_x && this->center_y == rhs.center_y;
  }
};

class MockSession : public fuchsia::ui::scenic::testing::Session_TestBase {
 public:
  static constexpr fuchsia::ui::gfx::ViewProperties kDefaultViewProperties = {
      .bounding_box = {.min = {.x = 10, .y = 5, .z = -100}, .max = {.x = 100, .y = 50, .z = 0}}};

  MockSession() : binding_(this) {}
  ~MockSession() override = default;

  void NotImplemented_(const std::string& name) override {}

  void Enqueue(std::vector<fuchsia::ui::scenic::Command> cmds) override;

  void Bind(fidl::InterfaceRequest<::fuchsia::ui::scenic::Session> request,
            ::fuchsia::ui::scenic::SessionListenerPtr listener);

  void ApplyCreateResourceCommand(const fuchsia::ui::gfx::CreateResourceCmd& command);

  void ApplyAddChildCommand(const fuchsia::ui::gfx::AddChildCmd& command);

  void ApplySetMaterialCommand(const fuchsia::ui::gfx::SetMaterialCmd& command);

  void ApplySetShapeCommand(const fuchsia::ui::gfx::SetShapeCmd& command);

  void ApplySetTranslationCommand(const fuchsia::ui::gfx::SetTranslationCmd& command);

  void ApplySetScaleCommand(const fuchsia::ui::gfx::SetScaleCmd& command);

  void ApplyDetachCommand(const fuchsia::ui::gfx::DetachCmd& command);

  void ApplySetViewPropertiesCommand(const fuchsia::ui::gfx::SetViewPropertiesCmd& command);

  void Present(uint64_t presentation_time, ::std::vector<::zx::event> acquire_fences,
               ::std::vector<::zx::event> release_fences, PresentCallback callback) override;

  void SendGfxEvent(fuchsia::ui::gfx::Event event);

  void SendViewPropertiesChangedEvent(uint32_t view_id,
                                      fuchsia::ui::gfx::ViewProperties properties);

  void SendViewDetachedFromSceneEvent(uint32_t view_id);

  void SendViewAttachedToSceneEvent(uint32_t view_id);

  void SendViewConnectedEvent(uint32_t view_holder_id);

  void SendViewHolderDisconnectedEvent(uint32_t view_id);

  const std::unordered_map<uint32_t, ViewHolderAttributes>& view_holders() { return view_holders_; }

  const std::set<uint32_t>& materials() { return materials_; }

  const std::unordered_map<uint32_t, ViewAttributes>& views() { return views_; }

  const std::unordered_map<uint32_t, EntityNodeAttributes>& entity_nodes() { return entity_nodes_; }

  const std::unordered_map<uint32_t, RectangleNodeAttributes>& rectangle_nodes() {
    return rectangle_nodes_;
  }

  const std::unordered_map<uint32_t, RectangleAttributes>& rectangles() { return rectangles_; }

  void Reset();

 private:
  fidl::Binding<fuchsia::ui::scenic::Session> binding_;
  fuchsia::ui::scenic::SessionListenerPtr listener_;
  std::vector<fuchsia::ui::scenic::Command> cmd_queue_;

  std::set<uint32_t> materials_;
  std::unordered_map<uint32_t, ViewHolderAttributes> view_holders_;
  std::unordered_map<uint32_t, ViewAttributes> views_;
  std::unordered_map<uint32_t, EntityNodeAttributes> entity_nodes_;
  std::unordered_map<uint32_t, RectangleNodeAttributes> rectangle_nodes_;
  std::unordered_map<uint32_t, RectangleAttributes> rectangles_;
};

class MockScenic : public fuchsia::ui::scenic::testing::Scenic_TestBase,
                   public fuchsia::ui::pointer::TouchSource {
 public:
  // TODO(fxb.dev/85349): Instantiate mock_session_ in CreateSession() instead
  // of taking a constructor argument, and offer a method to retrieve the
  // MockSession*.
  explicit MockScenic(std::unique_ptr<MockSession> mock_session)
      : mock_session_(std::move(mock_session)), touch_source_binding_(this) {}
  ~MockScenic() override = default;

  void NotImplemented_(const std::string& name) override {
    FX_LOGS(ERROR) << "NotImplemented_" << name;
  }

  // |fuchsia::ui::scenic::Scenic|
  void CreateSession(fidl::InterfaceRequest<fuchsia::ui::scenic::Session> session,
                     fidl::InterfaceHandle<fuchsia::ui::scenic::SessionListener> listener) override;

  // |fuchsia::ui::scenic::Scenic|
  void CreateSessionT(fuchsia::ui::scenic::SessionEndpoints endpoints,
                      CreateSessionTCallback callback) override;

  fidl::InterfaceRequestHandler<fuchsia::ui::scenic::Scenic> GetHandler(
      async_dispatcher_t* dispatcher = nullptr);

  bool create_session_called() { return create_session_called_; }

 private:
  void Watch(std::vector<::fuchsia::ui::pointer::TouchResponse> responses,
             WatchCallback callback) override {}
  void UpdateResponse(fuchsia::ui::pointer::TouchInteractionId interaction,
                      fuchsia::ui::pointer::TouchResponse response,
                      UpdateResponseCallback callback) override {}

  fidl::BindingSet<fuchsia::ui::scenic::Scenic> bindings_;
  std::unique_ptr<MockSession> mock_session_;
  fidl::Binding<fuchsia::ui::pointer::TouchSource> touch_source_binding_;
  bool create_session_called_;
};

class MockLocalHit : public fuchsia::ui::pointer::augment::LocalHit,
                     public fuchsia::ui::pointer::augment::TouchSourceWithLocalHit {
 public:
  MockLocalHit();
  ~MockLocalHit() = default;

  // |fuchsia::ui::pointer::augment::TouchSourceWithLocalHit|
  void Watch(std::vector<fuchsia::ui::pointer::TouchResponse> responses,
             WatchCallback callback) override;

  // |fuchsia::ui::pointer::augment::TouchSourceWithLocalHit|
  void UpdateResponse(fuchsia::ui::pointer::TouchInteractionId interaction,
                      fuchsia::ui::pointer::TouchResponse response,
                      UpdateResponseCallback callback) override;

  uint32_t NumWatchCalls() const;

  void SimulateEvents(std::vector<fuchsia::ui::pointer::augment::TouchEventWithLocalHit> events);

  std::vector<fuchsia::ui::pointer::TouchResponse> TakeResponses();

  std::vector<
      std::pair<fuchsia::ui::pointer::TouchInteractionId, fuchsia::ui::pointer::TouchResponse>>
  TakeUpdatedResponses();

  fidl::InterfaceRequestHandler<fuchsia::ui::pointer::augment::LocalHit> GetHandler(
      async_dispatcher_t* dispatcher = nullptr) {
    return [this,
            dispatcher](fidl::InterfaceRequest<fuchsia::ui::pointer::augment::LocalHit> request) {
      bindings_.AddBinding(this, std::move(request), dispatcher);
    };
  }

  // |fuchsia::ui::pointer::augment::LocalHit|
  void Upgrade(fidl::InterfaceHandle<fuchsia::ui::pointer::TouchSource> original,
               fuchsia::ui::pointer::augment::LocalHit::UpgradeCallback callback) override {
    callback(touch_source_binding_.NewBinding(), nullptr);
  }

  // Returns a bound touch source to this object.
  fuchsia::ui::pointer::augment::TouchSourceWithLocalHitPtr NewTouchSource() {
    fuchsia::ui::pointer::augment::TouchSourceWithLocalHitPtr touch_source;
    touch_source.Bind(touch_source_binding_.NewBinding());
    return touch_source;
  }

  void EnqueueTapToEvents();

  void SimulateEnqueuedEvents();

  // Configures the view ref koid that will be used to create fake touch events. This corresponds to
  // the view that would be hit by that event.
  void SetViewRefKoidForTouchEvents(uint64_t view_ref_koid);

 private:
  fidl::BindingSet<fuchsia::ui::pointer::augment::LocalHit> bindings_;
  fidl::Binding<fuchsia::ui::pointer::augment::TouchSourceWithLocalHit> touch_source_binding_;
  uint32_t num_watch_calls_ = 0;
  std::vector<fuchsia::ui::pointer::TouchResponse> responses_;
  std::vector<
      std::pair<fuchsia::ui::pointer::TouchInteractionId, fuchsia::ui::pointer::TouchResponse>>
      updated_responses_;
  WatchCallback callback_;
  std::vector<fuchsia::ui::pointer::augment::TouchEventWithLocalHit> enqueued_events_;
  bool view_parameters_sent_ = false;
  uint64_t view_ref_koid_for_hit_ = 0;
};

}  // namespace accessibility_test

#endif  // SRC_UI_A11Y_LIB_VIEW_TESTS_MOCKS_SCENIC_MOCKS_H_
