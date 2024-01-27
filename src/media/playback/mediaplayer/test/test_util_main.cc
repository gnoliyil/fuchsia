// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/syslog/cpp/log_settings.h>

#include "src/lib/fxl/command_line.h"
#include "src/lib/ui/base_view/view_provider_component.h"
#include "src/media/playback/mediaplayer/test/mediaplayer_test_util_params.h"
#include "src/media/playback/mediaplayer/test/mediaplayer_test_util_view.h"

int main(int argc, char** argv) {
  fuchsia_logging::SetTags({"mediaplayer_test"});

  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  media_player::test::MediaPlayerTestUtilParams params(command_line);
  if (!params.is_valid()) {
    return 1;
  }

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  int result;
  auto quit_callback = [&loop, &result](int exit_code) {
    result = exit_code;
    async::PostTask(loop.dispatcher(), [&loop]() { loop.Quit(); });
  };

  scenic::ViewProviderComponent component(
      [&params, quit_callback](scenic::ViewContext view_context) {
        return std::make_unique<media_player::test::MediaPlayerTestUtilView>(
            std::move(view_context), quit_callback, params);
      },
      &loop);

  loop.Run();

  return result;
}
