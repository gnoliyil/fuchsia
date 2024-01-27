// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "h264_multi_decoder.h"

#include <lib/media/codec_impl/codec_buffer.h>
#include <lib/stdcompat/variant.h>
#include <lib/trace/event.h>
#include <zircon/assert.h>
#include <zircon/syscalls.h>

#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

#include <fbl/algorithm.h>

#include "decoder_instance.h"
#include "h264_utils.h"
#include "lib/media/extend_bits/extend_bits.h"
#include "macros.h"
#include "media/base/decoder_buffer.h"
#include "media/gpu/h264_decoder.h"
#include "media/video/h264_level_limits.h"
#include "parser.h"
#include "registers.h"
#include "src/media/lib/metrics/metrics.cb.h"
#include "util.h"
#include "watchdog.h"

namespace fh_amlcanvas = fuchsia_hardware_amlogiccanvas;

namespace amlogic_decoder {

#if 0
        1: StreamCreated
        2: StreamDeleted
        3: StreamFlushed
        4: StreamEndOfStreamInput
        5: StreamEndOfStreamOutput
#In addition to separate reasons listed below.
        6: StreamFailureAnyReason
        7: CoreCreated
        8: CoreDeleted
        9: CoreFlushed
        10: CoreEndOfStreamInput
        11: CoreEndOfStreamOuput
#In addition to separate reasons listed below.
        12: CoreFailureAnyReason
        13: InputBufferAllocationStarted
        14: InputBufferAllocationSuccess
        15: InputBufferAllocationFailure
        16: OutputBufferAllocationStarted
        17: OutputBufferAllocationSuccess
        18: OutputBufferAllocationFailure
#endif

// TODO(fxbug.dev/13483): Currently there's one frame of latency imposed by the need for another
// NALU after the last byte of a frame for that frame to generate a pic data done interrupt.  A
// client can mitigate this by queueing an access unit delimeter NALU after each input frame's slice
// NALU(s), but we should consider paying attention to access unit flags on the packet so that a
// client delivering complete frames and never putting data from more than one frame in a single
// packet can set the flag on the last packet of a frame and not see 1 frame latency.  The argument
// against doing this is that nal_unit_type 9 is the h264 way to avoid the 1 frame latency, and
// isn't very difficult for clients to add after each complete frame.

namespace {

// See VLD_PADDING_SIZE.
constexpr uint32_t kPaddingSize = 1024;
const uint8_t kPadding[kPaddingSize] = {};

// See end_of_seq_rbsp() (empty) in h.264 spec.  The purpose of queueing this after the last input
// data is to avoid the FW generating decode buf empty interrupt (which it does when the last byte
// delivered to the FW is exactly the last byte of a frame), and instead generate pic data done
// interrupt (which the FW does if it sees a new NALU after the last byte of a frame).
const std::vector<uint8_t> kEOS = {0, 0, 0, 1, 0x0a};

// ISO 14496 part 10
// VUI parameters: Table E-1 "Meaning of sample aspect ratio indicator"
static const int kTableSarWidth[] = {0,  1,  12, 10, 16,  40, 24, 20, 32,
                                     80, 18, 15, 64, 160, 4,  3,  2};
static const int kTableSarHeight[] = {0,  1,  11, 11, 11, 33, 11, 11, 11,
                                      33, 11, 11, 33, 99, 3,  2,  1};
static_assert(base::size(kTableSarWidth) == base::size(kTableSarHeight),
              "sar tables must have the same size");

enum class ChromaFormatIdc : uint32_t {
  kMonochrome = 0,
  // Presently only 4:2:0 chroma_format_idc is supported:
  k420 = 1,
  k422 = 2,
  k444 = 3,
};

static constexpr uint32_t kMacroblockDimension = 16;

// We just set ViffBitCnt to a very large value that can still safely be multiplied by 8.  The HW
// doesn't seem to actually stop decoding if this hits zero, nor does the HW seem to care if this
// doesn't reach zero at the end of a frame.
constexpr uint32_t kBytesToDecode = 0x10000000;

constexpr uint32_t kStreamBufferReadAlignment = 512;

}  // namespace

class AmlogicH264Picture : public media::H264Picture {
 public:
  explicit AmlogicH264Picture(std::shared_ptr<H264MultiDecoder::ReferenceFrame> pic)
      : internal_picture(pic) {}
  ~AmlogicH264Picture() override {
    auto pic = internal_picture.lock();
    if (pic) {
      pic->in_internal_use = false;
    }
  }

  std::weak_ptr<H264MultiDecoder::ReferenceFrame> internal_picture;
};
class MultiAccelerator : public media::H264Decoder::H264Accelerator {
 public:
  explicit MultiAccelerator(H264MultiDecoder* owner) : owner_(owner) {}

  scoped_refptr<media::H264Picture> CreateH264Picture(bool is_for_output) override {
    DLOG("Got MultiAccelerator::CreateH264Picture");
    auto pic = owner_->GetUnusedReferenceFrame(is_for_output);
    if (!pic) {
      return nullptr;
    }
    return std::make_shared<AmlogicH264Picture>(pic);
  }

  Status SubmitFrameMetadata(const media::H264SPS* sps, const media::H264PPS* pps,
                             const media::H264DPB& dpb,
                             const media::H264Picture::Vector& ref_pic_listp0,
                             const media::H264Picture::Vector& ref_pic_listb0,
                             const media::H264Picture::Vector& ref_pic_listb1,
                             scoped_refptr<media::H264Picture> pic) override {
    DLOG("Got MultiAccelerator::SubmitFrameMetadata");
    ZX_DEBUG_ASSERT(owner_->is_decoder_started());
    ZX_DEBUG_ASSERT(!owner_->is_hw_active());
    auto ref_pic = static_cast<AmlogicH264Picture*>(pic.get())->internal_picture.lock();
    if (!ref_pic) {
      return Status::kFail;
    }
    // struct copy
    current_sps_ = *sps;
    owner_->SubmitFrameMetadata(ref_pic.get(), sps, pps, dpb);
    return Status::kOk;
  }

  Status SubmitSlice(const media::H264PPS* pps, const media::H264SliceHeader* slice_hdr,
                     const media::H264Picture::Vector& ref_pic_list0,
                     const media::H264Picture::Vector& ref_pic_list1,
                     scoped_refptr<media::H264Picture> pic, const uint8_t* data, size_t size,
                     const std::vector<media::SubsampleEntry>& subsamples) override {
    ZX_DEBUG_ASSERT(owner_->is_decoder_started());
    ZX_DEBUG_ASSERT(!owner_->is_hw_active());
    DLOG("Got MultiAccelerator::SubmitSlice");
    H264MultiDecoder::SliceData slice_data;
    // struct copy
    slice_data.sps = current_sps_;
    // struct copy
    slice_data.pps = *pps;
    // struct copy
    slice_data.header = *slice_hdr;
    slice_data.pic = pic;
    // vector copies
    slice_data.ref_pic_list0 = ref_pic_list0;
    slice_data.ref_pic_list1 = ref_pic_list1;
    owner_->SubmitSliceData(std::move(slice_data));
    return Status::kOk;
  }

  Status SubmitDecode(scoped_refptr<media::H264Picture> pic) override {
    ZX_DEBUG_ASSERT(owner_->is_decoder_started());
    ZX_DEBUG_ASSERT(!owner_->is_hw_active());
    auto ref_pic = static_cast<AmlogicH264Picture*>(pic.get())->internal_picture.lock();
    if (!ref_pic)
      return Status::kFail;
    DLOG("Got MultiAccelerator::SubmitDecode picture %d", ref_pic->index);
    return Status::kOk;
  }

  bool OutputPicture(scoped_refptr<media::H264Picture> pic) override {
    auto ref_pic = static_cast<AmlogicH264Picture*>(pic.get())->internal_picture.lock();
    if (!ref_pic)
      return false;
    ZX_DEBUG_ASSERT(ref_pic->in_internal_use);
    ref_pic->in_use = true;
    DLOG("Got MultiAccelerator::OutputPicture picture %d", ref_pic->index);
    owner_->OutputFrame(ref_pic.get(), pic->bitstream_id());
    return true;
  }

  void Reset() override {}

  Status SetStream(base::span<const uint8_t> stream,
                   const media::DecryptConfig* decrypt_config) override {
    ZX_DEBUG_ASSERT_MSG(false, "unreachable");
    return Status::kOk;
  }

 private:
  H264MultiDecoder* owner_;
  media::H264SPS current_sps_;
};

using InitFlagReg = AvScratch2;
using HeadPaddingReg = AvScratch3;
using H264DecodeModeReg = AvScratch4;
using H264DecodeSeqInfo = AvScratch5;
using NalSearchCtl = AvScratch9;
using ErrorStatusReg = AvScratch9;
using H264AuxAddr = AvScratchC;
using H264DecodeSizeReg = AvScratchE;
using H264AuxDataSize = AvScratchH;
using FrameCounterReg = AvScratchI;
using DpbStatusReg = AvScratchJ;
using LmemDumpAddr = AvScratchL;
using DebugReg1 = AvScratchM;
using DebugReg2 = AvScratchN;

using H264DecodeInfo = M4ControlReg;

// AvScratch1
class StreamInfo : public TypedRegisterBase<DosRegisterIo, StreamInfo, uint32_t> {
 public:
  DEF_FIELD(7, 0, width_in_mbs);
  DEF_FIELD(23, 8, total_mbs);

  // The upper_signficant bits are provided back to HW in some cases, but we don't (yet) know if
  // these bits really matter for that purpose.
  //
  // The amlogic code considers upper_signficant bits when determining whether to allocate buffers,
  // but this driver doesn't.
  //
  // Is this max_dec_frame_buffering?  It seems somewhat likely given that the amlogic driver bases
  // on this field in addition to mb_width and mb_height to decide whether to reallocate buffers,
  // and the value seems consistent enough so far.  Though it could also be another copy of
  // max_reference_size, or something else.  It doesn't appear to be max_num_reorder_frames
  // unfortunately.
  DEF_FIELD(30, 24, upper_significant);

  // This bit is not provided back to HW, and not considered by amlogic code or this driver for
  // determining whether to allocate buffers.
  DEF_FIELD(31, 31, insignificant);

  static auto Get() { return AddrType(0x09c1 * 4); }
};

// AvScratch2
class SequenceInfo : public TypedRegisterBase<DosRegisterIo, SequenceInfo, uint32_t> {
 public:
  DEF_BIT(0, aspect_ratio_info_present_flag);
  DEF_BIT(1, timing_info_present_flag);
  DEF_BIT(4, pic_struct_present_flag);

  // relatively lower-confidence vs. other bits - not confirmed
  DEF_BIT(6, fixed_frame_rate_flag);

  // This apparently is reliably 3 for 4:2:2 separate color plane, or not 3.
  // For non-IDC 4:2:0 frames, this can be 0 instead of the 1 it seems like it should be.
  DEF_FIELD(14, 13, chroma_format_idc);
  DEF_BIT(15, frame_mbs_only_flag);
  DEF_FIELD(23, 16, aspect_ratio_idc);

  // Bits 24 to 31 seem to be zero regardless of low-latency stream or stream with frame reordering.

  static auto Get() { return AddrType(0x09c2 * 4); }
};

// AvScratch6
class CropInfo : public TypedRegisterBase<DosRegisterIo, CropInfo, uint32_t> {
 public:
  // All quantities are the number of pixels to be cropped from each side.
  DEF_FIELD(7, 0, bottom);
  DEF_FIELD(15, 8, top);  // Ignored and unconfirmed
  DEF_FIELD(23, 16, right);
  DEF_FIELD(31, 24, left);  // Ignored and unconfirmed

  static auto Get() { return AddrType(0x09c6 * 4); }
};

// AvScratchB
class StreamInfo2 : public TypedRegisterBase<DosRegisterIo, StreamInfo2, uint32_t> {
 public:
  DEF_FIELD(7, 0, level_idc);
  DEF_FIELD(15, 8, max_reference_size);

  // Bits 16 to 31 seem to be zero regardless of low-latency stream or stream with frame reordering.

  static auto Get() { return AddrType(0x09cb * 4); }
};

// AvScratchF
class CodecSettings : public TypedRegisterBase<DosRegisterIo, CodecSettings, uint32_t> {
 public:
  DEF_BIT(1, trickmode_i);
  DEF_BIT(2, zeroed0);
  DEF_BIT(3, drop_b_frames);
  DEF_BIT(4, error_recovery_mode);
  DEF_BIT(5, zeroed1);
  DEF_BIT(6, ip_frames_only);
  DEF_BIT(7, disable_fast_poc);

  static auto Get() { return AddrType(0x09cf * 4); }
};

enum DecodeMode {
  // Mode where multiple streams can be decoded, and input doesn't have to be
  // broken into frame-sized chunks.
  kDecodeModeMultiStreamBased = 0x2
};

// Actions written by CPU into DpbStatusReg to tell the firmware what to do.
enum H264Action {
  // Start searching for the head of a frame to decode.
  //
  // Because the decode strategy for partial frames is to re-attempt frame decode later with more
  // input data present, this is always the way we start searching for and decoding a frame.  There
  // is no such thing as saving/restoring state in the middle of a frame decode - only re-attempting
  // the decode from the same saved state again later with more input data.
  kH264ActionSearchHead = 0xf0,

  // Done responding to a config request.
  kH264ActionConfigDone = 0xf2,

  // Decode a slice (not the first one) in a picture.
  kH264ActionDecodeSlice = 0xf1,

  // Decode the first slice in a new picture.
  kH264ActionDecodeNewpic = 0xf3,

  // Continue decoding.  IDK if we really need to use this.
  kH264ActionDecodeStart = 0xff,
};

// Actions written by the firmware into DpbStatusReg before an interrupt to tell
// the CPU what to do.
enum H264Status {
  // Configure the DPB.
  kH264ConfigRequest = 0x11,

  // Out of input data, so get more.
  kH264DataRequest = 0x12,

  // The firmware was in the middle of processing a NALU, and it was potentially processing fine,
  // but the firmware ran out of input data before processing was complete.  We handle this and
  // kH264SearchBufEmpty the same way, by re-attempting decode starting at the same saved state
  // again after adding more input data, in the hope that we'll get kH264PicDataDone before
  // kH264DecodeBufEmpty or kH264SearchBufEmpty.
  kH264DecodeBufEmpty = 0x20,

  // The firmware detected the hardware timed out while attempting to decode.
  kH264DecodeTimeout = 0x21,

  // kH264ActionSearchHead wasn't able to find a frame to decode.  See kH264DecodeBufEmpty
  // comments.
  kH264SearchBufEmpty = 0x22,

  // Initialize the current set of reference frames and output buffer to be
  // decoded into.
  kH264SliceHeadDone = 0x1,

  // Store the current frame into the DPB, or output it.
  kH264PicDataDone = 0x2,
};

const char* H264MultiDecoder::DecoderStateName(DecoderState state) {
  switch (state) {
    case DecoderState::kSwappedOut:
      return "SwappedOut";
    case DecoderState::kWaitingForInputOrOutput:
      return "WaitingForInputOrOutput";
    case DecoderState::kWaitingForConfigChange:
      return "WaitingForConfigChange";
    case DecoderState::kRunning:
      return "Running";
    default:
      return "UNKNOWN";
  }
}

static bool ProfileHasChromaFormatIdc(uint32_t profile_idc) {
  // From 7.3.2.1.1
  switch (profile_idc) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135:
      return true;
    default:
      return false;
  }
}

H264MultiDecoder::H264MultiDecoder(Owner* owner, Client* client, FrameDataProvider* provider,
                                   std::optional<InternalBuffers> internal_buffers, bool is_secure)
    : VideoDecoder(
          media_metrics::
              StreamProcessorEvents2MigratedMetricDimensionImplementation_AmlogicDecoderH264,
          owner, client, is_secure),
      frame_data_provider_(provider) {
  DLOG("create");
  media_decoder_ = std::make_unique<media::H264Decoder>(std::make_unique<MultiAccelerator>(this),
                                                        media::H264PROFILE_HIGH);
  use_parser_ = true;
  power_ref_ = std::make_unique<PowerReference>(owner_->vdec1_core());

  if (internal_buffers.has_value()) {
    GiveInternalBuffers(std::move(internal_buffers.value()));
  }
}

void H264MultiDecoder::ForceStopDuringRemoveLocked() {
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  owner_->watchdog()->Cancel();
  is_hw_active_ = false;
  owner_->core()->StopDecoding();
  is_decoder_started_ = false;
  owner_->core()->WaitForIdle();
}

H264MultiDecoder::~H264MultiDecoder() {
  if (owner_->IsDecoderCurrent(this)) {
    owner_->watchdog()->Cancel();
    is_hw_active_ = false;
    owner_->core()->StopDecoding();
    is_decoder_started_ = false;
    owner_->core()->WaitForIdle();
  }
  BarrierBeforeRelease();
  DLOG("delete");
}

zx_status_t H264MultiDecoder::Initialize() {
  TRACE_DURATION("media", "H264MultiDecoder::Initialize");
  zx_status_t status = InitializeBuffers();
  if (status != ZX_OK) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_InitializationError);
    LOG(ERROR, "Failed to initialize buffers");
    return status;
  }

  return InitializeHardware();
}

