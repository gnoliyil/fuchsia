// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "amlogic-video.h"

#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <lib/media/codec_impl/codec_diagnostics.h>
#include <lib/trace/event.h>
#include <lib/zx/channel.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <memory.h>
#include <stdint.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/smc.h>
#include <zircon/threads.h>

#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <thread>

#include <hwreg/bitfields.h>
#include <hwreg/mmio.h>

#include "device_ctx.h"
#include "device_fidl.h"
#include "hevcdec.h"
#include "local_codec_factory.h"
#include "macros.h"
#include "mpeg12_decoder.h"
#include "pts_manager.h"
#include "registers.h"
#include "src/lib/fsl/handles/object_info.h"
#include "src/media/lib/internal_buffer/internal_buffer.h"
#include "src/media/lib/memory_barriers/memory_barriers.h"
#include "util.h"
#include "vdec1.h"
#include "video_firmware_session.h"

namespace amlogic_decoder {

// TODO(fxbug.dev/35200):
//
// AllocateIoBuffer() - only used by VP9 - switch to InternalBuffer when we do zero copy on input
// for VP9.
//
// (AllocateStreamBuffer() has been moved to InternalBuffer.)
// (VideoDecoder::Owner::ProtectableHardwareUnit::kParser pays attention to is_secure.)
//
// (Fine as io_buffer_t, at least for now (for both h264 and VP9):
//  search_pattern_ - HW only reads this
//  parser_input_ - not used when secure)

// TODO(fxbug.dev/41972): bti::release_quarantine() or zx_bti_release_quarantine() somewhere during
// startup, after HW is known idle, before we allocate anything from sysmem.

namespace {

// These match the regions exported when the bus device was added.
enum MmioRegion {
  kCbus,
  kDosbus,
  kHiubus,
  kAobus,
  kDmc,
};

enum Interrupt {
  kDemuxIrq,
  kParserIrq,
  kDosMbox0Irq,
  kDosMbox1Irq,
};

}  // namespace

AmlogicVideo::AmlogicVideo(Owner* owner) : owner_(owner) {
  ZX_DEBUG_ASSERT(owner_);
  ZX_DEBUG_ASSERT(metrics_ == &default_nop_metrics_);
  vdec1_core_ = std::make_unique<Vdec1>(this);
  hevc_core_ = std::make_unique<HevcDec>(this);
}

AmlogicVideo::~AmlogicVideo() {
  LOG(INFO, "Tearing down AmlogicVideo");
  if (vdec0_interrupt_handle_) {
    zx_interrupt_destroy(vdec0_interrupt_handle_.get());
    if (vdec0_interrupt_thread_.joinable())
      vdec0_interrupt_thread_.join();
  }
  if (vdec1_interrupt_handle_) {
    zx_interrupt_destroy(vdec1_interrupt_handle_.get());
    if (vdec1_interrupt_thread_.joinable())
      vdec1_interrupt_thread_.join();
  }
  swapped_out_instances_.clear();
  current_instance_ = nullptr;
  core_ = nullptr;
  hevc_core_ = nullptr;
  vdec1_core_ = nullptr;
}

// TODO: Remove once we can add single-instance decoders through
// AddNewDecoderInstance.
void AmlogicVideo::SetDefaultInstance(std::unique_ptr<VideoDecoder> decoder, bool hevc) {
  TRACE_DURATION("media", "AmlogicVideo::SetDefaultInstance", "decoder", decoder.get(), "hevc",
                 hevc);
  DecoderCore* core = hevc ? hevc_core_.get() : vdec1_core_.get();
  assert(!stream_buffer_);
  assert(!current_instance_);
  current_instance_ = std::make_unique<DecoderInstance>(std::move(decoder), core);
  video_decoder_ = current_instance_->decoder();
  stream_buffer_ = current_instance_->stream_buffer();
  core_ = core;
}

void AmlogicVideo::AddNewDecoderInstance(std::unique_ptr<DecoderInstance> instance) {
  TRACE_DURATION("media", "AmlogicVideo::AddNewDecoderInstance", "instance", instance.get());
  swapped_out_instances_.push_back(std::move(instance));
}

// video_decoder_lock_ held
zx_status_t AmlogicVideo::UngateClocks() {
  TRACE_DURATION("media", "AmlogicVideo::UngateClocks");
  ++clock_ungate_ref_;
  if (clock_ungate_ref_ == 1) {
    zx_status_t status = ToggleClock(ClockType::kClkDos, true);
    if (status != ZX_OK) {
      zxlogf(ERROR, "Failed to toggle clock: %s", zx_status_get_string(status));
      return status;
    }
    HhiGclkMpeg1::Get()
        .ReadFrom(&*hiubus_)
        .set_aiu(0xff)
        .set_demux(true)
        .set_audio_in(true)
        .WriteTo(&*hiubus_);
    HhiGclkMpeg2::Get().ReadFrom(&*hiubus_).set_vpu_interrupt(true).WriteTo(&*hiubus_);
    UngateParserClock();
  }
  return ZX_OK;
}

void AmlogicVideo::UngateParserClock() {
  TRACE_DURATION("media", "AmlogicVideo::UngateParserClock");
  is_parser_gated_ = false;
  HhiGclkMpeg1::Get().ReadFrom(&*hiubus_).set_u_parser_top(true).WriteTo(&*hiubus_);
}

// video_decoder_lock_ held
zx_status_t AmlogicVideo::GateClocks() {
  TRACE_DURATION("media", "AmlogicVideo::GateClocks");
  ZX_ASSERT(clock_ungate_ref_ > 0);
  --clock_ungate_ref_;
  if (clock_ungate_ref_ == 0) {
    // Keep VPU interrupt enabled, as it's used for vsync by the display.
    HhiGclkMpeg1::Get()
        .ReadFrom(&*hiubus_)
        .set_u_parser_top(false)
        .set_aiu(0)
        .set_demux(false)
        .set_audio_in(false)
        .WriteTo(&*hiubus_);
    zx_status_t status = ToggleClock(ClockType::kClkDos, false);
    if (status != ZX_OK) {
      zxlogf(ERROR, "Failed to toggle clock: %s", zx_status_get_string(status));
      return status;
    }
    GateParserClock();
  }
  return ZX_OK;
}

void AmlogicVideo::GateParserClock() {
  TRACE_DURATION("media", "AmlogicVideo::GateParserClock");
  is_parser_gated_ = true;
  HhiGclkMpeg1::Get().ReadFrom(&*hiubus_).set_u_parser_top(false).WriteTo(&*hiubus_);
}

void AmlogicVideo::ClearDecoderInstance() {
  std::lock_guard<std::mutex> lock(video_decoder_lock_);
  TRACE_DURATION("media", "AmlogicVideo::ClearDecoderInstance", "current_instance_",
                 current_instance_.get());
  assert(current_instance_);
  assert(swapped_out_instances_.size() == 0);
  LOG(DEBUG, "current_instance_.reset()...");
  current_instance_ = nullptr;
  core_ = nullptr;
  video_decoder_ = nullptr;
  stream_buffer_ = nullptr;
}

void AmlogicVideo::RemoveDecoder(VideoDecoder* decoder) {
  TRACE_DURATION("media", "AmlogicVideo::RemoveDecoder", "decoder", decoder);
  std::lock_guard<std::mutex> lock(video_decoder_lock_);
  RemoveDecoderLocked(decoder);
}

void AmlogicVideo::RemoveDecoderLocked(VideoDecoder* decoder) {
  return RemoveDecoderWithCallbackLocked(decoder, [](DecoderInstance* unused) {});
}

void AmlogicVideo::RemoveDecoderWithCallbackLocked(VideoDecoder* decoder,
                                                   fit::function<void(DecoderInstance*)> callback) {
  TRACE_DURATION("media", "AmlogicVideo::RemoveDecoderLocked", "decoder", decoder);
  DLOG("Removing decoder: %p", decoder);
  ZX_DEBUG_ASSERT(decoder);
  if (current_instance_ && current_instance_->decoder() == decoder) {
    video_decoder_->ForceStopDuringRemoveLocked();
    BarrierBeforeRelease();
    callback(current_instance_.get());
    current_instance_ = nullptr;
    video_decoder_ = nullptr;
    stream_buffer_ = nullptr;
    core_ = nullptr;
    TryToReschedule();
    return;
  }
  for (auto it = swapped_out_instances_.begin(); it != swapped_out_instances_.end(); ++it) {
    if ((*it)->decoder() != decoder)
      continue;
    callback((*it).get());
    swapped_out_instances_.erase(it);
    return;
  }
  ZX_PANIC("attempted removal of non-existent decoder");
}

zx_status_t AmlogicVideo::AllocateStreamBuffer(StreamBuffer* buffer, uint32_t size,
                                               std::optional<InternalBuffer> saved_stream_buffer,
                                               bool use_parser, bool is_secure) {
  TRACE_DURATION("media", "AmlogicVideo::AllocateStreamBuffer");
  // So far, is_secure can only be true if use_parser is also true.
  ZX_DEBUG_ASSERT(!is_secure || use_parser);
  // is_writable is always true because we either need to write into this buffer using the CPU, or
  // using the parser - either way we'll be writing.
  constexpr bool kStreamBufferIsWritable = true;
  if (saved_stream_buffer.has_value() &&
      (saved_stream_buffer->size() != size || saved_stream_buffer->is_secure() != is_secure ||
       saved_stream_buffer->is_mapping_needed() != !use_parser)) {
    saved_stream_buffer.reset();
  }
  std::optional<InternalBuffer> stream_buffer;
  if (saved_stream_buffer.has_value()) {
    ZX_DEBUG_ASSERT(saved_stream_buffer->size() == size);
    ZX_DEBUG_ASSERT(saved_stream_buffer->is_secure() == is_secure);
    ZX_DEBUG_ASSERT(saved_stream_buffer->is_writable() == kStreamBufferIsWritable);
    ZX_DEBUG_ASSERT(saved_stream_buffer->is_mapping_needed() == !use_parser);
    stream_buffer = std::move(saved_stream_buffer);
  } else {
    auto create_result = InternalBuffer::Create(
        "AMLStreamBuffer", &sysmem_sync_ptr_, zx::unowned_bti(bti_), size, is_secure,
        /*is_writable=*/kStreamBufferIsWritable, /*is_mapping_needed=*/!use_parser);
    if (!create_result.is_ok()) {
      DECODE_ERROR("Failed to make video fifo: %d", create_result.error());
      return create_result.error();
    }
    stream_buffer.emplace(create_result.take_value());
  }
  buffer->optional_buffer() = std::move(stream_buffer);

  // Sysmem guarantees that the newly-allocated buffer starts out zeroed and cache clean, to the
  // extent possible based on is_secure.

  return ZX_OK;
}

zx_status_t AmlogicVideo::ConnectToTrustedApp(const fuchsia_tee::wire::Uuid& application_uuid,
                                              fuchsia::tee::ApplicationSyncPtr* tee) {
  TRACE_DURATION("media", "AmlogicVideo::ConnectToTrustedApp");
  ZX_DEBUG_ASSERT(tee);

  auto tee_endpoints = fidl::CreateEndpoints<fuchsia_tee::Application>();
  if (!tee_endpoints.is_ok()) {
    LOG(ERROR, "fidl::CreateEndpoints failed - status: %d", tee_endpoints.status_value());
    return tee_endpoints.status_value();
  }

  auto result = tee_proto_client_->ConnectToApplication(
      application_uuid, fidl::ClientEnd<::fuchsia_tee_manager::Provider>(),
      std::move(tee_endpoints->server));
  if (!result.ok()) {
    LOG(ERROR, "amlogic-video: tee_client_.ConnectToApplication() failed - status: %d",
        result.status());
    return result.status();
  }
  tee->Bind(tee_endpoints->client.TakeChannel());
  return ZX_OK;
}

zx_status_t AmlogicVideo::EnsureSecmemSessionIsConnected() {
  TRACE_DURATION("media", "AmlogicVideo::EnsureSecmemSessionIsConnected");
  if (secmem_session_.has_value()) {
    return ZX_OK;
  }

  fuchsia::tee::ApplicationSyncPtr tee_connection;
  const fuchsia_tee::wire::Uuid kSecmemUuid = {
      0x2c1a33c0, 0x44cc, 0x11e5, {0xbc, 0x3b, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
  zx_status_t status = ConnectToTrustedApp(kSecmemUuid, &tee_connection);
  if (status != ZX_OK) {
    LOG(ERROR, "ConnectToTrustedApp() failed - status: %d", status);
    return status;
  }

  auto secmem_session_result = SecmemSession::TryOpen(std::move(tee_connection));
  if (!secmem_session_result.is_ok()) {
    // Logging handled in `SecmemSession::TryOpen`
    return ZX_ERR_INTERNAL;
  }

  secmem_session_.emplace(secmem_session_result.take_value());
  return ZX_OK;
}

void AmlogicVideo::InitializeStreamInput(bool use_parser) {
  TRACE_DURATION("media", "AmlogicVideo::InitializeStreamInput");
  uint32_t buffer_address = truncate_to_32(stream_buffer_->buffer().phys_base());
  auto buffer_size = truncate_to_32(stream_buffer_->buffer().size());
  core_->InitializeStreamInput(use_parser, buffer_address, buffer_size);
}

zx_status_t AmlogicVideo::InitializeStreamBuffer(bool use_parser, uint32_t size, bool is_secure) {
  TRACE_DURATION("media", "AmlogicVideo::InitializeStreamBuffer");
  zx_status_t status =
      AllocateStreamBuffer(stream_buffer_, size, std::nullopt, use_parser, is_secure);
  if (status != ZX_OK) {
    return status;
  }

  status = SetProtected(VideoDecoder::Owner::ProtectableHardwareUnit::kParser, is_secure);
  if (status != ZX_OK) {
    return status;
  }

  InitializeStreamInput(use_parser);
  return ZX_OK;
}

std::unique_ptr<CanvasEntry> AmlogicVideo::ConfigureCanvas(
    io_buffer_t* io_buffer, uint32_t offset, uint32_t width, uint32_t height,
    fuchsia_hardware_amlogiccanvas::CanvasFlags flags,
    fuchsia_hardware_amlogiccanvas::CanvasBlockMode blockmode) {
  TRACE_DURATION("media", "AmlogicVideo::ConfigureCanvas");
  assert(width % 8 == 0);
  assert(offset % 8 == 0);
  fuchsia_hardware_amlogiccanvas::wire::CanvasInfo info;
  info.height = height;
  info.stride_bytes = width;
  info.flags = flags;
  info.blkmode = blockmode;

  // 64-bit big-endian to little-endian conversion.
  info.endianness = fuchsia_hardware_amlogiccanvas::CanvasEndianness::kSwap8Bits |
                    fuchsia_hardware_amlogiccanvas::CanvasEndianness::kSwap16Bits |
                    fuchsia_hardware_amlogiccanvas::CanvasEndianness::kSwap32Bits;

  zx::unowned_vmo vmo(io_buffer->vmo_handle);
  zx::vmo dup_vmo;
  zx_status_t status = vmo->duplicate(ZX_RIGHT_SAME_RIGHTS, &dup_vmo);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to duplicate handle, status: %d", status);
    return nullptr;
  }

  fidl::WireResult result = canvas_->Config(std::move(dup_vmo), offset, info);
  if (!result.ok()) {
    DECODE_ERROR("Failed to call configure on canvas, status: %s", result.error().status_string());
    return nullptr;
  }

  if (result->is_error()) {
    DECODE_ERROR("Failed to configure canvas, status: %s",
                 zx_status_get_string(result->error_value()));
    return nullptr;
  }

  return std::make_unique<CanvasEntry>(this, result->value()->canvas_idx);
}

void AmlogicVideo::FreeCanvas(CanvasEntry* canvas) {
  TRACE_DURATION("media", "AmlogicVideo::FreeCanvas");
  ZX_DEBUG_ASSERT(canvas->index() <= std::numeric_limits<uint8_t>::max());
  fidl::WireResult result = canvas_->Free(static_cast<uint8_t>(canvas->index()));
  if (!result.ok()) {
    DECODE_ERROR("Failed to call free on canvas, status: %s", result.error().status_string());
    return;
  }

  if (result->is_error()) {
    DECODE_ERROR("Failed to free canvas, status: %s", zx_status_get_string(result->error_value()));
  }
}

void AmlogicVideo::SetThreadProfile(zx::unowned_thread thread, ThreadRole role) const {
  owner_->SetThreadProfile(std::move(thread), role);
}

void AmlogicVideo::OnSignaledWatchdog() {
  TRACE_DURATION("media", "AmlogicVideo::OnSignaledWatchdog");
  std::lock_guard<std::mutex> lock(video_decoder_lock_);
  // Check after taking lock to ensure a cancel didn't just happen.
  if (!watchdog_.CheckAndResetTimeout())
    return;
  // The watchdog should never be valid if the decoder was disconnected.
  ZX_ASSERT(video_decoder_);
  video_decoder_->OnSignaledWatchdog();
}

zx_status_t AmlogicVideo::AllocateIoBuffer(io_buffer_t* buffer, size_t size,
                                           uint32_t alignment_log2, uint32_t flags,
                                           const char* name) {
  TRACE_DURATION("media", "AmlogicVideo::AllocateIoBuffer");
  zx_status_t status = io_buffer_init_aligned(buffer, bti_.get(), size, alignment_log2, flags);
  if (status != ZX_OK)
    return status;

  SetIoBufferName(buffer, name);

  return ZX_OK;
}

fuchsia::sysmem::AllocatorSyncPtr& AmlogicVideo::SysmemAllocatorSyncPtr() {
  return sysmem_sync_ptr_;
}

// This parser handles MPEG elementary streams.
zx_status_t AmlogicVideo::InitializeEsParser() {
  TRACE_DURATION("media", "AmlogicVideo::InitializeEsParser");
  std::lock_guard<std::mutex> lock(video_decoder_lock_);
  return parser_->InitializeEsParser(current_instance_.get());
}

uint32_t AmlogicVideo::GetStreamBufferEmptySpaceAfterWriteOffsetBeforeReadOffset(
    uint32_t write_offset, uint32_t read_offset) {
  TRACE_DURATION("media",
                 "AmlogicVideo::GetStreamBufferEmptySpaceAfterWriteOffsetBeforeReadOffset");
  uint32_t available_space;
  if (read_offset > write_offset) {
    available_space = read_offset - write_offset;
  } else {
    available_space = truncate_to_32(stream_buffer_->buffer().size() - write_offset + read_offset);
  }
  // Subtract 8 to ensure the read pointer doesn't become equal to the write
  // pointer, as that means the buffer is empty.
  available_space = available_space > 8 ? available_space - 8 : 0;
  return available_space;
}

uint32_t AmlogicVideo::GetStreamBufferEmptySpaceAfterOffset(uint32_t write_offset) {
  TRACE_DURATION("media", "AmlogicVideo::GetStreamBufferEmptySpaceAfterOffset", "write_offset",
                 write_offset);
  uint32_t read_offset = core_->GetReadOffset();
  return GetStreamBufferEmptySpaceAfterWriteOffsetBeforeReadOffset(write_offset, read_offset);
}

uint32_t AmlogicVideo::GetStreamBufferEmptySpace() {
  TRACE_DURATION("media", "AmlogicVideo::GetStreamBufferEmptySpace");
  return GetStreamBufferEmptySpaceAfterOffset(core_->GetStreamInputOffset());
}

zx_status_t AmlogicVideo::ProcessVideoNoParser(const void* data, uint32_t len,
                                               uint32_t* written_out) {
  TRACE_DURATION("media", "AmlogicVideo::ProcessVideoNoParser");
  return ProcessVideoNoParserAtOffset(data, len, core_->GetStreamInputOffset(), written_out);
}

zx_status_t AmlogicVideo::ProcessVideoNoParserAtOffset(const void* data, uint32_t len,
                                                       uint32_t write_offset,
                                                       uint32_t* written_out) {
  TRACE_DURATION("media", "AmlogicVideo::ProcessVideoNoParserAtOffset");
  uint32_t available_space = GetStreamBufferEmptySpaceAfterOffset(write_offset);
  if (!written_out) {
    if (len > available_space) {
      DECODE_ERROR("Video too large");
      return ZX_ERR_OUT_OF_RANGE;
    }
  } else {
    len = std::min(len, available_space);
    *written_out = len;
  }

  stream_buffer_->set_data_size(stream_buffer_->data_size() + len);
  uint32_t input_offset = 0;
  while (len > 0) {
    uint32_t write_length = len;
    if (write_offset + len > stream_buffer_->buffer().size())
      write_length = truncate_to_32(stream_buffer_->buffer().size() - write_offset);
    memcpy(stream_buffer_->buffer().virt_base() + write_offset,
           static_cast<const uint8_t*>(data) + input_offset, write_length);
    stream_buffer_->buffer().CacheFlush(write_offset, write_length);
    write_offset += write_length;
    if (write_offset == stream_buffer_->buffer().size())
      write_offset = 0;
    len -= write_length;
    input_offset += write_length;
  }
  BarrierAfterFlush();
  core_->UpdateWritePointer(truncate_to_32(stream_buffer_->buffer().phys_base() + write_offset));
  return ZX_OK;
}

void AmlogicVideo::SwapOutCurrentInstance() {
  TRACE_DURATION("media", "AmlogicVideo::SwapOutCurrentInstance", "current_instance_",
                 current_instance_.get());
  ZX_DEBUG_ASSERT(!!current_instance_);

  // VP9:
  //
  // FrameWasOutput() is called during handling of kVp9CommandNalDecodeDone on the interrupt thread,
  // which means the decoder HW is currently paused, which means it's ok to save the state before
  // the stop+wait (without any explicit pause before the save here).  The decoder HW remains paused
  // after the save, and makes no further progress until later after the restore.
  //
  // h264_multi_decoder:
  //
  // ShouldSaveInputContext() is true if the h264_multi_decoder made useful progress (decoded a
  // picture).  If no useful progress was made, the lack of save here allows the state restore later
  // to effectively back up and try decoding from the same location again, with more data present.
  // This backing up to the previous saved state is the main way that separate SPS PPS and pictures
  // split across packets are handled.  In other words, it's how the h264_multi_decoder handles
  // stream-based input.
  bool should_save = current_instance_->decoder()->ShouldSaveInputContext();
  DLOG("Swapping out %p should_save: %d", current_instance_->decoder(), should_save);
  if (should_save) {
    if (!current_instance_->input_context()) {
      current_instance_->InitializeInputContext();
      if (core_->InitializeInputContext(current_instance_->input_context(),
                                        current_instance_->decoder()->is_secure()) != ZX_OK) {
        video_decoder_->CallErrorHandler();
        // Continue trying to swap out.
      }
    }
  }
  video_decoder_->SetSwappedOut();
  if (should_save) {
    if (current_instance_->input_context()) {
      if (core_->SaveInputContext(current_instance_->input_context()) != ZX_OK) {
        video_decoder_->CallErrorHandler();
        // Continue trying to swap out.
      }
    }
  }
  video_decoder_ = nullptr;
  stream_buffer_ = nullptr;
  core_->StopDecoding();
  core_->WaitForIdle();

  core_ = nullptr;
  // Round-robin; place at the back of the line.
  swapped_out_instances_.push_back(std::move(current_instance_));
}

void AmlogicVideo::TryToReschedule() {
  TRACE_DURATION("media", "AmlogicVideo::TryToReschedule");
  DLOG("AmlogicVideo::TryToReschedule");

  if (current_instance_ && !current_instance_->decoder()->CanBeSwappedOut()) {
    DLOG("Current instance can't be swapped out");
    return;
  }

  // This is used by h264_multi_decoder to swap out without saving, so that the next swap in will
  // restore a previously-saved state again to re-attempt decode from that saved state's logical
  // read start position.  Unlike the read position which backs up for re-decode, the write position
  // is adjusted after restore to avoid overwriting data written since that save state was
  // originally created.
  if (current_instance_ && current_instance_->decoder()->MustBeSwappedOut()) {
    DLOG("MustBeSwappedOut() is true");
    SwapOutCurrentInstance();
  }

  if (current_instance_ && current_instance_->decoder()->test_hooks().force_context_save_restore) {
    DLOG("force_context_save_restore");
    SwapOutCurrentInstance();
  }

  if (swapped_out_instances_.size() == 0) {
    DLOG("Nothing swapped out; returning");
    return;
  }

  // Round-robin; first in line that can be swapped in goes first.
  // TODO: Use some priority mechanism to determine which to swap in.
  auto other_instance = swapped_out_instances_.begin();
  for (; other_instance != swapped_out_instances_.end(); ++other_instance) {
    if ((*other_instance)->decoder()->CanBeSwappedIn()) {
      break;
    }
  }
  if (other_instance == swapped_out_instances_.end()) {
    DLOG("nothing to swap to");
    return;
  }

  ZX_ASSERT(!watchdog_.is_running());
  if (current_instance_) {
    SwapOutCurrentInstance();
  }
  current_instance_ = std::move(*other_instance);
  swapped_out_instances_.erase(other_instance);

  SwapInCurrentInstance();
}

void AmlogicVideo::PowerOffForError() {
  TRACE_DURATION("media", "AmlogicVideo::PowerOffForError");
  ZX_DEBUG_ASSERT(core_);
  core_ = nullptr;
  swapped_out_instances_.push_back(std::move(current_instance_));
  VideoDecoder* video_decoder = video_decoder_;
  video_decoder_ = nullptr;
  stream_buffer_ = nullptr;
  video_decoder->CallErrorHandler();
  // CallErrorHandler should have marked the decoder as having a fatal error
  // so it will never be rescheduled.
  TryToReschedule();
}

void AmlogicVideo::SwapInCurrentInstance() {
  TRACE_DURATION("media", "AmlogicVideo::SwapInCurrentInstance", "current_instance_",
                 current_instance_.get());
  ZX_DEBUG_ASSERT(current_instance_);

  core_ = current_instance_->core();
  video_decoder_ = current_instance_->decoder();
  DLOG("Swapping in %p", video_decoder_);
  stream_buffer_ = current_instance_->stream_buffer();
  {
    zx_status_t status = video_decoder_->SetupProtection();
    if (status != ZX_OK) {
      DECODE_ERROR("Failed to setup protection: %d", status);
      PowerOffForError();
      return;
    }
  }
  if (!current_instance_->input_context()) {
    InitializeStreamInput(false);
    core_->InitializeDirectInput();
    // If data has added to the stream buffer before the first swap in(only
    // relevant in tests right now) then ensure the write pointer's updated to
    // that spot.
    // Generally data will only be added after this decoder is swapped in, so
    // RestoreInputContext will handle that state.
    if (stream_buffer_->data_size() + stream_buffer_->padding_size() > 0) {
      core_->UpdateWritePointer(truncate_to_32(stream_buffer_->buffer().phys_base() +
                                               stream_buffer_->data_size() +
                                               stream_buffer_->padding_size()));
    }
  } else {
    if (core_->RestoreInputContext(current_instance_->input_context()) != ZX_OK) {
      PowerOffForError();
      return;
    }
  }

  // Do InitializeHardware after setting up the input context, since for H264Multi the vififo can
  // start reading as soon as PowerCtlVld is set up (inside InitializeHardware), and we don't want
  // it to read incorrect data as we gradually set it up later.
  zx_status_t status = video_decoder_->InitializeHardware();
  if (status != ZX_OK) {
    // Probably failed to load the right firmware.
    DECODE_ERROR("Failed to initialize hardware: %d", status);
    PowerOffForError();
    return;
  }
  video_decoder_->SwappedIn();
}

zx::result<fidl::ClientEnd<fuchsia_sysmem::Allocator>> AmlogicVideo::ConnectToSysmem() {
  auto endpoints = fidl::CreateEndpoints<fuchsia_sysmem::Allocator>();
  if (!endpoints.is_ok()) {
    DECODE_ERROR("Failed to create sysmem allocator endpoints: %s", endpoints.status_string());
    return endpoints.take_error();
  }

  auto status = sysmem_->ConnectServer(std::move(endpoints->server));
  if (!status.ok()) {
    DECODE_ERROR("Failed to connect server: %s", status.status_string());
    return zx::error(status.status());
  }
  return zx::ok(std::move(endpoints->client));
}

namespace tee_smc {

enum CallType : uint8_t {
  kYieldingCall = 0,
  kFastCall = 1,
};

enum CallConvention : uint8_t {
  kSmc32CallConv = 0,
  kSmc64CallConv = 1,
};

enum Service : uint8_t {
  kArchService = 0x00,
  kCpuService = 0x01,
  kSipService = 0x02,
  kOemService = 0x03,
  kStandardService = 0x04,
  kTrustedOsService = 0x32,
  kTrustedOsServiceEnd = 0x3F,
};

constexpr uint8_t kCallTypeMask = 0x01;
constexpr uint8_t kCallTypeShift = 31;
constexpr uint8_t kCallConvMask = 0x01;
constexpr uint8_t kCallConvShift = 30;
constexpr uint8_t kServiceMask = ARM_SMC_SERVICE_CALL_NUM_MASK;
constexpr uint8_t kServiceShift = ARM_SMC_SERVICE_CALL_NUM_SHIFT;

static constexpr uint32_t CreateFunctionId(CallType call_type, CallConvention call_conv,
                                           Service service, uint16_t function_num) {
  return (((call_type & kCallTypeMask) << kCallTypeShift) |
          ((call_conv & kCallConvMask) << kCallConvShift) |
          ((service & kServiceMask) << kServiceShift) | function_num);
}
}  // namespace tee_smc

zx_status_t AmlogicVideo::SetProtected(ProtectableHardwareUnit unit, bool protect) {
  TRACE_DURATION("media", "AmlogicVideo::SetProtected", "unit", static_cast<uint32_t>(unit),
                 "protect", protect);
  if (!secure_monitor_)
    return protect ? ZX_ERR_INVALID_ARGS : ZX_OK;

  // Call into the TEE to mark a particular hardware unit as able to access
  // protected memory or not.
  zx_smc_parameters_t params = {};
  zx_smc_result_t result = {};
  constexpr uint32_t kFuncIdConfigDeviceSecure = 14;
  params.func_id = tee_smc::CreateFunctionId(tee_smc::kFastCall, tee_smc::kSmc32CallConv,
                                             tee_smc::kTrustedOsService, kFuncIdConfigDeviceSecure);
  params.arg1 = static_cast<uint32_t>(unit);
  params.arg2 = static_cast<uint32_t>(protect);
  zx_status_t status = zx_smc_call(secure_monitor_.get(), &params, &result);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to set unit %ld protected status %ld code: %d", params.arg1, params.arg2,
                 status);
    return status;
  }
  if (result.arg0 != 0) {
    DECODE_ERROR("Failed to set unit %ld protected status %ld: %lx", params.arg1, params.arg2,
                 result.arg0);
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

zx_status_t AmlogicVideo::TeeSmcLoadVideoFirmware(FirmwareBlob::FirmwareType index,
                                                  FirmwareBlob::FirmwareVdecLoadMode vdec) {
  TRACE_DURATION("media", "AmlogicVideo::TeeSmcLoadVideoFirmware");
  ZX_DEBUG_ASSERT(is_tee_available());
  ZX_DEBUG_ASSERT(secure_monitor_);

  // Call into the TEE to tell the HW to use a particular piece of the previously pre-loaded overall
  // firmware blob.
  zx_smc_parameters_t params = {};
  zx_smc_result_t result = {};
  constexpr uint32_t kFuncIdLoadVideoFirmware = 15;
  params.func_id = tee_smc::CreateFunctionId(tee_smc::kFastCall, tee_smc::kSmc32CallConv,
                                             tee_smc::kTrustedOsService, kFuncIdLoadVideoFirmware);
  params.arg1 = static_cast<uint32_t>(index);
  params.arg2 = static_cast<uint32_t>(vdec);
  zx_status_t status = zx_smc_call(secure_monitor_.get(), &params, &result);
  if (status != ZX_OK) {
    LOG(ERROR, "Failed to kFuncIdLoadVideoFirmware - index: %u vdec: %u status: %d", index, vdec,
        status);
    return status;
  }
  if (result.arg0 != 0) {
    LOG(ERROR, "kFuncIdLoadVideoFirmware result.arg0 != 0 - value: %lu", result.arg0);
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

zx_status_t AmlogicVideo::TeeVp9AddHeaders(zx_paddr_t page_phys_base, uint32_t before_size,
                                           uint32_t max_after_size, uint32_t* after_size) {
  TRACE_DURATION("media", "AmlogicVideo::TeeVp9AddHeaders");
  ZX_DEBUG_ASSERT(after_size);
  ZX_DEBUG_ASSERT(is_tee_available());

  // TODO(fxbug.dev/44674): Remove this retry loop once this issue is resolved.
  constexpr uint32_t kRetryCount = 20;
  zx_status_t status = ZX_OK;
  for (uint32_t i = 0; i < kRetryCount; ++i) {
    status = EnsureSecmemSessionIsConnected();
    if (status != ZX_OK) {
      continue;
    }

    status =
        secmem_session_->GetVp9HeaderSize(page_phys_base, before_size, max_after_size, after_size);
    if (status != ZX_OK) {
      LOG(ERROR, "secmem_session_->GetVp9HeaderSize() failed - status: %d", status);

      // Explicitly disconnect and clean up `secmem_session_`.
      secmem_session_ = std::nullopt;
      continue;
    }

    ZX_DEBUG_ASSERT(*after_size <= max_after_size);
    return ZX_OK;
  }

  return status;
}

zx_status_t AmlogicVideo::ToggleClock(ClockType type, bool enable) {
  TRACE_DURATION("media", "AmlogicVideo::ToggleClock");
  int type_int = static_cast<int>(type);
  if (enable) {
    fidl::WireResult result = clocks_[type_int]->Enable();
    if (!result.ok()) {
      zxlogf(ERROR, "Failed to send request to enable clock %d: %s", type_int,
             result.status_string());
      return result.status();
    }
    if (result->is_error()) {
      zxlogf(ERROR, "Failed to enable clock %d: %s", type_int,
             zx_status_get_string(result->error_value()));
      return result->error_value();
    }
  } else {
    fidl::WireResult result = clocks_[type_int]->Disable();
    if (!result.ok()) {
      zxlogf(ERROR, "Failed to send request to disable clock %d: %s", type_int,
             result.status_string());
      return result.status();
    }
    if (result->is_error()) {
      zxlogf(ERROR, "Failed to disable clock %d: %s", type_int,
             zx_status_get_string(result->error_value()));
      return result->error_value();
    }
  }
  return ZX_OK;
}

void AmlogicVideo::SetMetrics(CodecMetrics* metrics) {
  ZX_DEBUG_ASSERT(metrics);
  ZX_DEBUG_ASSERT(metrics != &default_nop_metrics_);
  ZX_DEBUG_ASSERT(metrics_ == &default_nop_metrics_);
  metrics_ = metrics;
}

zx_status_t AmlogicVideo::InitRegisters(zx_device_t* parent) {
  TRACE_DURATION("media", "AmlogicVideo::InitRegisters");
  parent_ = parent;

  pdev_ = ddk::PDevFidl::FromFragment(parent_);
  if (!pdev_.is_valid()) {
    DECODE_ERROR("Failed to get pdev protocol");
    return ZX_ERR_NO_RESOURCES;
  }

  zx::result sysmem_client =
      ddk::Device<void>::DdkConnectFragmentFidlProtocol<fuchsia_hardware_sysmem::Service::Sysmem>(
          parent, "sysmem-fidl");
  if (sysmem_client.is_error()) {
    zxlogf(ERROR, "Failed to get sysmem protocol: %s", sysmem_client.status_string());
    return sysmem_client.status_value();
  }

  sysmem_.Bind(std::move(*sysmem_client));

  zx::result canvas_result = ddk::Device<void>::DdkConnectFragmentFidlProtocol<
      fuchsia_hardware_amlogiccanvas::Service::Device>(parent_, "canvas");
  if (canvas_result.is_error()) {
    zxlogf(ERROR, "Could not obtain aml canvas protocol %s\n", canvas_result.status_string());
    return ZX_ERR_NO_RESOURCES;
  }

  canvas_.Bind(std::move(canvas_result.value()));

  const char* CLOCK_DOS_VDEC_FRAG_NAME = "clock-dos-vdec";
  zx::result clock_client =
      ddk::Device<void>::DdkConnectFragmentFidlProtocol<fuchsia_hardware_clock::Service::Clock>(
          parent, CLOCK_DOS_VDEC_FRAG_NAME);
  if (clock_client.is_error()) {
    zxlogf(ERROR, "Failed to get clock protocol from fragment '%s': %s\n", CLOCK_DOS_VDEC_FRAG_NAME,
           clock_client.status_string());
    return clock_client.status_value();
  }
  clocks_[static_cast<int>(ClockType::kGclkVdec)].Bind(std::move(*clock_client));

  const char* CLOCK_DOS_FRAG_NAME = "clock-dos";
  clock_client =
      ddk::Device<void>::DdkConnectFragmentFidlProtocol<fuchsia_hardware_clock::Service::Clock>(
          parent, CLOCK_DOS_FRAG_NAME);
  if (clock_client.is_error()) {
    zxlogf(ERROR, "Failed to get clock protocol from fragment '%s': %s\n", CLOCK_DOS_FRAG_NAME,
           clock_client.status_string());
    return clock_client.status_value();
  }
  clocks_[static_cast<int>(ClockType::kClkDos)].Bind(std::move(*clock_client));

  // If tee is available as a fragment, we require that we can get ZX_PROTOCOL_TEE.  It'd be nice
  // if there were a less fragile way to detect this.  Passing in driver metadata for this doesn't
  // seem worthwhile so far.  There's no tee on vim2.

  auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_tee::DeviceConnector>();
  if (endpoints.is_error()) {
    LOG(ERROR, "fidl::CreateEndpoints failed - status: %d", endpoints.status_value());
    return endpoints.status_value();
  }
  zx_status_t status =
      device_connect_fragment_fidl_protocol2(parent, "tee", fuchsia_hardware_tee::Service::Name,
                                             fuchsia_hardware_tee::Service::DeviceConnector::Name,
                                             endpoints->server.TakeChannel().release());
  is_tee_available_ = (status == ZX_OK);

  if (is_tee_available_) {
    tee_proto_client_.Bind(std::move(endpoints->client));
    // TODO(fxbug.dev/39808): remove log spam once we're loading firmware via video_firmware TA
    LOG(INFO, "Got ZX_PROTOCOL_TEE");
  } else {
    // TODO(fxbug.dev/39808): remove log spam once we're loading firmware via video_firmware TA
    LOG(INFO, "Skipped ZX_PROTOCOL_TEE");
  }

  pdev_device_info_t info;
  status = pdev_.GetDeviceInfo(&info);
  if (status != ZX_OK) {
    DECODE_ERROR("pdev_.GetDeviceInfo failed");
    return status;
  }

  switch (info.pid) {
    case PDEV_PID_AMLOGIC_S912:
      device_type_ = DeviceType::kGXM;
      break;
    case PDEV_PID_AMLOGIC_S905D2:
      device_type_ = DeviceType::kG12A;
      break;
    case PDEV_PID_AMLOGIC_T931:
    case PDEV_PID_AMLOGIC_A311D:
      device_type_ = DeviceType::kG12B;
      break;
    case PDEV_PID_AMLOGIC_S905D3:
      device_type_ = DeviceType::kSM1;
      break;
    default:
      DECODE_ERROR("Unknown soc pid: %d", info.pid);
      return ZX_ERR_INVALID_ARGS;
  }

  static constexpr uint32_t kTrustedOsSmcIndex = 0;
  status = pdev_.GetSmc(kTrustedOsSmcIndex, &secure_monitor_);
  if (status != ZX_OK) {
    // On systems where there's no protected memory it's fine if we can't get
    // a handle to the secure monitor.
    LOG(INFO, "amlogic-video: Unable to get secure monitor handle, assuming no protected memory");
  }

  std::optional<fdf::MmioBuffer> cbus_mmio;
  status = pdev_.MapMmio(kCbus, &cbus_mmio);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map cbus");
    return ZX_ERR_NO_MEMORY;
  }

  std::optional<fdf::MmioBuffer> mmio;
  status = pdev_.MapMmio(kDosbus, &mmio);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map dosbus");
    return ZX_ERR_NO_MEMORY;
  }
  dosbus_.emplace(*std::move(mmio));
  status = pdev_.MapMmio(kHiubus, &mmio);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map hiubus");
    return ZX_ERR_NO_MEMORY;
  }
  hiubus_.emplace(*std::move(mmio));
  status = pdev_.MapMmio(kAobus, &mmio);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map aobus");
    return ZX_ERR_NO_MEMORY;
  }
  aobus_.emplace(*std::move(mmio));
  status = pdev_.MapMmio(kDmc, &mmio);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map dmc");
    return ZX_ERR_NO_MEMORY;
  }
  dmc_.emplace(*std::move(mmio));
  status = pdev_.GetInterrupt(kParserIrq, 0, &parser_interrupt_handle_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get parser interrupt");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_.GetInterrupt(kDosMbox0Irq, 0, &vdec0_interrupt_handle_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get vdec0 interrupt");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_.GetInterrupt(kDosMbox1Irq, 0, &vdec1_interrupt_handle_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get vdec interrupt");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_.GetBti(0, &bti_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get bti");
    return ZX_ERR_NO_MEMORY;
  }

  int64_t reset_register_offset = 0x1100 * 4;
  int64_t parser_register_offset = 0;
  int64_t demux_register_offset = 0;
  if (IsDeviceAtLeast(device_type_, DeviceType::kG12A)) {
    // Some portions of the cbus moved in newer versions (TXL and later).
    reset_register_offset = 0x0400 * 4;
    parser_register_offset = (0x3800 - 0x2900) * 4;
    demux_register_offset = (0x1800 - 0x1600) * 4;
  }
  reset_.emplace(*cbus_mmio, reset_register_offset);
  parser_regs_.emplace(*cbus_mmio, parser_register_offset);
  demux_.emplace(*cbus_mmio, demux_register_offset);
  cbus_.emplace(*std::move(cbus_mmio));
  registers_ = std::unique_ptr<MmioRegisters>(new MmioRegisters{
      &*dosbus_, &*aobus_, &*dmc_, &*hiubus_, &*reset_, &*parser_regs_, &*demux_});

  firmware_ = std::make_unique<FirmwareBlob>(device_type_);
  status = firmware_->LoadFirmware(parent_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed load firmware");
    return status;
  }

  zx::result allocator_client = ConnectToSysmem();
  if (allocator_client.is_error()) {
    DECODE_ERROR("Failed to connect to sysmem: %s", allocator_client.status_string());
    return allocator_client.status_value();
  }
  sysmem_sync_ptr_.Bind(allocator_client->TakeChannel());
  if (!sysmem_sync_ptr_) {
    DECODE_ERROR("ConnectToSysmem() failed");
    status = ZX_ERR_INTERNAL;
    return status;
  }
  sysmem_sync_ptr_->SetDebugClientInfo(fsl::GetCurrentProcessName(), fsl::GetCurrentProcessKoid());
  parser_ = std::make_unique<Parser>(this, std::move(parser_interrupt_handle_));

  if (is_tee_available()) {
    // TODO(fxbug.dev/44674): Remove this retry loop once this issue is resolved.
    constexpr uint32_t kRetryCount = 10;
    for (uint32_t i = 0; i < kRetryCount; i++) {
      status = EnsureSecmemSessionIsConnected();
      if (status == ZX_OK) {
        break;
      }
    }

    if (!secmem_session_.has_value()) {
      LOG(ERROR,
          "OpenSession to secmem failed too many times. Bootloader version may be incorrect.");
      return ZX_ERR_INTERNAL;
    }
  }

  return ZX_OK;
}

