// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "h264_multi_decoder.h"

#include <lib/media/codec_impl/codec_buffer.h>

#include <cmath>

#include <fbl/algorithm.h>

#include "h264_utils.h"
#include "media/base/decoder_buffer.h"
#include "media/gpu/h264_decoder.h"
#include "registers.h"
#include "util.h"

namespace {
// See VLD_PADDING_SIZE.
constexpr uint32_t kPaddingSize = 1024;
const uint8_t kPadding[kPaddingSize] = {};
}  // namespace

class AmlogicH264Picture : public media::H264Picture {
 public:
  explicit AmlogicH264Picture(std::shared_ptr<H264MultiDecoder::ReferenceFrame> pic)
      : internal_picture(pic) {}
  ~AmlogicH264Picture() override {
    auto pic = internal_picture.lock();
    if (pic)
      pic->in_internal_use = false;
  }

  std::weak_ptr<H264MultiDecoder::ReferenceFrame> internal_picture;
};
class MultiAccelerator : public media::H264Decoder::H264Accelerator {
 public:
  explicit MultiAccelerator(H264MultiDecoder* owner) : owner_(owner) {}

  scoped_refptr<media::H264Picture> CreateH264Picture() override {
    DLOG("Got MultiAccelerator::CreateH264Picture");
    auto pic = owner_->GetUnusedReferenceFrame();
    if (!pic)
      return nullptr;
    return std::make_shared<AmlogicH264Picture>(pic);
  }

  Status SubmitFrameMetadata(const media::H264SPS* sps, const media::H264PPS* pps,
                             const media::H264DPB& dpb,
                             const media::H264Picture::Vector& ref_pic_listp0,
                             const media::H264Picture::Vector& ref_pic_listb0,
                             const media::H264Picture::Vector& ref_pic_listb1,
                             scoped_refptr<media::H264Picture> pic) override {
    DLOG("Got MultiAccelerator::SubmitFrameMetadata");
    // Only allow decoding one frame at a time. The received picture interrupt will set this to
    // false and trigger PumpDecoder again.
    if (owner_->currently_decoding())
      return Status::kTryAgain;
    auto ref_pic = static_cast<AmlogicH264Picture*>(pic.get())->internal_picture.lock();
    if (!ref_pic)
      return Status::kFail;
    current_sps_ = *sps;
    owner_->SubmitFrameMetadata(ref_pic.get(), sps, pps, dpb);
    return Status::kOk;
  }

  Status SubmitSlice(const media::H264PPS* pps, const media::H264SliceHeader* slice_hdr,
                     const media::H264Picture::Vector& ref_pic_list0,
                     const media::H264Picture::Vector& ref_pic_list1,
                     scoped_refptr<media::H264Picture> pic, const uint8_t* data, size_t size,
                     const std::vector<media::SubsampleEntry>& subsamples) override {
    if (owner_->currently_decoding())
      return Status::kTryAgain;
    DLOG("Got MultiAccelerator::SubmitSlice");
    constexpr uint8_t kHeader[] = {0, 0, 1};
    owner_->SubmitDataToHardware(kHeader, sizeof(kHeader));
    owner_->SubmitDataToHardware(data, size);
    H264MultiDecoder::SliceData slice_data;
    slice_data.sps = current_sps_;
    slice_data.pps = *pps;
    slice_data.header = *slice_hdr;
    slice_data.pic = pic;
    slice_data.ref_pic_list0 = ref_pic_list0;
    slice_data.ref_pic_list1 = ref_pic_list1;
    owner_->SubmitSliceData(std::move(slice_data));
    return Status::kOk;
  }

  Status SubmitDecode(scoped_refptr<media::H264Picture> pic) override {
    if (owner_->currently_decoding())
      return Status::kTryAgain;
    auto ref_pic = static_cast<AmlogicH264Picture*>(pic.get())->internal_picture.lock();
    if (!ref_pic)
      return Status::kFail;
    DLOG("Got MultiAccelerator::SubmitDecode picture %d", ref_pic->index);
    owner_->SubmitDataToHardware(kPadding, kPaddingSize);
    owner_->StartFrameDecode();
    return Status::kOk;
  }

  bool OutputPicture(scoped_refptr<media::H264Picture> pic) override {
    auto ref_pic = static_cast<AmlogicH264Picture*>(pic.get())->internal_picture.lock();
    if (!ref_pic)
      return false;
    DLOG("Got MultiAccelerator::OutputPicture picture %d", ref_pic->index);
    owner_->OutputFrame(ref_pic.get(), pic->bitstream_id());
    return true;
  }

  void Reset() override {}

  Status SetStream(base::span<const uint8_t> stream,
                   const media::DecryptConfig* decrypt_config) override {
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

  DEF_FIELD(14, 13, chroma_format_idc);
  DEF_BIT(15, frame_mbs_only_flag);
  DEF_FIELD(23, 16, aspect_ratio_idc);

  static auto Get() { return AddrType(0x09c2 * 4); }
};

// AvScratchB
class StreamInfo2 : public TypedRegisterBase<DosRegisterIo, StreamInfo2, uint32_t> {
 public:
  DEF_FIELD(7, 0, level_idc);
  DEF_FIELD(15, 8, max_reference_size);

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
  kH264ActionSearchHead = 0xf0,