zx_status_t H264MultiDecoder::LoadSecondaryFirmware(const uint8_t* data, uint32_t firmware_size) {
  TRACE_DURATION("media", "H264MultiDecoder::LoadSecondaryFirmware");
  ZX_DEBUG_ASSERT(!secondary_firmware_);
  // For some reason, some portions of the firmware aren't loaded into the
  // hardware directly, but are kept in main memory.
  constexpr uint32_t kSecondaryFirmwareSize = 4 * 1024;
  // Some sections of the input firmware are copied into multiple places in the output buffer, and 1
  // part of the output buffer seems to be unused.
  constexpr uint32_t kFirmwareSectionCount = 9;
  constexpr uint32_t kSecondaryFirmwareBufferSize = kSecondaryFirmwareSize * kFirmwareSectionCount;
  constexpr uint32_t kBufferAlignShift = 16;

  if (on_deck_internal_buffers_.has_value() &&
      on_deck_internal_buffers_->secondary_firmware_.has_value()) {
    auto& on_deck_secondary_firmware = on_deck_internal_buffers_->secondary_firmware_;
    ZX_DEBUG_ASSERT(on_deck_secondary_firmware->size() == kSecondaryFirmwareBufferSize);
    ZX_DEBUG_ASSERT(on_deck_secondary_firmware->alignment() == 1 << kBufferAlignShift);
    ZX_DEBUG_ASSERT(on_deck_secondary_firmware->is_secure() == false);
    ZX_DEBUG_ASSERT(on_deck_secondary_firmware->is_writable() == true);
    ZX_DEBUG_ASSERT(on_deck_secondary_firmware->is_mapping_needed() == true);
    secondary_firmware_ = std::move(on_deck_secondary_firmware);
    on_deck_secondary_firmware.reset();
  } else {
    auto result = InternalBuffer::CreateAligned(
        "H264MultiSecondaryFirmware", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
        kSecondaryFirmwareBufferSize, 1 << kBufferAlignShift, /*is_secure*/ false,
        /*is_writable=*/true, /*is_mapping_needed*/ true);
    if (!result.is_ok()) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_AllocationError);
      LOG(ERROR, "Failed to make second firmware buffer: %d", result.error());
      return result.error();
    }
    secondary_firmware_.emplace(result.take_value());
    auto addr = static_cast<uint8_t*>(secondary_firmware_->virt_base());
    // The secondary firmware is in a different order in the file than the main
    // firmware expects it to have.
    memcpy(addr + 0, data + 0x4000, kSecondaryFirmwareSize);                // header
    memcpy(addr + 0x1000, data + 0x2000, kSecondaryFirmwareSize);           // data
    memcpy(addr + 0x2000, data + 0x6000, kSecondaryFirmwareSize);           // mmc
    memcpy(addr + 0x3000, data + 0x3000, kSecondaryFirmwareSize);           // list
    memcpy(addr + 0x4000, data + 0x5000, kSecondaryFirmwareSize);           // slice
    memcpy(addr + 0x5000, data, 0x2000);                                    // main
    memcpy(addr + 0x5000 + 0x2000, data + 0x2000, kSecondaryFirmwareSize);  // data copy 2
    memcpy(addr + 0x5000 + 0x3000, data + 0x5000, kSecondaryFirmwareSize);  // slice copy 2
    ZX_DEBUG_ASSERT(0x5000 + 0x3000 + kSecondaryFirmwareSize == kSecondaryFirmwareBufferSize);

    // Flush the secondary firmware out to RAM.
    secondary_firmware_->CacheFlush(0, kSecondaryFirmwareBufferSize);
  }

  return ZX_OK;
}

constexpr uint32_t kAuxBufPrefixSize = 16 * 1024;
constexpr uint32_t kAuxBufSuffixSize = 0;

zx_status_t H264MultiDecoder::InitializeBuffers() {
  TRACE_DURATION("media", "H264MultiDecoder::InitializeBuffers");

  const uint32_t kBufferAlignShift = 16;
  const uint32_t kBufferAlignment = 1 << kBufferAlignShift;

  // If the TEE is available, we'll do secure loading of the firmware in InitializeHardware().
  if (!owner_->is_tee_available()) {
    // TODO(fxbug.dev/43496): Fix this up in "CL4" to filter to the current SoC as we're loading
    // video_ucode.bin, similar to how the video_firmware TA does filtering.  That way
    // kDec_H264_Multi will be for the correct SoC (assuming new video_ucode.bin).  At the moment,
    // if we were to take this path (which we won't for now), we'd likely get the wrong firmware
    // since there will be more than one firmware that matches kDec_H264_Multi, for different
    // SoC(s).
    FirmwareBlob::FirmwareType firmware_type = FirmwareBlob::FirmwareType::kDec_H264_Multi;
    uint8_t* data;
    uint32_t firmware_size;
    zx_status_t status =
        owner_->firmware_blob()->GetFirmwareData(firmware_type, &data, &firmware_size);
    if (status != ZX_OK)
      return status;
    static constexpr uint32_t kFirmwareSize = 4 * 4096;
    if (firmware_size < kFirmwareSize) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_FirmwareSizeError);
      LOG(ERROR, "Firmware too small");
      return ZX_ERR_INTERNAL;
    }

    constexpr uint32_t kFirmwareAlignment = kBufferAlignment;
    constexpr uint32_t kFirmwareIsSecure = false;
    constexpr uint32_t kFirmwareIsWritable = true;
    constexpr uint32_t kFirmwareIsMappingNeeded = true;
    if (on_deck_internal_buffers_.has_value() && on_deck_internal_buffers_->firmware_.has_value()) {
      auto& on_deck_firmware = on_deck_internal_buffers_->firmware_;
      ZX_DEBUG_ASSERT(on_deck_firmware->size() == kFirmwareSize);
      ZX_DEBUG_ASSERT(on_deck_firmware->alignment() == kFirmwareAlignment);
      ZX_DEBUG_ASSERT(on_deck_firmware->is_secure() == kFirmwareIsSecure);
      ZX_DEBUG_ASSERT(on_deck_firmware->is_writable() == kFirmwareIsWritable);
      ZX_DEBUG_ASSERT(on_deck_firmware->is_mapping_needed() == kFirmwareIsMappingNeeded);
      firmware_ = std::move(on_deck_firmware);
      on_deck_firmware.reset();
    } else {
      auto create_result = InternalBuffer::CreateAligned(
          "H264MultiFirmware", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(), kFirmwareSize,
          1 << kBufferAlignShift, /*is_secure=*/kFirmwareIsSecure,
          /*is_writable=*/kFirmwareIsWritable,
          /*is_mapping_needed=*/kFirmwareIsMappingNeeded);
      if (!create_result.is_ok()) {
        LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_AllocationError);
        LOG(ERROR, "Failed to make firmware buffer - %d", create_result.error());
        return {};
      }
      firmware_ = create_result.take_value();
      memcpy(firmware_->virt_base(), data, kFirmwareSize);
      // Flush the firmware out to RAM.
      firmware_->CacheFlush(0, kFirmwareSize);
    }

    status = LoadSecondaryFirmware(data, firmware_size);
    if (status != ZX_OK) {
      return status;
    }
  }

  constexpr uint32_t kCodecDataSize = 0x200000;
  constexpr uint32_t kCodecDataAlignment = kBufferAlignment;
  constexpr uint32_t kCodecDataIsWritable = true;
  constexpr uint32_t kCodecDataIsMappingNeeded = false;
  if (on_deck_internal_buffers_.has_value() && on_deck_internal_buffers_->codec_data_.has_value() &&
      (on_deck_internal_buffers_->codec_data_->is_secure() != is_secure())) {
    // For now we free this rather than keeping both secure and non-secure around, since switching
    // isn't likely to happen much within a single CodecAdapterH264Multi.
    on_deck_internal_buffers_->codec_data_.reset();
  }
  if (on_deck_internal_buffers_.has_value() && on_deck_internal_buffers_->codec_data_.has_value()) {
    auto& on_deck_codec_data = on_deck_internal_buffers_->codec_data_;
    ZX_DEBUG_ASSERT(on_deck_codec_data->size() == kCodecDataSize);
    ZX_DEBUG_ASSERT(on_deck_codec_data->alignment() == kCodecDataAlignment);
    ZX_DEBUG_ASSERT(on_deck_codec_data->is_secure() == is_secure());
    ZX_DEBUG_ASSERT(on_deck_codec_data->is_writable() == kCodecDataIsWritable);
    ZX_DEBUG_ASSERT(on_deck_codec_data->is_mapping_needed() == kCodecDataIsMappingNeeded);
    codec_data_ = std::move(on_deck_codec_data);
    on_deck_codec_data.reset();
  } else {
    auto codec_data_create_result = InternalBuffer::CreateAligned(
        "H264MultiCodecData", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(), kCodecDataSize,
        kCodecDataAlignment, is_secure(),
        /*is_writable=*/kCodecDataIsWritable, /*is_mapping_needed*/ kCodecDataIsMappingNeeded);
    if (!codec_data_create_result.is_ok()) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_AllocationError);
      LOG(ERROR, "Failed to make codec data buffer - status: %d", codec_data_create_result.error());
      return codec_data_create_result.error();
    }
    codec_data_.emplace(codec_data_create_result.take_value());
  }

  // Aux buf seems to be used for reading SEI data.
  constexpr uint32_t kAuxBufSize = kAuxBufPrefixSize + kAuxBufSuffixSize;
  constexpr uint32_t kAuxBufAlignment = kBufferAlignment;
  constexpr uint32_t kAuxBufIsSecure = false;
  constexpr uint32_t kAuxBufIsWritable = true;
  constexpr uint32_t kAuxBufIsMappingNeeded = false;
  if (on_deck_internal_buffers_.has_value() && on_deck_internal_buffers_->aux_buf_.has_value()) {
    auto& on_deck_aux_buf = on_deck_internal_buffers_->aux_buf_;
    ZX_DEBUG_ASSERT(on_deck_aux_buf->size() == kAuxBufSize);
    ZX_DEBUG_ASSERT(on_deck_aux_buf->alignment() == kAuxBufAlignment);
    ZX_DEBUG_ASSERT(on_deck_aux_buf->is_secure() == kAuxBufIsSecure);
    ZX_DEBUG_ASSERT(on_deck_aux_buf->is_writable() == kAuxBufIsWritable);
    ZX_DEBUG_ASSERT(on_deck_aux_buf->is_mapping_needed() == kAuxBufIsMappingNeeded);
    aux_buf_ = std::move(on_deck_aux_buf);
    on_deck_aux_buf.reset();
  } else {
    auto aux_buf_create_result = InternalBuffer::CreateAligned(
        "H264AuxBuf", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(), kAuxBufSize,
        kAuxBufAlignment, /*is_secure=*/kAuxBufIsSecure,
        /*is_writable=*/kAuxBufIsWritable, /*is_mapping_needed*/ kAuxBufIsMappingNeeded);
    if (!aux_buf_create_result.is_ok()) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_AllocationError);
      LOG(ERROR, "Failed to make aux buffer - status: %d", aux_buf_create_result.error());
      return aux_buf_create_result.error();
    }
    aux_buf_.emplace(aux_buf_create_result.take_value());
  }

  // Lmem is used to dump the AMRISC's local memory, which is needed for updating the DPB.
  constexpr uint32_t kLmemSize = 4096;
  constexpr uint32_t kLmemAlignment = kBufferAlignment;
  constexpr uint32_t kLmemIsSecure = false;
  constexpr uint32_t kLmemIsWritable = true;
  constexpr uint32_t kLmemIsMappingNeeded = true;
  if (on_deck_internal_buffers_.has_value() && on_deck_internal_buffers_->lmem_.has_value()) {
    auto& on_deck_lmem = on_deck_internal_buffers_->lmem_;
    ZX_DEBUG_ASSERT(on_deck_lmem->size() == kLmemSize);
    ZX_DEBUG_ASSERT(on_deck_lmem->alignment() == kLmemAlignment);
    ZX_DEBUG_ASSERT(on_deck_lmem->is_secure() == kLmemIsSecure);
    ZX_DEBUG_ASSERT(on_deck_lmem->is_mapping_needed() == kLmemIsMappingNeeded);
    lmem_ = std::move(on_deck_lmem);
    on_deck_lmem.reset();
  } else {
    auto lmem_create_result = InternalBuffer::CreateAligned(
        "H264Lmem", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(), kLmemSize, kLmemAlignment,
        /*is_secure=*/kLmemIsSecure,
        /*is_writable=*/kLmemIsWritable, /*is_mapping_needed*/ kLmemIsMappingNeeded);
    if (!lmem_create_result.is_ok()) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_AllocationError);
      LOG(ERROR, "Failed to make lmem buffer - status: %d", lmem_create_result.error());
      return lmem_create_result.error();
    }
    lmem_.emplace(lmem_create_result.take_value());
  }

  return ZX_OK;
}

void H264MultiDecoder::ResetHardware() {
  TRACE_DURATION("media", "H264MultiDecoder::ResetHardware");

  if (!WaitForRegister(std::chrono::milliseconds(100), [this]() {
        return !(DcacDmaCtrl::Get().ReadFrom(owner_->dosbus()).reg_value() & 0x8000);
      })) {
    DECODE_ERROR("Waiting for DCAC DMA timed out");
    return;
  }

  if (!WaitForRegister(std::chrono::milliseconds(100), [this]() {
        return !(LmemDmaCtrl::Get().ReadFrom(owner_->dosbus()).reg_value() & 0x8000);
      })) {
    DECODE_ERROR("Waiting for LMEM DMA timed out");
    return;
  }

  DosSwReset0::Get().FromValue(0).set_vdec_mc(1).set_vdec_iqidct(1).set_vdec_vld_part(1).WriteTo(
      owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Reads are used for delaying running later code.
  for (uint32_t i = 0; i < 3; i++) {
    DosSwReset0::Get().ReadFrom(owner_->dosbus());
  }

  DosSwReset0::Get().FromValue(0).set_vdec_mc(1).set_vdec_iqidct(1).set_vdec_vld_part(1).WriteTo(
      owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Reads are used for delaying running later code.
  for (uint32_t i = 0; i < 3; i++) {
    DosSwReset0::Get().ReadFrom(owner_->dosbus());
  }

  DosSwReset0::Get().FromValue(0).set_vdec_pic_dc(1).set_vdec_dblk(1).WriteTo(owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Reads are used for delaying running later code.
  for (uint32_t i = 0; i < 3; i++) {
    DosSwReset0::Get().ReadFrom(owner_->dosbus());
  }

  auto temp = PowerCtlVld::Get().ReadFrom(owner_->dosbus());
  temp.set_reg_value(temp.reg_value() | (1 << 9) | (1 << 6));
  temp.WriteTo(owner_->dosbus());

  PscaleCtrl::Get().FromValue(0).WriteTo(owner_->dosbus());

  is_hw_active_ = false;
  is_decoder_started_ = false;
}

zx_status_t H264MultiDecoder::InitializeHardware() {
  TRACE_DURATION("media", "H264MultiDecoder::InitializeHardware");
  ZX_DEBUG_ASSERT(state_ == DecoderState::kSwappedOut);
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  zx_status_t status =
      owner_->SetProtected(VideoDecoder::Owner::ProtectableHardwareUnit::kVdec, is_secure());
  if (status != ZX_OK)
    return status;

  if (owner_->is_tee_available()) {
    // The video_firmware TA has already filtered down to the codec core firmwares that are for
    // the current SoC, and video_ucode.bin (newer verions) ID the firmware using the more-generic
    // ID that's not SoC-specific.
    status = owner_->TeeSmcLoadVideoFirmware(FirmwareBlob::FirmwareType::kDec_H264_Multi,
                                             FirmwareBlob::FirmwareVdecLoadMode::kCompatible);
    if (status != ZX_OK) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_FirmwareLoadError);
      LOG(ERROR, "owner_->TeeSmcLoadVideoFirmware() failed - status: %d", status);
      return status;
    }

    ResetHardware();
  } else {
    // If the tee is not available, the secondary firmware was already loaded during
    // InitializeBuffers().
    ZX_DEBUG_ASSERT(firmware_);
    status = owner_->core()->LoadFirmware(*firmware_);
    if (status != ZX_OK)
      return status;

    ResetHardware();
    AvScratchG::Get()
        .FromValue(truncate_to_32(secondary_firmware_->phys_base()))
        .WriteTo(owner_->dosbus());
  }

  PscaleCtrl::Get().FromValue(0).WriteTo(owner_->dosbus());
  VdecAssistMbox1ClrReg::Get().FromValue(1).WriteTo(owner_->dosbus());
  VdecAssistMbox1Mask::Get().FromValue(1).WriteTo(owner_->dosbus());
  {
    auto temp = MdecPicDcCtrl::Get().ReadFrom(owner_->dosbus()).set_nv12_output(true);
    temp.WriteTo(owner_->dosbus());

    temp = MdecPicDcCtrl::Get().ReadFrom(owner_->dosbus());
    temp.set_reg_value(temp.reg_value() | (0xbf << 24));
    temp.WriteTo(owner_->dosbus());

    temp = MdecPicDcCtrl::Get().ReadFrom(owner_->dosbus());
    temp.set_reg_value(temp.reg_value() & ~(0xbf << 24));
    temp.WriteTo(owner_->dosbus());

    MdecPicDcCtrl::Get().ReadFrom(owner_->dosbus()).set_bit31(0).WriteTo(owner_->dosbus());
  }

  MdecPicDcMuxCtrl::Get().ReadFrom(owner_->dosbus()).set_bit31(0).WriteTo(owner_->dosbus());
  MdecExtIfCfg1::Get().FromValue(0).WriteTo(owner_->dosbus());
  MdecPicDcThresh::Get().FromValue(0x404038aa).WriteTo(owner_->dosbus());

  // Signal that the DPB hasn't been initialized yet.
  if (video_frames_.size() > 0) {
    AvScratch7::Get()
        .FromValue(static_cast<uint32_t>((next_max_reference_size_ << 24) |
                                         (video_frames_.size() << 16) |
                                         (video_frames_.size() << 8)))
        .WriteTo(owner_->dosbus());
    for (auto& frame : video_frames_) {
      VdecAssistCanvasBlk32::Get()
          .FromValue(0)
          .set_canvas_blk32_wr(true)
          .set_canvas_blk32_is_block(false)
          .set_canvas_index_wr(true)
          .set_canvas_index(frame->y_canvas->index())
          .WriteTo(owner_->dosbus());
      VdecAssistCanvasBlk32::Get()
          .FromValue(0)
          .set_canvas_blk32_wr(true)
          .set_canvas_blk32_is_block(false)
          .set_canvas_index_wr(true)
          .set_canvas_index(frame->uv_canvas->index())
          .WriteTo(owner_->dosbus());
      AncNCanvasAddr::Get(frame->index)
          .FromValue((frame->uv_canvas->index() << 16) | (frame->uv_canvas->index() << 8) |
                     (frame->y_canvas->index()))
          .WriteTo(owner_->dosbus());
    }
  } else {
    AvScratch0::Get().FromValue(0).WriteTo(owner_->dosbus());
    AvScratch9::Get().FromValue(0).WriteTo(owner_->dosbus());
  }

  // The amlogic driver sets to kH264ActionDecodeStart if have_initialized_ essentially, but 0 seems
  // to work fine here.
  DpbStatusReg::Get().FromValue(0).WriteTo(owner_->dosbus());

  FrameCounterReg::Get().FromValue(0).WriteTo(owner_->dosbus());

  constexpr uint32_t kBufferStartAddressOffset = 0x1000000;
  constexpr uint32_t kDcacReadMargin = 64 * 1024;
  uint32_t buffer_offset =
      truncate_to_32(codec_data_->phys_base()) - kBufferStartAddressOffset + kDcacReadMargin;
  AvScratch8::Get().FromValue(buffer_offset).WriteTo(owner_->dosbus());

  CodecSettings::Get()
      .ReadFrom(owner_->dosbus())
      .set_drop_b_frames(0)
      .set_zeroed0(0)
      .set_error_recovery_mode(1)
      .set_zeroed1(0)
      .set_ip_frames_only(0)
      .WriteTo(owner_->dosbus());

  LmemDumpAddr::Get().FromValue(truncate_to_32(lmem_->phys_base())).WriteTo(owner_->dosbus());

  // The amlogic driver writes this again, so we do also.
  MdecPicDcThresh::Get().FromValue(0x404038aa).WriteTo(owner_->dosbus());

  DebugReg1::Get().FromValue(0).WriteTo(owner_->dosbus());
  DebugReg2::Get().FromValue(0).WriteTo(owner_->dosbus());

  if (saved_iqidct_ctrl_) {
    IqidctCtrl::Get().FromValue(*saved_iqidct_ctrl_).WriteTo(owner_->dosbus());
  }
  if (saved_vcop_ctrl_) {
    VcopCtrl::Get().FromValue(*saved_vcop_ctrl_).WriteTo(owner_->dosbus());
  }
  if (saved_vld_decode_ctrl_) {
    VldDecodeCtrl::Get().FromValue(*saved_vld_decode_ctrl_).WriteTo(owner_->dosbus());
  }

  H264DecodeInfo::Get().FromValue(1 << 13).WriteTo(owner_->dosbus());
  constexpr uint32_t kDummyDoesNothingBytesToDecode = 100000;
  H264DecodeSizeReg::Get().FromValue(kDummyDoesNothingBytesToDecode).WriteTo(owner_->dosbus());
  ViffBitCnt::Get().FromValue(kBytesToDecode * 8).WriteTo(owner_->dosbus());

  // configure aux buffer
  H264AuxAddr::Get().FromValue(truncate_to_32(aux_buf_->phys_base())).WriteTo(owner_->dosbus());
  H264AuxDataSize::Get()
      .FromValue(((kAuxBufPrefixSize / 16) << 16) | (kAuxBufSuffixSize / 16))
      .WriteTo(owner_->dosbus());

  // configure decode mode
  H264DecodeModeReg::Get().FromValue(kDecodeModeMultiStreamBased).WriteTo(owner_->dosbus());
  H264DecodeSeqInfo::Get().FromValue(seq_info2_).WriteTo(owner_->dosbus());
  HeadPaddingReg::Get().FromValue(0).WriteTo(owner_->dosbus());
  // It's unclear whether configure_dpb_seen_ is exactly what belongs here, but so far this seems to
  // work better than anything else we've tried.  Beware that always passing 0 or 1 here may
  // initially appear to work, but actually can cause subtle glitches decoding frames later in a
  // stream (reason unknown).  Frame ordinal 15 of bear.h264 is known to glitch (infrequently) when
  // this is set to constant 0 or constant 1.  It's possible that !video_frames_.empty() would work
  // here.  If this is set to constant 0, decoding past the first frame may not work, or it may work
  // and glitch a frame later on in the stream at low repro rate.  Using input_context() != nullptr
  // may also work here.  When SEI, SPS, PPS are delivered separately from the first frame, this
  // needs to be 0 roughly until the first frame is encountered.  Because we currently require
  // frames to be delivered in their entirety, we don't yet need to know exactly how far into the
  // first frame implies setting this to 1.
  InitFlagReg::Get().FromValue(configure_dpb_seen_).WriteTo(owner_->dosbus());
  have_initialized_ = true;

  // TODO(fxbug.dev/13483): Set to 1 when SEI is supported.
  NalSearchCtl::Get().FromValue(0).WriteTo(owner_->dosbus());

  state_ = DecoderState::kWaitingForInputOrOutput;
  return ZX_OK;
}

void H264MultiDecoder::StartFrameDecode() {
  TRACE_DURATION("media", "H264MultiDecoder::StartFrameDecode");
  ZX_DEBUG_ASSERT(state_ == DecoderState::kWaitingForInputOrOutput);

  if (unwrapped_first_slice_header_of_frame_decoded_stream_offset_decode_tried_ ==
          unwrapped_first_slice_header_of_frame_decoded_stream_offset_ &&
      unwrapped_write_stream_offset_decode_tried_ == unwrapped_write_stream_offset_ &&
      per_frame_seen_first_mb_in_slice_ == per_frame_decoded_first_mb_in_slice_) {
    // This is the second time we're trying the exact same decode, despite having not decoded
    // anything on the first try.  This can happen if the input data is broken or a client is
    // queueing more PTS values than frames.  In these cases we fail the stream.
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_StuckError);
    LOG(ERROR, "no progress being made");
    OnFatalError();
    return;
  }
  unwrapped_first_slice_header_of_frame_decoded_stream_offset_decode_tried_ =
      unwrapped_first_slice_header_of_frame_decoded_stream_offset_;
  unwrapped_write_stream_offset_decode_tried_ = unwrapped_write_stream_offset_;

  per_frame_attempt_seen_first_mb_in_slice_ = -1;

  ZX_DEBUG_ASSERT(!is_decoder_started_);
  ViffBitCnt::Get().FromValue(kBytesToDecode * 8).WriteTo(owner_->dosbus());
  owner_->core()->StartDecoding();
  is_decoder_started_ = true;

  DpbStatusReg::Get().FromValue(kH264ActionSearchHead).WriteTo(owner_->dosbus());

  state_ = DecoderState::kRunning;
  is_hw_active_ = true;
  owner_->watchdog()->Start();
}

