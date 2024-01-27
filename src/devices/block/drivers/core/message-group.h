// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_CORE_MESSAGE_GROUP_H_
#define SRC_DEVICES_BLOCK_DRIVERS_CORE_MESSAGE_GROUP_H_

#include <fuchsia/hardware/block/c/banjo.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <zircon/types.h>

#include <cstring>
#include <optional>

#include <fbl/auto_lock.h>
#include <fbl/macros.h>
#include <fbl/mutex.h>

class Server;

// Impossible groupid used internally to signify that an operation
// has no accompanying group.
constexpr groupid_t kNoGroup = MAX_TXN_GROUP_COUNT;

// A MessageGroup represents a set of responses we expect to receive from the
// underlying block device that should correspond to one response sent to the client.
class MessageGroup {
 public:
  explicit MessageGroup(Server& server, groupid_t group = kNoGroup)
      : response_({
            .group = group,
        }),
        server_(server) {}

  MessageGroup(const MessageGroup&) = delete;
  MessageGroup& operator=(const MessageGroup&) = delete;

  // Add `response_count` responses to the message, which correspond to `request_count` requests
  // sent by the client. If request_id is not std::nullopt, will also mark this MessageGroup as
  // ready to respond to the client. If group is `kNoGroup, the MessageGroup will take ownership of
  // itself and free itself once the response has been sent.
  [[nodiscard]] zx_status_t ExpectResponses(int response_count, int request_count,
                                            std::optional<reqid_t> request_id) TA_EXCL(lock_);

  // Completes the MessageGroup and sends |response_| to the client. If either of |response_| or
  // |status| have a non-OK status, send that back.
  void Complete(zx_status_t status) TA_EXCL(lock_);

  bool StatusOkPendingLastOp() const {
    fbl::AutoLock guard(&lock_);
    return op_count_ == 1 && pending_ && response_.status == ZX_OK;
  }

 private:
  mutable fbl::Mutex lock_;
  bool pending_ TA_GUARDED(lock_) = false;
  block_fifo_response_t response_ TA_GUARDED(lock_);
  uint32_t op_count_ TA_GUARDED(lock_) = 0;
  Server& server_ TA_GUARDED(lock_);
};

#endif  // SRC_DEVICES_BLOCK_DRIVERS_CORE_MESSAGE_GROUP_H_
