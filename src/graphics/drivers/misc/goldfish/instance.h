// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DRIVERS_MISC_GOLDFISH_INSTANCE_H_
#define SRC_GRAPHICS_DRIVERS_MISC_GOLDFISH_INSTANCE_H_

#include <fidl/fuchsia.hardware.goldfish.pipe/cpp/wire.h>
#include <fidl/fuchsia.hardware.goldfish/cpp/wire.h>
#include <lib/ddk/device.h>
#include <threads.h>
#include <zircon/types.h>

#include <map>
#include <memory>

#include <ddktl/device.h>

#include "src/graphics/drivers/misc/goldfish/pipe_device.h"

namespace goldfish {

class Pipe;
class Instance;
using InstanceType =
    ddk::Device<Instance, ddk::Messageable<fuchsia_hardware_goldfish::PipeDevice>::Mixin>;

// This class implements a pipe instance device. By opening the pipe device,
// an instance of this class will be created to service a new channel
// to the virtual device.
class Instance : public InstanceType {
 public:
  Instance(zx_device_t* parent, PipeDevice* pipe_device, async_dispatcher_t* dispatcher);
  ~Instance() override;

  // |fidl::WireServer<fuchsia_hardware_goldfish::PipeDevice>|
  void OpenPipe(OpenPipeRequestView request, OpenPipeCompleter::Sync& completer) override;
  void OpenSession(OpenSessionRequestView request, OpenSessionCompleter::Sync& completer) override;

  // Device protocol implementation.
  void DdkRelease();

 private:
  using PipeMap = std::map<Pipe*, std::unique_ptr<Pipe>>;
  PipeDevice* const pipe_device_;
  PipeMap pipes_;
  async_dispatcher_t* const dispatcher_;

  DISALLOW_COPY_ASSIGN_AND_MOVE(Instance);
};

}  // namespace goldfish

#endif  // SRC_GRAPHICS_DRIVERS_MISC_GOLDFISH_INSTANCE_H_