void H264MultiDecoder::ConfigureDpb() {
  TRACE_DURATION("media", "H264MultiDecoder::ConfigureDpb");
  ZX_DEBUG_ASSERT(is_decoder_started_);
  ZX_DEBUG_ASSERT(is_hw_active_);
  owner_->watchdog()->Cancel();
  is_hw_active_ = false;

  configure_dpb_seen_ = true;

  saved_iqidct_ctrl_ = IqidctCtrl::Get().ReadFrom(owner_->dosbus()).reg_value();

  // The HW is told to continue decoding by writing DPB sizes to AvScratch0.  This can happen
  // immediately if the BufferCollection is already suitable, or after new sysmem allocation if
  // BufferCollection isn't suitable.

  // StreamInfo (aka AvScratch1)
  const auto seq_info2_value = StreamInfo::Get().ReadFrom(owner_->dosbus()).reg_value();
  auto seq_info2_tmp = StreamInfo::Get().FromValue(seq_info2_value);
  seq_info2_tmp.set_insignificant(0);
  // For local use in this method.
  const auto stream_info = StreamInfo::Get().FromValue(seq_info2_tmp.reg_value());
  // Stash for potentially restoring state in InitializeHardware().
  seq_info2_ = stream_info.reg_value();

  // SequenceInfo (aka AvScratch2)
  const auto sequence_info = SequenceInfo::Get().ReadFrom(owner_->dosbus());

  // CropInfo (aka AvScratch6)
  const auto crop_info = CropInfo::Get().ReadFrom(owner_->dosbus());

  // StreamInfo2 (aka AvScratchB)
  const auto stream_info2 = StreamInfo2::Get().ReadFrom(owner_->dosbus());

  if (!sequence_info.frame_mbs_only_flag()) {
    LogEvent(media_metrics::
                 StreamProcessorEvents2MigratedMetricDimensionEvent_InterlacedUnsupportedError);
    LOG(ERROR, "!sequence_info.frame_mbs_only_flag() - not supported");
    OnFatalError();
    return;
  }

  uint32_t mb_width = stream_info.width_in_mbs();
  // The maximum supported image width is 4096 bytes. The value of width_in_mbs should be 256 in
  // that case, but it wraps around since the field is only 8 bits. We need to correct for that
  // special case.
  if (!mb_width && stream_info.total_mbs())
    mb_width = 256;
  if (!mb_width) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_ZeroMbWidthError);
    LOG(ERROR, "0 mb_width");
    OnFatalError();
    return;
  }
  uint32_t mb_height = stream_info.total_mbs() / mb_width;

  uint32_t coded_width = mb_width * 16;
  uint32_t coded_height = mb_height * 16;
  constexpr uint32_t kMaxDimension = 4096;  // for both width and height.
  if (coded_width > kMaxDimension || coded_height > kMaxDimension) {
    LogEvent(
        media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_DimensionTooLargeError);
    LOG(ERROR, "Unsupported dimensions %dx%d", coded_width, coded_height);
    OnFatalError();
    return;
  }

  uint32_t stride = fbl::round_up(coded_width, kStrideAlignment);
  if (coded_width <= crop_info.right()) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_CropInfoError);
    LOG(ERROR, "coded_width <= crop_info.right()");
    OnFatalError();
    return;
  }
  uint32_t display_width = coded_width - crop_info.right();
  if (coded_height <= crop_info.bottom()) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_CropInfoError);
    LOG(ERROR, "coded_height <= crop_info.bottom()");
    OnFatalError();
    return;
  }
  uint32_t display_height = coded_height - crop_info.bottom();

  // Compute max_dpb_size.  For a conformant stream, max_num_ref_frames is in the range
  // 0..max_dpb_frames, but take the max below anyway.  This is mostly adapted from H264Decoder's
  // DPB sizing code (but we need to know the DPB size before the fake SPS is with H264Decoder).
  uint32_t max_num_ref_frames = stream_info2.max_reference_size();
  uint32_t level = stream_info2.level_idc();
  if (level != 0) {
    hw_level_idc_ = level;
  } else {
    level = hw_level_idc_;
  }
  if (level == 0) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_LevelZeroError);
    LOG(ERROR, "level == 0");
    OnFatalError();
    return;
  }
  if (level > std::numeric_limits<uint8_t>::max()) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_LevelTooLargeError);
    LOG(ERROR, "level > std::numeric_limits<uint8_t>()::max()");
    OnFatalError();
    return;
  }
  uint32_t max_dpb_mbs = media::H264LevelToMaxDpbMbs(static_cast<uint8_t>(level));
  if (!max_dpb_mbs) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_MaxDpbMbsError);
    LOG(ERROR, "!max_dpb_mbs");
    OnFatalError();
    return;
  }
  // MaxDpbFrames from level limits per spec.
  uint32_t max_dpb_frames = std::min(max_dpb_mbs / (mb_width * mb_height),
                                     static_cast<uint32_t>(media::H264DPB::kDPBMaxSize));
  // Set DPB size to at least the level limit, or what the stream requires.
  uint32_t max_dpb_size = std::max(max_dpb_frames, max_num_ref_frames);

  uint32_t min_frame_count =
      std::min(max_dpb_size, static_cast<uint32_t>(media::H264DPB::kDPBMaxSize)) + 1;
  static constexpr uint32_t max_frame_count = 24;

  // Now we determine if new buffers are needed, and whether we need to re-config the decoder's
  // notion of the buffers.  The "new" in this variable name does not prevent the buffers we get
  // later from being buffers that were previously used by a previous H264MultiDecoder instance.
  // This allows us to continue using the same buffers across a seek, or across any two consecutive
  // streams at the StreamProcessor level, as long as the old buffers are suitable for the new
  // config (buffer size big enough, enough buffers, sysmem image format constraints are ok, etc).
  bool new_frames_needed = false;
  bool config_update_needed = false;
  if (video_frames_.empty()) {
    // The frames this decoder instance gets in InitializedFrames() _may_ be using the same buffers
    // as were previously used by a previous H264MultiDecoder instance.
    new_frames_needed = true;
    config_update_needed = true;
  }
  if (!new_frames_needed && !client_->IsCurrentOutputBufferCollectionUsable(
                                min_frame_count, max_frame_count, coded_width, coded_height, stride,
                                display_width, display_height)) {
    DLOG("!IsCurrentOutputBufferCollectionUsable()");
    new_frames_needed = true;
  }
  if (new_frames_needed) {
    config_update_needed = true;
  }
  if (!config_update_needed) {
    if (hw_coded_width_ != coded_width || hw_coded_height_ != coded_height ||
        hw_stride_ != stride || hw_display_width_ != display_width ||
        hw_display_height_ != display_height) {
      config_update_needed = true;
    }
  }
  ZX_DEBUG_ASSERT(!new_frames_needed || config_update_needed);

  // Force new_frames_needed if config_update_needed.
  //
  // However, the "new" frames provided in InitializedBuffers() can actually still be using the same
  // buffers, as long as those buffers are still usable for the new config.  We handle it this way
  // to share more code with seeking / stream switching, which ends up giving the same buffers to
  // a new H264MultiDecoder instance, vs. a config_update_needed which is a single H264MultiDecoder
  // instance.
  //
  // In particular, the HW frame config update is happening in InitializedFrames(), whether the
  // "update" is initializing frames for a new H264MultiDecoder (seek / new stream), or
  // re-configuring frames of an existing H264MultiDecoder (mid-stream config update).
  if (config_update_needed) {
    new_frames_needed = true;
  }

  if (!new_frames_needed && !config_update_needed) {
    // Tell HW to continue immediately.
    AvScratch0::Get()
        .FromValue(static_cast<uint32_t>((next_max_reference_size_ << 24) |
                                         (video_frames_.size() << 16) |
                                         (video_frames_.size() << 8)))
        .WriteTo(owner_->dosbus());
    is_hw_active_ = true;
    owner_->watchdog()->Start();
    return;
  }

  if (new_frames_needed) {
    // This also excludes separate_colour_plane_flag true.
    if (sequence_info.chroma_format_idc() != static_cast<uint32_t>(ChromaFormatIdc::k420) &&
        sequence_info.chroma_format_idc() != static_cast<uint32_t>(ChromaFormatIdc::kMonochrome)) {
      LogEvent(media_metrics::
                   StreamProcessorEvents2MigratedMetricDimensionEvent_ChromaFormatUnsupportedError);
      LOG(ERROR,
          "sequence_info.chroma_format_idc() not in {k420, kMonochrome} - "
          "sequence_info.chroma_format_idc(): %u",
          sequence_info.chroma_format_idc());
      OnFatalError();
      return;
    }

    // It'd be nice if this were consistenty available at slice interrupt time, but it isn't.  Stash
    // it while we can.
    chroma_format_idc_ = sequence_info.chroma_format_idc();

    state_ = DecoderState::kWaitingForConfigChange;
    // Don't tell core to StopDecoding() - is_decoder_started_ remains true.  However is_hw_active_
    // is false.
    ZX_DEBUG_ASSERT(is_decoder_started_);
    ZX_DEBUG_ASSERT(!is_hw_active_);
    if (!media_decoder_->Flush()) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_FlushError);
      LOG(ERROR, "!media_decoder_->Flush()");
      OnFatalError();
      return;
    }
    OutputReadyFrames();
    ZX_DEBUG_ASSERT(frames_to_output_.empty());

    // Stash any reference_mv_buffer(s) in case they're already big enough to keep and use with the
    // new frames.  We'll check the sizes in InitializedFrames().
    for (auto& frame : video_frames_) {
      auto mv_buffer = std::move(frame->reference_mv_buffer);
      if (!on_deck_internal_buffers_.has_value()) {
        on_deck_internal_buffers_.emplace();
      }
      auto& on_deck_reference_mv_buffers = on_deck_internal_buffers_->reference_mv_buffers_;
      if (on_deck_reference_mv_buffers.size() == frame->index) {
        on_deck_reference_mv_buffers.emplace_back(std::move(mv_buffer));
      } else {
        ZX_DEBUG_ASSERT(frame->index < on_deck_reference_mv_buffers.size());
        ZX_DEBUG_ASSERT(!on_deck_reference_mv_buffers[frame->index].has_value());
        on_deck_reference_mv_buffers[frame->index] = std::move(mv_buffer);
      }
    }

    video_frames_.clear();

    // TODO(fxbug.dev/13483): Reset initial I frame tracking if FW doesn't do that itself.

    // This is doing the same thing as the amlogic code, but it's unlikely to matter.  This has
    // basically nothing to do with the DPB size, and is just round-tripping a number back to the HW
    // like the amlogic code does.  The actual DPB size is separate (and also conveyed to the HW).
    // Since all the DPB management is in SW, it's unlikely that the FW or HW really cares about
    // this value, but just in case the HW would get annoyed, plumb this value.
    static constexpr uint32_t kHwMaxReferenceSizeAdjustment = 4;
    next_max_reference_size_ = stream_info2.max_reference_size() + kHwMaxReferenceSizeAdjustment;

    pending_display_width_ = display_width;
    pending_display_height_ = display_height;
    // We handle SAR on the fly in this decoder since we don't get SAR until the slice header shows
    // up.  Or rather, that's when amlogic code gets SAR from the FW, so stick with that to avoid
    // reading at a different time than is known to work.
    static constexpr bool kHasSar = false;
    static constexpr uint32_t kSarWidth = 1;
    static constexpr uint32_t kSarHeight = 1;
    client_->InitializeFrames(min_frame_count, max_frame_count, coded_width, coded_height, stride,
                              display_width, display_height, kHasSar, kSarWidth, kSarHeight);
    waiting_for_surfaces_ = true;
    owner_->TryToReschedule();
    return;
  }

  // Not necessarily new buffers, but new frames.
  ZX_DEBUG_ASSERT_MSG(!config_update_needed, "config update implies 'new' frames");
}

bool H264MultiDecoder::InitializeRefPics(
    const std::vector<std::shared_ptr<media::H264Picture>>& ref_pic_list, uint32_t reg_offset) {
  TRACE_DURATION("media", "H264MultiDecoder::InitializeRefPics");
  uint32_t ref_list[8] = {};
  uint32_t ref_index = 0;
  ZX_DEBUG_ASSERT(ref_pic_list.size() <= sizeof(ref_list));
  for (uint32_t i = 0; i < ref_pic_list.size(); i++) {
    DLOG("Getting pic list (for reg_offset %d) %d of %lu\n", reg_offset, i, ref_pic_list.size());
    auto* amlogic_picture = static_cast<AmlogicH264Picture*>(ref_pic_list[i].get());
    DLOG("amlogic_picture: %p", amlogic_picture);
    // amlogic_picture may be null if the decoder was recently flushed. In that case we don't have
    // information about what the reference frame was, so don't try to update it.
    if (!amlogic_picture)
      continue;
    auto internal_picture = amlogic_picture->internal_picture.lock();
    if (!internal_picture) {
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_MissingPictureError);
      LOG(WARNING, "InitializeRefPics reg_offset %d missing internal picture %d", reg_offset, i);
      // internal_picture could be null if input data has gaps. Make best effort to continue without
      // error till next IDR is received.
      continue;
    }

    // Offset into AncNCanvasAddr registers.
    uint32_t canvas_index = internal_picture->index;
    constexpr uint32_t kFrameFlag = 0x3;
    constexpr uint32_t kFieldTypeBitOffset = 5;
    uint32_t cfg = canvas_index | (kFrameFlag << kFieldTypeBitOffset);
    // Every dword stores 4 reference pics, lowest index in the highest bits.
    uint32_t offset_into_dword = 8 * (3 - (ref_index % 4));
    ref_list[ref_index / 4] |= (cfg << offset_into_dword);

    ++ref_index;
  }

  H264BufferInfoIndex::Get().FromValue(reg_offset).WriteTo(owner_->dosbus());
  for (uint32_t reg_value : ref_list) {
    H264BufferInfoData::Get().FromValue(reg_value).WriteTo(owner_->dosbus());
  }
  return true;
}

