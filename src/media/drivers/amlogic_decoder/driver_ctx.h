// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DRIVER_CTX_H_
#define SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DRIVER_CTX_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/ddk/driver.h>
#include <lib/fit/function.h>
#include <lib/media/codec_impl/codec_diagnostics.h>
#include <lib/media/codec_impl/codec_metrics.h>

#include <memory>
#include <string_view>

namespace amlogic_decoder {

// The driver is split up into separate concerns, with one instance of this
// class unifying the lifetime of those concerns (including the creation and
// destruction order).
//
// This is per-driver context, not per-device context.
//
// Anything that's driver-wide can go in here, or in instances rooted here.
class DriverCtx {
 public:
  static zx_status_t Init(void** out_ctx);
  static zx_status_t Bind(void* ctx, zx_device_t* parent);

  DriverCtx();
  ~DriverCtx();

  // This is a dedicated devhost for the device, so clean exit is only important
  // to the extent that we need to be able to re-init the device, and possibly
  // to ensure the device is left in lower-power mode.  Since a new replacement
  // devhost is coming up very shortly, and it can re-init from any device state
  // (is the idea), it seems quite a lot better to let the new devhost worry
  // about the device vs. a potentially-broken devhost on it's way out.  Also,
  // the system as a whole must tolerate a crash of a dedicated devhost anyway.
  // So (at least for now) this method is free to just exit the devhost after
  // attempting to give some debug breadcrumbs.
  //
  // TODO(dustingreen): Do format, printf, log, maybe some epitaphs.
  void FatalError(const char* format, ...);

  // The returned async::Loop* lasts for the whole lifetime of the driver (up to
  // driver ops release()).  All the things using the loop are per-device, and
  // all devices are gone by the time the loop is deleted.
  //
  // Only non-blocking work can use this loop, as blocking could cause
  // interference among clients.  Blocking work should use some other thread.
  async::Loop* shared_fidl_loop() { return shared_fidl_loop_.get(); }

  // Can be compared to thrd_current() to determine if the caller is running on
  // the shared FIDL thread.
  thrd_t shared_fidl_thread() { return shared_fidl_thread_; }

  // Run to_run on given dispatcher, in order.
  void PostSerial(async_dispatcher_t* dispatcher, fit::closure to_run);

  // Run to_run_on_shared_fidl_thread on shared_fidl_thread().
  void PostToSharedFidl(fit::closure to_run_on_shared_fidl_thread);

  void SetAuxServiceDirectory(fidl::InterfaceHandle<fuchsia::io::Directory> aux_service_directory);

  CodecMetrics& metrics();

  CodecDiagnostics& diagnostics();

  void Suspend() {
    metrics_ = std::nullopt;
    shared_fidl_loop_->Quit();
    shared_fidl_loop_->JoinThreads();
    shared_fidl_loop_->Shutdown();
    shared_fidl_loop_.reset();
  }

 private:
  static constexpr std::string_view kDriverName = "amlogic-video-decoder";

  std::unique_ptr<async::Loop> shared_fidl_loop_;
  thrd_t shared_fidl_thread_;

  // Services provided by codec_factory:
  std::shared_ptr<sys::ServiceDirectory> aux_service_directory_;

  // Cobalt:
  std::optional<CodecMetrics> metrics_;

  // Inspector diagnostics
  CodecDiagnostics diagnostics_{kDriverName};
};

}  // namespace amlogic_decoder

#endif  // SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_DRIVER_CTX_H_
