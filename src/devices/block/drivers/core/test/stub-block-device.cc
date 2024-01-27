// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/stub-block-device.h"

void StubBlockDevice::BlockQueue(block_op_t* operation, block_queue_callback completion_cb,
                                 void* cookie) {
  command_sequence_.push_back(operation->command);

  if (callback_) {
    completion_cb(cookie, callback_(*operation), operation);
  } else {
    completion_cb(cookie, ZX_OK, operation);
  }
}