zx_status_t AmlogicVideo::PreloadFirmwareViaTee() {
  TRACE_DURATION("media", "AmlogicVideo::PreloadFirmwareViaTee");
  ZX_DEBUG_ASSERT(is_tee_available_);

  uint8_t* firmware_data;
  uint32_t firmware_size;
  firmware_->GetWholeBlob(&firmware_data, &firmware_size);

  // TODO(fxbug.dev/44764): Remove retry when video_firmware crash is fixed.
  zx_status_t status = ZX_OK;
  constexpr uint32_t kRetryCount = 10;
  for (uint32_t i = 0; i < kRetryCount; i++) {
    fuchsia::tee::ApplicationSyncPtr tee_connection;
    const fuchsia_tee::wire::Uuid kVideoFirmwareUuid = {
        0x526fc4fc, 0x7ee6, 0x4a12, {0x96, 0xe3, 0x83, 0xda, 0x95, 0x65, 0xbc, 0xe8}};
    status = ConnectToTrustedApp(kVideoFirmwareUuid, &tee_connection);
    if (status != ZX_OK) {
      LOG(ERROR, "ConnectToTrustedApp() failed - status: %d", status);
      continue;
    }

    auto video_firmware_session_result = VideoFirmwareSession::TryOpen(std::move(tee_connection));
    if (!video_firmware_session_result.is_ok()) {
      // Logging handled in `VideoFirmwareSession::TryOpen`
      status = ZX_ERR_INTERNAL;
      continue;
    }

    VideoFirmwareSession video_firmware_session = video_firmware_session_result.take_value();
    status = video_firmware_session.LoadVideoFirmware(firmware_data, firmware_size);
    if (status != ZX_OK) {
      LOG(ERROR, "video_firmware_session.LoadVideoFirmware() failed - status: %d", status);
      continue;
    }

    LOG(INFO, "Firmware loaded via video_firmware TA");
    break;
  }

  return status;
}

