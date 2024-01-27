// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::platform_metrics::PlatformMetric;
use crate::shutdown_request::ShutdownRequest;
use crate::types::{Celsius, PState, ThermalLoad, Watts};
use fuchsia_zircon::sys;

/// Defines the message types and arguments to be used for inter-node communication
#[derive(Debug, PartialEq)]
#[allow(dead_code)]
pub enum Message {
    /// Read the temperature
    ReadTemperature,

    /// Get the number of CPUs in the system
    GetNumCpus,

    /// Get the current load from each CPU in the system as a vector of values in the range [0.0 -
    /// 1.0]. Load is calculated by dividing the total time a CPU spent not idle during a duration
    /// by the total time elapsed during the same duration, where the duration is defined as the
    /// time since the previous GetCpuLoads call. The returned vector will have NUM_CPUS elements,
    /// where NUM_CPUS is the value returned by the GetNumCpus message.
    GetCpuLoads,

    /// Get all performance states for the handler's CPU domain
    GetCpuPerformanceStates,

    // Issues the zx_system_set_performance_info syscall.
    SetCpuPerformanceInfo(Vec<sys::zx_cpu_performance_info_t>),

    /// Instruct the node to limit the power consumption of its corresponding component (e.g., CPU)
    /// Arg: the max number of watts that the component should be allowed to consume
    SetMaxPowerConsumption(Watts),

    /// Command a system shutdown
    /// Arg: a ShutdownRequest indicating the requested shutdown state and reason
    SystemShutdown(ShutdownRequest),

    /// Communicate a new thermal load value for the given sensor
    /// Arg0: a ThermalLoad value which represents the severity of thermal load on the given sensor
    /// Arg1: the topological path which uniquely identifies a specific temperature sensor
    UpdateThermalLoad(ThermalLoad, String),

    /// Get the current performance state
    GetPerformanceState,

    /// Set the new performance state
    /// Arg: a value in the range [0 - x] where x is an upper bound defined in the
    /// dev_control_handler crate. An increasing value indicates a lower performance state.
    SetPerformanceState(u32),

    /// File a crash report
    /// Arg: the crash report signature
    FileCrashReport(String),

    /// Specify the termination system state, intended to be used in the DriverManagerHandler node.
    /// Arg: the SystemPowerState value indicating the termination state
    SetTerminationSystemState(fidl_fuchsia_device_manager::SystemPowerState),

    /// Notify that the mic enabled state has changed
    /// Arg: the new enabled state
    NotifyMicEnabledChanged(bool),

    /// Notify that the user active state has changed
    /// Arg: the new active state
    NotifyUserActiveChanged(bool),

    /// Log the given metric with the PlatformMetrics node
    /// Arg: the `PlatformMetric` to be logged
    LogPlatformMetric(PlatformMetric),

    /// Gets the topological path of the driver associated with the target node
    GetDriverPath,

    /// Send a debug command.
    /// Arg0: node-specific command as a string
    /// Arg1: args required to execute the command
    Debug(String, Vec<String>),
}

/// Defines the return values for each of the Message types from above
#[derive(Debug)]
#[allow(dead_code)]
pub enum MessageReturn {
    /// Arg: temperature in Celsius
    ReadTemperature(Celsius),

    /// Arg: the number of CPUs in the system
    GetNumCpus(u32),

    /// Arg: the current load from each CPU in the system as a vector of values in the range [0.0 -
    /// 1.0]. The returned vector will have NUM_CPUS elements, where NUM_CPUS is the value returned
    /// by the GetNumCpus message.
    GetCpuLoads(Vec<f32>),

    /// Arg: all performance states for the CPU domain seviced by the message handler.
    GetCpuPerformanceStates(Vec<PState>),

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    SetCpuPerformanceInfo,

    /// Arg: the max number of watts that the component will use. This number should typically be at
    /// or below the number that was specified in the Message, but there may be cases where it
    /// actually exceeds that number (e.g., a CPU that cannot operate below the requested power
    /// level).
    SetMaxPowerConsumption(Watts),

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    SystemShutdown,

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    UpdateThermalLoad,

    /// Arg: the performance state returned from the node
    GetPerformanceState(u32),

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    SetPerformanceState,

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    FileCrashReport,

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    SetTerminationSystemState,

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    NotifyMicEnabledChanged,

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    NotifyUserActiveChanged,

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    LogPlatformMetric,

    /// Arg: the topological path of the driver associated with the target node
    GetDriverPath(String),

    /// There is no arg in this MessageReturn type. It only serves as an ACK.
    Debug,
}

pub type MessageResult = Result<MessageReturn, crate::error::PowerManagerError>;
