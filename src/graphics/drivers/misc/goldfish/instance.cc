// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/drivers/misc/goldfish/instance.h"

#include <fidl/fuchsia.hardware.goldfish/cpp/wire.h>
#include <lib/async/cpp/task.h>
#include <lib/ddk/debug.h>
#include <lib/zx/bti.h>
#include <zircon/threads.h>

#include <ddktl/fidl.h>

#include "src/graphics/drivers/misc/goldfish/pipe.h"
#include "src/graphics/drivers/misc/goldfish/pipe_device.h"

namespace goldfish {
namespace {

const char kTag[] = "goldfish-pipe";

}  // namespace

Instance::Instance(zx_device_t* parent, PipeDevice* pipe_device, async_dispatcher_t* dispatcher)
    : InstanceType(parent), pipe_device_(pipe_device), dispatcher_(dispatcher) {}

Instance::~Instance() = default;

void Instance::OpenPipe(OpenPipeRequestView request, OpenPipeCompleter::Sync& completer) {
  if (!request->pipe_request.is_valid()) {
    zxlogf(ERROR, "%s: invalid channel", kTag);
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  async::PostTask(dispatcher_, [this, pipe_request = std::move(request->pipe_request)]() mutable {
    auto pipe = std::make_unique<Pipe>(pipe_device_, dispatcher_, /* OnBind */ nullptr,
                                       /* OnClose */ [this](Pipe* pipe_ptr) {
                                         // We know |pipe_ptr| is still alive because |pipe_ptr|
                                         // is still in |pipes_|.
                                         ZX_DEBUG_ASSERT(pipes_.find(pipe_ptr) != pipes_.end());
                                         pipes_.erase(pipe_ptr);
                                       });

    auto pipe_ptr = pipe.get();
    pipes_.insert({pipe_ptr, std::move(pipe)});

    pipe_ptr->Bind(std::move(pipe_request));
    // Init() must be called after Bind() as it can cause an asynchronous
    // failure. The pipe will be cleaned up later by the error handler in
    // the event of a failure.
    pipe_ptr->Init();
  });
}

void Instance::OpenSession(OpenSessionRequestView request, OpenSessionCompleter::Sync& completer) {
  pipe_device_->OpenSession(std::move(request->session));
}

void Instance::DdkRelease() { delete this; }

}  // namespace goldfish
