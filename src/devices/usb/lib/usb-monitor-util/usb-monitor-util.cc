// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/usb/request/c/banjo.h>
#include <lib/trace/event.h>

#include <fbl/auto_lock.h>
#include <usb-monitor-util/usb-monitor-util.h>
#include <usb/usb.h>

void USBMonitor::Start() {
  fbl::AutoLock start_lock(&mutex_);
  if (!started_) {
    TRACE_INSTANT("USB Monitor Util", "START", TRACE_SCOPE_PROCESS);
    started_ = true;
  }
}

void USBMonitor::Stop() {
  fbl::AutoLock<fbl::Mutex> start_lock(&mutex_);
  if (started_) {
    TRACE_INSTANT("USB Monitor Util", "STOP", TRACE_SCOPE_PROCESS);
    started_ = false;
  }
}

bool USBMonitor::Started() const {
  fbl::AutoLock start_lock(&mutex_);
  return started_;
}

void USBMonitor::AddRecord(usb_request_t* request) {
  fbl::AutoLock<fbl::Mutex> start_lock(&mutex_);

  if (started_) {
    ++num_records_;
    TRACE_INSTANT("USB Monitor Util", "Record Added", TRACE_SCOPE_GLOBAL, "ep_num",
                  TA_UINT32(usb_ep_num2(request->header.ep_address)), "device_id",
                  request->header.device_id, "length", request->header.length, "frame",
                  TA_UINT64(request->header.frame), "direct", TA_BOOL(request->direct));
  }
}

USBMonitorStats USBMonitor::GetStats() const { return USBMonitorStats{num_records_.load()}; }
