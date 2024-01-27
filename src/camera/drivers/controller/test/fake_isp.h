// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_CONTROLLER_TEST_FAKE_ISP_H_
#define SRC_CAMERA_DRIVERS_CONTROLLER_TEST_FAKE_ISP_H_

#include <fuchsia/hardware/isp/cpp/banjo.h>
#include <lib/sys/cpp/component_context.h>

class FakeIsp {
 public:
  FakeIsp() {
    isp_protocol_ops_.create_output_stream = IspCreateOutputStream;
    isp_protocol_ops_.set_frame_rate_range = IspSetFrameRateRange;
    isp_protocol_.ctx = this;
    isp_protocol_.ops = &isp_protocol_ops_;
  }

  ddk::IspProtocolClient client() { return ddk::IspProtocolClient(&isp_protocol_); }
  isp_protocol_t proto() { return isp_protocol_; }

  void PopulateStreamProtocol(output_stream_protocol_t* out_s) {
    out_s->ctx = this;
    static constexpr output_stream_protocol_ops_t ops = {
      .start = Start,
      .stop = Stop,
      .release_frame = ReleaseFrame,
      .shutdown = Shutdown,
    };
    out_s->ops = &ops;
  }

  zx_status_t Start() {
    start_stream_counter_++;
    return ZX_OK;
  }
  zx_status_t Stop() {
    stop_stream_counter_++;
    return ZX_OK;
  }
  zx_status_t ReleaseFrame(uint32_t /*buffer_index*/) {
    frame_released_ = true;
    return ZX_OK;
  }
  zx_status_t Shutdown(const isp_stream_shutdown_callback_t* shutdown_callback) {
    shutdown_callback->shutdown_complete(shutdown_callback->ctx, ZX_OK);
    return ZX_OK;
  }

  // |ZX_PROTOCOL_ISP|
  zx_status_t IspCreateOutputStream(const buffer_collection_info_2_t* /*buffer_collection*/,
                                    const image_format_2_t* /*image_format*/,
                                    const frame_rate_t* /*rate*/, stream_type_t /*type*/,
                                    const hw_accel_frame_callback_t* frame_callback,
                                    output_stream_protocol_t* out_s) {
    frame_callback_ = frame_callback;
    out_s->ctx = this;
    static constexpr output_stream_protocol_ops_t ops = {
      .start = Start,
      .stop = Stop,
      .release_frame = ReleaseFrame,
      .shutdown = Shutdown,
    };
    out_s->ops = &ops;
    return ZX_OK;
  }

  zx_status_t IspSetFrameRateRange(const frame_rate_t* /*min_rate*/,
                                   const frame_rate_t* /*max_rate*/) {
    return ZX_OK;
  }

  bool frame_released() const { return frame_released_; }
  uint32_t start_stream_counter() const { return start_stream_counter_; }
  uint32_t stop_stream_counter() const { return stop_stream_counter_; }

 private:
  static zx_status_t IspCreateOutputStream(void* ctx,
                                           const buffer_collection_info_2_t* buffer_collection,
                                           const image_format_2_t* image_format,
                                           const frame_rate_t* rate, stream_type_t type,
                                           const hw_accel_frame_callback_t* frame_callback,
                                           output_stream_protocol_t* out_st) {
    return static_cast<FakeIsp*>(ctx)->IspCreateOutputStream(buffer_collection, image_format, rate,
                                                             type, frame_callback, out_st);
  }

  static zx_status_t IspSetFrameRateRange(void* ctx, const frame_rate_t* min_rate,
                                          const frame_rate_t* max_rate) {
    return static_cast<FakeIsp*>(ctx)->IspSetFrameRateRange(min_rate, max_rate);
  }

  static zx_status_t Start(void* ctx) { return static_cast<FakeIsp*>(ctx)->Start(); }
  static zx_status_t Stop(void* ctx) { return static_cast<FakeIsp*>(ctx)->Stop(); }
  static zx_status_t ReleaseFrame(void* ctx, uint32_t index) {
    return static_cast<FakeIsp*>(ctx)->ReleaseFrame(index);
  }
  static zx_status_t Shutdown(void* ctx, const isp_stream_shutdown_callback_t* shutdown_callback) {
    return static_cast<FakeIsp*>(ctx)->Shutdown(shutdown_callback);
  }

  const hw_accel_frame_callback_t* frame_callback_;
  isp_protocol_t isp_protocol_ = {};
  isp_protocol_ops_t isp_protocol_ops_ = {};
  bool frame_released_ = false;
  uint32_t start_stream_counter_ = 0;
  uint32_t stop_stream_counter_ = 0;
};

#endif  // SRC_CAMERA_DRIVERS_CONTROLLER_TEST_FAKE_ISP_H_
