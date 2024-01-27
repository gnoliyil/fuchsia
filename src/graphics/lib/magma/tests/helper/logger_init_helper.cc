// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>
#include <platform_handle.h>
#include <platform_logger.h>
#include <platform_logger_provider.h>

class LoggerInitHelper {
 public:
  LoggerInitHelper() {
    zx::channel client_channel, server_channel;
    [[maybe_unused]] zx_status_t status = zx::channel::create(0, &client_channel, &server_channel);
    assert(status == ZX_OK);

    status = fdio_service_connect("/svc/fuchsia.logger.LogSink", server_channel.release());
    assert(status == ZX_OK);

    bool result = magma::PlatformLoggerProvider::Initialize(
        magma::PlatformHandle::Create(client_channel.release()));
    assert(result);
    (void)result;
  }

} g_logger_init_helper;