void H264MultiDecoder::HandleSliceHeadDone() {
  TRACE_DURATION("media", "H264MultiDecoder::HandleSliceHeadDone");
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  ZX_DEBUG_ASSERT(state_ == DecoderState::kRunning);
  owner_->watchdog()->Cancel();
  is_hw_active_ = false;

  saved_iqidct_ctrl_ = IqidctCtrl::Get().ReadFrom(owner_->dosbus()).reg_value();
  saved_vcop_ctrl_ = VcopCtrl::Get().ReadFrom(owner_->dosbus()).reg_value();
  saved_vld_decode_ctrl_ = VldDecodeCtrl::Get().ReadFrom(owner_->dosbus()).reg_value();

  // Setup reference frames and output buffers before decoding.
  params_.ReadFromLmem(&*lmem_);
  DLOG("NAL unit type: %d\n", params_.data[HardwareRenderParams::kNalUnitType]);
  DLOG("NAL ref_idc: %d\n", params_.data[HardwareRenderParams::kNalRefIdc]);
  DLOG("NAL slice_type: %d\n", params_.data[HardwareRenderParams::kSliceType]);
  DLOG("pic order cnt type: %d\n", params_.data[HardwareRenderParams::kPicOrderCntType]);
  DLOG("log2_max_frame_num: %d\n", params_.data[HardwareRenderParams::kLog2MaxFrameNum]);
  DLOG("log2_max_pic_order_cnt: %d\n", params_.data[HardwareRenderParams::kLog2MaxPicOrderCntLsb]);
  DLOG("entropy coding mode flag: %d\n",
       params_.data[HardwareRenderParams::kEntropyCodingModeFlag]);
  DLOG("profile idc mmc0: %d\n", (params_.data[HardwareRenderParams::kProfileIdcMmco] >> 8) & 0xff);
  DLOG("Offset delimiter %d", params_.Read32(HardwareRenderParams::kOffsetDelimiterLo));
  DLOG("Mode 8x8 flags: 0x%x\n", params_.data[HardwareRenderParams::kMode8x8Flags]);

  DLOG("kMaxReferenceFrameNum: 0x%x", params_.data[HardwareRenderParams::kMaxReferenceFrameNum]);
  DLOG("kMaxBufferFrame: 0x%x", params_.data[HardwareRenderParams::kMaxBufferFrame]);
  DLOG("kMaxNumReorderFramesNewerFirmware: 0x%x",
       params_.data[HardwareRenderParams::kMaxNumReorderFramesNewerFirmware]);

  // Don't need StreamInfo here - saved anything needed from there in ConfigureDpb().
  //
  // SequenceInfo may not be reliable at slice header interrupt time, judging from how
  // chroma_format_idc() portion wasn't when it was read here, so we used the stashed
  // chroma_format_idc_ from ConfigureDpb() time instead.
  //
  // CropInfo (aka AvScratch6)
  const auto crop_info = CropInfo::Get().ReadFrom(owner_->dosbus());
  // StreamInfo2 (aka AvScratchB)
  const auto stream_info2 = StreamInfo2::Get().ReadFrom(owner_->dosbus());

  // At this point, we queue some post-parsing NALUs to H264Decoder.  Specifically, SPS, PPS (TBD),
  // and slice header.  Then we call H264Decoder::Decode() which processes those queued NALUs to
  // basically catch the H264Decoder up to roughly where the HW is on the slice the HW just
  // indicated with an interrupt.
  //
  // Probably we could queue fewer SPS and PPS headers, but queuing before every picture works.
  //
  // Any "not avaialable from FW" comments below should be read as "not obviously avaialble from
  // FW, but maybe?".
  //
  // TODO(fxbug.dev/13483): Test with multi-slice pictures.

  // SPS
  //
  // This set of fields is not necessarily the minimum necessary set for this driver to work.  Nor
  // is this set of fields complete, as not all fields are available from the FW.

  auto sps_nalu = std::make_unique<media::H264NALU>();
  {  // scope sps
    ZX_DEBUG_ASSERT(!sps_nalu->data);
    ZX_DEBUG_ASSERT(!sps_nalu->size);
    // Just needs to be non-zero for SPS; not available from FW but doesn't matter.
    sps_nalu->nal_ref_idc = 1;
    sps_nalu->nal_unit_type = media::H264NALU::kSPS;
    auto sps = std::make_unique<media::H264SPS>();

    // These are what's known to be available from FW:
    sps->profile_idc = (params_.data[HardwareRenderParams::kProfileIdcMmco] >> 8) & 0xff;
    // These aren't available from FW, as far as I know:
    // constraint_set0_flag
    // constraint_set1_flag
    // constraint_set2_flag
    // constraint_set3_flag
    // constraint_set4_flag
    // constraint_set5_flag
    //
    // We'd like to have constraint_set3_flag, but the FW doesn't seem able to provide that.  In
    // H264Decoder::ProcessSPS(), this means we'll assume level == 11 instead of 9, which is
    // ok, because assuming 11 (vs 9) leads to higher limits not lower.
    sps->level_idc = params_.data[HardwareRenderParams::kLevelIdcMmco];
    sps->seq_parameter_set_id = params_.data[HardwareRenderParams::kCurrentSpsId];
    if (sps->seq_parameter_set_id >= 32) {
      LogEvent(
          media_metrics::
              StreamProcessorEvents2MigratedMetricDimensionEvent_SeqParameterSetIdTooLargeError);
      LOG(ERROR, "sps->seq_parameter_set_id >= 32");
      OnFatalError();
      return;
    }
    // From 7.4.2.1.1, chroma_format_idc defaults to 1 when not present.
    sps->chroma_format_idc = 1;
    if (ProfileHasChromaFormatIdc(sps->profile_idc))
      sps->chroma_format_idc = chroma_format_idc_;
    // These aren't available from FW:
    // separate_colour_plane_flag
    // bit_depth_luma_minus8
    // bit_depth_chroma_minus8
    // qpprime_y_zero_transform_bypass_flag
    // seq_scaling_matrix_present_flag
    // scaling_list4x4
    // scaling_list8x8
    sps->log2_max_frame_num_minus4 = params_.data[HardwareRenderParams::kLog2MaxFrameNum] - 4;
    if (sps->log2_max_frame_num_minus4 >= 13) {
      LogEvent(media_metrics::
                   StreamProcessorEvents2MigratedMetricDimensionEvent_MaxFrameNumTooLargeError);
      LOG(ERROR, "sps->log2_max_frame_num_minus4 >= 13");
      OnFatalError();
      return;
    }
    sps->pic_order_cnt_type = params_.data[HardwareRenderParams::kPicOrderCntType];
    sps->log2_max_pic_order_cnt_lsb_minus4 =
        params_.data[HardwareRenderParams::kLog2MaxPicOrderCntLsb] - 4;
    sps->delta_pic_order_always_zero_flag =
        params_.data[HardwareRenderParams::kDeltaPicOrderAlwaysZeroFlag];
    sps->offset_for_non_ref_pic =
        static_cast<int16_t>(params_.data[HardwareRenderParams::kOffsetForNonRefPic]);
    sps->offset_for_top_to_bottom_field =
        static_cast<int16_t>(params_.data[HardwareRenderParams::kOffsetForTopToBottomField]);
    sps->num_ref_frames_in_pic_order_cnt_cycle =
        params_.data[HardwareRenderParams::kNumRefFramesInPicOrderCntCycle];
    ZX_DEBUG_ASSERT(sps->num_ref_frames_in_pic_order_cnt_cycle >= 0);
    if (static_cast<uint32_t>(sps->num_ref_frames_in_pic_order_cnt_cycle) >
        HardwareRenderParams::kMaxNumRefFramesInPicOrderCntCycle) {
      LogEvent(media_metrics::
                   StreamProcessorEvents2MigratedMetricDimensionEvent_NumRefFramesInPocCycleError);
      LOG(ERROR,
          "sps->num_ref_frames_in_pic_order_cnt_cycle > kMaxNumRefFramesInPicOrderCntCycle (128) - "
          "FW supports up to 128 (not 255) - value: %d",
          sps->num_ref_frames_in_pic_order_cnt_cycle);
      OnFatalError();
      return;
    }
    // No point in setting sps->expected_delta_per_pic_order_cnt_cycle because never used.
    for (uint32_t i = 0; i < HardwareRenderParams::kMaxNumRefFramesInPicOrderCntCycle; ++i) {
      sps->offset_for_ref_frame[i] =
          static_cast<int16_t>(params_.data[HardwareRenderParams::kOffsetForRefFrameBase + i]);
    }
    sps->max_num_ref_frames = params_.data[HardwareRenderParams::kMaxReferenceFrameNum];
    ZX_DEBUG_ASSERT(static_cast<uint32_t>(sps->max_num_ref_frames) ==
                    stream_info2.max_reference_size());
    sps->gaps_in_frame_num_value_allowed_flag =
        params_.data[HardwareRenderParams::kFrameNumGapAllowed];

    ZX_DEBUG_ASSERT(hw_coded_width_ / kMacroblockDimension ==
                    params_.data[HardwareRenderParams::kMbWidth]);
    ZX_DEBUG_ASSERT(hw_coded_height_ / kMacroblockDimension ==
                    params_.data[HardwareRenderParams::kMbHeight]);
    sps->pic_width_in_mbs_minus1 = (hw_coded_width_ / kMacroblockDimension) - 1;
    // Because frame_mbs_only_flag true, we know this is in units of MBs.
    sps->pic_height_in_map_units_minus1 = (hw_coded_height_ / kMacroblockDimension) - 1;

    // Also available via SCRATCH2 during FW config request; since we already verified that
    // frame_mbs_only_flag is 1 there, we can just set true here.
    sps->frame_mbs_only_flag = true;
    if (!sps->frame_mbs_only_flag) {
      LogEvent(media_metrics::
                   StreamProcessorEvents2MigratedMetricDimensionEvent_InterlacedUnsupportedError);
      LOG(ERROR, "!sps->frame_mbs_only_flag - not supported");
      OnFatalError();
      return;
    }
    sps->mb_adaptive_frame_field_flag = !!(params_.data[HardwareRenderParams::kMbffInfo] & 0x2);
    // ignoring direct_8x8_inference_flag - might be in kMode8x8Flags
    sps->frame_cropping_flag = (params_.data[HardwareRenderParams::kCroppingLeftRight] ||
                                params_.data[HardwareRenderParams::kCroppingTopBottom]);
    sps->frame_crop_left_offset = params_.data[HardwareRenderParams::kCroppingLeftRight] >> 8;
    sps->frame_crop_right_offset = params_.data[HardwareRenderParams::kCroppingLeftRight] & 0xff;
    sps->frame_crop_top_offset = params_.data[HardwareRenderParams::kCroppingTopBottom] >> 8;
    sps->frame_crop_bottom_offset = params_.data[HardwareRenderParams::kCroppingTopBottom] & 0xff;
    ZX_DEBUG_ASSERT(crop_info.left() == static_cast<uint32_t>(sps->frame_crop_left_offset));
    ZX_DEBUG_ASSERT(crop_info.right() == static_cast<uint32_t>(sps->frame_crop_right_offset));
    ZX_DEBUG_ASSERT(crop_info.top() == static_cast<uint32_t>(sps->frame_crop_top_offset));
    ZX_DEBUG_ASSERT(crop_info.bottom() == static_cast<uint32_t>(sps->frame_crop_bottom_offset));

    // Re. VUI, we only extract sar_width and sar_height, not any other parameters under
    // vui_parameters_present_flag, for now.  In particular we ignore bitstream_restriction_flag
    // from FW since the FW doesn't provide max_num_reorder_frames (confirmed not made available by
    // FW), max_dec_frame_buffering (may be in StreamInfo.upper_significant?).
    bool aspect_ratio_info_present_flag =
        !!(params_.data[HardwareRenderParams::kVuiStatus] &
           HardwareRenderParams::kVuiStatusMaskAspectRatioInfoPresentFlag);
    // Some of the following could be shared with ParseVUIParameters() - it's not a lot of redundant
    // code though; we just need to get sar_width and sar_height filled out (or left zero, as
    // appropriate)
    ZX_DEBUG_ASSERT(!sps->sar_width);
    ZX_DEBUG_ASSERT(!sps->sar_height);
    if (aspect_ratio_info_present_flag) {
      uint16_t aspect_ratio_idc = params_.data[HardwareRenderParams::kAspectRatioIdc];
      if (aspect_ratio_idc == media::H264SPS::kExtendedSar) {
        sps->sar_width = params_.data[HardwareRenderParams::kAspectRatioSarWidth];
        sps->sar_height = params_.data[HardwareRenderParams::kAspectRatioSarHeight];
      } else {
        if (aspect_ratio_idc >= std::size(kTableSarWidth)) {
          LogEvent(
              media_metrics::
                  StreamProcessorEvents2MigratedMetricDimensionEvent_AspectRatioIdcTooLargeError);
          LOG(ERROR, "aspect_ratio_idc >= std::size(kTableSarWidth)");
          OnFatalError();
          return;
        }
        sps->sar_width = kTableSarWidth[aspect_ratio_idc];
        sps->sar_height = kTableSarHeight[aspect_ratio_idc];
      }
    }
    sps->vui_parameters_present_flag = aspect_ratio_info_present_flag;

    // We intentionally don't ever set bitstream_restriction_flag since it doesn't appear we can get
    // the sub-values from the FW:
    // max_num_reorder_frames
    // max_dec_frame_buffering
    //
    // We'd like to have max_dec_frame_buffering, but it seems the FW only provides
    // kMaxReferenceFrameNum (aka max_num_ref_frames).

    // We intentionally don't set these because they're not used:
    // timing_info_present_flag
    // num_units_in_tick
    // time_scale
    // fixed_frame_rate_flag

    // We intentionally don't set these because they're not used:
    // video_signal_type_present_flag
    // video_format
    // video_full_range_flag
    // colour_description_present_flag
    // colour_primaries
    // transfer_characteristics
    // matrix_coefficients

    // We intentionally don't set these because they're not used:
    // nal_hrd_parameters_present_flag
    // cpb_cnt_minus1
    // bit_rate_scale
    // cpb_size_scale
    // bit_rate_value_minus1
    // cpb_size_value_minus1
    // cbr_flag
    // initial_cpb_removal_delay_length_minus_1
    // cpb_removal_delay_length_minus1
    // dpb_output_delay_length_minus1
    // time_offset_length
    // low_delay_hrd_flag

    // We intentionally don't set chroma_array_type because we don't support
    // separate_colour_plane_flag true, so chroma_array_type should be 0.
    ZX_DEBUG_ASSERT(sps->chroma_array_type == 0);

    if (!current_sps_ || memcmp(&current_sps_.value(), sps.get(), sizeof(current_sps_.value()))) {
      if (!current_sps_) {
        current_sps_.emplace();
      }
      ZX_DEBUG_ASSERT(sizeof(current_sps_.value()) == sizeof(*sps.get()));
      memcpy(&current_sps_.value(), sps.get(), sizeof(current_sps_.value()));
      sps_nalu->preparsed_header.emplace<std::unique_ptr<media::H264SPS>>(std::move(sps));
    } else {
      sps_nalu = nullptr;
    }
  }  // ~sps

  // PPS
  //
  // This set of fields is not necessarily the minimum necessary set for this driver to work.  Nor
  // is this set of fields complete, as not all fields are available from the FW.

  auto pps_nalu = std::make_unique<media::H264NALU>();
  {  // scope pps
    ZX_DEBUG_ASSERT(!pps_nalu->data);
    ZX_DEBUG_ASSERT(!pps_nalu->size);
    // Just needs to be on-zero for PPS; not available from FW but doesn't matter.
    pps_nalu->nal_ref_idc = 1;
    pps_nalu->nal_unit_type = media::H264NALU::kPPS;
    auto pps = std::make_unique<media::H264PPS>();

    pps->pic_parameter_set_id = params_.data[HardwareRenderParams::kCurrentPpsId];
    pps->seq_parameter_set_id = params_.data[HardwareRenderParams::kCurrentSpsId];
    if (pps->seq_parameter_set_id >= 32) {
      LogEvent(
          media_metrics::
              StreamProcessorEvents2MigratedMetricDimensionEvent_SeqParameterSetIdTooLargeError);
      LOG(ERROR, "pps->seq_parameter_set_id >= 32");
      OnFatalError();
      return;
    }
    pps->entropy_coding_mode_flag = params_.data[HardwareRenderParams::kEntropyCodingModeFlag];
    // bottom_field_pic_order_in_frame_present_flag not available from FW
    pps->num_slice_groups_minus1 = params_.data[HardwareRenderParams::kNumSliceGroupsMinus1];
    if (pps->num_slice_groups_minus1 > 0) {
      LogEvent(
          media_metrics::
              StreamProcessorEvents2MigratedMetricDimensionEvent_NumSliceGroupsUnsupportedError);
      LOG(ERROR, "pps->num_slice_groups_minus1 > 0 - not supported");
      OnFatalError();
      return;
    }
    pps->num_ref_idx_l0_default_active_minus1 =
        params_.data[HardwareRenderParams::kPpsNumRefIdxL0ActiveMinus1];
    if (pps->num_ref_idx_l0_default_active_minus1 >= 32) {
      LogEvent(media_metrics::
                   StreamProcessorEvents2MigratedMetricDimensionEvent_NumRefIdxDefaultActiveError);
      LOG(ERROR, "pps->num_ref_idx_l0_default_active_minus1 >= 32");
      OnFatalError();
      return;
    }
    pps->num_ref_idx_l1_default_active_minus1 =
        params_.data[HardwareRenderParams::kPpsNumRefIdxL1ActiveMinus1];
    if (pps->num_ref_idx_l1_default_active_minus1 >= 32) {
      LogEvent(media_metrics::
                   StreamProcessorEvents2MigratedMetricDimensionEvent_NumRefIdxDefaultActiveError);
      LOG(ERROR, "pps->num_ref_idx_l1_default_active_minus1 >= 32");
      OnFatalError();
      return;
    }
    pps->weighted_pred_flag = params_.data[HardwareRenderParams::kWeightedPredFlag];
    pps->weighted_bipred_idc = params_.data[HardwareRenderParams::kWeightedBipredIdc];

    // We grab this just for the error checking.
    pps->pic_init_qp_minus26 =
        static_cast<int16_t>(params_.data[HardwareRenderParams::kPicInitQpMinus26]);
    if (pps->pic_init_qp_minus26 < -26 || pps->pic_init_qp_minus26 > 25) {
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_PicInitQpRangeError);
      LOG(ERROR, "pps->pic_init_qp_minus26 < -26 || pps->pic_init_qp_minus26 > 25 - value: %d",
          pps->pic_init_qp_minus26);
      OnFatalError();
      return;
    }
    // pic_init_qs_minus26 not available from FW
    // chroma_qp_index_offset not available from FW
    pps->deblocking_filter_control_present_flag =
        params_.data[HardwareRenderParams::kDeblockingFilterControlPresentFlag];
    // constrained_intra_pred_flag not available from FW
    pps->redundant_pic_cnt_present_flag =
        params_.data[HardwareRenderParams::kRedundantPicCntPresentFlag];
    if (pps->redundant_pic_cnt_present_flag) {
      // Since redundant_pic_cnt isn't available from the FW, we have to assume it might be non-zero
      // and fail here instead.  It also doesn't appear on first glance that H264Decoder handles
      // non-zero redundant_pic_cnt.  The kSkipPicCount field _might_ be the redundant_pic_cnt, or
      // maybe not.
      LogEvent(
          media_metrics::
              StreamProcessorEvents2MigratedMetricDimensionEvent_RedundantPicCntUnsupportedError);
      LOG(ERROR, "pps->redundant_pic_cnt_present_flag - not supported");
      OnFatalError();
      return;
    }
    // transform_8x8_mode_flag not available from FW?
    // pic_scaling_matrix_present_flag not available from FW.
    // scaling_list4x4 not available from FW.
    // scaling_list8x8 not available from FW.
    // second_chroma_qp_index_offset not avaialble from FW.
    if (!current_pps_ || memcmp(&current_pps_.value(), pps.get(), sizeof(current_pps_.value()))) {
      if (!current_pps_) {
        current_pps_.emplace();
      }
      ZX_DEBUG_ASSERT(sizeof(current_pps_.value()) == sizeof(*pps.get()));
      memcpy(&current_pps_.value(), pps.get(), sizeof(current_pps_.value()));
      pps_nalu->preparsed_header.emplace<std::unique_ptr<media::H264PPS>>(std::move(pps));
    } else {
      pps_nalu = nullptr;
    }
  }  // ~pps

  // SliceHeader
  auto slice_nalu = std::make_unique<media::H264NALU>();
  int frame_num = -1;
  int first_mb_in_slice = -1;
  {  // scope slice
    ZX_DEBUG_ASSERT(!slice_nalu->data);
    ZX_DEBUG_ASSERT(!slice_nalu->size);
    slice_nalu->nal_ref_idc = params_.data[HardwareRenderParams::kNalRefIdc];
    slice_nalu->nal_unit_type = params_.data[HardwareRenderParams::kNalUnitType];
    if (slice_nalu->nal_unit_type == media::H264NALU::kCodedSliceExtension) {
      LogEvent(
          media_metrics::
              StreamProcessorEvents2MigratedMetricDimensionEvent_SliceExtensionUnsupportedError);
      LOG(ERROR, "nal_unit_type == kCodedSliceExtension - not supported");
      OnFatalError();
      return;
    }
    auto slice = std::make_unique<media::H264SliceHeader>();
    slice->idr_pic_flag = (slice_nalu->nal_unit_type == 5);
    slice->nal_ref_idc = slice_nalu->nal_ref_idc;
    ZX_DEBUG_ASSERT(!slice->nalu_data);
    ZX_DEBUG_ASSERT(!slice->nalu_size);
    ZX_DEBUG_ASSERT(!slice->header_bit_size);
    slice->first_mb_in_slice = params_.data[HardwareRenderParams::kFirstMbInSlice];
    first_mb_in_slice = slice->first_mb_in_slice;
    slice->slice_type = params_.data[HardwareRenderParams::kSliceType];
    slice->pic_parameter_set_id = params_.data[HardwareRenderParams::kCurrentPpsId];
    ZX_DEBUG_ASSERT(!slice->colour_plane_id);
    slice->frame_num = params_.data[HardwareRenderParams::kFrameNum];
    DLOG("slice->frame_num: %d", slice->frame_num);
    frame_num = slice->frame_num;
    // interlaced not supported
    if (params_.data[HardwareRenderParams::kPictureStructureMmco] !=
        HardwareRenderParams::kPictureStructureMmcoFrame) {
      LogEvent(media_metrics::
                   StreamProcessorEvents2MigratedMetricDimensionEvent_InterlacedUnsupportedError);
      LOG(ERROR,
          "data[kPictureStructureMmco] != Frame - not supported - data[kPictureStructureMmco]: %x",
          params_.data[HardwareRenderParams::kPictureStructureMmco]);
      OnFatalError();
      return;
    }
    if (params_.data[HardwareRenderParams::kNewPictureStructure] !=
        HardwareRenderParams::kNewPictureStructureFrame) {
      LogEvent(media_metrics::
                   StreamProcessorEvents2MigratedMetricDimensionEvent_InterlacedUnsupportedError);
      LOG(ERROR, "data[kNewPictureStructure] != Frame - not supported");
      OnFatalError();
      return;
    }
    ZX_DEBUG_ASSERT(!slice->field_pic_flag);
    ZX_DEBUG_ASSERT(!slice->bottom_field_flag);
    slice->idr_pic_id = params_.data[HardwareRenderParams::kIdrPicId];
    slice->pic_order_cnt_lsb = params_.data[HardwareRenderParams::kPicOrderCntLsb];
    slice->delta_pic_order_cnt_bottom =
        params_.Read32(HardwareRenderParams::kDeltaPicOrderCntBottom_0);
    slice->delta_pic_order_cnt0 = params_.Read32(HardwareRenderParams::kDeltaPicOrderCnt0_0);
    slice->delta_pic_order_cnt1 = params_.Read32(HardwareRenderParams::kDeltaPicOrderCnt1_0);
    // redundant_pic_cnt not available from FW
    ZX_DEBUG_ASSERT(!slice->redundant_pic_cnt);
    // direct_spatial_mv_pred_flag not available from FW
    ZX_DEBUG_ASSERT(!slice->direct_spatial_mv_pred_flag);
    // Since num_ref_idx_active_override_flag isn't available from the FW, but the result of
    // aggregating PPS and SliceHeader is, we just pretend that the SliceHeader always overrides.
    // For all we know, it does, and there's no real benefit to avoiding the override if PPS already
    // matches, especially since we're less sure whether kPpsNumRefIdxL0ActiveMinus1 has the PPS's
    // value in the first place.
    slice->num_ref_idx_active_override_flag = true;
    slice->num_ref_idx_l0_active_minus1 =
        params_.data[HardwareRenderParams::kNumRefIdxL0ActiveMinus1];
    slice->num_ref_idx_l1_active_minus1 =
        params_.data[HardwareRenderParams::kNumRefIdxL1ActiveMinus1];
    // checked above
    ZX_DEBUG_ASSERT(slice_nalu->nal_unit_type != media::H264NALU::kCodedSliceExtension);
    // Each cmd is 2 uint16_t in src, and src has room for 33 commands so that the list of commands
    // can always be terminated by a 3.  In contrast, dst only has room for 32, and when all are
    // used there's no terminating 3.
    auto process_reorder_cmd_list = [this](const uint16_t* src_cmd_array,
                                           bool* ref_pic_list_modification_flag_lx_out,
                                           media::H264ModificationOfPicNum* dst_cmd_array) -> bool {
      ZX_DEBUG_ASSERT(src_cmd_array);
      ZX_DEBUG_ASSERT(ref_pic_list_modification_flag_lx_out);
      ZX_DEBUG_ASSERT(dst_cmd_array);
      if (src_cmd_array[0] != 3) {
        *ref_pic_list_modification_flag_lx_out = true;
        uint32_t src_index = 0;
        uint32_t dst_index = 0;
        uint32_t command;
        do {
          command = src_cmd_array[src_index];
          ZX_DEBUG_ASSERT(dst_index * 2 == src_index);
          if (dst_index >= media::H264SliceHeader::kRefListModSize) {
            // 32
            ZX_DEBUG_ASSERT(dst_index == media::H264SliceHeader::kRefListModSize);
            // 64
            ZX_DEBUG_ASSERT(src_index == HardwareRenderParams::kLxReorderCmdCount - 2);
            if (command == 3) {
              // this is actually ok, to have 32 commands with no terminating 3
              break;
            }
            LogEvent(
                media_metrics::
                    StreamProcessorEvents2MigratedMetricDimensionEvent_ReorderListTooLargeError);
            LOG(ERROR, "command != 3 && dst_index == kRefListModSize");
            OnFatalError();
            return false;
          }
          if (command != 0 && command != 1 && command != 2 & command != 3) {
            LogEvent(media_metrics::
                         StreamProcessorEvents2MigratedMetricDimensionEvent_ReorderCommandError);
            LOG(ERROR, "command not in {0, 1, 2, 3} - out of sync with FW?");
            OnFatalError();
            return false;
          }
          ZX_DEBUG_ASSERT(dst_index <= media::H264SliceHeader::kRefListModSize - 1);
          ZX_DEBUG_ASSERT(src_index <= HardwareRenderParams::kLxReorderCmdCount - 4);
          media::H264ModificationOfPicNum& dst = dst_cmd_array[dst_index];
          ZX_DEBUG_ASSERT(command == src_cmd_array[src_index]);
          dst.modification_of_pic_nums_idc = src_cmd_array[src_index++];
          ZX_DEBUG_ASSERT(src_index <= HardwareRenderParams::kLxReorderCmdCount - 3);
          if (command == 0 || command == 1) {
            dst.abs_diff_pic_num_minus1 = src_cmd_array[src_index++];
          } else if (command == 2) {
            dst.long_term_pic_num = src_cmd_array[src_index++];
          } else {
            ZX_DEBUG_ASSERT(command == 3);
          }
          ++dst_index;
        } while (command != 3);
      } else {
        ZX_DEBUG_ASSERT(!*ref_pic_list_modification_flag_lx_out);
      }
      return true;
    };
    if (!slice->IsISlice() && !slice->IsSISlice()) {
      if (!process_reorder_cmd_list(&params_.data[HardwareRenderParams::kL0ReorderCmdBase],
                                    &slice->ref_pic_list_modification_flag_l0,
                                    &slice->ref_list_l0_modifications[0])) {
        // OnFatalError() already called
        return;
      }
    }
    if (slice->IsBSlice()) {
      if (!process_reorder_cmd_list(&params_.data[HardwareRenderParams::kL1ReorderCmdBase],
                                    &slice->ref_pic_list_modification_flag_l1,
                                    &slice->ref_list_l1_modifications[0])) {
        // OnFatalError() already called
        return;
      }
    }
    // These don't appear to be available from FW:
    // luma_log2_weight_denom
    // chroma_log2_weight_denom
    // luma_weight_l0_flag
    // chroma_weight_l0_flag
    // pred_weight_table_l0
    // luma_weight_l1_flag
    // chroma_weight_l1_flag
    // pred_weight_table_l1
    if (slice->IsISlice()) {
      slice->no_output_of_prior_pics_flag =
          !!(params_.data[HardwareRenderParams::kMmcoCmd + 0] & 0x2);
      slice->long_term_reference_flag = !!(params_.data[HardwareRenderParams::kMmcoCmd + 0] & 0x1);
    }
    if (slice_nalu->nal_ref_idc) {
      uint32_t src_index = 0;
      uint32_t dst_index = 0;
      uint16_t* mmco_cmds = &params_.data[HardwareRenderParams::kMmcoCmd];
      constexpr uint32_t kSrcMmcoCmdCount = 44;
      // Probably 32 is enough for most streams, but unclear if 32 is really a limit in the h264
      // spec.
      constexpr uint32_t kDstMmcoCmdCount = media::H264SliceHeader::kRefListSize;
      while (true) {
        if (src_index >= kSrcMmcoCmdCount) {
          LogEvent(
              media_metrics::
                  StreamProcessorEvents2MigratedMetricDimensionEvent_MmcoSrcCmdCountUnsupportedError);
          LOG(ERROR, "src_index >= kSrcMmcoCmdCount - unsupported stream");
          OnFatalError();
          return;
        }
        if (dst_index >= kDstMmcoCmdCount) {
          LogEvent(
              media_metrics::
                  StreamProcessorEvents2MigratedMetricDimensionEvent_MmcoDstCmdCountUnsupportedError);
          LOG(ERROR, "dst_index >= kDstMmcoCmdCount - unsupported stream");
          OnFatalError();
          return;
        }
        uint16_t mmco = mmco_cmds[src_index++];
        if (mmco > 6) {
          LogEvent(
              media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_MmcoCommandError);
          LOG(ERROR, "mmco > 6");
          OnFatalError();
          return;
        }
        media::H264DecRefPicMarking& dst = slice->ref_pic_marking[dst_index];
        dst.memory_mgmnt_control_operation = mmco;
        if (mmco == 0) {
          break;
        }
        // We need at least enough room to read mmco == 0 next loop iteration, if not something else
        // sooner.
        if (src_index >= kSrcMmcoCmdCount) {
          LogEvent(
              media_metrics::
                  StreamProcessorEvents2MigratedMetricDimensionEvent_MmcoSrcCmdCountUnsupportedError);
          LOG(ERROR, "src_index >= kSrcMmcoCmdCount - unsupported stream");
          OnFatalError();
          return;
        }
        slice->adaptive_ref_pic_marking_mode_flag = true;
        if (mmco == 1 || mmco == 3) {
          dst.difference_of_pic_nums_minus1 = mmco_cmds[src_index++];
        } else if (mmco == 2) {
          dst.long_term_pic_num = mmco_cmds[src_index++];
        }
        // We need at least enough room to read mmco == 0 next loop iteration, if not something else
        // sooner.
        if (src_index >= kSrcMmcoCmdCount) {
          LogEvent(
              media_metrics::
                  StreamProcessorEvents2MigratedMetricDimensionEvent_MmcoSrcCmdCountUnsupportedError);
          LOG(ERROR, "src_index >= kSrcMmcoCmdCount - unsupported stream");
          OnFatalError();
          return;
        }
        if (mmco == 3 || mmco == 6) {
          dst.long_term_frame_idx = mmco_cmds[src_index++];
        } else if (mmco == 4) {
          dst.max_long_term_frame_idx_plus1 = mmco_cmds[src_index++];
        }
        ++dst_index;
        // src_index is checked first thing at top of loop
      }
      // Must end up 0 terminated, or we already failed above.  This comment is not intending to
      // imply that a stream with more mmco commands is necessarily invalid (TBD - h264 spec seems
      // a bit vague on how many there can be).
      ZX_DEBUG_ASSERT(dst_index < kDstMmcoCmdCount &&
                      slice->ref_pic_marking[dst_index].memory_mgmnt_control_operation == 0);
    }
    // Not available from FW:
    // cabac_init_idc
    // slice_qp_delta
    // sp_for_switch_flag
    // slice_qs_delta
    // disable_deblocking_filter_idc
    // slice_alpha_c0_offset_div2
    // slice_beta_offset_div2

    // These are set but never read in H264Decoder, so don't need to set them:
    // dec_ref_pic_marking_bit_size
    // pic_order_cnt_bit_size
    slice_nalu->preparsed_header.emplace<std::unique_ptr<media::H264SliceHeader>>(std::move(slice));
  }  // ~slice

  ZX_DEBUG_ASSERT(frame_num != -1);
  if (frame_num_ && frame_num_.value() != frame_num) {
    // If we didn't get a pic data done after a previous slice before this new slice, then probably
    // the input stream is broken (seen during fuzzing of the input stream).  For now we just fail
    // when broken input data is detected.
    //
    // TODO(fxbug.dev/13483): Be more resilient to broken input data.
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_FrameNumError);
    LOG(ERROR,
        "frame_num_ && frame_num_.value() != frame_num -- frame_num_.value(): %u frame_num: %u",
        frame_num_.value(), frame_num);
    OnFatalError();
    return;
  }
  frame_num_.emplace(frame_num);

  if (first_mb_in_slice <= per_frame_attempt_seen_first_mb_in_slice_) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_FirstMbInSliceError);
    LOG(ERROR, "first_mb_in_slice out of order or repeated - broken input data");
    OnFatalError();
    return;
  }
  per_frame_attempt_seen_first_mb_in_slice_ = first_mb_in_slice;

  if (first_mb_in_slice == per_frame_seen_first_mb_in_slice_) {
    if (sps_nalu) {
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_BrokenPictureBodyError);
      LOG(ERROR, "no pic data done after slice header before new SPS - broken input data");
      OnFatalError();
      return;
    }
    if (pps_nalu) {
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_BrokenPictureBodyError);
      LOG(ERROR, "no pic data done after slice header before new PPS - broken input data");
      OnFatalError();
      return;
    }
    if (memcmp(
            cpp17::get<std::unique_ptr<media::H264SliceHeader>>(slice_nalu->preparsed_header).get(),
            &stashed_latest_slice_header_, sizeof(stashed_latest_slice_header_))) {
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_FirstMbInSliceError);
      LOG(ERROR, "inconsistent slice data for same first_mb_in_slice - broken input data");
      OnFatalError();
      return;
    }
  }

  if (first_mb_in_slice > per_frame_seen_first_mb_in_slice_) {
    DLOG("first_mb_in_slice > per_frame_seen_first_mb_in_slice_");
    memcpy(&stashed_latest_slice_header_,
           cpp17::get<std::unique_ptr<media::H264SliceHeader>>(slice_nalu->preparsed_header).get(),
           sizeof(stashed_latest_slice_header_));
    if (sps_nalu) {
      media_decoder_->QueuePreparsedNalu(std::move(sps_nalu));
    }
    if (pps_nalu) {
      media_decoder_->QueuePreparsedNalu(std::move(pps_nalu));
    }
    media_decoder_->QueuePreparsedNalu(std::move(slice_nalu));
    per_frame_seen_first_mb_in_slice_ = first_mb_in_slice;
  }

  if (first_mb_in_slice > per_frame_decoded_first_mb_in_slice_) {
    media::AcceleratedVideoDecoder::DecodeResult decode_result;
    bool decode_done = false;
    while (!decode_done) {
      decode_result = media_decoder_->Decode();
      switch (decode_result) {
        case media::AcceleratedVideoDecoder::kDecodeError:
          LogEvent(
              media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_GenericDecodeError);
          LOG(ERROR, "kDecodeError");
          OnFatalError();
          return;
        case media::AcceleratedVideoDecoder::kConfigChange:
          // TODO: verify that the config change is a NOP vs. the previous ConfigureDpb().
          continue;
        case media::AcceleratedVideoDecoder::kRanOutOfStreamData:
          decode_done = true;
          break;
        case media::AcceleratedVideoDecoder::kRanOutOfSurfaces:
          // The pre-check in PumpDecoder() is intended to prevent this from happening most of the
          // time.  However, if there's a frame_num gap, that can use up additional frames, so we
          // need to treat this the same as kTryAgain.
          //
          // fall through on purpose
        case media::AcceleratedVideoDecoder::kTryAgain:
          // When there's a frame_num gap, and insufficient surfaces to handle the gap, Decode()
          // will (intentionally) return kTryAgain despite our accelerator never returning
          // kTryAgain (not allocating a frame is like kTryAgain).
          //
          // In this (typically rare) case we feed the decoder the same data again when an empty
          // frame becomes available, since there's no way to save/restore in the middle of a slice
          // header.  Until then, we need to allow the decoder HW to switch to a different stream.
          ZX_DEBUG_ASSERT(!IsUnusedReferenceFrameAvailable());
          state_ = DecoderState::kWaitingForInputOrOutput;
          owner_->core()->StopDecoding();
          is_decoder_started_ = false;

          // Force swap out so we can restore from saved state later when we have another free
          // output frame.  Don't attempt to save (saving in the middle of a slice header isn't a
          // thing for this HW).
          ZX_DEBUG_ASSERT(!force_swap_out_);
          force_swap_out_ = true;
          ZX_DEBUG_ASSERT(!should_save_input_context_);
          owner_->TryToReschedule();
          // Set these back to default state.
          ZX_DEBUG_ASSERT(!should_save_input_context_);
          force_swap_out_ = false;
          UpdateDiagnostics();
          return;
        case media::AcceleratedVideoDecoder::kNeedContextUpdate:
          LogEvent(
              media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_UnreachableError);
          LOG(ERROR, "kNeedContextUpdate is impossible");
          OnFatalError();
          return;
        default:
          LogEvent(media_metrics::
                       StreamProcessorEvents2MigratedMetricDimensionEvent_DecodeResultInvalidError);
          LOG(ERROR, "unexpected decode_result: %u", decode_result);
          OnFatalError();
          return;
      }
    }
    ZX_DEBUG_ASSERT(decode_result == media::AcceleratedVideoDecoder::kRanOutOfStreamData);
    per_frame_decoded_first_mb_in_slice_ = first_mb_in_slice;
  }

  ZX_DEBUG_ASSERT(state_ == DecoderState::kRunning);

  // Set up to decode the current slice.
  if (!current_frame_) {
    current_frame_ = current_metadata_frame_;
    if (!current_frame_) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_SwHwSyncError);
      LOG(ERROR, "HandleSliceDecode with no metadata frame available");
      OnFatalError();
      return;
    }

    uint64_t offset_delimiter = params_.data[HardwareRenderParams::kOffsetDelimiterHi] << 16 |
                                params_.data[HardwareRenderParams::kOffsetDelimiterLo];
    unwrapped_first_slice_header_of_frame_detected_stream_offset_ =
        ExtendBits(unwrapped_write_stream_offset_, offset_delimiter, 32);

    PtsManager::LookupResult lookup_result =
        pts_manager_->Lookup(unwrapped_first_slice_header_of_frame_detected_stream_offset_);

    if (lookup_result.has_pts()) {
      current_frame_->frame->has_pts = true;
      current_frame_->frame->pts = lookup_result.pts();
    } else {
      current_frame_->frame->has_pts = false;
      current_frame_->frame->pts = 0;
    }
  } else {
    // We're relying on the HW to do a pic data done interrupt before switching to a new frame, even
    // if the old frame didn't decode correctly.
    ZX_DEBUG_ASSERT(current_frame_ == current_metadata_frame_);
  }
  // We fed the media_decoder_ with pre-parsed SPS, PPS, SliceHeader, so the decoder will have
  // indicated at least 1 slice for the current frame.
  ZX_DEBUG_ASSERT(slice_data_map_.size() >= 1);
  ZX_DEBUG_ASSERT(slice_data_map_.find(first_mb_in_slice) != slice_data_map_.end());
  const SliceData& current_slice_data = slice_data_map_[first_mb_in_slice];

  // Configure the HW and decode the body of the current slice (corresponding to current_slice_data_
  // and current_frame_).  We may repeat this part later if the client is splitting slices across
  // packet boundaries.

  // The following checks are to try to ensure what the hardware's parsing matches what H264Decoder
  // processed from sps_nalu, pps_nalu, slice_nalu.
  //
  // Slices 5-9 are equivalent for this purpose with slices 0-4 - see 7.4.3
  constexpr uint32_t kSliceTypeMod = 5;
  ZX_DEBUG_ASSERT(current_slice_data.header.slice_type % kSliceTypeMod ==
                  params_.data[HardwareRenderParams::kSliceType] % kSliceTypeMod);
  // Check for interlacing (already rejected above).
  constexpr uint32_t kPictureStructureFrame = 3;
  ZX_DEBUG_ASSERT(params_.data[HardwareRenderParams::kNewPictureStructure] ==
                  kPictureStructureFrame);

  auto poc = poc_.ComputePicOrderCnt(&current_slice_data.sps, current_slice_data.header);
  if (!poc) {
    LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_PicOrderCntError);
    LOG(ERROR, "No poc");
    OnFatalError();
    return;
  }
  DLOG("Frame POC %d", poc.value());

  H264CurrentPocIdxReset::Get().FromValue(0).WriteTo(owner_->dosbus());
  // Assume all fields have the same POC, since the chromium code doesn't support interlacing.
  // frame
  H264CurrentPoc::Get().FromValue(poc.value()).WriteTo(owner_->dosbus());
  // top field
  H264CurrentPoc::Get().FromValue(poc.value()).WriteTo(owner_->dosbus());
  // bottom field
  H264CurrentPoc::Get().FromValue(poc.value()).WriteTo(owner_->dosbus());
  CurrCanvasCtrl::Get()
      .FromValue(0)
      .set_canvas_index(current_frame_->index)
      .WriteTo(owner_->dosbus());
  // Unclear if reading from the register is actually necessary, or if this
  // would always be the same as above.
  uint32_t curr_canvas_index =
      CurrCanvasCtrl::Get().ReadFrom(owner_->dosbus()).lower_canvas_index();
  RecCanvasCtrl::Get().FromValue(curr_canvas_index).WriteTo(owner_->dosbus());
  DbkrCanvasCtrl::Get().FromValue(curr_canvas_index).WriteTo(owner_->dosbus());
  DbkwCanvasCtrl::Get().FromValue(curr_canvas_index).WriteTo(owner_->dosbus());

  // Info for a progressive frame.
  constexpr uint32_t kProgressiveFrameInfo = 0xf480;
  current_frame_->info0 = kProgressiveFrameInfo;
  // Top field
  current_frame_->info1 = poc.value();
  // Bottom field
  current_frame_->info2 = poc.value();
  current_frame_->is_long_term_reference = current_slice_data.pic->long_term;

  H264BufferInfoIndex::Get().FromValue(16).WriteTo(owner_->dosbus());

  // Store information about the properties of each canvas image.
  for (uint32_t i = 0; i < video_frames_.size(); ++i) {
    bool is_long_term = video_frames_[i]->is_long_term_reference;
    if (is_long_term) {
      // Everything is progressive, so mark as having both bottom and top as long-term references.
      constexpr uint32_t kTopFieldLongTerm = 1 << 4;
      constexpr uint32_t kBottomFieldLongTerm = 1 << 5;
      video_frames_[i]->info0 |= kTopFieldLongTerm | kBottomFieldLongTerm;
    }
    uint32_t info_to_write = video_frames_[i]->info0;
    if (video_frames_[i].get() == current_frame_) {
      constexpr uint32_t kCurrentFrameBufInfo = 0xf;
      info_to_write |= kCurrentFrameBufInfo;
    }
    ZX_DEBUG_ASSERT(video_frames_[i]->index == i);
    H264BufferInfoData::Get().FromValue(info_to_write).WriteTo(owner_->dosbus());
    H264BufferInfoData::Get().FromValue(video_frames_[i]->info1).WriteTo(owner_->dosbus());
    H264BufferInfoData::Get().FromValue(video_frames_[i]->info2).WriteTo(owner_->dosbus());
  }
  if (!InitializeRefPics(current_slice_data.ref_pic_list0, 0))
    return;
  if (!InitializeRefPics(current_slice_data.ref_pic_list1, 8))
    return;

  // Wait for the hardware to finish processing its current mbs.  Normally this should be quick, but
  // wait a while to avoid potential spurious timeout (none observed at 100ms).
  if (!SpinWaitForRegister(std::chrono::milliseconds(400), [&] {
        return !H264CoMbRwCtl::Get().ReadFrom(owner_->dosbus()).busy();
      })) {
    LogEvent(
        media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_TimeoutWaitingForHwError);
    LOG(ERROR, "Failed to wait for rw register nonbusy");
    OnFatalError();
    return;
  }

  constexpr uint32_t kMvRefDataSizePerMb = 96;
  uint32_t mv_size = kMvRefDataSizePerMb;

  if ((params_.data[HardwareRenderParams::kMode8x8Flags] & 4) &&
      (params_.data[HardwareRenderParams::kMode8x8Flags] & 2)) {
    // direct 8x8 mode seems to store 1/4 the data, so the offsets need to be less as well.
    mv_size /= 4;
  }
  uint32_t mv_byte_offset = current_slice_data.header.first_mb_in_slice * mv_size;

  H264CoMbWrAddr::Get()
      .FromValue(truncate_to_32(current_frame_->reference_mv_buffer.phys_base()) + mv_byte_offset)
      .WriteTo(owner_->dosbus());

  // 8.4.1.2.1 - co-located motion vectors come from RefPictList1[0] for frames.
  if (current_slice_data.ref_pic_list1.size() > 0) {
    auto* amlogic_picture =
        static_cast<AmlogicH264Picture*>(current_slice_data.ref_pic_list1[0].get());
    if (amlogic_picture) {
      auto internal_picture = amlogic_picture->internal_picture.lock();
      if (!internal_picture) {
        LogEvent(media_metrics::
                     StreamProcessorEvents2MigratedMetricDimensionEvent_MotionVectorContextError);
        LOG(ERROR, "Co-mb read buffer nonexistent");
        frame_data_provider_->AsyncResetStreamAfterCurrentFrame();
        return;
      }
      uint32_t read_addr =
          truncate_to_32(internal_picture->reference_mv_buffer.phys_base()) + mv_byte_offset;
      ZX_DEBUG_ASSERT(read_addr % 8 == 0);
      H264CoMbRdAddr::Get().FromValue((read_addr >> 3) | (2u << 30)).WriteTo(owner_->dosbus());
    }
  }

  // TODO: Maybe we could do what H264Decoder::IsNewPrimaryCodedPicture() does to detect this, but
  // this seems to work for now, and I'm not aware of any specific cases where it doesn't work.
  if (current_slice_data.header.first_mb_in_slice == 0) {
    DpbStatusReg::Get().FromValue(kH264ActionDecodeNewpic).WriteTo(owner_->dosbus());
  } else {
    DpbStatusReg::Get().FromValue(kH264ActionDecodeSlice).WriteTo(owner_->dosbus());
  }
  is_hw_active_ = true;
  owner_->watchdog()->Start();
}

