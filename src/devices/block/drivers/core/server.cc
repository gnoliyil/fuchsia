// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "server.h"

#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/trace/event.h>
#include <lib/zx/fifo.h>
#include <string.h>
#include <unistd.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_ptr.h>

#include "message-group.h"
#include "src/devices/lib/block/block.h"

namespace {

// This signal is set on the FIFO when the server should be instructed
// to terminate.
constexpr zx_signals_t kSignalFifoTerminate = ZX_USER_SIGNAL_0;

void BlockCompleteCb(void* cookie, zx_status_t status, block_op_t* bop) {
  ZX_DEBUG_ASSERT(bop != nullptr);
  std::unique_ptr<Message> msg(static_cast<Message*>(cookie));
  msg->set_result(status);
  msg->Complete();
  msg.reset();
}

block_command_t OpcodeAndFlagsToCommand(block_fifo_command_t command) {
  // BLOCK_IO_FLAG_GROUP_LAST and BLOCK_IO_FLAG_GROUP_ITEM are used in block_client, and these flags
  // are not used in block driver.
  const uint32_t group_mask = BLOCK_IO_FLAG_GROUP_LAST | BLOCK_IO_FLAG_GROUP_ITEM;
  return {
      .opcode = command.opcode,
      .flags = command.flags & ~group_mask,
  };
}

}  // namespace

void Server::Enqueue(std::unique_ptr<Message> message) {
  {
    fbl::AutoLock server_lock(&server_lock_);
    ++pending_count_;
  }
  bp_->Queue(message->Op(), BlockCompleteCb, message.release());
}

void Server::SendResponse(const block_fifo_response_t& response) {
  TRACE_DURATION("storage", "SendResponse");
  for (;;) {
    zx_status_t status = fifo_.write_one(response);
    switch (status) {
      case ZX_OK:
        return;
      case ZX_ERR_SHOULD_WAIT: {
        zx_signals_t signals;
        status = zx_object_wait_one(fifo_.get_handle(),
                                    ZX_FIFO_WRITABLE | ZX_FIFO_PEER_CLOSED | kSignalFifoTerminate,
                                    ZX_TIME_INFINITE, &signals);
        if (status != ZX_OK) {
          zxlogf(WARNING, "(fifo) zx_object_wait_one failed: %s", zx_status_get_string(status));
          return;
        }
        if (signals & kSignalFifoTerminate) {
          // The server is shutting down and we shouldn't block, so dump the response and return.
          return;
        }
        break;
      }
      default:
        zxlogf(WARNING, "Fifo write failed: %s", zx_status_get_string(status));
        return;
    }
  }
}

void Server::FinishTransaction(zx_status_t status, reqid_t reqid, groupid_t group) {
  if (group != kNoGroup) {
    groups_[group]->Complete(status);
  } else {
    SendResponse(block_fifo_response_t{
        .status = status,
        .reqid = reqid,
        .group = group,
        .count = 1,
    });
  }
}

zx_status_t Server::Read(block_fifo_request_t* requests, size_t* count) {
  // Keep trying to read messages from the fifo until we have a reason to
  // terminate
  while (true) {
    zx_status_t status = fifo_.read(requests, BLOCK_FIFO_MAX_DEPTH, count);
    zx_signals_t signals;
    zx_signals_t seen;
    switch (status) {
      case ZX_ERR_SHOULD_WAIT:
        signals = ZX_FIFO_READABLE | ZX_FIFO_PEER_CLOSED | kSignalFifoTerminate;
        if (zx_status_t status = fifo_.wait_one(signals, zx::time::infinite(), &seen);
            status != ZX_OK) {
          return status;
        }
        if ((seen & ZX_FIFO_PEER_CLOSED) || (seen & kSignalFifoTerminate)) {
          return ZX_ERR_PEER_CLOSED;
        }
        // Try reading again...
        break;
      case ZX_OK:
        return ZX_OK;
      default:
        return status;
    }
  }
}

zx::result<vmoid_t> Server::FindVmoIdLocked() {
  for (vmoid_t i = last_id_; i < std::numeric_limits<vmoid_t>::max(); i++) {
    if (!tree_.find(i).IsValid()) {
      last_id_ = static_cast<vmoid_t>(i + 1);
      return zx::ok(i);
    }
  }
  for (vmoid_t i = BLOCK_VMOID_INVALID + 1; i < last_id_; i++) {
    if (!tree_.find(i).IsValid()) {
      last_id_ = static_cast<vmoid_t>(i + 1);
      return zx::ok(i);
    }
  }
  zxlogf(WARNING, "FindVmoId: No vmoids available");
  return zx::error(ZX_ERR_NO_RESOURCES);
}