void AmlogicVideo::InitializeInterrupts() {
  TRACE_DURATION("media", "AmlogicVideo::InitializeInterrupts");
  vdec0_interrupt_thread_ = std::thread([this]() {
    while (true) {
      zx_time_t time;
      zx_status_t status = zx_interrupt_wait(vdec0_interrupt_handle_.get(), &time);
      if (status != ZX_OK) {
        if (status != ZX_ERR_CANCELED) {
          DECODE_ERROR("vdec0_interrupt_thread_ zx_interrupt_wait() failed - status: %d", status);
        }
        return;
      }
      std::lock_guard<std::mutex> lock(video_decoder_lock_);
      if (video_decoder_) {
        video_decoder_->HandleInterrupt();
      }
    }
  });

  SetThreadProfile(
      zx::unowned_thread(native_thread_get_zx_handle(vdec0_interrupt_thread_.native_handle())),
      ThreadRole::kVdec0Irq);

  vdec1_interrupt_thread_ = std::thread([this]() {
    while (true) {
      zx_time_t time;
      zx_status_t status = zx_interrupt_wait(vdec1_interrupt_handle_.get(), &time);
      if (status == ZX_ERR_CANCELED) {
        // expected when zx_interrupt_destroy() is called
        return;
      }
      if (status != ZX_OK) {
        // unexpected errors
        DECODE_ERROR(
            "AmlogicVideo::InitializeInterrupts() zx_interrupt_wait() failed "
            "status: %d\n",
            status);
        return;
      }
      std::lock_guard<std::mutex> lock(video_decoder_lock_);
      if (video_decoder_) {
        video_decoder_->HandleInterrupt();
      }
    }
  });

  SetThreadProfile(
      zx::unowned_thread(native_thread_get_zx_handle(vdec1_interrupt_thread_.native_handle())),
      ThreadRole::kVdec1Irq);
}

zx_status_t AmlogicVideo::InitDecoder() {
  TRACE_DURATION("media", "AmlogicVideo::InitDecoder");
  if (is_tee_available_) {
    zx_status_t status = PreloadFirmwareViaTee();
    if (status != ZX_OK) {
      is_tee_available_ = false;
      // TODO(jbauman): Fail this function when everyone's updated their bootloaders.
      LOG(INFO, "Preloading firmware failed with status %d. protected decode won't work.", status);
    } else {
      // TODO(dustingreen): Remove log spam after secure decode works.
      LOG(INFO, "PreloadFirmwareViaTee() succeeded.");
    }
  } else {
    LOG(INFO, "!is_tee_available_");
  }

  InitializeInterrupts();

  return ZX_OK;
}

}  // namespace amlogic_decoder