// not currently used
void H264MultiDecoder::FlushFrames() {
  TRACE_DURATION("media", "H264MultiDecoder::FlushFrames");
  auto res = media_decoder_->Flush();
  DLOG("Got media decoder res %d", res);
}

uint32_t H264MultiDecoder::GetApproximateConsumedBytes() {
  TRACE_DURATION("media", "H264MultiDecoder::GetApproximateConsumedBytes");
  return kBytesToDecode - (ViffBitCnt::Get().ReadFrom(owner_->dosbus()).reg_value() + 7) / 8;
}

void H264MultiDecoder::DumpStatus() {
  TRACE_DURATION("media", "H264MultiDecoder::DumpStatus");
  auto viff_bit_cnt = ViffBitCnt::Get().ReadFrom(owner_->dosbus());
  DLOG("ViffBitCnt: %x", viff_bit_cnt.reg_value());
  DLOG("GetApproximateConsumedBytes(): 0x%x", GetApproximateConsumedBytes());
  // Number of bytes that are in the fifo that RP has already moved past.
  DLOG("Viifolevel: 0x%x", VldMemVififoLevel::Get().ReadFrom(owner_->dosbus()).reg_value());
  DLOG("VldMemVififoBytesAvail: 0x%x",
       VldMemVififoBytesAvail::Get().ReadFrom(owner_->dosbus()).reg_value());
  DLOG("Error status reg %d mbymbx reg %d",
       ErrorStatusReg::Get().ReadFrom(owner_->dosbus()).reg_value(),
       MbyMbx::Get().ReadFrom(owner_->dosbus()).reg_value());
  DLOG("DpbStatusReg 0x%x", DpbStatusReg::Get().ReadFrom(owner_->dosbus()).reg_value());

  uint32_t stream_input_offset = owner_->core()->GetStreamInputOffset();
  uint32_t read_offset = owner_->core()->GetReadOffset();
  DLOG("input offset: %d (0x%x) read offset: %d (0x%x)", stream_input_offset, stream_input_offset,
       read_offset, read_offset);
  DLOG("unwrapped_write_stream_offset_: 0x%" PRIx64, unwrapped_write_stream_offset_);
  DLOG("unwrapped_saved_read_stream_offset_: 0x%" PRIx64, unwrapped_saved_read_stream_offset_);
  DLOG("unwrapped_first_slice_header_of_frame_detected_stream_offset_: 0x%" PRIx64,
       unwrapped_first_slice_header_of_frame_detected_stream_offset_);
  DLOG("unwrapped_first_slice_header_of_frame_decoded_stream_offset_: 0x%" PRIx64,
       unwrapped_first_slice_header_of_frame_decoded_stream_offset_);
  DLOG("unwrapped_write_stream_offset_decode_tried_: 0x%" PRIx64,
       unwrapped_write_stream_offset_decode_tried_);
  DLOG("unwrapped_first_slice_header_of_frame_decoded_stream_offset_decode_tried_: 0x%" PRIx64,
       unwrapped_first_slice_header_of_frame_decoded_stream_offset_decode_tried_);
}

