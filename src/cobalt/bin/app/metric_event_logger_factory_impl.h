// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_COBALT_BIN_APP_METRIC_EVENT_LOGGER_FACTORY_IMPL_H_
#define SRC_COBALT_BIN_APP_METRIC_EVENT_LOGGER_FACTORY_IMPL_H_

#include <fuchsia/cobalt/cpp/fidl.h>
#include <stdlib.h>

#include "lib/fidl/cpp/binding_set.h"
#include "src/cobalt/bin/app/metric_event_logger_impl.h"
#include "third_party/cobalt/src/logger/project_context_factory.h"
#include "third_party/cobalt/src/public/cobalt_service_interface.h"

namespace cobalt {

class MetricEventLoggerFactoryImpl : public fuchsia::cobalt::MetricEventLoggerFactory {
 public:
  MetricEventLoggerFactoryImpl(
      std::shared_ptr<cobalt::logger::ProjectContextFactory> global_project_context_factory,
      CobaltServiceInterface* cobalt_service);

 private:
  void CreateMetricEventLogger(fuchsia::cobalt::ProjectSpec project_spec,
                               fidl::InterfaceRequest<fuchsia::cobalt::MetricEventLogger> request,
                               CreateMetricEventLoggerCallback callback);

  fidl::BindingSet<fuchsia::cobalt::MetricEventLogger,
                   std::unique_ptr<fuchsia::cobalt::MetricEventLogger>>
      logger_bindings_;

  std::shared_ptr<cobalt::logger::ProjectContextFactory> global_project_context_factory_;
  CobaltServiceInterface* cobalt_service_;  // not owned

  FXL_DISALLOW_COPY_AND_ASSIGN(MetricEventLoggerFactoryImpl);
};

}  // namespace cobalt

#endif  // SRC_COBALT_BIN_APP_METRIC_EVENT_LOGGER_FACTORY_IMPL_H_