zx::result<vmoid_t> Server::AttachVmo(zx::vmo vmo) {
  fbl::AutoLock server_lock(&server_lock_);
  zx::result vmoid = FindVmoIdLocked();
  if (vmoid.is_ok()) {
    fbl::AllocChecker ac;
    fbl::RefPtr<IoBuffer> ibuf = fbl::AdoptRef(new (&ac) IoBuffer(std::move(vmo), vmoid.value()));
    if (!ac.check()) {
      return zx::error(ZX_ERR_NO_MEMORY);
    }
    tree_.insert(std::move(ibuf));
  }
  return vmoid;
}

void Server::AttachVmo(AttachVmoRequestView request, AttachVmoCompleter::Sync& completer) {
  zx::result vmoid = AttachVmo(std::move(request->vmo));
  if (vmoid.is_error()) {
    return completer.ReplyError(vmoid.error_value());
  }
  completer.ReplySuccess({
      .id = vmoid.value(),
  });
}

void Server::TxnEnd() {
  fbl::AutoLock lock(&server_lock_);
  // N.B. If pending_count_ hits zero, after dropping the lock the instance of Server can be
  // destroyed.
  if (--pending_count_ == 0) {
    condition_.Broadcast();
  }
}

zx::result<std::unique_ptr<Server>> Server::Create(ddk::BlockProtocolClient* bp) {
  fbl::AllocChecker ac;
  std::unique_ptr<Server> bs(new (&ac) Server(bp));
  if (!ac.check()) {
    return zx::error(ZX_ERR_NO_MEMORY);
  }

  if (zx_status_t status = fzl::create_fifo(BLOCK_FIFO_MAX_DEPTH, 0, &bs->fifo_peer_, &bs->fifo_);
      status != ZX_OK) {
    return zx::error(status);
  }

  for (size_t i = 0; i < std::size(bs->groups_); i++) {
    bs->groups_[i] = std::make_unique<MessageGroup>(*bs, static_cast<groupid_t>(i));
  }

  bp->Query(&bs->info_, &bs->block_op_size_);

  // TODO(fxbug.dev/31467): Allocate BlockMsg arena based on block_op_size_.

  return zx::ok(std::move(bs));
}

zx::result<zx::fifo> Server::GetFifo() {
  // Notably, drop ZX_RIGHT_SIGNAL_PEER, since we use bs->fifo for thread
  // signalling internally within the block server.
  zx_rights_t rights =
      ZX_RIGHT_TRANSFER | ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_SIGNAL | ZX_RIGHT_WAIT;
  zx::fifo fifo;
  zx_status_t status = fifo_peer_.get().duplicate(rights, &fifo);
  return zx::make_result(status, std::move(fifo));
}

void Server::GetFifo(GetFifoCompleter::Sync& completer) {
  zx::result fifo = GetFifo();
  if (fifo.is_error()) {
    return completer.ReplyError(fifo.error_value());
  }
  completer.ReplySuccess(std::move(fifo.value()));
}