void H264MultiDecoder::HandlePicDataDone() {
  TRACE_DURATION("media", "H264MultiDecoder::HandlePicDataDone");
  DLOG("HandlePicDataDone()");
  ZX_DEBUG_ASSERT(current_frame_);

  owner_->watchdog()->Cancel();
  is_hw_active_ = false;

  unwrapped_first_slice_header_of_frame_decoded_stream_offset_ =
      unwrapped_first_slice_header_of_frame_detected_stream_offset_;

  current_frame_ = nullptr;
  current_metadata_frame_ = nullptr;
  per_frame_seen_first_mb_in_slice_ = -1;
  per_frame_decoded_first_mb_in_slice_ = -1;
  frame_num_ = std::nullopt;

  // Bring the decoder into sync that the frame is done decoding.  This way media_decoder_ can
  // output frames and do post-decode DPB or MMCO updates.  This pushes media_decoder_ from
  // searching for NAL end (pre-frame-decode) to post-frame-decode and post-any-frames-output.
  auto aud_nalu = std::make_unique<media::H264NALU>();
  ZX_DEBUG_ASSERT(!aud_nalu->data);
  ZX_DEBUG_ASSERT(!aud_nalu->size);
  aud_nalu->nal_ref_idc = 0;
  aud_nalu->nal_unit_type = media::H264NALU::kAUD;
  media_decoder_->QueuePreparsedNalu(std::move(aud_nalu));
  media::AcceleratedVideoDecoder::DecodeResult decode_result = media_decoder_->Decode();
  switch (decode_result) {
    case media::AcceleratedVideoDecoder::kDecodeError:
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_GenericDecodeError);
      LOG(ERROR, "kDecodeError");
      OnFatalError();
      return;
    case media::AcceleratedVideoDecoder::kConfigChange:
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_UnreachableError);
      LOG(ERROR, "kConfigChange unexpected here");
      OnFatalError();
      return;
    case media::AcceleratedVideoDecoder::kRanOutOfStreamData:
      // keep going
      break;
    case media::AcceleratedVideoDecoder::kRanOutOfSurfaces:
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_SwHwSyncError);
      LOG(ERROR, "kRanOutOfSurfaces desipte checking in advance of starting frame decode");
      OnFatalError();
      return;
    case media::AcceleratedVideoDecoder::kNeedContextUpdate:
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_UnreachableError);
      LOG(ERROR, "kNeedContextUpdate is impossible");
      OnFatalError();
      return;
    case media::AcceleratedVideoDecoder::kTryAgain:
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_UnreachableError);
      LOG(ERROR, "kTryAgain despite this accelerator never indicating that");
      OnFatalError();
      return;
  }

  OutputReadyFrames();

  state_ = DecoderState::kWaitingForInputOrOutput;
  // No need for owner_->core()->StopDecoding() here, as the forced swap-out below will call
  // StopDecoding().
  is_decoder_started_ = false;

  slice_data_map_.clear();

  // Force swap out, and do save input state, to persist the progress we just made decoding a frame.
  //
  // In part this can be thought of as forcing a checkpoint of the successful work accomplished so
  // far.  We'll potentially restore from this checkpoint multiple times until we have enough input
  // data to completely decode the next frame (so we need to save here so we can restore back to
  // here if the next frame decode doesn't complete with input data available so far).  Typically
  // we'll have enough input data to avoid excessive re-decodes.
  ZX_DEBUG_ASSERT(!force_swap_out_);
  force_swap_out_ = true;
  ZX_DEBUG_ASSERT(!should_save_input_context_);
  should_save_input_context_ = true;
  owner_->TryToReschedule();
  // Set these back to default state.
  should_save_input_context_ = false;
  force_swap_out_ = false;
  UpdateDiagnostics();
  if (state_ == DecoderState::kWaitingForInputOrOutput) {
    PumpDecoder();
  }
}

void H264MultiDecoder::HandleBufEmpty() {
  TRACE_DURATION("media", "H264MultiDecoder::HandleBufEmpty");
  // This can happen if non-slice NALU(s) show up in a packet without any slice NALU(s).
  state_ = DecoderState::kWaitingForInputOrOutput;
  owner_->watchdog()->Cancel();
  is_hw_active_ = false;

  if (input_eos_queued_) {
    // We've consumed all the input data, so complete EOS handling.
    //
    // This Flush() may output a few more frames.
    if (!media_decoder_->Flush()) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_FlushError);
      LOG(ERROR, "Flush failed");
      OnFatalError();
      return;
    }
    // This prevents ever swapping back in after the forced swap-out below.
    sent_output_eos_to_client_ = true;
    client_->OnEos();
    // swap out is forced below
  }

  force_swap_out_ = true;
  // We need (if not EOS) to re-attempt decode from the old saved read pointer, so don't save the
  // current state.  Later we'll restore the old state with the old saved read pointer.  We haven't
  // advanced our unwrapped virtual read pointers past the old saved read pointer, so those will be
  // equal after restore.
  ZX_DEBUG_ASSERT(!should_save_input_context_);
  owner_->TryToReschedule();
  force_swap_out_ = false;
  UpdateDiagnostics();
  PumpOrReschedule();
}

