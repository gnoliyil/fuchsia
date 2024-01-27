// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/performance/trace/commands/list_categories.h"

#include <lib/syslog/cpp/macros.h>

#include <iostream>

namespace tracing {

Command::Info ListCategoriesCommand::Describe() {
  return Command::Info{[](sys::ComponentContext* context) {
                         return std::make_unique<ListCategoriesCommand>(context);
                       },
                       "list-categories",
                       "list all known categories",
                       {}};
}

ListCategoriesCommand::ListCategoriesCommand(sys::ComponentContext* context)
    : CommandWithController(context) {}

void ListCategoriesCommand::Start(const fxl::CommandLine& command_line) {
  if (!(command_line.options().empty() && command_line.positional_args().empty())) {
    FX_LOGS(ERROR) << "We encountered unknown options, please check your "
                   << "command invocation";
    Done(EXIT_FAILURE);
    return;
  }

  controller()->GetKnownCategories(
      [this](std::vector<fuchsia::tracing::KnownCategory> known_categories) {
        out() << "Known categories" << std::endl;
        for (const auto& it : known_categories) {
          out() << "  " << it.name << ": " << it.description << std::endl;
        }
        Done(EXIT_SUCCESS);
      });
}

}  // namespace tracing
