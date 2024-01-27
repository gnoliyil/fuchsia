// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/trace-provider/provider.h>

#include "app.h"

int main(int argc, const char** argv) {
  // kAsyncLoopConfigAttachToCurrentThread is currently required by
  // component::Outgoing() which can currently only construct using
  // async_get_default_dispatcher().
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  fuchsia_logging::SetTags({"sysmem_connector"});
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());
  App app(loop.dispatcher());
  loop.Run();
  return 0;
}
