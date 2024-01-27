// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(target_os = "fuchsia")]
mod zircon;

#[cfg(target_os = "fuchsia")]
pub use zircon::*;

#[cfg(not(target_os = "fuchsia"))]
mod emulated;

#[cfg(not(target_os = "fuchsia"))]
pub use emulated::*;

/// invoke_for_handle_types!{mmm} calls the macro `mmm!` with two arguments: one is the name of a
/// Zircon handle, the second is one of:
///   * Everywhere for handle types that are supported everywhere FIDL is
///   * FuchsiaOnly for handle types that are supported only on Fuchsia
///   * Stub for handle types that have not yet had a Fuchsia API implemented in the zircon crate
///
/// To make a handle available everywhere, a polyfill must be implemented in
/// crate::handle::emulated.
#[macro_export]
macro_rules! invoke_for_handle_types {
    ($x:ident) => {
        $x! {Process, "Process", PROCESS, 1, FuchsiaOnly}
        $x! {Thread, "Thread", THREAD, 2, FuchsiaOnly}
        $x! {Vmo, "Vmo", VMO, 3, FuchsiaOnly}
        $x! {Channel, "Channel", CHANNEL, 4, Everywhere}
        $x! {Event, "Event", EVENT, 5, Everywhere}
        $x! {Port, "Port", PORT, 6,  FuchsiaOnly}
        $x! {Interrupt, "Interrupt", INTERRUPT, 7, FuchsiaOnly}
        $x! {DebugLog, "Debug Log", DEBUGLOG, 9, FuchsiaOnly}
        $x! {Socket, "Socket", SOCKET, 10, Everywhere}
        $x! {Resource, "Resource", RESOURCE, 12, FuchsiaOnly}
        $x! {EventPair, "Event Pair", EVENTPAIR, 13, Everywhere}
        $x! {Job, "Job", JOB, 14, FuchsiaOnly}
        $x! {Vmar, "VMAR", VMAR, 15, FuchsiaOnly}
        $x! {Fifo, "FIFO", FIFO, 16, FuchsiaOnly}
        $x! {Guest, "Guest", GUEST, 17, FuchsiaOnly}
        $x! {Vcpu, "VCPU", VCPU, 18, FuchsiaOnly}
        $x! {Timer, "Timer", TIMER, 19, FuchsiaOnly}
        $x! {Iommu, "IOMMU", IOMMU, 20, Stub}
        $x! {Bti, "BTI", BTI, 21, Stub}
        $x! {Profile, "Profile", PROFILE, 22, FuchsiaOnly}
        $x! {Pmt, "PMT", PMT, 23, Stub}
        $x! {SuspendToken, "Suspend Token", SUSPEND_TOKEN, 24, Stub}
        $x! {Pager, "Pager", PAGER, 25, Stub}
        $x! {Exception, "Exception", EXCEPTION, 26, Stub}
        $x! {Clock, "Clock", CLOCK, 27, FuchsiaOnly}
        $x! {Stream, "Stream", STREAM, 11, FuchsiaOnly}
        $x! {MsiAllocation, "MSI", MSI, 28, Stub}
    };
}