  // Done responding to a config request.
  kH264ActionConfigDone = 0xf2,

  // Decode a slice (not the first one) in a picture.
  kH264ActionDecodeSlice = 0xf1,

  // Decode the first slice in a new picture.
  kH264ActionDecodeNewpic = 0xf3,
};

// Actions written by the firmware into DpbStatusReg before an interrupt to tell
// the CPU what to do.
enum H264Status {
  // Configure the DPB.
  kH264ConfigRequest = 0x11,

  // Out of input data, so get more.
  kH264DataRequest = 0x12,

  // Initialize the current set of reference frames and output buffer to be
  // decoded into.
  kH264SliceHeadDone = 0x1,

  // Store the current frame into the DPB, or output it.
  kH264PicDataDone = 0x2,
};

H264MultiDecoder::H264MultiDecoder(Owner* owner, Client* client, FrameDataProvider* provider)
    : VideoDecoder(owner, client, /*is_secure=*/false), frame_data_provider_(provider) {
  media_decoder_ = std::make_unique<media::H264Decoder>(std::make_unique<MultiAccelerator>(this),
                                                        media::H264PROFILE_HIGH);
}

H264MultiDecoder::~H264MultiDecoder() {
  if (owner_->IsDecoderCurrent(this)) {
    owner_->core()->StopDecoding();
    owner_->core()->WaitForIdle();
  }
  BarrierBeforeRelease();
}

zx_status_t H264MultiDecoder::Initialize() {
  zx_status_t status = InitializeBuffers();
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to initialize buffers");
    return status;
  }

  return InitializeHardware();
}

