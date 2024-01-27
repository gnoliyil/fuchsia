// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_BRINGUP_BIN_SVCHOST_INCLUDE_CRASHSVC_EXCEPTION_HANDLER_H_
#define SRC_BRINGUP_BIN_SVCHOST_INCLUDE_CRASHSVC_EXCEPTION_HANDLER_H_

#include <fidl/fuchsia.exception/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async/dispatcher.h>
#include <lib/fidl/cpp/wire/channel.h>
#include <lib/fidl/cpp/wire/client.h>
#include <lib/zx/exception.h>
#include <lib/zx/handle.h>
#include <lib/zx/time.h>
#include <zircon/syscalls/exception.h>
#include <zircon/types.h>

#include "src/lib/fxl/memory/weak_ptr.h"

class ExceptionHandler : public fidl::WireAsyncEventHandler<fuchsia_exception::Handler> {
 public:
  ExceptionHandler(async_dispatcher_t* dispatcher,
                   fidl::ClientEnd<fuchsia_io::Directory> exception_handler_svc,
                   zx::duration is_active_timeout);

  void Handle(zx::exception exception, const zx_exception_info_t& info);

  // Exposed for testing.
  bool ConnectedToServer() const;

 private:
  void SetUpClient();

  // Send |server_endpoit_| to the server of fuchsia.exception.Handler.
  void ConnectToServer();

  void on_fidl_error(fidl::UnbindInfo info) override;

  async_dispatcher_t* dispatcher_;
  fidl::ClientEnd<fuchsia_io::Directory> exception_handler_svc_;

  // Becomes true if exceptions cannot be sent to the exception handling service. Once we reach that
  // state, we will never handle exceptions again.
  bool drop_exceptions_;
  fidl::WireClient<fuchsia_exception::Handler> connection_;

  // The other endpoint of |connection_| before it has been sent to the server.
  fidl::ServerEnd<fuchsia_exception::Handler> server_endpoint_;

  zx::duration is_active_timeout_;

  fxl::WeakPtrFactory<ExceptionHandler> weak_factory_{this};
};

#endif  // SRC_BRINGUP_BIN_SVCHOST_INCLUDE_CRASHSVC_EXCEPTION_HANDLER_H_