void H264MultiDecoder::OutputReadyFrames() {
  TRACE_DURATION("media", "H264MultiDecoder::OutputReadyFrames");
  while (!frames_to_output_.empty()) {
    uint32_t index = frames_to_output_.front();
    frames_to_output_.pop_front();
    DLOG("OnFrameReady()");
    client_->OnFrameReady(video_frames_[index]->frame);
  }
}

void H264MultiDecoder::HandleHardwareError() {
  TRACE_DURATION("media", "H264MultiDecoder::HandleHardwareError");
  owner_->watchdog()->Cancel();
  is_hw_active_ = false;
  owner_->core()->StopDecoding();
  is_decoder_started_ = false;
  // We need to reset the hardware here or for some malformed hardware streams (e.g.
  // bear_h264[638] = 44) the CPU will hang when trying to isolate VDEC1 power on shutdown.
  ResetHardware();
  LOG(WARNING, "ResetHardware() done.");
  frame_data_provider_->AsyncResetStreamAfterCurrentFrame();
}

void H264MultiDecoder::HandleInterrupt() {
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  // Clear interrupt
  VdecAssistMbox1ClrReg::Get().FromValue(1).WriteTo(owner_->dosbus());
  uint32_t decode_status = DpbStatusReg::Get().ReadFrom(owner_->dosbus()).reg_value();
  TRACE_DURATION("media", "H264MultiDecoder::HandleInterrupt", "decode_status", decode_status);
  DLOG("Got H264MultiDecoder::HandleInterrupt, decode status: 0x%x", decode_status);

  switch (decode_status) {
    case kH264ConfigRequest: {
      DpbStatusReg::Get().FromValue(kH264ActionConfigDone).WriteTo(owner_->dosbus());
      ConfigureDpb();
      break;
    }
    case kH264DataRequest:
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_SwHwSyncError);
      LOG(ERROR, "Got unhandled data request");

      // Not used via this path so far, but potentially needed if we start using kH264DataRequest.
      saved_iqidct_ctrl_ = IqidctCtrl::Get().ReadFrom(owner_->dosbus()).reg_value();

      HandleHardwareError();
      break;
    case kH264SliceHeadDone: {
      HandleSliceHeadDone();
      break;
    }
    case kH264PicDataDone: {
      HandlePicDataDone();
      break;
    }
    case kH264SearchBufEmpty:
    case kH264DecodeBufEmpty: {
      HandleBufEmpty();
      break;
    }
    case kH264DecodeTimeout:
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_HwTimeoutError);
      LOG(ERROR, "Decoder got kH264DecodeTimeout");
      HandleHardwareError();
      break;
    default:
      // We can remove decoders while they're actively decoding.  The upside of that is we can
      // stop doing useless work sooner so we can do useful work sooner.  The downside is the
      // removal of an active decoder can leave in-flight an interrupt previously generated from the
      // HW but not yet delivered to this method.
      //
      // If the interrupt got delivered when there was no active video_decoder_, then it got
      // ignored which is fine.
      //
      // If we created or swapped in a new video_decoder_ before the stale interrupt is delivered,
      // then we know because of the continuous video_decoder_lock() hold interval during swap-in
      // that by the time that interrupt is delivered, the DpbStatusReg will have a value which is
      // not any of the non-"default" values handled by this switch statement.  In this case the
      // stale interrupt is ignored in this path here.
      //
      // Ignore stale interrupt, but log an event so we can know how often this happens outside
      // stress testing.  This is not considered an error.
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_StaleInterruptSeen);
      break;
  }
}

void H264MultiDecoder::PumpOrReschedule() {
  TRACE_DURATION("media", "H264MultiDecoder::PumpOrReschedule");
  if (state_ == DecoderState::kSwappedOut) {
    DLOG("PumpOrReschedule() sees kSwappedOut");
    owner_->TryToReschedule();
    // TryToReschedule will pump the decoder (using SwappedIn) once the decoder is finally
    // rescheduled.
  } else {
    DLOG("PumpOrReschedule() pumping");
    is_async_pump_pending_ = false;
    UpdateDiagnostics();
    PumpDecoder();
  }
}

void H264MultiDecoder::ReturnFrame(std::shared_ptr<VideoFrame> frame) {
  TRACE_DURATION("media", "H264MultiDecoder::ReturnFrame");
  DLOG("H264MultiDecoder::ReturnFrame %d", frame->index);
  ZX_DEBUG_ASSERT(frame->index < video_frames_.size());
  ZX_DEBUG_ASSERT(video_frames_[frame->index]->frame == frame);
  video_frames_[frame->index]->in_use = false;
  waiting_for_surfaces_ = false;
  DLOG("ReturnFrame() state_: %u", static_cast<unsigned int>(state_));
  PumpOrReschedule();
}

void H264MultiDecoder::CallErrorHandler() { OnFatalError(); }

void H264MultiDecoder::InitializedFrames(std::vector<CodecFrame> frames, uint32_t coded_width,
                                         uint32_t coded_height, uint32_t stride) {
  TRACE_DURATION("media", "H264MultiDecoder::InitializedFrames");
  DLOG("H264MultiDecoder::InitializedFrames");
  // not swapped out, not running
  ZX_DEBUG_ASSERT(state_ == DecoderState::kWaitingForConfigChange);
  ZX_DEBUG_ASSERT(video_frames_.empty());
  ZX_DEBUG_ASSERT(frames.size() <= std::numeric_limits<uint32_t>::max());
  uint32_t frame_count = static_cast<uint32_t>(frames.size());

  for (uint32_t i = 0; i < frame_count; ++i) {
    auto frame = std::make_shared<VideoFrame>();
    // While we'd like to pass in IO_BUFFER_CONTIG, since we know the VMO was
    // allocated with zx_vmo_create_contiguous(), the io_buffer_init_vmo()
    // treats that flag as an invalid argument, so instead we have to pretend as
    // if it's a non-contiguous VMO, then validate that the VMO is actually
    // contiguous later in aml_canvas_config() called by
    // owner_->ConfigureCanvas() below.
    zx_status_t status =
        io_buffer_init_vmo(&frame->buffer, owner_->bti()->get(),
                           frames[i].buffer_spec().vmo_range.vmo().get(), 0, IO_BUFFER_RW);
    if (status != ZX_OK) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_AllocationError);
      LOG(ERROR, "Failed to io_buffer_init_vmo() for frame - status: %d\n", status);
      OnFatalError();
      return;
    }

    // Flush so that there are no dirty CPU cache lines that would potentially overwrite HW-written
    // data.
    io_buffer_cache_flush(&frame->buffer, 0, io_buffer_size(&frame->buffer, 0));
    BarrierAfterFlush();

    frame->hw_width = coded_width;
    frame->hw_height = coded_height;
    frame->coded_width = coded_width;
    frame->coded_height = coded_height;
    frame->stride = stride;
    frame->uv_plane_offset = stride * coded_height;
    frame->display_width = pending_display_width_;
    frame->display_height = pending_display_height_;
    frame->index = i;

    // can be nullptr
    frame->codec_buffer = frames[i].buffer_ptr();
    if (frames[i].buffer_ptr()) {
      frames[i].buffer_ptr()->SetVideoFrame(frame);
    }

    // The ConfigureCanvas() calls validate that the VMO is physically
    // contiguous, regardless of how the VMO was created.
    auto y_canvas = owner_->ConfigureCanvas(
        &frame->buffer, 0, frame->stride, frame->coded_height,
        fh_amlcanvas::CanvasFlags::kRead | fh_amlcanvas::CanvasFlags::kWrite,
        fh_amlcanvas::CanvasBlockMode::kLinear);
    auto uv_canvas = owner_->ConfigureCanvas(
        &frame->buffer, frame->uv_plane_offset, frame->stride, frame->coded_height / 2,
        fh_amlcanvas::CanvasFlags::kRead | fh_amlcanvas::CanvasFlags::kWrite,
        fh_amlcanvas::CanvasBlockMode::kLinear);
    if (!y_canvas || !uv_canvas) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_AllocationError);
      LOG(ERROR, "ConfigureCanvas() failed - y: %d uv: %d", !!y_canvas, !!uv_canvas);
      OnFatalError();
      return;
    }

    // FWIW, this is the leading candidate for what StreamInfo::insignificant() bit would control,
    // but 96 works fine here regardless.  If insignificant() is 1, 24 (maybe), else 96.  Or just
    // 96 always is fine.  This speculative association could be wrong (and/or obsolete) in the
    // first place, so just use 96 here.
    constexpr uint32_t kMvRefDataSizePerMb = 96;

    uint32_t mb_width = coded_width / 16;
    uint32_t mb_height = coded_height / 16;
    uint64_t colocated_buffer_size =
        fbl::round_up(mb_width * mb_height * kMvRefDataSizePerMb, ZX_PAGE_SIZE);

    std::optional<InternalBuffer> mv_buffer;
    std::optional<InternalBuffer> on_deck_mv_buffer;
    if (on_deck_internal_buffers_.has_value() &&
        i < on_deck_internal_buffers_->reference_mv_buffers_.size() &&
        on_deck_internal_buffers_->reference_mv_buffers_[i].has_value()) {
      ZX_DEBUG_ASSERT(on_deck_internal_buffers_->reference_mv_buffers_[i]->present());
      on_deck_mv_buffer = std::move(on_deck_internal_buffers_->reference_mv_buffers_[i]);
      ZX_DEBUG_ASSERT(on_deck_mv_buffer->present());
      ZX_DEBUG_ASSERT(on_deck_internal_buffers_->reference_mv_buffers_[i].has_value());
      ZX_DEBUG_ASSERT(!on_deck_internal_buffers_->reference_mv_buffers_[i]->present());
      on_deck_internal_buffers_->reference_mv_buffers_[i].reset();
    }
    if (on_deck_mv_buffer.has_value()) {
      if (on_deck_mv_buffer->size() < colocated_buffer_size) {
        // For frame index i, we'll replace a buffer that's too small with a buffer that's big
        // enough.
        on_deck_mv_buffer = std::nullopt;
      } else if (on_deck_mv_buffer->is_secure() != is_secure_) {
        // The if condition is essentially using is_secure() of the first on-deck MV buffer as an
        // indicator of whether all the rest of the on-deck MV buffers are also mis-matched
        // is_secure().
        //
        // Can't use this buffer.
        on_deck_mv_buffer = std::nullopt;
        // Go ahead and deallocate these early.  By design we don't keep MV buffers around in case
        // we happen to stream switch back to is_secure() matching again.  In other words we don't
        // keep a mix of is_secure() and !is_secure() MV buffers (since we can't re-use / temporally
        // share any when is_secure() differs).
        on_deck_internal_buffers_->reference_mv_buffers_.clear();
      }
    }
    constexpr bool kMvBufferIsWritable = true;
    constexpr bool kMvBufferIsMappingNeeded = false;
    if (on_deck_mv_buffer.has_value()) {
      ZX_DEBUG_ASSERT(on_deck_mv_buffer->size() >= colocated_buffer_size);
      ZX_DEBUG_ASSERT(on_deck_mv_buffer->is_secure() == is_secure_);
      ZX_DEBUG_ASSERT(on_deck_mv_buffer->is_writable() == kMvBufferIsWritable);
      ZX_DEBUG_ASSERT(on_deck_mv_buffer->is_mapping_needed() == kMvBufferIsMappingNeeded);
      mv_buffer = std::move(on_deck_mv_buffer);
      on_deck_mv_buffer.reset();
    } else {
      auto create_result = InternalBuffer::Create(
          "H264ReferenceMvs", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
          colocated_buffer_size, is_secure_,
          /*is_writable=*/kMvBufferIsWritable, /*is_mapping_needed*/ kMvBufferIsMappingNeeded);
      if (!create_result.is_ok()) {
        LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_AllocationError);
        LOG(ERROR, "Couldn't allocate reference mv buffer - status: %d", create_result.error());
        OnFatalError();
        return;
      }
      mv_buffer.emplace(create_result.take_value());
    }

    video_frames_.push_back(std::shared_ptr<ReferenceFrame>(new ReferenceFrame{
        !!frames[i].initial_usage_count(), false, false, i, std::move(frame), std::move(y_canvas),
        std::move(uv_canvas), std::move(mv_buffer.value())}));
  }
  // Intentionally leave any on-deck mv buffers we don't need for now in
  // on_deck_reference_mv_buffers_, to avoid deallocating and re-allocating if an app is switching
  // between streams with higher and lower DPB count.  We keep the overall ordering consistent so
  // that two streams with fewer larger frames and more smaller frames can still share MV buffers
  // without leading to having more of the bigger MV buffers than we need.

  for (auto& frame : video_frames_) {
    VdecAssistCanvasBlk32::Get()
        .FromValue(0)
        .set_canvas_blk32_wr(true)
        .set_canvas_blk32_is_block(false)
        .set_canvas_index_wr(true)
        .set_canvas_index(frame->y_canvas->index())
        .WriteTo(owner_->dosbus());
    VdecAssistCanvasBlk32::Get()
        .FromValue(0)
        .set_canvas_blk32_wr(true)
        .set_canvas_blk32_is_block(false)
        .set_canvas_index_wr(true)
        .set_canvas_index(frame->uv_canvas->index())
        .WriteTo(owner_->dosbus());
    AncNCanvasAddr::Get(frame->index)
        .FromValue((frame->uv_canvas->index() << 16) | (frame->uv_canvas->index() << 8) |
                   (frame->y_canvas->index()))
        .WriteTo(owner_->dosbus());
  }

  hw_coded_width_ = coded_width;
  hw_coded_height_ = coded_height;
  hw_stride_ = stride;
  // We pretend like these are configured in the HW even though they're not really.
  hw_display_width_ = pending_display_width_;
  hw_display_height_ = pending_display_height_;

  ZX_DEBUG_ASSERT(is_decoder_started_);
  waiting_for_surfaces_ = false;
  state_ = DecoderState::kRunning;
  // this tells hw to go
  AvScratch0::Get()
      .FromValue(static_cast<uint32_t>((next_max_reference_size_ << 24) |
                                       (video_frames_.size() << 16) | (video_frames_.size() << 8)))
      .WriteTo(owner_->dosbus());
  is_hw_active_ = true;
  owner_->watchdog()->Start();
}

void H264MultiDecoder::SubmitFrameMetadata(ReferenceFrame* reference_frame,
                                           const media::H264SPS* sps, const media::H264PPS* pps,
                                           const media::H264DPB& dpb) {
  current_metadata_frame_ = reference_frame;
}

void H264MultiDecoder::SubmitSliceData(SliceData data) {
  // The slices of a picture can get re-used during decode process more than once, if we don't get
  // a pic data done interrupt this time.
  slice_data_map_.emplace(std::make_pair(data.header.first_mb_in_slice, data));
}

void H264MultiDecoder::OutputFrame(ReferenceFrame* reference_frame, uint32_t pts_id) {
  TRACE_DURATION("media", "H264MultiDecoder::OutputFrame");
  ZX_DEBUG_ASSERT(reference_frame->in_use);
  if (reference_frame->is_for_output) {
    frames_to_output_.push_back(reference_frame->index);
  } else {
    // Drop output frame that doesn't correspond to any input frame.  This happens when there are
    // frame_num gaps.  The frame may still have in_internal_use true.
    reference_frame->in_use = false;
  }
  // Don't output a frame that's currently being decoded into, and don't output frames out of order
  // if one's already been queued up.
  if ((frames_to_output_.size() == 1) && (current_metadata_frame_ != reference_frame)) {
    OutputReadyFrames();
  }
}

void H264MultiDecoder::SubmitDataToHardware(const uint8_t* data, size_t length,
                                            const CodecBuffer* codec_buffer,
                                            uint32_t buffer_start_offset) {
  TRACE_DURATION("media", "H264MultiDecoder::SubmitDataToHardware");
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  ZX_DEBUG_ASSERT(length <= std::numeric_limits<uint32_t>::max());
  zx_paddr_t phys_addr{};
  ZX_DEBUG_ASSERT(!phys_addr);
  if (codec_buffer) {
    ZX_DEBUG_ASSERT(codec_buffer->is_known_contiguous());
    phys_addr = codec_buffer->physical_base() + buffer_start_offset;
  }
  if (use_parser_) {
    zx_status_t status =
        owner_->SetProtected(VideoDecoder::Owner::ProtectableHardwareUnit::kParser, is_secure_);
    if (status != ZX_OK) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_DrmConfigError);
      LOG(ERROR, "video_->SetProtected(kParser) failed - status: %d", status);
      OnFatalError();
      return;
    }
    // Pass nullptr because we'll handle syncing updates manually.
    status = owner_->parser()->InitializeEsParser(nullptr);
    if (status != ZX_OK) {
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_InitializationError);
      LOG(ERROR, "InitializeEsParser failed - status: %d", status);
      OnFatalError();
      return;
    }
    uint32_t stream_buffer_empty_space =
        owner_->GetStreamBufferEmptySpaceAfterWriteOffsetBeforeReadOffset(
            owner_->core()->GetStreamInputOffset(),
            unwrapped_saved_read_stream_offset_ % GetStreamBufferSize());
    if (length > stream_buffer_empty_space) {
      // We don't want the parser to hang waiting for output buffer space, since new space will
      // never be released to it since we need to manually update the read pointer.
      //
      // Also, we don't want to overwrite any portion of the stream buffer which we may later need
      // to re-decode.
      //
      // TODO(fxbug.dev/13483): Handle copying only as much as can fit, then copying more in later
      // from the same input packet (a TODO for PumpDecoder()).  Convert this case into an assert.
      //
      // This may happen if a stream fails to provide any decode-able data within the size of the
      // stream buffer.  This is currently how we partially mitigate the cost of the re-decode
      // strategy should a client provide no useful input data.
      //
      // TODO(fxbug.dev/13483): Test, and possibly mitigate better, a hostile client providing 1
      // byte of useless data at a time, causing repeated re-decode of the whole stream buffer as it
      // slowly grows to maximum size, before finally hitting this case and failing the stream.  The
      // test should verify that the decoder remains reasonably avaialble to a competing concurrent
      // well-behaved client providing a well-behaved stream.
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_InputBufferFullError);
      LOG(ERROR, "Empty space in stream buffer %u too small for video data (0x%zx)",
          stream_buffer_empty_space, length);
      OnFatalError();
      return;
    }
    owner_->parser()->SyncFromDecoderInstance(owner_->current_instance());
    DLOG("data: 0x%p phys_addr: 0x%p length: 0x%zx buffer_start_offset: %u", data,
         reinterpret_cast<void*>(phys_addr), length, buffer_start_offset);
    if (phys_addr) {
      status = owner_->parser()->ParseVideoPhysical(phys_addr, static_cast<uint32_t>(length));
    } else {
      status = owner_->parser()->ParseVideo(data, static_cast<uint32_t>(length));
    }
    if (status != ZX_OK) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_InputHwError);
      LOG(ERROR, "Parsing video failed - status: %d", status);
      OnFatalError();
      return;
    }
    status = owner_->parser()->WaitForParsingCompleted(ZX_SEC(10));
    if (status != ZX_OK) {
      LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_InputHwTimeout);
      LOG(ERROR, "Parsing video timed out - status: %d", status);
      owner_->parser()->CancelParsing();
      OnFatalError();
      return;
    }

    owner_->parser()->SyncToDecoderInstance(owner_->current_instance());
  } else {
    zx_status_t status = owner_->ProcessVideoNoParser(data, static_cast<uint32_t>(length));
    if (status != ZX_OK) {
      LogEvent(
          media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_InputProcessingError);
      LOG(ERROR, "Failed to write video");
      OnFatalError();
    }
  }
  unwrapped_write_stream_offset_ += length;
}