zx_status_t H264MultiDecoder::LoadSecondaryFirmware(const uint8_t* data, uint32_t firmware_size) {
  if (secondary_firmware_)
    return ZX_OK;
  // For some reason, some portions of the firmware aren't loaded into the
  // hardware directly, but are kept in main memory.
  constexpr uint32_t kSecondaryFirmwareSize = 4 * 1024;
  // Some sections of the input firmware are copied into multiple places in the output buffer, and 1
  // part of the output buffer seems to be unused.
  constexpr uint32_t kFirmwareSectionCount = 9;
  constexpr uint32_t kSecondaryFirmwareBufferSize = kSecondaryFirmwareSize * kFirmwareSectionCount;
  constexpr uint32_t kBufferAlignShift = 16;
  auto result = InternalBuffer::CreateAligned(
      "H264MultiSecondaryFirmware", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
      kSecondaryFirmwareBufferSize, 1 << kBufferAlignShift, /*is_secure*/ false,
      /*is_writable=*/true, /*is_mapping_needed*/ true);
  if (!result.is_ok()) {
    DECODE_ERROR("Failed to make second firmware buffer: %d", result.error());
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
  secondary_firmware_->CacheFlush(0, kSecondaryFirmwareBufferSize);
  BarrierAfterFlush();
  return ZX_OK;
}

constexpr uint32_t kAuxBufPrefixSize = 16 * 1024;
constexpr uint32_t kAuxBufSuffixSize = 0;

zx_status_t H264MultiDecoder::InitializeBuffers() {
  constexpr uint32_t kBufferAlignment = 1 << 16;
  constexpr uint32_t kCodecDataSize = 0x200000;
  auto codec_data_create_result =
      InternalBuffer::CreateAligned("H264MultiCodecData", &owner_->SysmemAllocatorSyncPtr(),
                                    owner_->bti(), kCodecDataSize, kBufferAlignment, is_secure(),
                                    /*is_writable=*/true, /*is_mapping_needed*/ false);
  if (!codec_data_create_result.is_ok()) {
    LOG(ERROR, "Failed to make codec data buffer - status: %d", codec_data_create_result.error());
    return codec_data_create_result.error();
  }
  codec_data_.emplace(codec_data_create_result.take_value());

  // Aux buf seems to be used for reading SEI data.
  constexpr uint32_t kAuxBufSize = kAuxBufPrefixSize + kAuxBufSuffixSize;
  auto aux_buf_create_result =
      InternalBuffer::CreateAligned("H264AuxBuf", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
                                    kAuxBufSize, kBufferAlignment, /*is_secure=*/false,
                                    /*is_writable=*/true, /*is_mapping_needed*/ false);
  if (!aux_buf_create_result.is_ok()) {
    LOG(ERROR, "Failed to make aux buffer - status: %d", aux_buf_create_result.error());
    return aux_buf_create_result.error();
  }
  aux_buf_.emplace(aux_buf_create_result.take_value());

  // Lmem is used to dump the AMRISC's local memory, which is needed for updating the DPB.
  constexpr uint32_t kLmemBufSize = 4096;
  auto lmem_create_result =
      InternalBuffer::CreateAligned("H264AuxBuf", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
                                    kLmemBufSize, kBufferAlignment, /*is_secure=*/false,
                                    /*is_writable=*/true, /*is_mapping_needed*/ true);
  if (!lmem_create_result.is_ok()) {
    LOG(ERROR, "Failed to make lmem buffer - status: %d", lmem_create_result.error());
    return lmem_create_result.error();
  }
  lmem_.emplace(lmem_create_result.take_value());

  return ZX_OK;
}

void H264MultiDecoder::ResetHardware() {
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

  DosSwReset0::Get().FromValue(0).set_vdec_pic_dc(1).set_vdec_dblk(1).WriteTo(owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Reads are used for delaying running later code.
  for (uint32_t i = 0; i < 3; i++) {
    DosSwReset0::Get().ReadFrom(owner_->dosbus());
  }

  auto temp = PowerCtlVld::Get().ReadFrom(owner_->dosbus());
  temp.set_reg_value(temp.reg_value() | (1 << 9) | (1 << 6));
  temp.WriteTo(owner_->dosbus());
}

zx_status_t H264MultiDecoder::InitializeHardware() {
  ZX_DEBUG_ASSERT(state_ == DecoderState::kSwappedOut);
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  if (is_secure()) {
    DECODE_ERROR("is_secure() == true not yet supported by H264MultiDecoder");
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t status =
      owner_->SetProtected(VideoDecoder::Owner::ProtectableHardwareUnit::kVdec, is_secure());
  if (status != ZX_OK)
    return status;
  FirmwareBlob::FirmwareType firmware_type = FirmwareBlob::FirmwareType::kDec_H264_Multi_Gxm;

  // Don't use the TEE to load the firmware, since the version we're using on astro and sherlock
  // doesn't support H264_Multi_Gxm.
  uint8_t* data;
  uint32_t firmware_size;
  status = owner_->firmware_blob()->GetFirmwareData(firmware_type, &data, &firmware_size);
  if (status != ZX_OK)
    return status;
  status = owner_->core()->LoadFirmware(data, firmware_size);
  if (status != ZX_OK)
    return status;
  status = LoadSecondaryFirmware(data, firmware_size);
  if (status != ZX_OK)
    return status;

  ResetHardware();
  AvScratchG::Get()
      .FromValue(truncate_to_32(secondary_firmware_->phys_base()))
      .WriteTo(owner_->dosbus());

  PscaleCtrl::Get().FromValue(0).WriteTo(owner_->dosbus());
  VdecAssistMbox1ClrReg::Get().FromValue(1).WriteTo(owner_->dosbus());
  VdecAssistMbox1Mask::Get().FromValue(1).WriteTo(owner_->dosbus());
  {
    auto temp = MdecPicDcCtrl::Get().ReadFrom(owner_->dosbus()).set_nv12_output(true);
    temp.set_reg_value(temp.reg_value() | (0xbf << 24));
    temp.WriteTo(owner_->dosbus());
    temp.set_reg_value(temp.reg_value() & ~(0xbf << 24));
    temp.WriteTo(owner_->dosbus());
  }
  MdecPicDcMuxCtrl::Get().ReadFrom(owner_->dosbus()).set_bit31(0).WriteTo(owner_->dosbus());
  MdecExtIfCfg0::Get().FromValue(0).WriteTo(owner_->dosbus());
  MdecPicDcThresh::Get().FromValue(0x404038aa).WriteTo(owner_->dosbus());

  // Signal that the DPB hasn't been initialized yet.
  if (video_frames_.size() > 0) {
    for (auto& frame : video_frames_) {
      AncNCanvasAddr::Get(frame->index)
          .FromValue((frame->uv_canvas->index() << 16) | (frame->uv_canvas->index() << 8) |
                     (frame->y_canvas->index()))
          .WriteTo(owner_->dosbus());
    }
    AvScratch7::Get()
        .FromValue(static_cast<uint32_t>((next_max_reference_size_ << 24) |
                                         (video_frames_.size() << 16) |
                                         (video_frames_.size() << 8)))
        .WriteTo(owner_->dosbus());
  } else {
    AvScratch0::Get().FromValue(0).WriteTo(owner_->dosbus());
    AvScratch9::Get().FromValue(0).WriteTo(owner_->dosbus());
  }
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
  DebugReg1::Get().FromValue(0).WriteTo(owner_->dosbus());
  DebugReg2::Get().FromValue(0).WriteTo(owner_->dosbus());
  H264DecodeInfo::Get().FromValue(1 << 13).WriteTo(owner_->dosbus());
  // TODO(fxb/13483): Use real values.
  constexpr uint32_t kBytesToDecode = 100000;
  H264DecodeSizeReg::Get().FromValue(kBytesToDecode).WriteTo(owner_->dosbus());
  ViffBitCnt::Get().FromValue(kBytesToDecode * 8).WriteTo(owner_->dosbus());

  H264AuxAddr::Get().FromValue(truncate_to_32(aux_buf_->phys_base())).WriteTo(owner_->dosbus());
  H264AuxDataSize::Get()
      .FromValue(((kAuxBufPrefixSize / 16) << 16) | (kAuxBufSuffixSize / 16))
      .WriteTo(owner_->dosbus());
  H264DecodeModeReg::Get().FromValue(kDecodeModeMultiStreamBased).WriteTo(owner_->dosbus());
  H264DecodeSeqInfo::Get().FromValue(seq_info2_).WriteTo(owner_->dosbus());
  HeadPaddingReg::Get().FromValue(0).WriteTo(owner_->dosbus());
  InitFlagReg::Get().FromValue(have_initialized_).WriteTo(owner_->dosbus());
  have_initialized_ = true;

  // TODO(fxb/13483): Set to 1 when SEI is supported.
  NalSearchCtl::Get().FromValue(0).WriteTo(owner_->dosbus());
  state_ = DecoderState::kInitialWaitingForInput;
  return ZX_OK;
}

void H264MultiDecoder::StartFrameDecode() {
  ZX_DEBUG_ASSERT(state_ == DecoderState::kInitialWaitingForInput ||
                  state_ == DecoderState::kStoppedWaitingForInput);
  currently_decoding_ = true;

  // For now, just use the decode size from InitializeHardware.
  if (state_ == DecoderState::kInitialWaitingForInput) {
    // TODO(fxb/13483): Use real value.
    constexpr uint32_t kBytesToDecode = 100000;
    ViffBitCnt::Get().FromValue(kBytesToDecode * 8).WriteTo(owner_->dosbus());
    owner_->core()->StartDecoding();
  }
  DpbStatusReg::Get().FromValue(kH264ActionSearchHead).WriteTo(owner_->dosbus());
  state_ = DecoderState::kRunning;
}

void H264MultiDecoder::ConfigureDpb() {
  ZX_DEBUG_ASSERT(currently_decoding_);
  ZX_DEBUG_ASSERT(!video_frames_.empty());
  seq_info2_ = AvScratch1::Get().ReadFrom(owner_->dosbus()).reg_value();
  for (auto& frame : video_frames_) {
    AncNCanvasAddr::Get(frame->index)
        .FromValue((frame->uv_canvas->index() << 16) | (frame->uv_canvas->index() << 8) |
                   (frame->y_canvas->index()))
        .WriteTo(owner_->dosbus());
  }
  AvScratch0::Get()
      .FromValue(static_cast<uint32_t>((next_max_reference_size_ << 24) |
                                       (video_frames_.size() << 16) | (video_frames_.size() << 8)))
      .WriteTo(owner_->dosbus());
}

// This struct contains parameters for the current frame that are dumped from
// lmem
struct HardwareRenderParams {
  uint16_t data[0x400];
  static constexpr uint32_t kOffsetDelimiterLo = 0x2f;
  static constexpr uint32_t kOffsetDelimiterHi = 0x30;

  static constexpr uint32_t kNalUnitType = 0x80;
  static constexpr uint32_t kNalRefIdc = 0x81;
  static constexpr uint32_t kSliceType = 0x82;
  static constexpr uint32_t kLog2MaxFrameNum = 0x83;
  static constexpr uint32_t kPicOrderCntType = 0x85;
  static constexpr uint32_t kLog2MaxPicOrderCntLsb = 0x86;
  static constexpr uint32_t kMode8x8Flag = 0x8c;
  static constexpr uint32_t kEntropyCodingModeFlag = 0x8d;
  static constexpr uint32_t kProfileIdcMmco = 0xe7;

  // offset to dpb_max_buffer_frame.
  static constexpr uint32_t kDpbStructStart = 0x100 + 24 * 8;
  static constexpr uint32_t kPicOrderCntLsb = kDpbStructStart + 14;
  static constexpr uint32_t kDeltaPicOrderCntBottom0 = kDpbStructStart + 19;
  static constexpr uint32_t kDeltaPicOrderCntBottom1 = kDpbStructStart + 20;

  // Read a pair of entries starting at |offset| as a 32-bit number.
  uint32_t Read32(uint32_t offset) {
    // Little endian.
    return data[offset] | (static_cast<uint32_t>(data[offset + 1]) << 16);
  }

  void ReadFromLmem(InternalBuffer* lmem) {
    lmem->CacheFlushInvalidate(0, sizeof(data));
    uint16_t* input_params = reinterpret_cast<uint16_t*>(lmem->virt_base());

    // Convert from middle-endian.
    for (uint32_t i = 0; i < fbl::count_of(data); i += 4) {
      for (uint32_t j = 0; j < 4; j++) {
        data[i + j] = input_params[i + (3 - j)];
      }
    }
  }
};

void H264MultiDecoder::InitializeRefPics(
    const std::vector<std::shared_ptr<media::H264Picture>>& ref_pic_list, uint32_t reg_offset) {
  uint32_t ref_list[8] = {};
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
    ZX_DEBUG_ASSERT(internal_picture);

    // Offset into AncNCanvasAddr registers.
    uint32_t canvas_index = internal_picture->index;
    constexpr uint32_t kFrameFlag = 0x3;
    constexpr uint32_t kFieldTypeBitOffset = 5;
    uint32_t cfg = canvas_index | (kFrameFlag << kFieldTypeBitOffset);
    // Every dword stores 4 reference pics, lowest index in the highest bits.
    uint32_t offset_into_dword = 8 * (3 - (i % 4));
    ref_list[i / 4] |= (cfg << offset_into_dword);
  }

  H264BufferInfoIndex::Get().FromValue(reg_offset).WriteTo(owner_->dosbus());
  for (uint32_t reg_value : ref_list) {
    H264BufferInfoData::Get().FromValue(reg_value).WriteTo(owner_->dosbus());
  }
}

void H264MultiDecoder::HandleSliceHeadDone() {
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  // Setup reference frames and output buffers before decoding.
  HardwareRenderParams params;
  params.ReadFromLmem(&*lmem_);
  DLOG("NAL unit type: %d\n", params.data[HardwareRenderParams::kNalUnitType]);
  DLOG("NAL ref_idc: %d\n", params.data[HardwareRenderParams::kNalRefIdc]);
  DLOG("NAL slice_type: %d\n", params.data[HardwareRenderParams::kSliceType]);
  DLOG("pic order cnt type: %d\n", params.data[HardwareRenderParams::kPicOrderCntType]);
  DLOG("log2_max_frame_num: %d\n", params.data[HardwareRenderParams::kLog2MaxFrameNum]);
  DLOG("log2_max_pic_order_cnt: %d\n", params.data[HardwareRenderParams::kLog2MaxPicOrderCntLsb]);
  DLOG("entropy coding mode flag: %d\n", params.data[HardwareRenderParams::kEntropyCodingModeFlag]);
  DLOG("profile idc mmc0: %d\n", params.data[HardwareRenderParams::kProfileIdcMmco]);
  DLOG("Offset delimiter %d", params.Read32(HardwareRenderParams::kOffsetDelimiterLo));
  DLOG("Mode 8x8 flags: 0x%x\n", params.data[HardwareRenderParams::kMode8x8Flag]);
  current_frame_ = current_metadata_frame_;
  if (slice_data_list_.empty()) {
    DECODE_ERROR("No slice data for frame");
    OnFatalError();
    return;
  }
  SliceData slice_data = std::move(slice_data_list_.front());
  slice_data_list_.pop_front();
  auto poc = poc_.ComputePicOrderCnt(&slice_data.sps, slice_data.header);
  if (!poc) {
    DECODE_ERROR("No poc");
    OnFatalError();
    return;
  }

  DLOG("Frame POC %d", poc.value());
  DLOG("mb_adaptive_frame_field %d field pic pic flag %d\n",
       slice_data.sps.mb_adaptive_frame_field_flag, slice_data.header.field_pic_flag);

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
  current_frame_->is_long_term_reference = slice_data.pic->long_term;

  H264BufferInfoIndex::Get().FromValue(16).WriteTo(owner_->dosbus());

  // Store information about the properties of each canvas image.
  for (uint32_t i = 0; i < video_frames_.size(); ++i) {
    bool is_long_term = video_frames_[i]->is_long_term_reference;
    if (is_long_term) {
      // Everything is progressive, so mark as having both bottom and top as long-term refrences.
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
  InitializeRefPics(slice_data.ref_pic_list0, 0);
  InitializeRefPics(slice_data.ref_pic_list1, 8);

  // Wait for the hardware to finish processing its current mbs.
  if (!SpinWaitForRegister(std::chrono::milliseconds(100), [&] {
        return !H264CoMbRwCtl::Get().ReadFrom(owner_->dosbus()).busy();
      })) {
    DECODE_ERROR("Failed to wait for rw register nonbusy");
    OnFatalError();
    return;
  }

  constexpr uint32_t kMvRefDataSizePerMb = 96;
  uint32_t mv_size = kMvRefDataSizePerMb;

  if ((params.data[HardwareRenderParams::kMode8x8Flag] & 4) &&
      (params.data[HardwareRenderParams::kMode8x8Flag] & 2)) {
    // direct 8x8 mode seems to store 1/4 the data, so the offsets need to be less as well.
    mv_size /= 4;
  }
  uint32_t mv_byte_offset = slice_data.header.first_mb_in_slice * mv_size;

  H264CoMbWrAddr::Get()
      .FromValue(truncate_to_32(current_frame_->reference_mv_buffer.phys_base()) + mv_byte_offset)
      .WriteTo(owner_->dosbus());

  // 8.4.1.2.1 - co-located motion vectors come from RefPictList1[0] for frames.
  if (slice_data.ref_pic_list1.size() > 0) {
    auto* amlogic_picture = static_cast<AmlogicH264Picture*>(slice_data.ref_pic_list1[0].get());
    if (amlogic_picture) {
      auto internal_picture = amlogic_picture->internal_picture.lock();
      ZX_DEBUG_ASSERT(internal_picture);
      uint32_t read_addr =
          truncate_to_32(internal_picture->reference_mv_buffer.phys_base()) + mv_byte_offset;
      ZX_DEBUG_ASSERT(read_addr % 8 == 0);
      H264CoMbRdAddr::Get().FromValue((read_addr >> 3) | (2u << 30)).WriteTo(owner_->dosbus());
    }
  }

  if (slice_data.header.first_mb_in_slice == 0)
    DpbStatusReg::Get().FromValue(kH264ActionDecodeNewpic).WriteTo(owner_->dosbus());
  else
    DpbStatusReg::Get().FromValue(kH264ActionDecodeSlice).WriteTo(owner_->dosbus());
}

void H264MultiDecoder::FlushFrames() {
  auto res = media_decoder_->Flush();
  DLOG("Got media decoder res %d", res);
}

void H264MultiDecoder::DumpStatus() {
  DLOG("ViffBitCnt: %d", ViffBitCnt::Get().ReadFrom(owner_->dosbus()).reg_value());
  DLOG("Viifolevel: %d", VldMemVififoLevel::Get().ReadFrom(owner_->dosbus()).reg_value());
  DLOG("input offset: %d read offset: %d", owner_->core()->GetStreamInputOffset(),
       owner_->core()->GetReadOffset());
  DLOG("Error status reg %d mbymbx reg %d",
       ErrorStatusReg::Get().ReadFrom(owner_->dosbus()).reg_value(),
       MbyMbx::Get().ReadFrom(owner_->dosbus()).reg_value());
  DLOG("DpbStatusReg 0x%x", DpbStatusReg::Get().ReadFrom(owner_->dosbus()).reg_value());
}

void H264MultiDecoder::HandlePicDataDone() {
  ZX_DEBUG_ASSERT(current_frame_);
  // TODO(fxb/13483): Get PTS
  current_frame_ = nullptr;
  current_metadata_frame_ = nullptr;
  ZX_DEBUG_ASSERT(slice_data_list_.size() == 0);

  OutputReadyFrames();
  state_ = DecoderState::kInitialWaitingForInput;
  owner_->core()->StopDecoding();

  currently_decoding_ = false;
  if (pending_config_change_) {
    StartConfigChange();
  } else {
    owner_->TryToReschedule();
    if (state_ == DecoderState::kInitialWaitingForInput) {
      PumpDecoder();
    }
  }
}

void H264MultiDecoder::OutputReadyFrames() {
  while (!frames_to_output_.empty()) {
    uint32_t index = frames_to_output_.front();
    frames_to_output_.pop_front();
    client_->OnFrameReady(video_frames_[index]->frame);
  }
}

void H264MultiDecoder::HandleInterrupt() {
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  // Clear interrupt
  VdecAssistMbox1ClrReg::Get().FromValue(1).WriteTo(owner_->dosbus());
  uint32_t decode_status = DpbStatusReg::Get().ReadFrom(owner_->dosbus()).reg_value();
  DLOG("Got H264MultiDecoder::HandleInterrupt, decode status: %x", decode_status);
  switch (decode_status) {
    case kH264ConfigRequest: {
      DpbStatusReg::Get().FromValue(kH264ActionConfigDone).WriteTo(owner_->dosbus());
      ConfigureDpb();
      break;
    }
    case kH264DataRequest:
      DECODE_ERROR("Got unhandled data request");
      break;
    case kH264SliceHeadDone: {
      HandleSliceHeadDone();
      break;
    }
    case kH264PicDataDone: {
      HandlePicDataDone();
      break;
    }
  }
}

void H264MultiDecoder::PumpOrReschedule() {
  if (state_ == DecoderState::kSwappedOut) {
    owner_->TryToReschedule();
    // TryToReschedule will pump the decoder (using SwappedIn) once the decoder is finally
    // rescheduled.
  } else {
    PumpDecoder();
  }
}

void H264MultiDecoder::ReturnFrame(std::shared_ptr<VideoFrame> frame) {
  DLOG("H264MultiDecoder::ReturnFrame %d", frame->index);
  ZX_DEBUG_ASSERT(frame->index < video_frames_.size());
  ZX_DEBUG_ASSERT(video_frames_[frame->index]->frame == frame);
  video_frames_[frame->index]->in_use = false;
  waiting_for_surfaces_ = false;
  PumpOrReschedule();
}

void H264MultiDecoder::CallErrorHandler() { OnFatalError(); }

void H264MultiDecoder::InitializedFrames(std::vector<CodecFrame> frames, uint32_t coded_width,
                                         uint32_t coded_height, uint32_t stride) {
  DLOG("H264MultiDecoder::InitializeFrame");
  uint32_t frame_count = frames.size();
  video_frames_.clear();
  for (uint32_t i = 0; i < frame_count; ++i) {
    auto frame = std::make_shared<VideoFrame>();
    // While we'd like to pass in IO_BUFFER_CONTIG, since we know the VMO was
    // allocated with zx_vmo_create_contiguous(), the io_buffer_init_vmo()
    // treats that flag as an invalid argument, so instead we have to pretend as
    // if it's a non-contiguous VMO, then validate that the VMO is actually
    // contiguous later in aml_canvas_config() called by
    // owner_->ConfigureCanvas() below.
    assert(frames[i].codec_buffer_spec.has_data());
    assert(frames[i].codec_buffer_spec.data().is_vmo());
    assert(frames[i].codec_buffer_spec.data().vmo().has_vmo_handle());
    zx_status_t status = io_buffer_init_vmo(
        &frame->buffer, owner_->bti()->get(),
        frames[i].codec_buffer_spec.data().vmo().vmo_handle().get(), 0, IO_BUFFER_RW);
    if (status != ZX_OK) {
      DECODE_ERROR("Failed to io_buffer_init_vmo() for frame - status: %d\n", status);
      OnFatalError();
      return;
    }
    io_buffer_cache_flush(&frame->buffer, 0, io_buffer_size(&frame->buffer, 0));

    BarrierAfterFlush();

    frame->hw_width = coded_width;
    frame->hw_height = coded_height;
    frame->coded_width = coded_width;
    frame->coded_height = coded_height;
    frame->stride = stride;
    frame->uv_plane_offset = stride * coded_height;
    frame->display_width = display_width_;
    frame->display_height = display_height_;
    frame->index = i;

    // can be nullptr
    frame->codec_buffer = frames[i].codec_buffer_ptr;
    if (frames[i].codec_buffer_ptr) {
      frames[i].codec_buffer_ptr->SetVideoFrame(frame);
    }

    // The ConfigureCanvas() calls validate that the VMO is physically
    // contiguous, regardless of how the VMO was created.
    auto y_canvas =
        owner_->ConfigureCanvas(&frame->buffer, 0, frame->stride, frame->coded_height, 0, 0);
    auto uv_canvas = owner_->ConfigureCanvas(&frame->buffer, frame->uv_plane_offset, frame->stride,
                                             frame->coded_height / 2, 0, 0);
    if (!y_canvas || !uv_canvas) {
      OnFatalError();
      return;
    }
    constexpr uint32_t kMvRefDataSizePerMb = 96;
    uint32_t colocated_buffer_size =
        fbl::round_up(mb_width_ * mb_height_ * kMvRefDataSizePerMb, ZX_PAGE_SIZE);

    auto create_result =
        InternalBuffer::Create("H264ReferenceMvs", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(),
                               colocated_buffer_size, is_secure_,
                               /*is_writable=*/true, /*is_mapping_needed*/ false);
    if (!create_result.is_ok()) {
      LOG(ERROR, "Couldn't allocate reference mv buffer - status: %d", create_result.error());
      OnFatalError();
      return;
    }

    video_frames_.push_back(std::shared_ptr<ReferenceFrame>(
        new ReferenceFrame{false, false, i, std::move(frame), std::move(y_canvas),
                           std::move(uv_canvas), create_result.take_value()}));
  }
  waiting_for_surfaces_ = false;
  PumpOrReschedule();
}

void H264MultiDecoder::SubmitFrameMetadata(ReferenceFrame* reference_frame,
                                           const media::H264SPS* sps, const media::H264PPS* pps,
                                           const media::H264DPB& dpb) {
  current_metadata_frame_ = reference_frame;
  // Unclear why this is, but matches the linux decoder.
  constexpr uint32_t kReferenceBufMargin = 4;
  next_max_reference_size_ = sps->max_num_ref_frames + kReferenceBufMargin;
}

void H264MultiDecoder::SubmitSliceData(SliceData data) {
  // Only queue up data in a list instead of starting the decode in hardware. We could try to submit
  // it now, but that makes it more difficult to swap out if we only receive data for a partial
  // frame from the client and would want to try to swap out between slices.
  slice_data_list_.push_back(data);
}

void H264MultiDecoder::OutputFrame(ReferenceFrame* reference_frame, uint32_t pts_id) {
  ZX_DEBUG_ASSERT(reference_frame->in_use);
  auto it = id_to_pts_map_.find(pts_id);
  if (it != id_to_pts_map_.end()) {
    reference_frame->frame->has_pts = true;
    reference_frame->frame->pts = it->second;
    id_to_pts_map_.erase(it);
  }
  frames_to_output_.push_back(reference_frame->index);
  // Don't output a frame that's currently being decoded into, and don't output frames out of order
  // if one's already been queued up.
  if ((frames_to_output_.size() == 1) && (current_metadata_frame_ != reference_frame)) {
    OutputReadyFrames();
  }
}

void H264MultiDecoder::SubmitDataToHardware(const uint8_t* data, size_t length) {
  ZX_DEBUG_ASSERT(owner_->IsDecoderCurrent(this));
  zx_status_t status = owner_->ProcessVideoNoParser(data, length);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to write video");
    OnFatalError();
  }
}
bool H264MultiDecoder::CanBeSwappedIn() {
  if (fatal_error_)
    return false;
  if (waiting_for_surfaces_)
    return false;

  ZX_DEBUG_ASSERT(!pending_config_change_ || currently_decoding_);
  // If there aren't any free output frames the decoder will be swapped in, hit kRanOutOfSurfaces,
  // then be swapped out (if necessary). Similarly, if there isn't enough data for a complete frame
  // it will be swapped in, will put what data exists in the stream buffer, then hit
  // kRanOutOfStreamData before trying to decode any of it.
  // TODO(fxb/13483): Wait for all requirements before swapping in the hardware to avoid unnecessary
  // changes.
  return current_decoder_buffer_ || frame_data_provider_->HasMoreInputData();
}

bool H264MultiDecoder::CanBeSwappedOut() const {
  return state_ == DecoderState::kInitialWaitingForInput ||
         state_ == DecoderState::kStoppedWaitingForInput;
}

void H264MultiDecoder::SetSwappedOut() {
  ZX_DEBUG_ASSERT(state_ == DecoderState::kInitialWaitingForInput);
  ZX_DEBUG_ASSERT(CanBeSwappedOut());
  state_ = DecoderState::kSwappedOut;
}

void H264MultiDecoder::SwappedIn() { PumpDecoder(); }

void H264MultiDecoder::OnFatalError() {
  if (!fatal_error_) {
    fatal_error_ = true;
    client_->OnError();
  }
}

void H264MultiDecoder::ReceivedNewInput() { PumpOrReschedule(); }

void H264MultiDecoder::StartConfigChange() {
  ZX_DEBUG_ASSERT(pending_config_change_);
  // We shouldn't try to run this if decoding is currently ongoing, since the interrupt handlers are
  // using the current set of video_frames_.
  ZX_DEBUG_ASSERT(!currently_decoding_);
  ZX_DEBUG_ASSERT(frames_to_output_.empty());

  video_frames_.clear();
  zx::bti bti;
  zx_status_t status = owner_->bti()->duplicate(ZX_RIGHT_SAME_RIGHTS, &bti);
  if (status != ZX_OK) {
    DECODE_ERROR("bti duplicate failed, status: %d\n", status);
    return;
  }
  display_width_ = media_decoder_->GetVisibleRect().width();
  display_height_ = media_decoder_->GetVisibleRect().height();
  mb_width_ = media_decoder_->GetPicSize().width() / 16;
  mb_height_ = media_decoder_->GetPicSize().height() / 16;
  uint32_t min_frame_count = media_decoder_->GetRequiredNumOfPictures();
  uint32_t max_frame_count = 24;
  uint32_t coded_width = media_decoder_->GetPicSize().width();
  uint32_t coded_height = media_decoder_->GetPicSize().height();
  uint32_t stride = fbl::round_up(coded_width, 32u);
  // TODO(fxb/13483): Plumb SAR through somehow.
  bool has_sar = false;
  uint32_t sar_width = 1;
  uint32_t sar_height = 1;
  client_->InitializeFrames(std::move(bti), min_frame_count, max_frame_count, coded_width,
                            coded_height, stride, display_width_, display_height_, has_sar,
                            sar_width, sar_height);
  pending_config_change_ = false;
  waiting_for_surfaces_ = true;
  owner_->TryToReschedule();
}

void H264MultiDecoder::PumpDecoder() {
  // Don't try to reenter media_decoder_->Decode().
  if (in_pump_decoder_)
    return;

  while (true) {
    if (waiting_for_surfaces_ || currently_decoding_ || pending_config_change_ ||
        (state_ == DecoderState::kSwappedOut))
      return;
    ZX_DEBUG_ASSERT(!in_pump_decoder_);
    in_pump_decoder_ = true;
    auto res = media_decoder_->Decode();
    in_pump_decoder_ = false;
    DLOG("H264MultiDecoder::PumpDecoder Got result of %d\n", static_cast<int>(res));
    if (res == media::AcceleratedVideoDecoder::kConfigChange) {
      pending_config_change_ = true;
      if (!currently_decoding_) {
        StartConfigChange();
      }
    }
    if (res == media::AcceleratedVideoDecoder::kRanOutOfStreamData) {
      current_decoder_buffer_.reset();
      auto next_decoder_buffer = frame_data_provider_->ReadMoreInputData(this);
      if (next_decoder_buffer.data.empty()) {
        DLOG("Not decoding because decoder ran out of inputs, state %d", static_cast<int>(state_));
        owner_->TryToReschedule();
        return;
      }
      current_decoder_buffer_ =
          std::make_unique<media::DecoderBuffer>(std::move(next_decoder_buffer.data));
      if (next_decoder_buffer.pts) {
        id_to_pts_map_[next_pts_id_] = next_decoder_buffer.pts.value();
      }
      media_decoder_->SetStream(next_pts_id_++, *current_decoder_buffer_);

      uint32_t nal_unit_type = GetNalUnitType(fbl::Span<const uint8_t>(
          current_decoder_buffer_->data(), current_decoder_buffer_->data_size()));
      constexpr uint32_t kSpsNalUnitType = 7;
      constexpr uint32_t kPpsNalUnitType = 8;
      if (nal_unit_type == kSpsNalUnitType || nal_unit_type == kPpsNalUnitType)
        SubmitDataToHardware(current_decoder_buffer_->data(), current_decoder_buffer_->data_size());
    } else if (res == media::AcceleratedVideoDecoder::kRanOutOfSurfaces) {
      waiting_for_surfaces_ = true;
      owner_->TryToReschedule();
      return;
    } else if (res == media::AcceleratedVideoDecoder::kDecodeError) {
      OnFatalError();
      owner_->TryToReschedule();
      return;
    } else if (res == media::AcceleratedVideoDecoder::kTryAgain) {
      owner_->TryToReschedule();
      return;
    }
  }
}

std::shared_ptr<H264MultiDecoder::ReferenceFrame> H264MultiDecoder::GetUnusedReferenceFrame() {
  ZX_DEBUG_ASSERT(!pending_config_change_);
  for (auto& frame : video_frames_) {
    ZX_DEBUG_ASSERT(frame->frame->coded_width ==
                    static_cast<uint32_t>(media_decoder_->GetPicSize().width()));
    ZX_DEBUG_ASSERT(frame->frame->coded_height ==
                    static_cast<uint32_t>(media_decoder_->GetPicSize().height()));
    if (!frame->in_use && !frame->in_internal_use) {
      frame->in_use = true;
      frame->in_internal_use = true;
      return frame;
    }
  }
  return nullptr;
}
