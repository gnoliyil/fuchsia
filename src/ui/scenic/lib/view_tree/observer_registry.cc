// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "observer_registry.h"

#include <lib/syslog/cpp/log_level.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>

namespace view_tree {

Registry::Registry(view_tree::GeometryProvider& geometry_provider)
    : geometry_provider_(geometry_provider) {}

void Registry::RegisterGlobalViewTreeWatcher(
    fidl::InterfaceRequest<fuchsia::ui::observation::geometry::ViewTreeWatcher> request,
    Registry::RegisterGlobalViewTreeWatcherCallback callback) {
  geometry_provider_.RegisterGlobalViewTreeWatcher(std::move(request));

  callback();
}

void Registry::Publish(sys::ComponentContext* app_context) {
  app_context->outgoing()->AddPublicService<fuchsia::ui::observation::test::Registry>(
      bindings_.GetHandler(this));
}
}  // namespace view_tree
