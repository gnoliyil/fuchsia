// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_LIB_INTEL_HDA_INCLUDE_INTEL_HDA_CODEC_UTILS_CODEC_DRIVER_BASE_H_
#define SRC_MEDIA_AUDIO_DRIVERS_LIB_INTEL_HDA_INCLUDE_INTEL_HDA_CODEC_UTILS_CODEC_DRIVER_BASE_H_

#include <fuchsia/hardware/intelhda/codec/c/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/zx/handle.h>
#include <lib/zx/result.h>

#include <fbl/intrusive_wavl_tree.h>
#include <fbl/mutex.h>
#include <fbl/ref_ptr.h>
#include <intel-hda/utils/codec-commands.h>
#include <intel-hda/utils/intel-hda-proto.h>

#include "channel.h"

namespace audio {
namespace intel_hda {
namespace codecs {

class IntelHDAStreamBase;

class IntelHDACodecDriverBase : public fbl::RefCounted<IntelHDACodecDriverBase> {
 public:
  virtual void Shutdown();
  virtual zx_status_t Suspend(uint8_t requested_state, bool enable_wake, uint8_t suspend_reason,
                              uint8_t* out_state);

  void ChannelSignalled(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                        const zx_packet_signal_t* signal);

  // Properties
  zx_device_t* codec_device() const { return codec_device_; }
  zx_device_t* zxdev() const { return zxdev_; }
  zx_time_t create_time() const { return create_time_; }

  // Unsolicited tag allocation for streams
  zx_status_t AllocateUnsolTag(const IntelHDAStreamBase& stream, uint8_t* out_tag);
  void ReleaseUnsolTag(const IntelHDAStreamBase& stream, uint8_t tag);
  void ReleaseAllUnsolTags(const IntelHDAStreamBase& stream);
  zx_status_t EraseStream(uint32_t stream_id) __TA_EXCLUDES(active_streams_lock_);

 protected:
  static constexpr uint32_t CODEC_TID = 0xFFFFFFFF;

  IntelHDACodecDriverBase();
  virtual ~IntelHDACodecDriverBase() {}

  ///////////////////////////////////////////////////////////////////////////
  //
  // Methods used by derived classes in order to implement their driver.
  //
  ///////////////////////////////////////////////////////////////////////////

  // Bind should only ever be called exactly once (during driver
  // instantiation).  Drivers must make sure that no other methods are in
  // flight during a call to Bind.
  zx::result<> Bind(zx_device_t* codec_dev, const char* name);

  // Send a codec command to our codec device.
  zx_status_t SendCodecCommand(uint16_t nid, CodecVerb verb, bool no_ack);

  virtual zx_status_t Start() { return ZX_OK; }
  virtual zx_status_t ProcessUnsolicitedResponse(const CodecResponse& resp) { return ZX_OK; }
  virtual zx_status_t ProcessSolicitedResponse(const CodecResponse& resp) { return ZX_OK; }

  // Unsolicited tag allocation for codecs.
  zx_status_t AllocateUnsolTag(uint8_t* out_tag) { return AllocateUnsolTag(CODEC_TID, out_tag); }
  void ReleaseUnsolTag(uint8_t tag) { ReleaseUnsolTag(CODEC_TID, tag); }

  fbl::RefPtr<IntelHDAStreamBase> GetActiveStream(uint32_t stream_id)
      __TA_EXCLUDES(active_streams_lock_);
  zx_status_t ActivateStream(const fbl::RefPtr<IntelHDAStreamBase>& stream)
      __TA_EXCLUDES(active_streams_lock_);
  zx_status_t DeactivateStream(uint32_t stream_id) __TA_EXCLUDES(active_streams_lock_);

  // Debug logging
  virtual void PrintDebugPrefix() const;
  void DeviceRelease();

 private:
  friend class fbl::RefPtr<IntelHDACodecDriverBase>;

  union CodecChannelResponses {
    ihda_proto::CmdHdr hdr;
    ihda_proto::SendCORBCmdResp send_corb;
    ihda_proto::RequestStreamResp request_stream;
    ihda_proto::SetStreamFmtResp set_stream_fmt;
  };

  // Thunks for dispatching channel events.
  zx_status_t ProcessClientRequest(Channel* channel);
  void ProcessClientDeactivate();

  // Unsolicited response tag to stream ID bookkeeping.
  zx_status_t AllocateUnsolTag(uint32_t stream_id, uint8_t* out_tag);
  void ReleaseUnsolTag(uint32_t stream_id, uint8_t tag);
  void ReleaseAllUnsolTags(uint32_t stream_id);
  zx_status_t MapUnsolTagToStreamId(uint8_t tag, uint32_t* out_stream_id);

  // Called in order to unlink this device from the controller driver.  After
  // this call returns, the codec driver is guaranteed that no calls to any of
  // the driver implemented callbacks (see below) are in flight, and that no
  // new calls will be initiated.  It is not safe to make this call during a
  // controller callback.  To unlink from a controller during a callback,
  // return an error code from the callback.
  void UnlinkFromController();

  zx_status_t ProcessStreamResponse(const fbl::RefPtr<IntelHDAStreamBase>& stream,
                                    const CodecChannelResponses& resp, uint32_t resp_size,
                                    zx::handle&& rxed_handle);

  static zx_protocol_device_t CODEC_DEVICE_THUNKS;
  zx_device_t* codec_device_ = nullptr;
  zx_device_t* zxdev_ = nullptr;
  zx_time_t create_time_ = zx_clock_get_monotonic();

  fbl::Mutex device_channel_lock_;
  fbl::RefPtr<Channel> device_channel_ __TA_GUARDED(device_channel_lock_);

  using ActiveStreams = fbl::WAVLTree<uint32_t, fbl::RefPtr<IntelHDAStreamBase>>;
  fbl::Mutex active_streams_lock_;
  ActiveStreams active_streams_ __TA_GUARDED(active_streams_lock_);

  fbl::Mutex shutdown_lock_ __TA_ACQUIRED_BEFORE(device_channel_lock_, active_streams_lock_);
  bool shutting_down_ __TA_GUARDED(shutdown_lock_) = false;

  // State for tracking unsolicited response tag allocations.
  //
  // Note: If we wanted to save a bit of RAM, we could move this to a
  // dynamically allocated list/tree based system.  For now, however, this LUT
  // is dirt simple and does the job.
  fbl::Mutex unsol_tag_lock_;
  uint64_t free_unsol_tags_ __TA_GUARDED(unsol_tag_lock_) = 0xFFFFFFFFFFFFFFFEu;
  uint32_t unsol_tag_to_stream_id_map_[sizeof(free_unsol_tags_) << 3] __TA_GUARDED(unsol_tag_lock_);

  async::Loop loop_;
};

}  // namespace codecs
}  // namespace intel_hda
}  // namespace audio

#endif  // SRC_MEDIA_AUDIO_DRIVERS_LIB_INTEL_HDA_INCLUDE_INTEL_HDA_CODEC_UTILS_CODEC_DRIVER_BASE_H_