zx_status_t Server::ProcessReadWriteRequest(block_fifo_request_t* request, bool do_postflush) {
  fbl::RefPtr<IoBuffer> iobuf;
  {
    fbl::AutoLock lock(&server_lock_);
    auto iter = tree_.find(request->vmoid);
    if (!iter.IsValid()) {
      // Operation which is not accessing a valid vmo.
      zxlogf(WARNING, "ProcessReadWriteRequest: vmoid %d is not valid, failing request",
             request->vmoid);
      return ZX_ERR_IO;
    }
    iobuf = iter.CopyPointer();
  }

  if (!request->length) {
    zxlogf(WARNING,
           "ProcessReadWriteRequest: Invalid request range [%" PRIu64 ",%" PRIu64 "), failing",
           request->dev_offset, request->dev_offset + request->length);
    return ZX_ERR_INVALID_ARGS;
  }

  // Hack to ensure that the vmo is valid.
  // In the future, this code will be responsible for pinning VMO pages,
  // and the completion will be responsible for un-pinning those same pages.
  uint64_t bsz = info_.block_size;
  zx_status_t status = iobuf->ValidateVmoHack(bsz * request->length, bsz * request->vmo_offset);
  if (status != ZX_OK) {
    return status;
  }

  const uint32_t max_xfer = info_.max_transfer_size / bsz;
  if (max_xfer != 0 && max_xfer < request->length) {
    // If the request is larger than the maximum transfer size,
    // split it up into a collection of smaller block messages.
    uint32_t len_remaining = request->length;
    uint64_t vmo_offset = request->vmo_offset;
    uint64_t dev_offset = request->dev_offset;
    uint32_t sub_txns = fbl::round_up(len_remaining, max_xfer) / max_xfer;

    // For groups, we simply add extra (uncounted) messages to the existing MessageGroup,
    // but for ungrouped messages we create a oneshot MessageGroup.
    // The oneshot group has to be shared, since there might be multiple Messages.
    // A copy will be passed into each Message's completion callback, so the group is deallocated
    // once all Messages are complete.
    std::shared_ptr<MessageGroup> oneshot_group = nullptr;
    MessageGroup* transaction_group = nullptr;

    if (request->group == kNoGroup) {
      oneshot_group = std::make_shared<MessageGroup>(*this);
      ZX_ASSERT(oneshot_group->ExpectResponses(sub_txns, 1, request->reqid) == ZX_OK);
      transaction_group = oneshot_group.get();
    } else {
      transaction_group = groups_[request->group].get();
      // If != ZX_OK, it means that we've just received a response to an earlier request that
      // failed.  It should happen rarely because we called ExpectedResponses just prior to this
      // function and it returned ZX_OK.  It's safe to continue at this point and just assume things
      // are OK; it's not worth trying to handle this as a special case.
      [[maybe_unused]] zx_status_t status =
          transaction_group->ExpectResponses(sub_txns - 1, 0, std::nullopt);
    }

    uint32_t sub_txn_idx = 0;

    while (sub_txn_idx != sub_txns) {
      // We'll be using a new BlockMsg for each sub-component.
      // Take a copy of the |oneshot_group| shared_ptr into each completer, so oneshot_group is
      // deallocated after all messages complete.
      auto completer = [this, oneshot_group, transaction_group, do_postflush, request](
                           zx_status_t status, block_fifo_request_t& req) mutable {
        TRACE_DURATION("storage", "FinishTransactionGroup");
        if (req.trace_flow_id) {
          TRACE_FLOW_STEP("storage", "BlockOp", req.trace_flow_id);
        }
        if (do_postflush && transaction_group->StatusOkPendingLastOp() && status == ZX_OK) {
          // Issue (Post)Flush command when last sub transaction completed.
          auto postflush_completer = [transaction_group](zx_status_t postflush_status,
                                                         block_fifo_request_t& req) {
            transaction_group->Complete(postflush_status);
          };
          if (zx_status_t status =
                  IssueFlushCommand(request, std::move(postflush_completer), /*internal_cmd=*/true);
              status != ZX_OK) {
            zxlogf(ERROR, "ProcessReadWriteRequest: (Post)Flush command issue has failed, %s",
                   zx_status_get_string(status));
            transaction_group->Complete(status);
          }
        } else {
          transaction_group->Complete(status);
        }
      };

      std::unique_ptr<Message> msg;
      if (zx_status_t status =
              Message::Create(iobuf, this, request, block_op_size_, std::move(completer), &msg);
          status != ZX_OK) {
        return status;
      }

      uint32_t length = std::min(len_remaining, max_xfer);
      len_remaining -= length;

      *msg->Op() = block_op{.rw = {
                                .command = OpcodeAndFlagsToCommand(request->command),
                                .vmo = iobuf->vmo(),
                                .length = length,
                                .offset_dev = dev_offset,
                                .offset_vmo = vmo_offset,
                            }};
      Enqueue(std::move(msg));
      vmo_offset += length;
      dev_offset += length;
      sub_txn_idx++;
    }
    ZX_DEBUG_ASSERT(len_remaining == 0);
  } else {
    auto completer = [this, do_postflush, request](zx_status_t status, block_fifo_request_t& req) {
      TRACE_DURATION("storage", "FinishTransaction");
      if (req.trace_flow_id) {
        TRACE_FLOW_STEP("storage", "BlockOp", req.trace_flow_id);
      }
      if (do_postflush && status == ZX_OK) {
        // Issue (Post)Flush command
        auto postflush_completer = [this](zx_status_t postflush_status, block_fifo_request_t& req) {
          FinishTransaction(postflush_status, req.reqid, req.group);
        };
        if (zx_status_t status =
                IssueFlushCommand(request, std::move(postflush_completer), /*internal_cmd=*/true);
            status != ZX_OK) {
          zxlogf(ERROR, "ProcessReadWriteRequest: (Post)Flush command issue failed, %s",
                 zx_status_get_string(status));
          FinishTransaction(status, req.reqid, req.group);
        }
      } else {
        FinishTransaction(status, req.reqid, req.group);
      }
    };

    std::unique_ptr<Message> msg;
    if (zx_status_t status =
            Message::Create(iobuf, this, request, block_op_size_, std::move(completer), &msg);
        status != ZX_OK) {
      return status;
    }

    *msg->Op() = block_op{.rw = {
                              .command = OpcodeAndFlagsToCommand(request->command),
                              .vmo = iobuf->vmo(),
                              .length = request->length,
                              .offset_dev = request->dev_offset,
                              .offset_vmo = request->vmo_offset,
                          }};
    Enqueue(std::move(msg));
  }
  return ZX_OK;
}

