// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>
#include <unistd.h>

#include <charconv>

#include <examples/canvas/addlinemetered/cpp/fidl.h>
#include <examples/fidl/new/canvas/add_line_metered/hlcpp/client/config.h>

#include "lib/fpromise/result.h"

// A helper function that takes a coordinate in string form, like "123,-456", and parses it into a
// a struct of the form |{ in64 x; int64 y; }|.
::examples::canvas::addlinemetered::Point ParsePoint(std::string_view input) {
  int64_t x = 0;
  int64_t y = 0;
  size_t index = input.find(',');
  if (index != std::string::npos) {
    std::from_chars(input.data(), input.data() + index, x);
    std::from_chars(input.data() + index + 1, input.data() + input.length(), y);
  }
  return ::examples::canvas::addlinemetered::Point{.x = x, .y = y};
}

// A helper function that takes a coordinate pair in string form, like "1,2:-3,-4", and parses it
// into an array of 2 |Point| structs.
::std::array<::examples::canvas::addlinemetered::Point, 2> ParseLine(const std::string& action) {
  auto input = std::string_view(action);
  size_t index = input.find(':');
  if (index != std::string::npos) {
    return {ParsePoint(input.substr(0, index)), ParsePoint(input.substr(index + 1))};
  }
  return {};
}

int main(int argc, const char** argv) {
  FX_LOGS(INFO) << "Started";

  // Retrieve component configuration.
  auto conf = config::Config::TakeFromStartupHandle();

  // Start up an async loop.
  async::Loop loop(&kAsyncLoopConfigNeverAttachToThread);
  async_dispatcher_t* dispatcher = loop.dispatcher();

  // [START connect-protocol]
  // Connect to the protocol inside the component's namespace, then create an asynchronous client
  // using the newly-established connection.
  examples::canvas::addlinemetered::InstancePtr instance_proxy;
  auto context = sys::ComponentContext::Create();
  context->svc()->Connect(instance_proxy.NewRequest(dispatcher));
  FX_LOGS(INFO) << "Outgoing connection enabled";

  instance_proxy.set_error_handler([&loop](zx_status_t status) {
    FX_LOGS(ERROR) << "Shutdown unexpectedly";
    loop.Quit();
  });
  // [END connect-protocol]

  // Provide a lambda to handle incoming |OnDrawn| events asynchronously.
  instance_proxy.events().OnDrawn = [&loop](
                                        ::examples::canvas::addlinemetered::Point top_left,
                                        ::examples::canvas::addlinemetered::Point bottom_right) {
    FX_LOGS(INFO) << "OnDrawn event received: top_left: Point { x: " << top_left.x
                  << ", y: " << top_left.y << " }, bottom_right: Point { x: " << bottom_right.x
                  << ", y: " << bottom_right.y << " }";
    loop.Quit();
  };

  instance_proxy.events().handle_unknown_event = [](uint64_t ordinal) {
    FX_LOGS(WARNING) << "Received an unknown event with ordinal " << ordinal;
  };

  for (const auto& action : conf.script()) {
    // If the next action in the script is to "WAIT", block until an |OnDrawn| event is received
    // from the server.
    if (action == "WAIT") {
      loop.Run();
      loop.ResetQuit();
      continue;
    }

    // Draw a line to the canvas by calling the server, using the two points we just parsed
    // above as arguments.
    auto line = ParseLine(action);
    FX_LOGS(INFO) << "AddLine request sent: [Point { x: " << line[1].x << ", y: " << line[1].y
                  << " }, Point { x: " << line[0].x << ", y: " << line[0].y << " }]";

    // [START diff_1]
    instance_proxy->AddLine(line, [&](fpromise::result<void, fidl::TransportErr> result) {
      if (result.is_error()) {
        // Check that our flexible two-way call was known to the server and handle the case of an
        // unknown method appropriately. In the case of this example, there is nothing we can do to
        // recover here, except to log an error and exit the program.
        FX_LOGS(ERROR) << "Server does not implement AddLine";
      }
      FX_LOGS(INFO) << "AddLine response received";

      // Quit the loop, thereby handing control back to the outer loop of actions being iterated
      // over.
      loop.Quit();
    });

    // Run the loop until the callback is resolved, at which point we can continue from here.
    loop.Run();
    loop.ResetQuit();
    // [END diff_1]
  }

  // TODO(fxbug.dev/76579): We need to sleep here to make sure all logs get drained. Once the
  // referenced bug has been resolved, we can remove the sleep.
  sleep(2);
  return 0;
}
