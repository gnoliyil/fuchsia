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

// This class implements a pipe device.
// Closing the connection to this pipe device will close all of the pipes that
// it created.
class Instance : public fidl::WireServer<fuchsia_hardware_goldfish::PipeDevice> {
 public:
  Instance(PipeDevice* pipe_device, async_dispatcher_t* dispatcher);
  ~Instance() override;

  // |fidl::WireServer<fuchsia_hardware_goldfish::PipeDevice>|
  void OpenPipe(OpenPipeRequestView request, OpenPipeCompleter::Sync& completer) override;

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
