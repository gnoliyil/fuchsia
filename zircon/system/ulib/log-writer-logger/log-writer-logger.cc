// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.logger/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/log-writer-logger/log-writer-logger.h>
#include <lib/log-writer-logger/wire_format.h>
#include <lib/log/log.h>
#include <lib/log/log_writer.h>
#include <lib/zx/channel.h>
#include <lib/zx/process.h>
#include <lib/zx/socket.h>
#include <zircon/assert.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fbl/algorithm.h>

namespace {

zx_koid_t get_koid(zx_handle_t handle) {
  zx_info_handle_basic_t info;
  zx_status_t status =
      zx_object_get_info(handle, ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  return status == ZX_OK ? info.koid : ZX_KOID_INVALID;
}

zx_koid_t get_current_process_koid() {
  auto koid = get_koid(zx_process_self());
  ZX_DEBUG_ASSERT(koid != ZX_KOID_INVALID);
  return koid;
}

// This thread's koid.
// Initialized on first use.
thread_local zx_koid_t tls_thread_koid{ZX_KOID_INVALID};

zx_koid_t get_current_thread_koid() {
  if (unlikely(tls_thread_koid == ZX_KOID_INVALID)) {
    tls_thread_koid = get_koid(zx_thread_self());
  }
  ZX_DEBUG_ASSERT(tls_thread_koid != ZX_KOID_INVALID);
  return tls_thread_koid;
}

bool connect_to_logger(zx::socket* socket) {
  zx::result client_end = component::Connect<fuchsia_logger::LogSink>();
  if (client_end.is_error()) {
    return false;
  }
  fidl::WireSyncClient logger_client(std::move(client_end.value()));
  zx::socket local, remote;
  if (zx::socket::create(ZX_SOCKET_DATAGRAM, &local, &remote) != ZX_OK) {
    return false;
  }
  const fidl::OneWayStatus result = logger_client->Connect(std::move(remote));
  if (!result.ok()) {
    return false;
  }
  *socket = std::move(local);
  return true;
}

class LoggerWriter final : public log_writer {
 public:
  LoggerWriter() : log_writer{&kOps}, pid_(get_current_process_koid()), dropped_logs_(0) {
    socket_error_encountered_ = !connect_to_logger(&socket_);
  }

  void Write(const log_message_t* message);
  void SetSocket(zx_handle_t handle);

 private:
  static const log_writer_ops_t kOps;

  zx_koid_t pid_;
  zx::socket socket_;
  std::atomic<uint32_t> dropped_logs_;
  bool socket_error_encountered_;
};

void logger_writer_write(log_writer_t* writer, const log_message_t* message) {
  auto self = static_cast<LoggerWriter*>(writer);
  self->Write(message);
}

const log_writer_ops_t LoggerWriter::kOps = {
    .version = LOG_WRITER_OPS_V1,
    .reserved = 0,
    .v1 =
        {
            .write = logger_writer_write,
        },
};

// Given a c string and a destination to write the string, copies in a uint8_t
// of the string length, the string contents, and returns the number of bytes
// copied in (i.e. string length + 1). If the string length is greater than
// max_allowed_write, the write is aborted and 0 bytes are written.
size_t write_tag(const char* tag, void* dest, size_t max_allowed_write) {
  size_t tag_len = strlen(tag);
  if (max_allowed_write < tag_len + 1) {
    // Writing this tag would exceed our allowance, so write nothing instead
    return 0;
  }
  *static_cast<char*>(dest) = static_cast<char>(tag_len);
  memcpy(static_cast<char*>(dest) + 1, tag, tag_len);
  return tag_len + 1;
}

void LoggerWriter::Write(const log_message* message) {
  if (socket_error_encountered_) {
    return;
  }

  zx_time_t time = zx_clock_get_monotonic();
  log_packet_t packet;
  memset(&packet, 0, sizeof(packet));
  constexpr size_t kDataSize = sizeof(packet.data);
  packet.metadata.pid = pid_;
  packet.metadata.tid = get_current_thread_koid();
  packet.metadata.time = time;
  packet.metadata.level = message->level;
  packet.metadata.dropped_logs = dropped_logs_.load();

  size_t pos = 0;

  // Write tags
  int tag_counter = 0;
  for (size_t i = 0; i < message->num_static_tags; i++) {
    if (++tag_counter > LOG_MAX_TAGS) {
      break;
    }
    pos += write_tag(message->static_tags[i], packet.data + pos,
                     std::min(kDataSize - pos, size_t{LOG_MAX_TAG_LEN}));
  }
  for (size_t i = 0; i < message->num_dynamic_tags; i++) {
    if (++tag_counter > LOG_MAX_TAGS) {
      break;
    }
    pos += write_tag(message->dynamic_tags[i], packet.data + pos,
                     std::min(kDataSize - pos, size_t{LOG_MAX_TAG_LEN}));
  }

  packet.data[pos++] = 0;
  ZX_DEBUG_ASSERT(pos < kDataSize);

  // Write msg
  size_t msg_len = message->text_len + 1;  // Include the null byte here
  bool cutoff = false;
  if (msg_len > kDataSize - pos) {
    msg_len = kDataSize - pos;
    cutoff = true;
  }
  memcpy(packet.data + pos, message->text, msg_len);
  pos += msg_len;
  if (cutoff) {
    memcpy(packet.data + kDataSize - 4, "...", 4);
  }

  // Send msg
  auto size = sizeof(packet.metadata) + pos;
  ZX_DEBUG_ASSERT(size <= sizeof(packet));
  auto status = socket_.write(0, &packet, size, nullptr);
  if (status == ZX_ERR_BAD_STATE || status == ZX_ERR_PEER_CLOSED) {
    // The socket is no longer usable, mark this as broken.
    socket_error_encountered_ = true;
    return;
  }
  if (status != ZX_OK) {
    dropped_logs_.fetch_add(1);
  }
}

void LoggerWriter::SetSocket(zx_handle_t handle) {
  socket_ = zx::socket(handle);
  socket_error_encountered_ = false;
}

}  // namespace

__EXPORT
log_writer_t* log_create_logger_writer(void) { return new LoggerWriter(); }

__EXPORT
void log_destroy_logger_writer(log_writer_t* writer) { delete static_cast<LoggerWriter*>(writer); }

__EXPORT
void log_set_logger_writer_socket(log_writer_t* writer, zx_handle_t handle) {
  static_cast<LoggerWriter*>(writer)->SetSocket(handle);
}
