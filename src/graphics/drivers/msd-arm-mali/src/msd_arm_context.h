// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_MSD_ARM_CONTEXT_H_
#define SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_MSD_ARM_CONTEXT_H_

#include "src/graphics/drivers/msd-arm-mali/src/msd_arm_connection.h"

class MsdArmContext : public msd::Context {
 public:
  MsdArmContext(std::weak_ptr<MsdArmConnection> connection) : connection_(connection) {
    connection.lock()->IncrementContextCount();
  }
  ~MsdArmContext() {
    auto locked = connection_.lock();
    if (locked) {
      locked->DecrementContextCount();
      locked->MarkDestroyed();
    }
  }

  magma_status_t ExecuteImmediateCommands(cpp20::span<uint8_t> commands,
                                          cpp20::span<msd::Semaphore*> semaphores) override;

  std::weak_ptr<MsdArmConnection> connection() { return connection_; }

 private:
  std::weak_ptr<MsdArmConnection> connection_;
};

#endif  // SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_MSD_ARM_CONTEXT_H_