zx_status_t Server::ProcessReadWriteRequestWithFlush(block_fifo_request_t* request) {
  // PREFLUSH|FORCE_ACCESS request must operate differently depending on the device's writeback
  // cache and FUA command support. There are two cases.
  // 1. Device has writeback cache and support FUA command
  //    => Issue (Pre)Flush + FUA(w/ completion) write commands
  // 2. Device has writeback cache but doesn't support FUA command
  //    => Issue (Pre)Flush + Write + (Post)Flush(w/ completion) commands
  bool need_preflush = false, need_postflush = false;
  if (request->command.flags & BLOCK_IO_FLAG_PREFLUSH) {
    // The BLOCK_IO_FLAG_PREFLUSH flag is not used by the device. Therefore, clear the
    // BLOCK_IO_FLAG_PREFLUSH flag.
    request->command.flags &= ~BLOCK_IO_FLAG_PREFLUSH;
    need_preflush = true;
  }

  if ((request->command.flags & BLOCK_IO_FLAG_FORCE_ACCESS) && !(info_.flags & FLAG_FUA_SUPPORT)) {
    // If the device does not support the FUA command, clear the BLOCK_IO_FLAG_FORCE_ACCESS flag and
    // send the (Post)Flush command. A completion for the request must be sent after the last
    // (Post)Flush is completed.
    request->command.flags &= ~BLOCK_IO_FLAG_FORCE_ACCESS;
    need_postflush = true;
  }

  if (need_preflush) {
    auto preflush_completer = [this, need_postflush](zx_status_t preflush_status,
                                                     block_fifo_request_t& req) {
      if (preflush_status != ZX_OK) {
        FinishTransaction(preflush_status, req.reqid, req.group);
        return;
      }
      if (zx_status_t status = ProcessReadWriteRequest(&req, need_postflush); status != ZX_OK) {
        FinishTransaction(status, req.reqid, req.group);
      }
    };

    // Issue (Pre)Flush command
    if (zx_status_t status =
            IssueFlushCommand(request, std::move(preflush_completer), /*internal_cmd=*/true);
        status != ZX_OK) {
      zxlogf(ERROR, "ProcessReadWriteRequestWithFlush: (Pre)Flush command issue failed, %s",
             zx_status_get_string(status));
      return status;
    }
  } else {
    if (zx_status_t status = ProcessReadWriteRequest(request, need_postflush); status != ZX_OK) {
      return status;
    }
  }
  return ZX_OK;
}

zx_status_t Server::ProcessCloseVmoRequest(block_fifo_request_t* request) {
  fbl::AutoLock lock(&server_lock_);
  auto iobuf = tree_.find(request->vmoid);
  if (!iobuf.IsValid()) {
    // Operation which is not accessing a valid vmo
    zxlogf(WARNING, "ProcessCloseVmoRequest: vmoid %d is not valid, failing request",
           request->vmoid);
    return ZX_ERR_IO;
  }

  // TODO(smklein): Ensure that "iobuf" is not being used by
  // any in-flight txns.
  tree_.erase(*iobuf);
  return ZX_OK;
}

zx_status_t Server::IssueFlushCommand(block_fifo_request_t* request, MessageCompleter completer,
                                      bool internal_cmd) {
  std::unique_ptr<Message> msg;
  zx_status_t status =
      Message::Create(nullptr, this, request, block_op_size_, std::move(completer), &msg);
  if (status != ZX_OK) {
    return status;
  }
  if (internal_cmd) {
    msg->Op()->command = {.opcode = BLOCK_OPCODE_FLUSH, .flags = 0};
  } else {
    msg->Op()->command = OpcodeAndFlagsToCommand(request->command);
  }
  Enqueue(std::move(msg));
  return ZX_OK;
}

zx_status_t Server::ProcessFlushRequest(block_fifo_request_t* request) {
  auto completer = [this](zx_status_t result, block_fifo_request_t& req) {
    FinishTransaction(result, req.reqid, req.group);
  };
  return IssueFlushCommand(request, std::move(completer), /*internal_cmd=*/false);
}

