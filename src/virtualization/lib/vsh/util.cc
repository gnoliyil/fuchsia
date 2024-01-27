// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/lib/vsh/util.h"

#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>

#include <iostream>

#include <google/protobuf/message_lite.h>

using google::protobuf::MessageLite;

namespace vsh {

bool SendAllBytes(const zx::socket& socket, const uint8_t* buf, uint32_t buf_size) {
  uint32_t msg_size = htole32(buf_size);
  size_t actual = 0;

  zx_status_t status = socket.write(0, &msg_size, sizeof(msg_size), &actual);
  if (status != ZX_OK || actual != sizeof(msg_size)) {
    FX_LOGS(ERROR) << "Failed to write message size to socket";
    return false;
  }

  status = socket.write(0, buf, buf_size, &actual);
  if (status != ZX_OK || actual != buf_size) {
    FX_LOGS(ERROR) << "Failed to write full message to socket";
    return false;
  }

  return true;
}

bool SendMessage(const zx::socket& socket, const MessageLite& message) {
  size_t msg_size = message.ByteSizeLong();
  if (msg_size > kMaxMessageSize) {
    FX_LOGS(ERROR) << "Serialized message too large: " << msg_size;
    return false;
  }

  uint8_t buf[kMaxMessageSize];

  if (!message.SerializeToArray(buf, sizeof(buf))) {
    FX_LOGS(ERROR) << "Failed to serialize message";
    return false;
  }

  return SendAllBytes(socket, buf, static_cast<uint32_t>(msg_size));
}

int32_t RecvSockBlocking(const zx::socket& socket, uint8_t* buf, int32_t buf_size) {
  if (buf_size < 0) {
    FX_LOGS(ERROR) << "Buffer size must be greater than 0";
    return -1;
  }

  size_t bytes_left = buf_size;

  while (bytes_left > 0) {
    zx_status_t status =
        socket.wait_one(ZX_SOCKET_READABLE | ZX_SOCKET_PEER_CLOSED, zx::time::infinite(), nullptr);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Something happened to the socket while waiting: "
                     << zx_status_get_string(status);
      return -1;
    }

    size_t actual;
    status = socket.read(0, buf, bytes_left, &actual);
    if (status != ZX_OK) {
      // Only non-failure cases are ZX_OK and ZX_ERR_SHOULD_WAIT.
      // Clearly we did wait, so there must be another issue.
      FX_LOGS(ERROR) << "Failed to read from socket (" << zx_status_get_string(status) << ") with "
                     << bytes_left << " bytes left.";
      return -1;
    }

    buf += actual;
    bytes_left -= actual;
  }

  return buf_size;
}

int32_t RecvAllBytes(const zx::socket& socket, uint8_t* buf, int32_t buf_size) {
  int32_t msg_size;

  // Receive the message's size
  if (RecvSockBlocking(socket, reinterpret_cast<uint8_t*>(&msg_size), sizeof(msg_size)) < 0) {
    return -1;
  }

  // Revert msg_size from wire representation to host representation
  msg_size = le32toh(msg_size);

  if (buf_size < msg_size || msg_size < 0) {
    FX_LOGS(ERROR) << "Message size of " << msg_size << " exceeds buffer size of " << buf_size;
    return -1;
  }

  // Receive the message body.
  return RecvSockBlocking(socket, buf, msg_size);
}

bool RecvMessage(const zx::socket& socket, MessageLite* message) {
  uint8_t buf[kMaxMessageSize];
  int32_t count = RecvAllBytes(socket, buf, sizeof(buf));
  if (count < 0) {
    return false;
  }

  if (!message->ParseFromArray(buf, count)) {
    FX_LOGS(ERROR) << "Failed to parse message:";
    return false;
  }

  return true;
}

}  // namespace vsh