bool H264MultiDecoder::IsUtilizingHardware() const {
  return !CanBeSwappedOut() && state_ != DecoderState::kSwappedOut;
}

bool H264MultiDecoder::CanBeSwappedIn() {
  ZX_DEBUG_ASSERT(!in_pump_decoder_);
  if (fatal_error_) {
    return false;
  }
  if (sent_output_eos_to_client_) {
    return false;
  }
  if (waiting_for_surfaces_) {
    return false;
  }
  if (waiting_for_input_) {
    return false;
  }
  return true;
}

bool H264MultiDecoder::CanBeSwappedOut() const {
  // TODO(fxbug.dev/13483): kWaitingForConfigChange ideally would allow swapping out decoder; VP9
  // doesn't yet either, so punt for the moment.
  return force_swap_out_ ||
         (!is_async_pump_pending_ && state_ == DecoderState::kWaitingForInputOrOutput);
}

bool H264MultiDecoder::MustBeSwappedOut() const { return force_swap_out_; }

bool H264MultiDecoder::ShouldSaveInputContext() const { return should_save_input_context_; }

void H264MultiDecoder::SetSwappedOut() {
  ZX_DEBUG_ASSERT_MSG(state_ == DecoderState::kWaitingForInputOrOutput, "state_: %u",
                      static_cast<unsigned int>(state_));
  ZX_DEBUG_ASSERT(CanBeSwappedOut());
  is_async_pump_pending_ = false;
  state_ = DecoderState::kSwappedOut;
}

void H264MultiDecoder::SwappedIn() {
  TRACE_DURATION("media", "H264MultiDecoder::SwappedIn");
  if (!stream_buffer_size_) {
    // Stash this early when we know it's safe to do so, since it's convoluted to get.  This decoder
    // deals with stream buffer details more than other decoders.
    stream_buffer_size_ =
        truncate_to_32(owner_->current_instance()->stream_buffer()->buffer().size());
    ZX_DEBUG_ASSERT(stream_buffer_size_ > kStreamBufferReadAlignment);
    ZX_DEBUG_ASSERT(stream_buffer_size_ % kStreamBufferReadAlignment == 0);
    ZX_DEBUG_ASSERT(stream_buffer_size_ % ZX_PAGE_SIZE == 0);
  }

  // ExtendBits() doesn't know to only let the unwrapped read offset be less than the unwrapped
  // write offset, but rather than teaching ExtendBits() how to do that, just subtract as necessary
  // here instead.
  unwrapped_saved_read_stream_offset_ = ExtendBitsGeneral(
      unwrapped_write_stream_offset_, owner_->core()->GetReadOffset(), stream_buffer_size_);
  if (unwrapped_saved_read_stream_offset_ > unwrapped_write_stream_offset_) {
    unwrapped_saved_read_stream_offset_ -= GetStreamBufferSize();
  }
  ZX_DEBUG_ASSERT(unwrapped_saved_read_stream_offset_ <= unwrapped_write_stream_offset_);

  // Restore the most up-to-date write offset, even if we just restored an old save state, since we
  // want to add more data to decode, not overwrite data we previously wrote.  This also immediately
  // starts allowing the FIFO to fill using the data written previously, which is fine.  But reading
  // from the FIFO won't happen until we tell the decoder to kH264ActionSearchHead.
  owner_->core()->UpdateWriteOffset(unwrapped_write_stream_offset_ % GetStreamBufferSize());

  // Ensure at least one PumpDecoder() before swapping out again.
  //
  // Also, don't pump decoder A synchronously here because we may already be in PumpDecoder() of a
  // different decoder B presently.  This avoids being in PumpDecoder() of more than one decoder
  // at the same time (on the same stack), and avoids re-entering PumpDecoder() of the same decoder.
  is_async_pump_pending_ = true;
  UpdateDiagnostics();
  frame_data_provider_->AsyncPumpDecoder();
}

void H264MultiDecoder::OnSignaledWatchdog() {
  TRACE_DURATION("media", "H264MultiDecoder::OnSignaledWatchdog");
  LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_WatchdogFired);
  LOG(ERROR, "Hit watchdog");
  HandleHardwareError();
}

void H264MultiDecoder::OnFatalError() {
  if (!fatal_error_) {
    // This causes most/all fatal errors to generate two Cobalt events, one more specific and one
    // more generic (WAI).
    fatal_error_ = true;
    client_->OnError();
  }
}

void H264MultiDecoder::QueueInputEos() {
  TRACE_DURATION("media", "H264MultiDecoder::QueueInputEos");
  DLOG("QueueInputEos()");
  ZX_DEBUG_ASSERT(!input_eos_queued_);
  input_eos_queued_ = true;
  ZX_DEBUG_ASSERT(in_pump_decoder_);
  ZX_DEBUG_ASSERT(!sent_output_eos_to_client_);
  ZX_DEBUG_ASSERT(!frame_data_provider_->HasMoreInputData());
  ZX_DEBUG_ASSERT(!is_hw_active_);
  SubmitDataToHardware(kEOS.data(), kEOS.size(), nullptr, 0);
  SubmitDataToHardware(kPadding, kPaddingSize, nullptr, 0);
}

void H264MultiDecoder::ReceivedNewInput() {
  TRACE_DURATION("media", "H264MultiDecoder::ReceivedNewInput");
  waiting_for_input_ = false;
  PumpOrReschedule();
}

void H264MultiDecoder::PropagatePotentialEos() {}

void H264MultiDecoder::RequestStreamReset() {
  TRACE_DURATION("media", "H264MultiDecoder::RequestStreamReset");
  fatal_error_ = true;
  LogEvent(media_metrics::StreamProcessorEvents2MigratedMetricDimensionEvent_StreamReset);
  LOG(ERROR, "fatal_error_ = true");
  frame_data_provider_->AsyncResetStreamAfterCurrentFrame();
  owner_->TryToReschedule();
}

// TODO(fxbug.dev/13483): Overhaul PumpDecoder to do these things:
//  * Separate into fill stream buffer vs. start decode stages.
//  * As long as there's progress since last decode or more input to decode vs. last time we
//    attempted decode, start decode even if no new data was added, or even if only data added was
//    EOS.
//  * Copy partial data from a packet into stream buffer.
//  * Increase PTS-count-driven limit.
//  * Allow more of the stream buffer to be used, and remove dependence on free space standoff >=
//    max packet size.
//  * Allow copying in multiple packets worth of data before starting decode, but put a packet count
//    threshold on this proportional to the # of input packets that exist, and/or a time duration
//    threshold spent copying into stream buffer.  This should balance avoiding excessive re-decode
//    against a duration spike from spending too much time copying into stream buffer before
//    attempting decode and relinquishing the HW to a concurrent stream.
void H264MultiDecoder::PumpDecoder() {
  TRACE_DURATION("media", "H264MultiDecoder::PumpDecoder");
  ZX_DEBUG_ASSERT(!in_pump_decoder_);
  in_pump_decoder_ = true;
  auto set_not_in_pump_decoder = fit::defer([this] { in_pump_decoder_ = false; });

  DLOG(
      "PumpDecoder() - waiting_for_surfaces_: %u waiting_for_input_: %u is_hw_active_: %u state_: "
      "%u sent_output_eos_to_client_: %u fatal_error_: %u",
      waiting_for_surfaces_, waiting_for_input_, is_hw_active_, static_cast<unsigned int>(state_),
      sent_output_eos_to_client_, fatal_error_);

  if (waiting_for_surfaces_ || waiting_for_input_ || is_hw_active_ ||
      (state_ == DecoderState::kSwappedOut) || sent_output_eos_to_client_ || fatal_error_) {
    set_not_in_pump_decoder.call();
    // Depending on case, this call is for swapping out, for swapping in, or is irrelevant.
    owner_->TryToReschedule();
    return;
  }

  // Don't start the HW decoding a frame until we know we have a frame to decode into.
  if (!video_frames_.empty() && !current_frame_ && !IsUnusedReferenceFrameAvailable()) {
    waiting_for_surfaces_ = true;
    DLOG("waiting_for_surfaces_ = true");
    set_not_in_pump_decoder.call();
    owner_->TryToReschedule();
    return;
  }

  // If PtsManager is already holding many offsets after the last decoded frame's first slice
  // header offset, decode more without adding more offsets to PtsManager.
  if (pts_manager_->CountEntriesBeyond(
          unwrapped_first_slice_header_of_frame_decoded_stream_offset_) >=
      PtsManager::kH264MultiQueuedEntryCountThreshold) {
    DLOG("kH264MultiQueuedEntryCountThreshold");
    StartFrameDecode();
    return;
  }

  if (input_eos_queued_) {
    // consume the rest, until an out-of-data interrupt happens
    DLOG("input_eos_queued_");
    StartFrameDecode();
    return;
  }

  // Now we try to get some input data.
  if (!current_data_input_) {
    DLOG("calling ReadMoreInputData()");
    current_data_input_ = frame_data_provider_->ReadMoreInputData();
  }
  if (!current_data_input_) {
    DLOG("!current_data_input_");
    // Don't necessarily need more input to make progress, but avoid triggering detection of no
    // progress being made in StartFrameDecode() if we've already tried decoding with the input
    // data we have so far without any complete frame decode happening last time.
    if (unwrapped_write_stream_offset_ != unwrapped_write_stream_offset_decode_tried_ ||
        unwrapped_first_slice_header_of_frame_decoded_stream_offset_ !=
            unwrapped_first_slice_header_of_frame_decoded_stream_offset_decode_tried_ ||
        per_frame_seen_first_mb_in_slice_ != per_frame_decoded_first_mb_in_slice_) {
      DLOG("might make progress despite lack of new input");
      StartFrameDecode();
      return;
    }
    DLOG("waiting_for_input_ = true");
    waiting_for_input_ = true;
    set_not_in_pump_decoder.call();
    owner_->TryToReschedule();
    return;
  }

  auto& current_input = current_data_input_.value();
  if (current_input.is_eos) {
    DLOG("calling QueueInputEos()");
    QueueInputEos();
    StartFrameDecode();
    return;
  }

  ZX_DEBUG_ASSERT(!current_input.is_eos);
  ZX_DEBUG_ASSERT(current_input.data.empty() == !!current_input.codec_buffer);
  ZX_DEBUG_ASSERT(current_input.length != 0);

  // If the ReadMoreInputData() above gave us more data than will immediately fit in the stream
  // buffer, require the read pointer to advance before adding more.
  //
  // It's possible for a stream with huge headers and/or zero padding to not be decodable with this
  // HW decoder just due to the overall size of the stream buffer and the HW decoder not keeping any
  // incremental progress until decode of a frame is complete.  We can never make the stream buffer
  // large enough to successfully decode all streams with arbitrarily large headers or arbitrarily
  // long runs of zero padding in between frames.  Such streams are not expected to be encountered
  // from any normal source, but if a stream like that is seen, it'll hit the progress check in
  // StartFrameDecode(), so we'll fail quickly instead of getting stuck.
  if (current_input.length + kPaddingSize > owner_->GetStreamBufferEmptySpace()) {
    DLOG("Stream buffer too full, so StartFrameDecode() without adding more");
    StartFrameDecode();
    return;
  }

  if (current_input.pts) {
    pts_manager_->InsertPts(unwrapped_write_stream_offset_, /*has_pts=*/true,
                            current_input.pts.value());
  } else {
    pts_manager_->InsertPts(unwrapped_write_stream_offset_, /*has_pts=*/false, /*pts=*/0);
  }

  // Now we can submit all the data of this AU/packet plus padding to the HW decoder and start it
  // decoding.  We know (at least for now), that the packet boundary doesn't split a NALU, and
  // doesn't split an encoded frame either.  For now, this is similar to VP9 decode on this HW
  // where a whole VP9 superframe has to be in a physically contiguous packet.
  //
  // In future we may need to allow a packet boundary to separate the slices of a multi-slice
  // frame at NALU boundary.  In future we may need to pay attention to known_end_access_unit
  // instead of assuming it is true.  We may need to allow split NALUs.  We may need to allow
  // context switching any time we're not actively decoding which in future could be in the middle
  // of an AU that splits across multiple packets.  At the moment none of these are supported.
  SubmitDataToHardware(current_input.data.data(), current_input.length, current_input.codec_buffer,
                       current_input.buffer_start_offset);
  // TODO(fxbug.dev/13483): We need padding here or else the decoder may stall forever in some
  // circumstances (e.g. if the input ends between 768 and 832 bytes in the buffer). The padding
  // will cause corruption if the input data isn't NAL unit aligned, but that works with existing
  // clients. In the future we could try either detecting that padding was read and caused
  // corruption, which would trigger redecoding of the frame, or we could continually feed as much
  // input as is available and let the H264Decoder lag a bit behind.  Editing out the padding seems
  // less feasible since the fifo (whether in HW or in save/restore context) has potentially already
  // absorbed some of the padding, and the stream offset for propagating PTS(es) through would also
  // need fixup.  Overall, without knowing the frame boundaries on input it doesn't seem there's any
  // way to get low latency decode using this HW.  Thankfully we do know the frame bouanaries on
  // input though, so in practice it's not a big problem so far.
  SubmitDataToHardware(kPadding, kPaddingSize, nullptr, 0);

  // After this, we'll see an interrupt from the HW, either slice header or one of the out-of-data
  // interrupts.
  DLOG("StartFrameDecode() after submit to HW");
  StartFrameDecode();

  // recycle input packet
  current_data_input_.reset();
}

bool H264MultiDecoder::IsUnusedReferenceFrameAvailable() {
  TRACE_DURATION("media", "H264MultiDecoder::IsUnusedReferenceFrameAvailable");
  auto frame = GetUnusedReferenceFrame(/*is_for_output=*/true);
  if (!frame) {
    return false;
  }
  // put back - maybe not ideal, but works for now
  ZX_DEBUG_ASSERT(!frame->in_use);
  frame->in_internal_use = false;
  return true;
}

std::shared_ptr<H264MultiDecoder::ReferenceFrame> H264MultiDecoder::GetUnusedReferenceFrame(
    bool is_for_output) {
  TRACE_DURATION("media", "H264MultiDecoder::GetUnusedReferenceFrame");
  ZX_DEBUG_ASSERT(state_ != DecoderState::kWaitingForConfigChange);
  for (auto& frame : video_frames_) {
    ZX_DEBUG_ASSERT(frame->frame->coded_width ==
                    static_cast<uint32_t>(media_decoder_->GetPicSize().width()));
    ZX_DEBUG_ASSERT(frame->frame->coded_height ==
                    static_cast<uint32_t>(media_decoder_->GetPicSize().height()));
    if (!frame->in_use && !frame->in_internal_use) {
      frame->in_internal_use = true;
      frame->is_for_output = is_for_output;
      return frame;
    }
  }
  return nullptr;
}

H264MultiDecoder::InternalBuffers H264MultiDecoder::TakeInternalBuffers() {
  InternalBuffers result;

  if (firmware_.has_value()) {
    result.firmware_ = std::move(firmware_);
    firmware_.reset();
  }

  if (secondary_firmware_.has_value()) {
    result.secondary_firmware_ = std::move(secondary_firmware_);
    secondary_firmware_.reset();
  }

  if (codec_data_.has_value()) {
    result.codec_data_ = std::move(codec_data_);
    codec_data_.reset();
  }

  if (aux_buf_.has_value()) {
    result.aux_buf_ = std::move(aux_buf_);
    aux_buf_.reset();
  }

  if (lmem_.has_value()) {
    result.lmem_ = std::move(lmem_);
    lmem_.reset();
  }

  // Preserve ordering so that two streams with fewer big buffers and more smaller buffers can still
  // share the overall set of MV buffers without needing extra bigger MV buffers.
  for (auto& frame : video_frames_) {
    result.reference_mv_buffers_.emplace_back(std::move(frame->reference_mv_buffer));
    ZX_DEBUG_ASSERT(!frame->reference_mv_buffer.present());
  }
  if (on_deck_internal_buffers_.has_value()) {
    for (auto& on_deck_mv_buffer : on_deck_internal_buffers_->reference_mv_buffers_) {
      if (!on_deck_mv_buffer.has_value()) {
        // The buffer that was here was obtained just above from video_frames_.
        continue;
      }
      ZX_DEBUG_ASSERT(on_deck_mv_buffer->present());
      result.reference_mv_buffers_.emplace_back(std::move(on_deck_mv_buffer));
      ZX_DEBUG_ASSERT(on_deck_mv_buffer.has_value());
      ZX_DEBUG_ASSERT(!on_deck_mv_buffer->present());
      // Not strictly needed, but nice to avoid leaving !present() values around, and better to be
      // consistent with other places where we need to avoid leaving has_value() but !present().
      on_deck_mv_buffer.reset();
    }
  }

  on_deck_internal_buffers_.reset();

  return result;
}

void H264MultiDecoder::GiveInternalBuffers(InternalBuffers internal_buffers) {
  ZX_DEBUG_ASSERT(video_frames_.empty());
  ZX_DEBUG_ASSERT(!on_deck_internal_buffers_.has_value());
  on_deck_internal_buffers_.emplace(std::move(internal_buffers));
}

zx_status_t H264MultiDecoder::SetupProtection() {
  return owner_->SetProtected(VideoDecoder::Owner::ProtectableHardwareUnit::kVdec, is_secure());
}

uint32_t H264MultiDecoder::GetStreamBufferSize() {
  ZX_DEBUG_ASSERT(stream_buffer_size_);
  return stream_buffer_size_;
}

}  // namespace amlogic_decoder
