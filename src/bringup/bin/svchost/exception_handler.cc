// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async/cpp/task.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <zircon/status.h>

#include <crashsvc/exception_handler.h>
#include <crashsvc/logging.h>

ExceptionHandler::ExceptionHandler(async_dispatcher_t* dispatcher,
                                   fidl::ClientEnd<fuchsia_io::Directory> exception_handler_svc,
                                   const zx::duration is_active_timeout)
    : dispatcher_(dispatcher),
      exception_handler_svc_(std::move(exception_handler_svc)),
      // We are in a build without a server for fuchsia.exception.Handler, e.g., bringup.
      drop_exceptions_(!exception_handler_svc_.is_valid()),
      is_active_timeout_(is_active_timeout) {
  SetUpClient();
  ConnectToServer();
}

void ExceptionHandler::SetUpClient() {
  if (drop_exceptions_) {
    return;
  }

  auto exception_handler_endpoints = fidl::CreateEndpoints<fuchsia_exception::Handler>();
  if (!exception_handler_endpoints.is_ok()) {
    LogError("Failed to create channel for fuchsia.exception.Handler",
             exception_handler_endpoints.status_value());
    drop_exceptions_ = true;
    return;
  }

  connection_ = {};
  connection_.Bind(std::move(exception_handler_endpoints->client), dispatcher_, this);
  server_endpoint_ = std::move(exception_handler_endpoints->server);
}

void ExceptionHandler::on_fidl_error(const fidl::UnbindInfo info) {
  // If the unbind was only due to dispatcher shutdown, don't reconnect and stop sending exceptions
  // to fuchsia.exception.Handler. This should only happen in tests.
  if (info.is_dispatcher_shutdown()) {
    drop_exceptions_ = true;
    return;
  }

  LogError("Lost connection to fuchsia.exception.Handler", info.status());

  // We immediately bind the |connection_| again, but we don't re-connect to the server of
  // fuchsia.exception.Handler, i.e sending the other endpoint of the channel to the server. Instead
  // the re-connection will be done on the next exception. The reason we don't re-connect (1)
  // immediately is because the server could have been shut down by the system or (2) with a backoff
  // is because we don't want to be queueing up exceptions which underlying processes need to be
  // terminated.
  SetUpClient();
}

void ExceptionHandler::ConnectToServer() {
  if (ConnectedToServer() || drop_exceptions_) {
    return;
  }

  zx::result result = component::ConnectAt(exception_handler_svc_, std::move(server_endpoint_));
  if (result.is_error()) {
    LogError("unable to connect to fuchsia.exception.Handler", result.error_value());
    drop_exceptions_ = true;
    return;
  }
}

void ExceptionHandler::Handle(zx::exception exception, const zx_exception_info_t& info) {
  if (drop_exceptions_) {
    return;
  }

  ConnectToServer();

  auto shared_exception = std::make_shared<zx::exception>(std::move(exception));

  auto weak_this = weak_factory_.GetWeakPtr();

  // Sends the exception to the server, if it is still valid, after the call to IsActive
  // has been acknowledged.
  auto is_active_cb = [weak_this, info, shared_exception](auto& result) {
    if (!result.ok()) {
      LogError("Failed to check if handler is active", info, result.status());
      return;
    }

    if (!shared_exception->is_valid()) {
      LogError("Exception was released before handler responded", info);
      return;
    }

    if (weak_this->drop_exceptions_) {
      return;
    }

    weak_this->ConnectToServer();

    fuchsia_exception::wire::ExceptionInfo exception_info;
    exception_info.process_koid = info.pid;
    exception_info.thread_koid = info.tid;
    exception_info.type = static_cast<fuchsia_exception::wire::ExceptionType>(info.type);

    // The server may be in an unresponsive state, unknown here, despite responding to IsActive.
    // However, the response to IsActive narrows window during which it's unknown the server
    // became unresponsive.
    weak_this->connection_->OnException(std::move(*shared_exception), exception_info)
        .ThenExactlyOnce([info](auto& result) {
          if (!result.ok()) {
            LogError("Failed to pass exception to handler", info, result.status());
          }
        });
  };

  // Releases the exception if it is still valid.
  auto release_exception = [shared_exception, info] {
    if (shared_exception->is_valid()) {
      LogError("Exception handler may be unresponsive, releasing exception to kernel", info);
      shared_exception->reset();
    }
  };

  connection_->IsActive().ThenExactlyOnce(std::move(is_active_cb));
  async::PostDelayedTask(dispatcher_, std::move(release_exception), is_active_timeout_);
}

bool ExceptionHandler::ConnectedToServer() const { return !server_endpoint_.is_valid(); }
