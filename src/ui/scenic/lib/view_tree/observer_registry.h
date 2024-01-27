// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_VIEW_TREE_OBSERVER_REGISTRY_H_
#define SRC_UI_SCENIC_LIB_VIEW_TREE_OBSERVER_REGISTRY_H_

#include <fuchsia/ui/observation/test/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>

#include "src/ui/scenic/lib/view_tree/geometry_provider.h"

namespace view_tree {

// The Registry class allows a client to receive global view geometry updates, in conjunction with
// the |fuchsia::ui::observation::Geometry| protocol.
//
// This is a sensitive protocol, so it should only be used in tests.
class Registry : public fuchsia::ui::observation::test::Registry {
 public:
  // Sets up forwarding of geometry requests to the geometry provider manager.
  explicit Registry(view_tree::GeometryProvider& geometry_provider);

  // |fuchsia.ui.observation.test.Registry.RegisterGlobalViewTreeWatcher|.
  void RegisterGlobalViewTreeWatcher(
      fidl::InterfaceRequest<fuchsia::ui::observation::geometry::ViewTreeWatcher> request,
      Registry::RegisterGlobalViewTreeWatcherCallback callback) override;

  void Publish(sys::ComponentContext* app_context);

 private:
  fidl::BindingSet<fuchsia::ui::observation::test::Registry> bindings_;

  view_tree::GeometryProvider& geometry_provider_;
};

}  // namespace view_tree

#endif  // SRC_UI_SCENIC_LIB_VIEW_TREE_OBSERVER_REGISTRY_H_
