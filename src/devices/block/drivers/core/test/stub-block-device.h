// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_CORE_TEST_STUB_BLOCK_DEVICE_H_
#define SRC_DEVICES_BLOCK_DRIVERS_CORE_TEST_STUB_BLOCK_DEVICE_H_

#include <fuchsia/hardware/block/c/banjo.h>
#include <fuchsia/hardware/block/cpp/banjo.h>

#include <cstdint>
#include <cstdlib>
#include <functional>

constexpr uint32_t kBlockSize = 1024;
constexpr uint64_t kBlockCount = 4096;

class StubBlockDevice : public ddk::BlockProtocol<StubBlockDevice> {
 public:
  using Callback = std::function<zx_status_t(const block_op_t&)>;

  StubBlockDevice() : proto_({&block_protocol_ops_, this}) {
    info_.block_count = kBlockCount;
    info_.block_size = kBlockSize;
    info_.max_transfer_size = 131'072;
  }

  block_protocol_t* proto() { return &proto_; }
  void SetInfo(const block_info_t* info) { info_ = *info; }

  void set_callback(Callback callback) { callback_ = callback; }

  // BlockProtocol ops implementation.
  // -----------------------------------
  void BlockQuery(block_info_t* info_out, size_t* block_op_size_out) {
    *info_out = info_;
    *block_op_size_out = sizeof(block_op_t);
  }

  void BlockQueue(block_op_t* operation, block_queue_callback completion_cb, void* cookie);
  // -----------------------------------

  std::vector<uint32_t>& GetCommandSequence() { return command_sequence_; }

 private:
  block_protocol_t proto_{};
  block_info_t info_{};
  Callback callback_;
  std::vector<uint32_t> command_sequence_;
};

#endif  // SRC_DEVICES_BLOCK_DRIVERS_CORE_TEST_STUB_BLOCK_DEVICE_H_
