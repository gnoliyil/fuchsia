// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.examples/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <stdlib.h>

// An implementation of the Echo protocol. Protocols are implemented in the new
// C++ bindings by creating a subclass of the |Server| or |WireServer| class for
// the protocol.
class EchoImpl final : public fidl::WireServer<fuchsia_examples::Echo> {
 public:
  explicit EchoImpl(bool reverse) : reverse_(reverse) {}

  // Handle a SendString request by sending on OnString event with the request value. For
  // fire and forget methods, the completer can be used to close the channel with an epitaph.
  void SendString(SendStringRequestView request, SendStringCompleter::Sync& completer) override {}

  // Handle an EchoString request by responding with the request value. For two-way
  // methods, the completer is also used to send a response.
  void EchoString(EchoStringRequestView request, EchoStringCompleter::Sync& completer) override {
    std::string value(request->value.get());
    FX_LOGS(INFO) << "Got echo request: " << value;
    if (reverse_) {
      std::reverse(value.begin(), value.end());
    }
    FX_LOGS(INFO) << "Sending response: " << value;
    auto reply = fidl::StringView::FromExternal(value);
    completer.Reply(reply);
  }

  void OnFidlClosed(fidl::UnbindInfo info) {
    if (info.is_user_initiated() || info.is_peer_closed()) {
      return;
    }
    FX_LOGS(ERROR) << "EchoImpl server error: " << info;
  }

 private:
  const bool reverse_;
};

int main(int argc, const char** argv) {
  FX_LOGS(INFO) << "Starting echo service server";
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  auto outgoing = component::OutgoingDirectory(loop.dispatcher());

  auto regular_echo = EchoImpl{/*reverse=*/false};
  auto reversed_echo = EchoImpl{/*reverse=*/true};
  fidl::ServerBindingGroup<fuchsia_examples::Echo> bindings;
  auto result = outgoing.AddService<fuchsia_examples::EchoService>(
      fuchsia_examples::EchoService::InstanceHandler({
          .regular_echo = bindings.CreateHandler(&regular_echo, loop.dispatcher(),
                                                 std::mem_fn(&EchoImpl::OnFidlClosed)),
          .reversed_echo = bindings.CreateHandler(&reversed_echo, loop.dispatcher(),
                                                  std::mem_fn(&EchoImpl::OnFidlClosed)),
      }));

  if (result.is_error()) {
    FX_LOGS(ERROR) << "Failed to add EchoService: " << result.status_string();
    return result.status_value();
  }

  result = outgoing.ServeFromStartupInfo();
  if (result.is_error()) {
    FX_LOGS(ERROR) << "Failed to Serve OutgoingDirectory: " << result.status_string();
    return result.status_value();
  }

  FX_LOGS(INFO) << "Running echo service server";
  loop.Run();
  return 0;
}
