// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hda-codec-connection.h"

#include <stdio.h>
#include <zircon/assert.h>

#include <memory>
#include <utility>

#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <intel-hda/utils/codec-commands.h>

#include "debug-logging.h"
#include "intel-hda-controller.h"
#include "intel-hda-stream.h"
#include "utils.h"

namespace audio::intel_hda {

#define SET_DEVICE_PROP(_prop, _value)                                                             \
  do {                                                                                             \
    static_assert(PROP_##_prop < std::size(decltype(dev_props_){}), "Invalid Device Property ID"); \
    dev_props_[PROP_##_prop].id = BIND_IHDA_CODEC_##_prop;                                         \
    dev_props_[PROP_##_prop].value = (_value);                                                     \
  } while (false)

HdaCodecConnection::ProbeCommandListEntry HdaCodecConnection::PROBE_COMMANDS[] = {
    {.verb = GET_PARAM(CodecParam::VENDOR_ID), .parse = &HdaCodecConnection::ParseVidDid},
    {.verb = GET_PARAM(CodecParam::REVISION_ID), .parse = &HdaCodecConnection::ParseRevisionId},
};

zx_protocol_device_t HdaCodecConnection::CODEC_DEVICE_THUNKS = []() {
  zx_protocol_device_t ops = {};
  ops.version = DEVICE_OPS_VERSION;
  ops.message = [](void* ctx, fidl_incoming_msg_t msg, device_fidl_txn_t txn) {
    HdaCodecConnection* thiz = static_cast<HdaCodecConnection*>(ctx);
    fidl::WireDispatch<fuchsia_hardware_intel_hda::CodecDevice>(
        thiz, fidl::IncomingHeaderAndMessage::FromEncodedCMessage(&msg),
        ddk::FromDeviceFIDLTransaction(txn));
  };
  return ops;
}();

ihda_codec_protocol_ops_t HdaCodecConnection::CODEC_PROTO_THUNKS = {
    .get_driver_channel = [](void* ctx, zx_handle_t* channel_out) -> zx_status_t {
      ZX_DEBUG_ASSERT(ctx);
      return static_cast<HdaCodecConnection*>(ctx)->CodecGetDispatcherChannel(channel_out);
    },
};

HdaCodecConnection::HdaCodecConnection(IntelHDAController& controller, uint8_t codec_id)
    : controller_(controller), codec_id_(codec_id), loop_(&kAsyncLoopConfigNeverAttachToThread) {
  ::memset(&dev_props_, 0, sizeof(dev_props_));
  dev_props_[PROP_PROTOCOL].id = BIND_PROTOCOL;
  dev_props_[PROP_PROTOCOL].value = ZX_PROTOCOL_IHDA_CODEC;

  const auto& info = controller_.dev_info();
  snprintf(log_prefix_, sizeof(log_prefix_), "IHDA Codec %02x:%02x.%01x/%02x", info.bus_id,
           info.dev_id, info.func_id, codec_id_);
}

fbl::RefPtr<HdaCodecConnection> HdaCodecConnection::Create(IntelHDAController& controller,
                                                           uint8_t codec_id) {
  ZX_DEBUG_ASSERT(codec_id < HDA_MAX_CODECS);

  fbl::AllocChecker ac;
  auto ret = fbl::AdoptRef(new (&ac) HdaCodecConnection(controller, codec_id));
  if (!ac.check()) {
    GLOBAL_LOG(ERROR, "Out of memory attempting to allocate codec");
    return nullptr;
  }
  zx_status_t status = ret->loop_.StartThread("codec-connection-loop");
  if (status != ZX_OK) {
    GLOBAL_LOG(ERROR, "Could not start loop thread (res = %d)", status);
    return nullptr;
  }
  return ret;
}

zx_status_t HdaCodecConnection::Startup() {
  ZX_DEBUG_ASSERT(state_ == State::PROBING);

  for (const auto& probe_cmd : PROBE_COMMANDS) {
    CodecCommand cmd(id(), 0u, probe_cmd.verb);
    auto job = CodecCmdJobAllocator::New(cmd);

    if (job == nullptr) {
      LOG(ERROR, "Failed to allocate job during initial codec probe!");
      return ZX_ERR_NO_MEMORY;
    }

    zx_status_t res = controller_.QueueCodecCmd(std::move(job));
    if (res != ZX_OK) {
      LOG(ERROR, "Failed to queue job (res = %d) during initial codec probe!", res);
      return res;
    }
  }

  return ZX_OK;
}

void HdaCodecConnection::SendCORBResponse(const fbl::RefPtr<Channel>& channel,
                                          const CodecResponse& resp,
                                          uint32_t transaction_id) const {
  ZX_DEBUG_ASSERT(channel != nullptr);
  ihda_codec_send_corb_cmd_resp_t payload;

  payload.hdr.transaction_id = transaction_id;
  payload.hdr.cmd = IHDA_CODEC_SEND_CORB_CMD;
  payload.data = resp.data;
  payload.data_ex = resp.data_ex;

  zx_status_t res = channel->Write(&payload, sizeof(payload));
  if (res != ZX_OK) {
    LOG(DEBUG, "Error writing CORB response (%08x, %08x) res = %d", resp.data, resp.data_ex, res);
  }
}

void HdaCodecConnection::ProcessSolicitedResponse(const CodecResponse& resp,
                                                  std::unique_ptr<CodecCmdJob>&& job) {
  if (state_ == State::PROBING) {
    // Are we still in the PROBING stage of things?  If so, this job should
    // have no response channel assigned to it, and we should still be
    // waiting for responses from the codec to complete the initial probe.
    ZX_DEBUG_ASSERT(probe_rx_ndx_ < std::size(PROBE_COMMANDS));

    const auto& cmd = PROBE_COMMANDS[probe_rx_ndx_];

    zx_status_t res = (this->*cmd.parse)(resp);
    if (res == ZX_OK) {
      ++probe_rx_ndx_;
    } else {
      LOG(ERROR, "Error parsing solicited response during codec probe! (data %08x)", resp.data);

      // TODO(johngro) : shutdown and cleanup somehow.
      state_ = State::FATAL_ERROR;
    }
  } else if (job->response_channel() != nullptr) {
    LOG(TRACE, "Sending solicited response [%08x, %08x] to channel %p", resp.data, resp.data_ex,
        job->response_channel().get());

    // Does this job have a response channel?  If so, attempt to send the
    // response back on the channel (assuming that it is still open).
    SendCORBResponse(job->response_channel(), resp, job->transaction_id());
  }
}

void HdaCodecConnection::ProcessUnsolicitedResponse(const CodecResponse& resp) {
  // If we still have a channel to our codec driver, grab a reference to it
  // and send the unsolicited response to it.
  fbl::RefPtr<Channel> codec_driver_channel;
  {
    fbl::AutoLock codec_driver_channel_lock(&codec_driver_channel_lock_);
    codec_driver_channel = codec_driver_channel_;
  }

  if (codec_driver_channel)
    SendCORBResponse(codec_driver_channel, resp);
}

void HdaCodecConnection::ProcessWakeupEvt() const {
  // TODO(johngro) : handle wakeup events.  Wakeup events are delivered for
  // two reasons.
  //
  // 1) The codec had brought the controller out of a low power state for some
  //    reason.
  // 2) The codec has been hot-unplugged.
  //
  // Currently, we support neither power management, nor hot-unplug.  Just log
  // the fact that we have been woken up and do nothing.
  LOG(WARNING, "Wakeup event received - Don't know how to handle this yet!");
}

void HdaCodecConnection::Shutdown() {
  // Close all existing connections and synchronize with any client threads
  // who are currently processing requests.
  state_ = State::SHUTTING_DOWN;
  ProcessCodecDeactivate();
  loop_.Shutdown();

  // Give any active streams we had back to our controller.
  IntelHDAStream::Tree streams;
  {
    fbl::AutoLock lock(&active_streams_lock_);
    streams.swap(active_streams_);
  }

  while (!streams.is_empty())
    controller_.ReturnStream(streams.pop_front());

  state_ = State::SHUT_DOWN;
}

zx_status_t HdaCodecConnection::PublishDevice() {
  // Generate our name.
  char name[ZX_DEVICE_NAME_MAX];
  snprintf(name, sizeof(name), "intel-hda-codec-%03u", codec_id_);

  // Initialize our device and fill out the protocol hooks
  device_add_args_t args = {};
  args.version = DEVICE_ADD_ARGS_VERSION;
  args.name = name;
  args.ctx = this;
  args.ops = &CODEC_DEVICE_THUNKS;
  args.proto_id = ZX_PROTOCOL_IHDA_CODEC;
  args.proto_ops = &CODEC_PROTO_THUNKS;
  args.props = dev_props_;
  args.prop_count = std::size(dev_props_);

  // Publish the device.
  zx_status_t res = device_add(controller_.dev_node(), &args, &dev_node_);
  if (res != ZX_OK) {
    LOG(ERROR, "Failed to add codec device for \"%s\" (res %d)", name, res);
    return res;
  }

  return ZX_OK;
}

zx_status_t HdaCodecConnection::ParseVidDid(const CodecResponse& resp) {
  props_.vid = static_cast<uint16_t>((resp.data >> 16) & 0xFFFF);
  props_.did = static_cast<uint16_t>(resp.data & 0xFFFF);

  SET_DEVICE_PROP(VID, props_.vid);
  SET_DEVICE_PROP(DID, props_.did);

  return (props_.vid != 0) ? ZX_OK : ZX_ERR_INTERNAL;
}

zx_status_t HdaCodecConnection::ParseRevisionId(const CodecResponse& resp) {
  props_.ihda_vmaj = static_cast<uint8_t>((resp.data >> 20) & 0xF);
  props_.ihda_vmin = static_cast<uint8_t>((resp.data >> 16) & 0xF);
  props_.rev_id = static_cast<uint8_t>((resp.data >> 8) & 0xFF);
  props_.step_id = static_cast<uint8_t>(resp.data & 0xFF);

  SET_DEVICE_PROP(MAJOR_REV, props_.ihda_vmaj);
  SET_DEVICE_PROP(MINOR_REV, props_.ihda_vmin);
  SET_DEVICE_PROP(VENDOR_REV, props_.rev_id);
  SET_DEVICE_PROP(VENDOR_STEP, props_.step_id);

  state_ = State::FINDING_DRIVER;
  return PublishDevice();
}

void HdaCodecConnection::GetChannel(GetChannelCompleter::Sync& completer) {
  zx::channel channel_local;
  zx::channel channel_remote;
  if (zx_status_t status = zx::channel::create(0, &channel_local, &channel_remote);
      status != ZX_OK) {
    return completer.Close(status);
  }

  fbl::RefPtr<Channel> channel = Channel::Create(std::move(channel_local));
  if (channel == nullptr) {
    return completer.Close(ZX_ERR_NO_MEMORY);
  }
  fbl::AutoLock lock(&channel_lock_);
  channel_ = std::move(channel);
  channel_->SetHandler([codec = fbl::RefPtr(this)](async_dispatcher_t* dispatcher,
                                                   async::WaitBase* wait, zx_status_t status,
                                                   const zx_packet_signal_t* signal) {
    codec->GetChannelSignalled(dispatcher, wait, status, signal);
  });
  if (zx_status_t status = channel_->BeginWait(loop_.dispatcher()); status != ZX_OK) {
    channel_.reset();
    // We let channel_remote go out of scope to trigger channel deactivation via peer close.
    return completer.Close(status);
  }

  completer.Reply(std::move(channel_remote));
}

void HdaCodecConnection::GetChannelSignalled(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                             zx_status_t status, const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    if (status != ZX_ERR_CANCELED) {  // Cancel is expected.
      return;
    }
  }
  bool readable_asserted = signal->observed & ZX_CHANNEL_READABLE;
  bool peer_closed_asserted = signal->observed & ZX_CHANNEL_PEER_CLOSED;
  if (readable_asserted) {
    // Grab a reference to the channel, the processing may grab the lock.
    fbl::RefPtr<Channel> channel;
    {
      fbl::AutoLock lock(&channel_lock_);
      channel = channel_;
    }
    zx_status_t status = ProcessUserRequest(channel.get());
    if (status != ZX_OK) {
      peer_closed_asserted = true;
    }
  }
  if (peer_closed_asserted) {
    fbl::AutoLock lock(&channel_lock_);
    channel_.reset();
  } else if (readable_asserted) {
    wait->Begin(dispatcher);
  }
}

#define PROCESS_CMD_INNER(_req_ack, _ioctl, _payload, _handler)                                   \
  case _ioctl:                                                                                    \
    if (req_size != sizeof(req._payload)) {                                                       \
      LOG(DEBUG, "Bad " #_payload " request length (%u != %zu)", req_size, sizeof(req._payload)); \
      return ZX_ERR_INVALID_ARGS;                                                                 \
    }                                                                                             \
    if ((_req_ack) && (req.hdr.cmd & IHDA_NOACK_FLAG)) {                                          \
      LOG(DEBUG, "Cmd " #_payload                                                                 \
                 " requires acknowledgement, but the "                                            \
                 "NOACK flag was set!");                                                          \
      return ZX_ERR_INVALID_ARGS;                                                                 \
    }

#define PROCESS_CMD(_req_ack, _ioctl, _payload, _handler) \
  PROCESS_CMD_INNER(_req_ack, _ioctl, _payload, _handler) \
  return _handler(channel, req._payload)

#define PROCESS_CMD_WITH_HANDLE(_req_ack, _ioctl, _payload, _handler) \
  PROCESS_CMD_INNER(_req_ack, _ioctl, _payload, _handler)             \
  return _handler(channel, req._payload, std::move(received_handle))

zx_status_t HdaCodecConnection::ProcessCodecRequest(Channel* channel) {
  zx_status_t res;
  uint32_t req_size;
  union {
    ihda_proto::CmdHdr hdr;
    ihda_proto::SendCORBCmdReq corb_cmd;
    ihda_proto::RequestStreamReq request_stream;
    ihda_proto::ReleaseStreamReq release_stream;
    ihda_proto::SetStreamFmtReq set_stream_fmt;
  } req;
  // TODO(johngro) : How large is too large?
  static_assert(sizeof(req) <= 256, "Request buffer is too large to hold on the stack!");

  // Read the user request.
  ZX_DEBUG_ASSERT(channel != nullptr);
  zx::handle received_handle;
  res = channel->Read(&req, sizeof(req), &req_size, received_handle);
  if (res != ZX_OK) {
    LOG(DEBUG, "Failed to read user request (res %d)", res);
    return res;
  }

  // Sanity checks.
  if (req_size < sizeof(req.hdr)) {
    LOG(DEBUG, "Client request too small to contain header (%u < %zu)", req_size, sizeof(req.hdr));
    return ZX_ERR_INVALID_ARGS;
  }

  auto cmd_id = static_cast<ihda_cmd_t>(req.hdr.cmd & ~IHDA_NOACK_FLAG);
  if (req.hdr.transaction_id == IHDA_INVALID_TRANSACTION_ID) {
    LOG(DEBUG, "Invalid transaction ID in client request 0x%04x", cmd_id);
    return ZX_ERR_INVALID_ARGS;
  }

  // Dispatch
  LOG(TRACE, "Codec Request (cmd 0x%04x tid %u) len %u", req.hdr.cmd, req.hdr.transaction_id,
      req_size);

  switch (cmd_id) {
    PROCESS_CMD(true, IHDA_CODEC_REQUEST_STREAM, request_stream, ProcessRequestStream);
    PROCESS_CMD(false, IHDA_CODEC_RELEASE_STREAM, release_stream, ProcessReleaseStream);
    PROCESS_CMD_WITH_HANDLE(false, IHDA_CODEC_SET_STREAM_FORMAT, set_stream_fmt,
                            ProcessSetStreamFmt);
    PROCESS_CMD(false, IHDA_CODEC_SEND_CORB_CMD, corb_cmd, ProcessSendCORBCmd);
    default:
      LOG(DEBUG, "Unrecognized command ID 0x%04x", req.hdr.cmd);
      return ZX_ERR_INVALID_ARGS;
  }
}

zx_status_t HdaCodecConnection::ProcessUserRequest(Channel* channel) {
  zx_status_t res;
  uint32_t req_size;
  union {
    ihda_proto::CmdHdr hdr;
    ihda_proto::GetIDsReq get_ids;
    ihda_proto::SendCORBCmdReq corb_cmd;
  } req;
  // TODO(johngro) : How large is too large?
  static_assert(sizeof(req) <= 256, "Request buffer is too large to hold on the stack!");

  // Read the client request.
  ZX_DEBUG_ASSERT(channel != nullptr);
  res = channel->Read(&req, sizeof(req), &req_size);
  if (res != ZX_OK) {
    LOG(DEBUG, "Failed to read client request (res %d)", res);
    return res;
  }

  // Sanity checks.
  if (req_size < sizeof(req.hdr)) {
    LOG(DEBUG, "Client request too small to contain header (%u < %zu)", req_size, sizeof(req.hdr));
    return ZX_ERR_INVALID_ARGS;
  }

  auto cmd_id = static_cast<ihda_cmd_t>(req.hdr.cmd & ~IHDA_NOACK_FLAG);
  if (req.hdr.transaction_id == IHDA_INVALID_TRANSACTION_ID) {
    LOG(DEBUG, "Invalid transaction ID in client request 0x%04x", cmd_id);
    return ZX_ERR_INVALID_ARGS;
  }

  // Dispatch
  LOG(TRACE, "User Request (cmd 0x%04x tid %u) len %u", req.hdr.cmd, req.hdr.transaction_id,
      req_size);

  // We only allow CORB "get" requests.
  if (cmd_id == IHDA_CODEC_SEND_CORB_CMD && CodecVerb(req.corb_cmd.verb).is_set()) {
    LOG(DEBUG, "User attempted to perform privileged command.");
    return ZX_ERR_ACCESS_DENIED;
  }

  switch (cmd_id) {
    PROCESS_CMD(true, IHDA_CMD_GET_IDS, get_ids, ProcessGetIDs);
    PROCESS_CMD(false, IHDA_CODEC_SEND_CORB_CMD, corb_cmd, ProcessSendCORBCmd);
    default:
      LOG(DEBUG, "Unrecognized command ID 0x%04x", req.hdr.cmd);
      return ZX_ERR_INVALID_ARGS;
  }
}

#undef PROCESS_CMD

void HdaCodecConnection::ProcessCodecDeactivate() {
  // This should be the driver channel (client channels created with IOCTL do
  // not register a deactivate handler).  Start by releasing the internal
  // channel reference from within the codec_driver_channel_lock.
  {
    fbl::AutoLock lock(&codec_driver_channel_lock_);
    codec_driver_channel_.reset();
  }

  // Return any DMA streams the codec driver had owned back to the controller.
  IntelHDAStream::Tree tmp;
  {
    fbl::AutoLock lock(&active_streams_lock_);
    tmp = std::move(active_streams_);
  }

  while (!tmp.is_empty()) {
    auto stream = tmp.pop_front();
    stream->Deactivate();
    controller_.ReturnStream(std::move(stream));
  }
}

zx_status_t HdaCodecConnection::ProcessGetIDs(Channel* channel,
                                              const ihda_proto::GetIDsReq& req) const {
  ZX_DEBUG_ASSERT(channel != nullptr);

  ihda_proto::GetIDsResp resp;
  resp.hdr = req.hdr;
  resp.vid = props_.vid;
  resp.did = props_.did;
  resp.ihda_vmaj = props_.ihda_vmaj;
  resp.ihda_vmin = props_.ihda_vmin;
  resp.rev_id = props_.rev_id;
  resp.step_id = props_.step_id;

  return channel->Write(&resp, sizeof(resp));
}

zx_status_t HdaCodecConnection::ProcessSendCORBCmd(Channel* channel,
                                                   const ihda_proto::SendCORBCmdReq& req) {
  ZX_DEBUG_ASSERT(channel != nullptr);

  CodecVerb verb(req.verb);

  // Make sure that the command is well formed.
  if (!CodecCommand::SanityCheck(id(), req.nid, verb)) {
    LOG(DEBUG, "Bad SEND_CORB_CMD request values [%u, %hu, 0x%05x]", id(), req.nid, verb.val);
    return ZX_ERR_INVALID_ARGS;
  }

  fbl::RefPtr<Channel> chan_ref = (req.hdr.cmd & IHDA_NOACK_FLAG) ? nullptr : fbl::RefPtr(channel);

  auto job = CodecCmdJobAllocator::New(std::move(chan_ref), req.hdr.transaction_id,
                                       CodecCommand(id(), req.nid, verb));

  if (job == nullptr)
    return ZX_ERR_NO_MEMORY;

  zx_status_t res = controller_.QueueCodecCmd(std::move(job));
  if (res != ZX_OK) {
    LOG(DEBUG, "Failed to queue CORB command [%u, %hu, 0x%05x] (res %d)", id(), req.nid, verb.val,
        res);
  }

  return res;
}

zx_status_t HdaCodecConnection::ProcessRequestStream(Channel* channel,
                                                     const ihda_proto::RequestStreamReq& req) {
  ZX_DEBUG_ASSERT(channel != nullptr);

  ihda_proto::RequestStreamResp resp;
  resp.hdr = req.hdr;

  // Attempt to get a stream of the proper type.
  auto type = req.input ? IntelHDAStream::Type::INPUT : IntelHDAStream::Type::OUTPUT;
  auto stream = controller_.AllocateStream(type);

  if (stream != nullptr) {
    // Success, send its ID and its tag back to the codec and add it to the
    // set of active streams owned by this codec.
    resp.result = ZX_OK;
    resp.stream_id = stream->id();
    resp.stream_tag = stream->tag();

    fbl::AutoLock lock(&active_streams_lock_);
    active_streams_.insert(std::move(stream));
  } else {
    // Failure; tell the codec that we are out of streams.
    resp.result = ZX_ERR_NO_MEMORY;
    resp.stream_id = 0;
    resp.stream_tag = 0;
  }

  return channel->Write(&resp, sizeof(resp));
}

zx_status_t HdaCodecConnection::ProcessReleaseStream(Channel* channel,
                                                     const ihda_proto::ReleaseStreamReq& req) {
  ZX_DEBUG_ASSERT(channel != nullptr);

  // Remove the stream from the active set.
  fbl::RefPtr<IntelHDAStream> stream;
  {
    fbl::AutoLock lock(&active_streams_lock_);
    stream = active_streams_.erase(req.stream_id);
  }

  // If the stream was not active, our codec driver has some sort of internal
  // inconsistency.  Hang up the phone on it.
  if (stream == nullptr)
    return ZX_ERR_BAD_STATE;

  // Give the stream back to the controller and (if an ack was requested) tell
  // our codec driver that things went well.
  stream->Deactivate();
  controller_.ReturnStream(std::move(stream));

  if (req.hdr.cmd & IHDA_NOACK_FLAG)
    return ZX_OK;

  ihda_proto::RequestStreamResp resp;
  resp.hdr = req.hdr;
  return channel->Write(&resp, sizeof(resp));
}

zx_status_t HdaCodecConnection::ProcessSetStreamFmt(Channel* channel,
                                                    const ihda_proto::SetStreamFmtReq& req,
                                                    zx::handle received_handle) {
  ZX_DEBUG_ASSERT(channel != nullptr);

  zx::channel server_channel;
  zx_status_t res = ConvertHandle(&received_handle, &server_channel);
  if (res != ZX_OK) {
    LOG(DEBUG, "Failed to convert handle to channel (res %d)", res);
    return res;
  }

  // Sanity check the requested format.
  if (!StreamFormat(req.format).SanityCheck()) {
    LOG(DEBUG, "Invalid encoded stream format 0x%04hx!", req.format);
    return ZX_ERR_INVALID_ARGS;
  }

  // Grab a reference to the stream from the active set.
  fbl::RefPtr<IntelHDAStream> stream;
  {
    fbl::AutoLock lock(&active_streams_lock_);
    auto iter = active_streams_.find(req.stream_id);
    if (iter.IsValid())
      stream = iter.CopyPointer();
  }

  // If the stream was not active, our codec driver has some sort of internal
  // inconsistency.  Hang up the phone on it.
  if (stream == nullptr)
    return ZX_ERR_BAD_STATE;

  // Set the stream format and assign the server channel to the stream.  If
  // this stream is already bound to a client, this will cause that connection
  // to be closed.
  res = stream->SetStreamFormat(loop_.dispatcher(), req.format, std::move(server_channel));
  if (res != ZX_OK) {
    LOG(DEBUG, "Failed to set stream format 0x%04hx for stream %hu (res %d)", req.format,
        req.stream_id, res);
    return res;
  }

  // Reply to the codec driver.
  ihda_proto::SetStreamFmtResp resp;
  resp.hdr = req.hdr;
  res = channel->Write(&resp, sizeof(resp));

  if (res != ZX_OK)
    LOG(DEBUG, "Failed to send stream channel back to codec driver (res %d)", res);

  return res;
}

zx_status_t HdaCodecConnection::CodecGetDispatcherChannel(zx_handle_t* remote_endpoint_out) {
  if (!remote_endpoint_out)
    return ZX_ERR_INVALID_ARGS;

  zx::channel channel_local;
  zx::channel channel_remote;
  zx_status_t status = zx::channel::create(0, &channel_local, &channel_remote);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<Channel> channel = Channel::Create(std::move(channel_local));
  if (channel == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }
  fbl::AutoLock lock(&codec_driver_channel_lock_);
  codec_driver_channel_ = channel;
  codec_driver_channel_->SetHandler(
      [codec = fbl::RefPtr(this)](async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                  zx_status_t status, const zx_packet_signal_t* signal) {
        codec->GetDispatcherChannelSignalled(dispatcher, wait, status, signal);
      });
  status = codec_driver_channel_->BeginWait(loop_.dispatcher());
  if (status != ZX_OK) {
    codec_driver_channel_.reset();
    // We let channel_remote go out of scope to trigger channel deactivation via peer close.
    return status;
  }

  if (status == ZX_OK) {
    // If things went well, release the reference to the remote endpoint
    // from the zx::channel instance into the unmanaged world of DDK
    // protocols.
    *remote_endpoint_out = channel_remote.release();
  }

  return status;
}

void HdaCodecConnection::GetDispatcherChannelSignalled(async_dispatcher_t* dispatcher,
                                                       async::WaitBase* wait, zx_status_t status,
                                                       const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    if (status != ZX_ERR_CANCELED) {  // Cancel is expected.
      return;
    }
  }
  bool readable_asserted = signal->observed & ZX_CHANNEL_READABLE;
  bool peer_closed_asserted = signal->observed & ZX_CHANNEL_PEER_CLOSED;
  if (readable_asserted) {
    // Grab a reference to the codec driver channel, the processing may grab the lock.
    fbl::RefPtr<Channel> codec_driver_channel;
    {
      fbl::AutoLock lock(&codec_driver_channel_lock_);
      codec_driver_channel = codec_driver_channel_;
    }
    zx_status_t status = ProcessCodecRequest(codec_driver_channel.get());
    if (status != ZX_OK) {
      peer_closed_asserted = true;
    }
  }
  if (peer_closed_asserted) {
    fbl::AutoLock lock(&codec_driver_channel_lock_);
    codec_driver_channel_.reset();
  } else if (readable_asserted) {
    wait->Begin(dispatcher);
  }
}

}  // namespace audio::intel_hda