zx_status_t Server::ProcessTrimRequest(block_fifo_request_t* request) {
  if (!request->length) {
    zxlogf(WARNING, "ProcessTrimRequest: Invalid request range [%" PRIu64 ",%" PRIu64 "), failing",
           request->dev_offset, request->dev_offset + request->length);
    return ZX_ERR_INVALID_ARGS;
  }

  std::unique_ptr<Message> msg;
  auto completer = [this](zx_status_t result, block_fifo_request_t& req) {
    FinishTransaction(result, req.reqid, req.group);
  };
  zx_status_t status =
      Message::Create(nullptr, this, request, block_op_size_, std::move(completer), &msg);
  if (status != ZX_OK) {
    return status;
  }
  msg->Op()->command = OpcodeAndFlagsToCommand(request->command);
  msg->Op()->trim.length = request->length;
  msg->Op()->trim.offset_dev = request->dev_offset;
  Enqueue(std::move(msg));
  return ZX_OK;
}

void Server::ProcessRequest(block_fifo_request_t* request) {
  TRACE_DURATION("storage", "Server::ProcessRequest", "opcode", request->command.opcode);
  if (request->trace_flow_id) {
    TRACE_FLOW_STEP("storage", "BlockOp", request->trace_flow_id);
  }
  switch (request->command.opcode) {
    case BLOCK_OPCODE_READ:
    case BLOCK_OPCODE_WRITE:
      if (zx_status_t status = ProcessReadWriteRequestWithFlush(request); status != ZX_OK) {
        FinishTransaction(status, request->reqid, request->group);
      }
      break;
    case BLOCK_OPCODE_FLUSH:
      if (zx_status_t status = ProcessFlushRequest(request); status != ZX_OK) {
        FinishTransaction(status, request->reqid, request->group);
      }
      break;
    case BLOCK_OPCODE_TRIM:
      if (zx_status_t status = ProcessTrimRequest(request); status != ZX_OK) {
        FinishTransaction(status, request->reqid, request->group);
      }
      break;
    case BLOCK_OPCODE_CLOSE_VMO:
      FinishTransaction(ProcessCloseVmoRequest(request), request->reqid, request->group);
      break;
    default:
      zxlogf(WARNING, "Unrecognized block server operation: %d", request->command.opcode);
      FinishTransaction(ZX_ERR_NOT_SUPPORTED, request->reqid, request->group);
  }
}

zx_status_t Server::Serve() {
  block_fifo_request_t requests[BLOCK_FIFO_MAX_DEPTH];
  size_t count;
  while (true) {
    if (zx_status_t status = Read(requests, &count); status != ZX_OK) {
      return status;
    }

    for (size_t i = 0; i < count; i++) {
      bool wants_reply = requests[i].command.flags & BLOCK_IO_FLAG_GROUP_LAST;
      bool use_group = requests[i].command.flags & BLOCK_IO_FLAG_GROUP_ITEM;

      reqid_t reqid = requests[i].reqid;

      if (use_group) {
        groupid_t group = requests[i].group;
        if (group >= MAX_TXN_GROUP_COUNT) {
          // Operation which is not accessing a valid group.
          zxlogf(WARNING, "Serve: group %d is not valid, failing request", group);
          if (wants_reply) {
            FinishTransaction(ZX_ERR_IO, reqid, group);
          }
          continue;
        }

        // Enqueue the message against the transaction group.
        if (zx_status_t status = groups_[group]->ExpectResponses(
                1, 1, wants_reply ? std::optional{reqid} : std::nullopt);
            status != ZX_OK) {
          // This can happen if an earlier request that has been submitted has already failed.
          FinishTransaction(status, reqid, group);
          continue;
        }
      } else {
        requests[i].group = kNoGroup;
      }

      ProcessRequest(&requests[i]);
    }
  }
}

void Server::Close() {
  Shutdown();
  fbl::AutoLock lock(&server_lock_);
  while (pending_count_ > 0)
    condition_.Wait(&server_lock_);
}

void Server::Close(CloseCompleter::Sync& completer) {
  Close();
  completer.ReplySuccess();
  completer.Close(ZX_OK);
}

Server::Server(ddk::BlockProtocolClient* bp)
    : bp_(bp), block_op_size_(0), pending_count_(0), last_id_(BLOCK_VMOID_INVALID + 1) {
  size_t block_op_size;
  bp->Query(&info_, &block_op_size);
}

Server::~Server() { Close(); }

void Server::Shutdown() { fifo_.signal(0, kSignalFifoTerminate); }

bool Server::WillTerminate() const {
  zx_signals_t signals;
  return fifo_.wait_one(ZX_FIFO_PEER_CLOSED, zx::time::infinite_past(), &signals) == ZX_OK;
}
