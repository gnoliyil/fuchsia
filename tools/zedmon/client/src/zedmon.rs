// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::protocol::{self, ParameterValue, ReportFormat, Value, MAX_PACKET_SIZE},
    anyhow::{bail, format_err, Error},
    serde::{Deserialize, Serialize},
    serde_json as json,
    std::{
        cell::RefCell,
        collections::HashMap,
        io::{Read, Write},
        os::raw::{c_uchar, c_ushort},
        sync::mpsc,
        time::{Duration, SystemTime},
    },
    thiserror::Error,
    usb_bulk::{InterfaceInfo, Open},
};

const GOOGLE_VENDOR_ID: c_ushort = 0x18d1;
const ZEDMON_PRODUCT_ID: c_ushort = 0xaf00;
const VENDOR_SPECIFIC_CLASS_ID: c_uchar = 0xff;
const ZEDMON_SUBCLASS_ID: c_uchar = 0xff;
const ZEDMON_PROTOCOL_ID: c_uchar = 0x00;

/// Determines if `ifc` is a Zedmon device. If so, the Zedmon's serial is returned. Otherwise, None.
fn zedmon_match(ifc: &InterfaceInfo) -> Option<String> {
    if (ifc.dev_vendor == GOOGLE_VENDOR_ID)
        && (ifc.dev_product == ZEDMON_PRODUCT_ID)
        && (ifc.ifc_class == VENDOR_SPECIFIC_CLASS_ID)
        && (ifc.ifc_subclass == ZEDMON_SUBCLASS_ID)
        && (ifc.ifc_protocol == ZEDMON_PROTOCOL_ID)
    {
        let null_pos = match ifc.serial_number.iter().position(|&c| c == 0) {
            Some(p) => p,
            None => {
                eprintln!(
                    "Warning: Detected a USB device whose serial number was not null-terminated:"
                );
                eprintln!("{}", (*String::from_utf8_lossy(&ifc.serial_number)).to_string());
                return None;
            }
        };
        let serial = (*String::from_utf8_lossy(&ifc.serial_number[..null_pos])).to_string();
        Some(serial)
    } else {
        None
    }
}

/// Used by Client to determine when data reporting should stop.
pub trait StopSignal {
    fn should_stop(&mut self, timestamp_micros: u64) -> Result<bool, Error>;
}

/// Raises a stop signal after Zedmon has reported for a given duration. The duration is measured
/// from the timestamp provided by the first call to `should_stop`.
///
/// Measuring this way is important to ensure consistency between the durations specified for
/// recording and the downsampling interval. Most importantly, if the recording duration and
/// downsampling interval are identical, exactly one record should be emitted.
pub struct DurationStopper {
    duration_micros: u64,
    start_micros: Option<u64>,
}

impl DurationStopper {
    pub fn new(duration: Duration) -> DurationStopper {
        DurationStopper { duration_micros: duration.as_micros() as u64, start_micros: None }
    }
}

impl StopSignal for DurationStopper {
    fn should_stop(&mut self, timestamp_micros: u64) -> Result<bool, Error> {
        if self.start_micros.is_none() {
            self.start_micros.replace(timestamp_micros);
        }

        Ok(timestamp_micros - self.start_micros.unwrap() >= self.duration_micros)
    }
}

/// Properties that can be queried using Client::describe.
pub const DESCRIBABLE_PROPERTIES: [&'static str; 3] =
    ["shunt_resistance", "csv_header", "time_offset"];

/// A single record of output from Client.
#[derive(Debug, Default, Deserialize, Serialize)]
struct Record {
    timestamp_micros: u64,
    shunt_voltage: f32,
    bus_voltage: f32,
    power: f32,
}

#[derive(Debug)]
struct DownsamplerState {
    last_output_micros: u64,
    prev_record: Record,
    bus_voltage_integral: f32,
    shunt_voltage_integral: f32,
    power_integral: f32,
}

/// Helper struct for downsampling Zedmon data.
struct Downsampler {
    interval_micros: u64,
    state: Option<DownsamplerState>,
}

impl Downsampler {
    fn new(interval_micros: u64) -> Self {
        Self { interval_micros, state: None }
    }

    /// Process a new record. If `record` completes a resampling interval, returns a Record
    /// containing values averaged over the interval. Otherwise, returns None.
    ///
    /// The average value of y(t) over the interval [t_low, t_high] is computed using the definition
    ///     avg(y(t), [t_low, t_high]) := 1 / (t_high - t_low) * \int_{t_low}^{t_high} y(s) ds.
    /// As each record is processed, the integral is udpated over the previous time interval using
    /// trapezoid rule integration, i.e.
    ///     \int_{t_1}^{t_2} y(s) ds \approx (t2 - t1) * (y(t1) + y(t2)) / 2.
    fn process(&mut self, record: Record) -> Option<Record> {
        // Initialize self.state if it is unset; otherwise grab a reference to it.
        let state = match self.state.as_mut() {
            None => {
                self.state = Some(DownsamplerState {
                    last_output_micros: record.timestamp_micros,
                    prev_record: record,
                    bus_voltage_integral: 0.0,
                    shunt_voltage_integral: 0.0,
                    power_integral: 0.0,
                });
                return None;
            }
            Some(state) => state,
        };

        // If the latest raw data interval spans multiple output samples, skip output and
        // reinitialize.
        if record.timestamp_micros >= state.last_output_micros + 2 * self.interval_micros {
            eprintln!(
                "Raw data interval [{}, {}] contains multiple downsampling output times. Skipping \
                output and reinitializing downsampling.",
                state.last_output_micros, record.timestamp_micros
            );
            self.state = None;
            return self.process(record);
        }

        let output_micros = state.last_output_micros + self.interval_micros;
        let dt_full = (record.timestamp_micros - state.prev_record.timestamp_micros) as f32;

        // No output this cycle -- update the state and return None.
        if record.timestamp_micros < output_micros {
            let dt_half = dt_full / 2.0;
            state.shunt_voltage_integral +=
                dt_half * (state.prev_record.shunt_voltage + record.shunt_voltage);
            state.bus_voltage_integral +=
                dt_half * (state.prev_record.bus_voltage + record.bus_voltage);
            state.power_integral += dt_half * (state.prev_record.power + record.power);

            state.prev_record = record;
            return None;
        }

        // Output occurs this cycle. Subdivide the interval between the previous and current records
        // into two intervals of length dt1 and dt2, as shown here:
        //
        // prev_record.timestamp_micros         output_micros          record.timestamp_micros
        //              |                             |                           |
        //              |                             |                           |
        //              +-----------------------------+---------------------------+
        //                             ^                            ^
        //                             |                            |
        //                            dt1                          dt2
        let dt1 = (output_micros - state.prev_record.timestamp_micros) as f32;
        let dt2 = (record.timestamp_micros - output_micros) as f32;

        // Use linear interpolation to estimate the instantaneous value of each measured
        // quantity at output_micros.
        let interpolate = |y1, y2| y1 + dt1 / dt_full * (y2 - y1);
        let shunt_voltage_interp =
            interpolate(state.prev_record.shunt_voltage, record.shunt_voltage);
        let bus_voltage_interp = interpolate(state.prev_record.bus_voltage, record.bus_voltage);
        let power_interp = interpolate(state.prev_record.power, record.power);

        // For each measured quantity, use the previous value and the interpolated value to
        // update its integral over [previous_record.timestamp_micros, output_micros].
        {
            let dt1_half = dt1 / 2.0;
            state.shunt_voltage_integral +=
                dt1_half * (state.prev_record.shunt_voltage + shunt_voltage_interp);
            state.bus_voltage_integral +=
                dt1_half * (state.prev_record.bus_voltage + bus_voltage_interp);
            state.power_integral += dt1_half * (state.prev_record.power + power_interp);
        }

        // Divide each integral by the total length of the integration interval to get an average
        // value of the measured quanity. This populates the output record.
        let record_out = Record {
            timestamp_micros: output_micros,
            shunt_voltage: state.shunt_voltage_integral / (self.interval_micros as f32),
            bus_voltage: state.bus_voltage_integral / (self.interval_micros as f32),
            power: state.power_integral / (self.interval_micros as f32),
        };

        // Now integrate over [output_micros, record.timestamp_micros] to seed the integrals for the
        // next output interval. If record.timestamp_micros == output_micros, then dt2 == 0.0, and
        // the integrals will be appropriately set to 0.0.
        {
            let dt2_half = dt2 / 2.0;
            state.shunt_voltage_integral = dt2_half * (shunt_voltage_interp + record.shunt_voltage);
            state.bus_voltage_integral = dt2_half * (bus_voltage_interp + record.bus_voltage);
            state.power_integral = dt2_half * (power_interp + record.power);
        }

        // Update remaining state and return.
        state.last_output_micros = output_micros;
        state.prev_record = record;
        Some(record_out)
    }
}

/// Options to customize the behavior of Client::read_reports.
pub struct ReportingOptions {
    /// Time interval on which to resample data.
    pub interval: Option<Duration>,

    /// Whether to report timestamps using host time (based on an estimate of host/target offset)
    /// rather than Zedmon time.
    pub use_host_timestamps: bool,

    /// Whether to output just the power calculation (and not the entire `Record`).
    pub output_power_only: bool,
}

/// Interface to a Zedmon device.
#[derive(Debug)]
pub struct Client<InterfaceType>
where
    InterfaceType: usb_bulk::Open<InterfaceType> + Read + Write,
{
    /// USB interface to the Zedmon device, or equivalent.
    interface: RefCell<InterfaceType>,

    /// Format of each field in a Report.
    field_formats: Vec<ReportFormat>,

    /// Information necessary to obtain power data from direct Zedmon measurements.
    shunt_resistance: f32,
    v_shunt_index: usize,
    v_bus_index: usize,
}

impl<InterfaceType: usb_bulk::Open<InterfaceType> + Read + Write> Client<InterfaceType> {
    /// Enumerates all connected Zedmons. Returns a `Vec<String>` of their serial numbers.
    fn enumerate() -> Vec<String> {
        let mut serials = Vec::new();

        // Instead of matching any devices, this callback extracts Zedmon serial numbers as
        // InterfaceType::open iterates through them. InterfaceType::open is expected to return an
        // error because no devices match.
        let mut cb = |info: &InterfaceInfo| -> bool {
            if let Some(serial) = zedmon_match(info) {
                serials.push(serial);
            }

            false
        };

        assert!(
            InterfaceType::open(&mut cb).is_err(),
            "open() should return an error, as the supplied callback cannot match any devices."
        );
        serials
    }

    // Number of USB packets that can be queued in Zedmon's USB interface, based on STM32F072
    // hardware limitations. The firmware currently only enqueues one packet, but the STM32F072 does
    // support double-buffering.
    const ZEDMON_USB_QUEUE_SIZE: u32 = 2;

    /// Disables reporting and drains all enqueued packets from `interface`.
    ///
    /// This should be done if a packet of incorrect type is received on the first read from
    /// `interface`. Typically, that scenario occurs if a previous invocation of the client
    /// terminated irregularly and left reporting enabled, but in principle a packet could still be
    /// enqueued from another request as well.
    ///
    /// Empirically, this process takes 4-5x as long as the timeout configured on `interface`, and
    /// it should not be performed unconditionally to avoid unnecessary delays.
    ///
    /// An error is returned if a packet is still received once the packet queue should be clear.
    fn disable_reporting_and_drain_packets(interface: &mut InterfaceType) -> Result<(), Error> {
        Self::disable_reporting(interface)?;

        // Read packets from the USB interface until an error (assumed to be due to lack of packets
        // -- the reason for an error is not exposed) is encountered. If we do not encounter an
        // error after reading more than the queue length, Zedmon is in an unexpected state.
        let mut response = [0; MAX_PACKET_SIZE];
        for _ in 0..Self::ZEDMON_USB_QUEUE_SIZE + 1 {
            if interface.read(&mut response).is_err() {
                return Ok(());
            }
        }
        return Err(format_err!(
            "The Zedmon device is in an unexpected state; received more than {} packets after \
            disabling reporting. Consider rebooting the device.",
            Self::ZEDMON_USB_QUEUE_SIZE
        ));
    }

    /// Creates a new Client instance.
    // TODO(fxbug.dev/61148): Make the behavior predictable if multiple Zedmons are attached.
    fn new(mut interface: InterfaceType) -> Result<Self, Error> {
        // Query parameters, disabling reporting and draining packets if a packet of the wrong type
        // is received on the first attempt.
        let parameters = match Self::get_parameters(&mut interface) {
            Err(e) => match e.downcast_ref::<protocol::Error>() {
                Some(protocol::Error::WrongPacketType { .. }) => {
                    eprintln!(
                        "WARNING: Received unexpected packet type while initializing; a prior \
                        client invocation may have not terminated cleanly. Disabling reporting and \
                        draining buffered packets. Be sure to terminate recording with ENTER."
                    );
                    Self::disable_reporting_and_drain_packets(&mut interface)?;
                    Self::get_parameters(&mut interface)
                }
                _ => Err(e),
            },
            ok => ok,
        }?;

        let field_formats = Self::get_field_formats(&mut interface)?;

        let shunt_resistance = {
            let value = parameters["shunt_resistance"];
            if let Value::F32(v) = value {
                v
            } else {
                bail!("Wrong value type for shunt_resistance: {:?}", value);
            }
        };

        // Use a HashMap to assist in field lookup for simplicity. Note that the Vec<ReportFormat>
        // representation needs to be retained for later Report-parsing.
        let formats_by_name: HashMap<String, ReportFormat> =
            field_formats.iter().map(|f| (f.name.clone(), f.clone())).collect();
        let v_shunt_index = formats_by_name["v_shunt"].index as usize;
        let v_bus_index = formats_by_name["v_bus"].index as usize;

        Ok(Self {
            interface: RefCell::new(interface),
            field_formats,
            shunt_resistance,
            v_shunt_index,
            v_bus_index,
        })
    }

    /// Describes properties of the Zedmon device and/or zedmon Client.
    pub fn describe(&self, name: &str) -> Result<json::Value, Error> {
        match name {
            "shunt_resistance" => Ok(json::json!(self.shunt_resistance)),
            "csv_header" => {
                let mut writer = csv::Writer::from_writer(Vec::new());
                writer.serialize(Record::default())?;
                let lines = String::from_utf8(writer.into_inner()?)?;
                let header = lines.split('\n').nth(0).unwrap();
                Ok(json::json!(header))
            }
            "time_offset" => {
                let (offset, uncertainty) = self.get_time_offset_micros()?;
                Ok(json::json!({"offset_micros": offset, "uncertainty_micros": uncertainty}))
            }
            _ => panic!(
                "'{}' is not a valid property name. Valid names are: {:?}",
                name, DESCRIBABLE_PROPERTIES
            ),
        }
    }

    #[allow(clippy::unused_io_amount)] // TODO(fxbug.dev/95036)
    /// Retrieves a ParameterValue from the provided Zedmon interface.
    fn get_parameter(interface: &mut InterfaceType, index: u8) -> Result<ParameterValue, Error> {
        let request = protocol::encode_query_parameter(index);
        interface.write(&request)?;

        let mut response = [0; MAX_PACKET_SIZE];
        let len = interface.read(&mut response)?;

        Ok(protocol::parse_parameter_value(&mut &response[0..len])?)
    }

    /// Retrieves every ParameterValue from the provided Zedmon interface.
    fn get_parameters(interface: &mut InterfaceType) -> Result<HashMap<String, Value>, Error> {
        let mut parameters = HashMap::new();
        loop {
            let parameter = Self::get_parameter(interface, parameters.len() as u8)?;
            if parameter.name.is_empty() {
                return Ok(parameters);
            }
            parameters.insert(parameter.name, parameter.value);
        }
    }

    #[allow(clippy::unused_io_amount)] // TODO(fxbug.dev/95036)
    /// Retrieves a ReportFormat from the provided Zedmon interface.
    fn get_report_format(interface: &mut InterfaceType, index: u8) -> Result<ReportFormat, Error> {
        let request = protocol::encode_query_report_format(index);
        interface.write(&request)?;

        let mut response = [0; MAX_PACKET_SIZE];
        let len = interface.read(&mut response)?;

        Ok(protocol::parse_report_format(&response[..len])?)
    }

    /// Retrieves the ReportFormat for each Report field from the provided Zedmon interface.
    fn get_field_formats(interface: &mut InterfaceType) -> Result<Vec<ReportFormat>, Error> {
        let mut all_fields = vec![];
        loop {
            let format = Self::get_report_format(interface, all_fields.len() as u8)?;
            if format.index == protocol::REPORT_FORMAT_INDEX_END {
                return Ok(all_fields);
            }
            all_fields.push(format);
        }
    }

    #[allow(clippy::unused_io_amount)] // TODO(fxbug.dev/95036)
    /// Disables reporting on the Zedmon device.
    fn disable_reporting(interface: &mut InterfaceType) -> Result<(), Error> {
        let request = protocol::encode_disable_reporting();
        interface.write(&request)?;
        Ok(())
    }

    #[allow(clippy::unused_io_amount)] // TODO(fxbug.dev/95036)
    /// Enables reporting on the Zedmon device.
    fn enable_reporting(&self) -> Result<(), Error> {
        let request = protocol::encode_enable_reporting();
        self.interface.borrow_mut().write(&request)?;
        Ok(())
    }

    /// Starts a thread to process Report packets.
    fn start_report_processing_thread(
        packet_receiver: mpsc::Receiver<Vec<u8>>,
        parser: protocol::ReportParser,
        writer: Box<dyn Write + Send>,
        mut stopper: impl StopSignal + Send + 'static,
        shunt_resistance: f32,
        v_shunt_index: usize,
        v_bus_index: usize,
        reporting_interval_micros: Option<u64>,
        time_offset_micros: i64,
        output_power_only: bool,
    ) -> std::thread::JoinHandle<Result<(), Error>> {
        std::thread::spawn(move || {
            // The CSV header is suppressed. Clients may query it by using `describe`.
            let mut writer = csv::WriterBuilder::new().has_headers(false).from_writer(writer);
            let mut downsampler = reporting_interval_micros.map(|r| Downsampler::new(r));

            // Determine allowable bounds for the Zedmon timestamp. These values are chosen so that
            // adding `time_offset_micros` to a raw timestamp will not cause it to wrap around u64
            // bounds.
            let min_zedmon_timestamp =
                if time_offset_micros >= 0 { 0u64 } else { -time_offset_micros as u64 };
            let max_zedmon_timestamp = if time_offset_micros >= 0 {
                std::u64::MAX - (time_offset_micros as u64)
            } else {
                std::u64::MAX
            };

            for buffer in packet_receiver.iter() {
                let reports = parser.parse_reports(&buffer)?;
                for report in reports.into_iter() {
                    let (shunt_voltage, bus_voltage) =
                        match (report.values[v_shunt_index], report.values[v_bus_index]) {
                            (Value::F32(x), Value::F32(y)) => (x, y),
                            t => {
                                return Err(format_err!(
                                    "Got wrong value types for (v_shunt, v_bus): {:?}",
                                    t
                                ))
                            }
                        };

                    // Check raw timestamp against bounds, and apply the time offset.
                    if report.timestamp_micros < min_zedmon_timestamp
                        || report.timestamp_micros > max_zedmon_timestamp
                    {
                        return Err(format_err!(
                            "Zedmon timestamp ({}) is outside of allowable bounds [{}, {}]",
                            report.timestamp_micros,
                            min_zedmon_timestamp,
                            max_zedmon_timestamp
                        ));
                    }
                    let timestamp_micros = if time_offset_micros < 0 {
                        report.timestamp_micros - (-time_offset_micros as u64)
                    } else {
                        report.timestamp_micros + (time_offset_micros as u64)
                    };

                    let record = Record {
                        timestamp_micros,
                        shunt_voltage,
                        bus_voltage,
                        power: bus_voltage * shunt_voltage / shunt_resistance,
                    };

                    // Explicit flushing is performed when downsampling because typically the
                    // reporting rate is relatively low, and the user may want to see the output in
                    // realtime. Meanwhile, if not downsampling, there's sufficient output that
                    // automatic flushing is fairly frequent. This behavior could be tuned if the
                    // need arises.
                    match downsampler.as_mut() {
                        Some(downsampler) => match downsampler.process(record) {
                            Some(r) => {
                                if output_power_only {
                                    writer.serialize(r.power)?;
                                } else {
                                    writer.serialize(r)?;
                                }
                                writer.flush()?;
                            }
                            None => {}
                        },
                        None => {
                            if output_power_only {
                                writer.serialize(record.power)?;
                            } else {
                                writer.serialize(record)?;
                            }
                        }
                    }

                    if stopper.should_stop(report.timestamp_micros)? {
                        writer.flush()?;

                        // The main thread will detect this thread's completion via disconnection
                        // of packet_receiver.
                        return Ok(());
                    }
                }
            }
            Err(format_err!("Packet sender should not close before packet receiver"))
        })
    }

    // Number of retries to attempt in case of a USB read failure.
    const NUM_USB_READ_RETRIES: usize = 3;

    #[cfg(test)]
    pub fn num_usb_read_retries() -> usize {
        Self::NUM_USB_READ_RETRIES
    }

    /// Runs the I/O part of reporting.
    fn run_report_io(&self, packet_sender: mpsc::Sender<Vec<u8>>) -> Result<(), Error> {
        // Enable reporting and run the main loop.
        self.enable_reporting()?;

        let mut num_retries = Self::NUM_USB_READ_RETRIES;

        loop {
            let mut buffer = vec![0; MAX_PACKET_SIZE];

            match self.interface.borrow_mut().read(&mut buffer) {
                Err(e) => {
                    eprintln!("USB read error: {}.", e);
                    if num_retries > 0 {
                        num_retries -= 1;
                        continue;
                    }
                    eprintln!(""); // Finish "USB read error" line
                    break Err(format_err!(
                        "Giving up after {} USB read failures.",
                        Self::NUM_USB_READ_RETRIES + 1
                    ));
                }
                Ok(bytes_read) => {
                    num_retries = Self::NUM_USB_READ_RETRIES;
                    buffer.truncate(bytes_read);
                }
            }

            if let Err(_) = packet_sender.send(buffer) {
                Self::disable_reporting(&mut self.interface.borrow_mut())?;
                break Ok(());
            }
        }
    }

    /// Reads reported data from the Zedmon device, taking care of enabling/disabling reporting.
    ///
    /// Measurement data is written to `writer`. Reporting will cease when `stopper` raises its stop
    /// signal.
    pub fn read_reports(
        &self,
        writer: Box<dyn Write + Send>,
        stopper: impl StopSignal + Send + 'static,
        options: ReportingOptions,
    ) -> Result<(), Error> {
        // This function's workload is shared between its main thread and processing_thread.
        //
        // The main thread enables reporting, reads USB packets via blocking reads, and sends those
        // packets to processing_thread via packet_sender. Meanwhile, processing_thread parses each
        // packet it receives to a Vec<Report>, which it then formats and outputs via `writer`.
        //
        // When `stopper` indicates that reporting should stop, processing_thread exits, and the
        // main thread learns of the termination via the closure of packet_receiver.
        //
        // The multithreading has not been confirmed as necessary for performance reasons, but it
        // seems like a reasonable thing to do, as both reading from USB and outputting (typically
        // to stdout or a file) involve blocking on I/O.
        let (packet_sender, packet_receiver) = mpsc::channel::<Vec<u8>>();

        let time_offset_micros =
            if options.use_host_timestamps { self.get_time_offset_micros()?.0 } else { 0 };

        let processing_thread = Self::start_report_processing_thread(
            packet_receiver,
            protocol::ReportParser::new(&self.field_formats)?,
            writer,
            stopper,
            self.shunt_resistance,
            self.v_shunt_index,
            self.v_bus_index,
            options.interval.map(|d| d.as_micros() as u64),
            time_offset_micros,
            options.output_power_only,
        );

        let report_io_result = self.run_report_io(packet_sender);

        // Join with the processing thread, and attempt to interpret the result of any panic that
        // may have occurred.
        let processing_result = match processing_thread.join() {
            Ok(result) => result,
            Err(e) => match e.downcast::<&str>() {
                Ok(s) => panic!("Processing thread panicked with error '{}'", s),
                Err(e) => match e.downcast::<String>() {
                    Ok(s) => panic!("Processing thread panicked with error '{}'", s),
                    Err(_) => panic!("Processing thread panicked; unable to interpret error."),
                },
            },
        };

        report_io_result.and(processing_result)
    }

    #[allow(clippy::unused_io_amount)] // TODO(fxbug.dev/95036)
    /// Returns a tuple consisting of:
    ///   - An estimate of the offset between the Zedmon clock and the host clock, in microseconds.
    ///     The offset is defined such that, in the absence of drift,
    ///         `zedmon_clock + offset = host_time`.
    ///     It is typically, but not necessarily, positive. (Note the signedness prevents
    ///     std::time::Duration from being a valid return type.)
    ///   - An estimate of the uncertainty in the offset, in microseconds. In the absence of clock
    ///     drift, the offset is accurate within ±uncertainty.
    pub fn get_time_offset_micros(&self) -> Result<(i64, i64), Error> {
        // Helper to convert nanos to micros, rounding up at `threshold` nanos.
        fn nanos_to_micros(nanos: i64, threshold: i64) -> i64 {
            nanos / 1_000 + if nanos % 1_000 >= threshold { 1 } else { 0 }
        }

        let mut interface = self.interface.borrow_mut();

        // For each query, we estimate that the retrieved timestamp reflects Zedmon's clock halfway
        // between the duration spanned by our QueryTime request and Zedmon's Timestamp response.
        // That allows us to estimate the offset between the host clock and Zedmon's.
        //
        // We run `TIME_OFFSET_NUM_QUERIES` queries and keep the estimate corresponding to the
        // shortest query duration. That provides some guarding against transient sources of
        // latency.
        //
        // Note: Unlike other methods that interact with the USB interface, this method does not
        // separate out a "get_timestamp" function to keep the parsing time from contributing to
        // transit time.
        let mut best_offset_micros = 0;
        let mut best_query_duration_nanos = i64::MAX;

        // Number of timestamp query round trips used to determine time offset.
        const TIME_OFFSET_NUM_QUERIES: u32 = 10;

        for _ in 0..TIME_OFFSET_NUM_QUERIES {
            let request = protocol::encode_query_time();
            let mut response = [0u8; MAX_PACKET_SIZE];

            let host_clock_at_start = SystemTime::now();
            interface.write(&request)?;
            let len = interface.read(&mut response)?;

            // `elapsed` could return an error if the host clock moved backwards. That should be
            // rare, but if it does occur, throw out this query. (Note, however, that the offset
            // would be invalidated by future jumps in the system clock.)
            let query_duration_nanos = match host_clock_at_start.elapsed() {
                Ok(duration) => duration.as_nanos() as i64,
                Err(_) => continue,
            };

            if query_duration_nanos < best_query_duration_nanos {
                let timestamp_micros = protocol::parse_timestamp_micros(&response[..len])? as i64;
                let host_nanos_at_timestamp =
                    host_clock_at_start.duration_since(SystemTime::UNIX_EPOCH)?.as_nanos() as i64
                        + query_duration_nanos / 2;

                // Round up at 500ns to minimize average rounding error.
                best_offset_micros =
                    nanos_to_micros(host_nanos_at_timestamp, 500) - timestamp_micros;

                best_query_duration_nanos = query_duration_nanos;
            }
        }

        // Uncertainty comes from two sources:
        //  - Mapping a microsecond Zedmon timestamp to nanoseconds. In the worst case, the
        //    timestamp is 999ns stale.
        //  - Estimating the instant of the timestamp in the query interval. Since we used the
        //    midpoint, at worst we're off by one half the query duration.
        // Since this is an uncertainty, any fractional part must be rounded up.
        let uncertainty_micros =
            nanos_to_micros(999 + best_query_duration_nanos / 2 + best_query_duration_nanos % 2, 1);

        Ok((best_offset_micros, uncertainty_micros))
    }

    #[allow(clippy::unused_io_amount)] // TODO(fxbug.dev/95036)
    /// Enables or disables the relay on the Zedmon device.
    pub fn set_relay(&self, enable: bool) -> Result<(), Error> {
        let request = protocol::encode_set_output(protocol::Output::Relay as u8, enable);
        self.interface.borrow_mut().write(&request)?;
        Ok(())
    }
}

/// Lists the serial numbers of all connected Zedmons.
pub fn list() -> Vec<String> {
    Client::<usb_bulk::Interface>::enumerate()
}

#[derive(Debug, Error)]
pub enum InitError {
    #[error("No Zedmon devices are connected")]
    NoDevices,

    #[error("Multiple Zedmon devices are connected without disambiguation")]
    MultipleDevices,

    #[error("A Zedmon device with the specified serial is not connected")]
    MissingDevice,
}

/// Attempts to open a `Client`.
///
/// If `serial` is specified and an attached Zedmon device is not found with that serial then an
/// error is returned.
///
/// If `serial` is not specified and one Zedmon device is attached then it will be selected by
/// default.
///
/// If `serial` is not specified and multiple Zedmon devices are attached then an error is returned.
pub fn zedmon(serial: Option<&str>) -> Result<Client<usb_bulk::Interface>, InitError> {
    const MAX_ATTEMPTS: u32 = 3;

    // Figure out the serial of the Zedmon device we intend to connect with
    let target_serial = if let Some(s) = serial {
        s.to_string()
    } else {
        let mut attached_serials = Client::<usb_bulk::Interface>::enumerate();
        match attached_serials.len() {
            0 => Err(InitError::NoDevices),
            1 => Ok(attached_serials.remove(0)),
            _ => Err(InitError::MultipleDevices),
        }?
    };

    for attempt in 1..=MAX_ATTEMPTS {
        let interface =
            usb_bulk::Interface::open(&mut |ifc: &InterfaceInfo| match zedmon_match(ifc) {
                Some(serial) => serial == target_serial,
                None => false,
            })
            .map_err(|_| InitError::MissingDevice)?;

        match Client::new(interface) {
            Ok(z) => return Ok(z),
            Err(e) => {
                eprintln!("Error initializing zedmon client: {:?}", e);
                if attempt < MAX_ATTEMPTS {
                    eprintln!("Will retry in 1 second.");
                    std::thread::sleep(Duration::from_secs(1));
                }
            }
        }
    }

    panic!("Failed to create Zedmon client after {} attempts. Aborting.", MAX_ATTEMPTS);
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        anyhow::Error,
        num_traits::FromPrimitive,
        protocol::{tests::serialize_reports, PacketType, Report, ScalarType},
        std::collections::VecDeque,
        std::rc::Rc,
        test_util::assert_near,
    };

    // Used by `interface_info`, below, as a convenient means of constructing InterfaceInfo.
    struct ShortInterface<'a> {
        dev_vendor: ::std::os::raw::c_ushort,
        dev_product: ::std::os::raw::c_ushort,
        ifc_class: ::std::os::raw::c_uchar,
        ifc_subclass: ::std::os::raw::c_uchar,
        ifc_protocol: ::std::os::raw::c_uchar,
        serial_number: &'a str,
    }

    fn interface_info(short: ShortInterface<'_>) -> InterfaceInfo {
        let mut serial = [0; 256];
        for (i, c) in short.serial_number.as_bytes().iter().enumerate() {
            serial[i] = *c;
        }

        InterfaceInfo {
            dev_vendor: short.dev_vendor,
            dev_product: short.dev_product,
            dev_class: 0,
            dev_subclass: 0,
            dev_protocol: 0,
            ifc_class: short.ifc_class,
            ifc_subclass: short.ifc_subclass,
            ifc_protocol: short.ifc_protocol,
            has_bulk_in: 0,
            has_bulk_out: 0,
            writable: 0,
            serial_number: serial,
            device_path: [0; 256usize],
        }
    }

    #[test]
    fn test_enumerate() {
        // AVAILABLE_DEVICES is state for the static method FakeEnumerationInterface::open. This
        // test is single-threaded, so a thread-local static provides the most appropriate safe
        // interface.
        thread_local! {
            static AVAILABLE_DEVICES: RefCell<Vec<InterfaceInfo>> = RefCell::new(Vec::new());
        }
        fn push_device(short: ShortInterface<'_>) {
            AVAILABLE_DEVICES.with(|devices| {
                devices.borrow_mut().push(interface_info(short));
            });
        }

        struct FakeEnumerationInterface {}

        impl usb_bulk::Open<FakeEnumerationInterface> for FakeEnumerationInterface {
            fn open<F>(matcher: &mut F) -> Result<FakeEnumerationInterface, usb_bulk::Error>
            where
                F: FnMut(&InterfaceInfo) -> bool,
            {
                AVAILABLE_DEVICES.with(|devices| {
                    let devices = devices.borrow();
                    for device in devices.iter() {
                        if matcher(device) {
                            return Ok(FakeEnumerationInterface {});
                        }
                    }
                    Err(usb_bulk::Error::NoDeviceMatched)
                })
            }
        }

        impl Read for FakeEnumerationInterface {
            fn read(&mut self, _: &mut [u8]) -> std::io::Result<usize> {
                Ok(0)
            }
        }

        impl Write for FakeEnumerationInterface {
            fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
                Ok(0)
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }

        // No devices connected
        let serials = Client::<FakeEnumerationInterface>::enumerate();
        assert!(serials.is_empty());

        // One device: not-a-zedmon-1
        push_device(ShortInterface {
            dev_vendor: 0xdead,
            dev_product: ZEDMON_PRODUCT_ID,
            ifc_class: VENDOR_SPECIFIC_CLASS_ID,
            ifc_subclass: ZEDMON_SUBCLASS_ID,
            ifc_protocol: ZEDMON_PROTOCOL_ID,
            serial_number: "not-a-zedmon-1",
        });
        let serials = Client::<FakeEnumerationInterface>::enumerate();
        assert!(serials.is_empty());

        // Two devices: not-a-zedmon-1, zedmon-1
        push_device(ShortInterface {
            dev_vendor: GOOGLE_VENDOR_ID,
            dev_product: ZEDMON_PRODUCT_ID,
            ifc_class: VENDOR_SPECIFIC_CLASS_ID,
            ifc_subclass: ZEDMON_SUBCLASS_ID,
            ifc_protocol: ZEDMON_PROTOCOL_ID,
            serial_number: "zedmon-1",
        });
        let serials = Client::<FakeEnumerationInterface>::enumerate();
        assert_eq!(serials, ["zedmon-1"]);

        // Three devices: not-a-zedmon-1, zedmon-1, not-a-zedmon-2
        push_device(ShortInterface {
            dev_vendor: GOOGLE_VENDOR_ID,
            dev_product: 0xbeef,
            ifc_class: VENDOR_SPECIFIC_CLASS_ID,
            ifc_subclass: ZEDMON_SUBCLASS_ID,
            ifc_protocol: ZEDMON_PROTOCOL_ID,
            serial_number: "not-a-zedmon-2",
        });
        let serials = Client::<FakeEnumerationInterface>::enumerate();
        assert_eq!(serials, ["zedmon-1"]);

        // Four devices: not-a-zedmon-1, zedmon-1, not-a-zedmon-2, zedmon-2
        push_device(ShortInterface {
            dev_vendor: GOOGLE_VENDOR_ID,
            dev_product: ZEDMON_PRODUCT_ID,
            ifc_class: VENDOR_SPECIFIC_CLASS_ID,
            ifc_subclass: ZEDMON_SUBCLASS_ID,
            ifc_protocol: ZEDMON_PROTOCOL_ID,
            serial_number: "zedmon-2",
        });
        let serials = Client::<FakeEnumerationInterface>::enumerate();
        assert_eq!(serials, ["zedmon-1", "zedmon-2"]);
    }

    // Provides test support for Client functionality that interacts with a Zedmon device. It
    // chiefly provides:
    //  - FakeZedmonInterface, for faking Client's access to a Zedmon device.
    //  - Coordinator, for coordinating activity between the test and a FakeZedmonInterface.
    //  - CoordinatorBuilder, for producing a Coordinator instance with its various optional
    //    settings.
    mod fake_device {
        use {
            super::*,
            num_traits::FromPrimitive,
            protocol::{tests::*, PacketType, Unit},
        };

        // Coordinates interactions between FakeZedmonInterface and a test.
        //
        // To test Client::read_reports:
        //  - Use CoordinatorBuilder::with_report_queue to populate the `report_queue` field. Each
        //    entry in the queue is a Vec<Report> that will be serialized into a single packet. The
        //    caller will need to ensure that reports are written to an accessible location.
        //  - Create Client with a DurationStopper whose duration is spanned by the enqueued
        //    Reports.
        //
        // To test Client::get_time_offset_nanos, use CoordinatorBuilder::with_offset_time to
        // populate `offset_time`. Timestamps will be reported as `host_time - offset_time`; the
        // fake Zedmon's clock will run perfectly in parallel to the host clock. Note that only
        // timestamps reported in Timestamp packets are affected by `offset_time`; timestamps in
        // Report packets are directly specified by the test, via `report_queue`.
        //
        // To test Client::set_relay, use CoordinatorBuilder::with_relay_enabled to
        // populate the relay state, and Coordinator::relay_enabled to check expectations.
        pub struct Coordinator {
            // Constants that define the fake device.
            device_config: DeviceConfiguration,

            // See struct comment.
            report_queue: Option<VecDeque<Vec<Report>>>,

            // Offset between the fake Zedmon's clock and host clock, such that `zedmon_time +
            // offset_time = host_time`. Only used for Timestamp packets; the timestamps in
            // Reports are set by the test when populating `report_queue`.
            offset_time: Option<Duration>,

            // Whether Zedmon's relay is enabled.
            relay_enabled: Option<bool>,
        }

        impl Coordinator {
            pub fn relay_enabled(&self) -> bool {
                self.relay_enabled.expect("relay_enabled not set")
            }

            fn get_device_config(&self) -> DeviceConfiguration {
                self.device_config.clone()
            }

            // Gets the next packet's worth of Reports, if available.
            fn get_reports_for_packet(&mut self) -> Option<Vec<Report>> {
                let report_queue = self.report_queue.as_mut().expect("report_queue not set");
                report_queue.pop_front()
            }

            // Retrieves a timestamp in microseconds to fill a Timestamp packet.
            fn get_timestamp_micros(&self) -> u64 {
                let offset_time = self.offset_time.expect("offset_time not set");

                let zedmon_now = SystemTime::now() - offset_time;
                let timestamp = zedmon_now.duration_since(SystemTime::UNIX_EPOCH).unwrap();
                timestamp.as_micros() as u64
            }

            fn set_relay_enabled(&mut self, enabled: bool) {
                let relay_enabled = self.relay_enabled.as_mut().expect("relay_enabled not set");
                *relay_enabled = enabled;
            }
        }

        // Constants that are inherent to a Zedmon device. Even if these values are not exercised by
        // a test, dummy values will be required by Client::new().
        #[derive(Clone, Debug)]
        pub struct DeviceConfiguration {
            pub shunt_resistance: f32,
            pub v_shunt_scale: f32,
            pub v_bus_scale: f32,
        }

        // Provides the interface for building a Coordinator with its various optional settings.
        pub struct CoordinatorBuilder {
            device_config: DeviceConfiguration,
            report_queue: Option<VecDeque<Vec<Report>>>,
            offset_time: Option<Duration>,
            relay_enabled: Option<bool>,
        }

        impl CoordinatorBuilder {
            pub fn new(device_config: DeviceConfiguration) -> Self {
                CoordinatorBuilder {
                    device_config,
                    report_queue: None,
                    offset_time: None,
                    relay_enabled: None,
                }
            }

            pub fn with_report_queue(mut self, report_queue: VecDeque<Vec<Report>>) -> Self {
                self.report_queue.replace(report_queue);
                self
            }

            pub fn with_offset_time(mut self, offset_time: Duration) -> Self {
                self.offset_time.replace(offset_time);
                self
            }

            pub fn with_relay_enabled(mut self, enabled: bool) -> Self {
                self.relay_enabled.replace(enabled);
                self
            }

            pub fn build(self) -> Rc<RefCell<Coordinator>> {
                Rc::new(RefCell::new(Coordinator {
                    device_config: self.device_config,
                    report_queue: self.report_queue,
                    offset_time: self.offset_time,
                    relay_enabled: self.relay_enabled,
                }))
            }
        }

        // Indicates the contents of the next read from FakeZedmonInterface.
        #[derive(Debug)]
        enum NextRead {
            ParameterValue(u8),
            ReportFormat(u8),
            Report,
            Timestamp,
        }

        // Interface that provides fakes for testing interactions with a Zedmon device.
        pub struct FakeZedmonInterface {
            coordinator: Rc<RefCell<Coordinator>>,

            // The type of read that wil be performed next from this interface, if any.
            next_read: Option<NextRead>,
        }

        impl usb_bulk::Open<FakeZedmonInterface> for FakeZedmonInterface {
            fn open<F>(_matcher: &mut F) -> Result<FakeZedmonInterface, usb_bulk::Error>
            where
                F: FnMut(&InterfaceInfo) -> bool,
            {
                Err(usb_bulk::Error::NoDeviceMatched)
            }
        }

        impl FakeZedmonInterface {
            pub fn new(coordinator: Rc<RefCell<Coordinator>>) -> Self {
                Self { coordinator, next_read: None }
            }

            // Populates a ParameterValue packet.
            fn read_parameter_value(&mut self, index: u8, buffer: &mut [u8]) -> usize {
                match index {
                    0 => serialize_parameter_value(
                        ParameterValue {
                            name: "shunt_resistance".to_string(),
                            value: Value::F32(
                                self.coordinator.borrow().get_device_config().shunt_resistance,
                            ),
                        },
                        buffer,
                    ),
                    1 => serialize_parameter_value(
                        ParameterValue { name: "".to_string(), value: Value::U8(0) },
                        buffer,
                    ),
                    _ => panic!("Should only receive 0 or 1 as indices"),
                }
            }

            // Populates a ReportFormat packet.
            fn read_report_format(&self, index: u8, buffer: &mut [u8]) -> usize {
                match index {
                    0 => serialize_report_format(
                        ReportFormat {
                            index,
                            field_type: ScalarType::I16,
                            unit: Unit::Volts,
                            scale: self.coordinator.borrow().get_device_config().v_shunt_scale,
                            name: "v_shunt".to_string(),
                        },
                        buffer,
                    ),
                    1 => serialize_report_format(
                        ReportFormat {
                            index,
                            field_type: ScalarType::I16,
                            unit: Unit::Volts,
                            scale: self.coordinator.borrow().get_device_config().v_bus_scale,
                            name: "v_bus".to_string(),
                        },
                        buffer,
                    ),
                    2 => serialize_report_format(
                        ReportFormat {
                            index: protocol::REPORT_FORMAT_INDEX_END,
                            field_type: ScalarType::U8,
                            unit: Unit::Volts,
                            scale: 0.0,
                            name: "".to_string(),
                        },
                        buffer,
                    ),
                    _ => panic!("Should only receive 0, 1, or 2 as indices"),
                }
            }

            // Populates a Report packet. If no Reports are available, the buffer is not modified,
            // and attempting to parse reports from it will be an error. The processing thread will
            // be guarded from attempting such parses by the StopSignal.
            fn read_reports(&mut self, buffer: &mut [u8]) -> usize {
                match self.coordinator.borrow_mut().get_reports_for_packet() {
                    Some(reports) => serialize_reports(&reports, buffer),
                    None => 0,
                }
            }

            // Populates a Timestamp packet.
            fn read_timestamp(&mut self, buffer: &mut [u8]) -> usize {
                buffer[0] = PacketType::Timestamp as u8;
                serialize_timestamp_micros(self.coordinator.borrow().get_timestamp_micros(), buffer)
            }

            fn set_output(&self, index: u8, value: u8) {
                if index == protocol::Output::Relay as u8 {
                    self.coordinator.borrow_mut().set_relay_enabled(value != 0);
                }
            }
        }

        impl Read for FakeZedmonInterface {
            fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
                match self.next_read.take() {
                    Some(value) => Ok(match value {
                        NextRead::ParameterValue(index) => self.read_parameter_value(index, buffer),
                        NextRead::ReportFormat(index) => self.read_report_format(index, buffer),
                        NextRead::Report => {
                            self.next_read = Some(NextRead::Report);
                            self.read_reports(buffer)
                        }
                        NextRead::Timestamp => self.read_timestamp(buffer),
                    }),
                    None => Err(std::io::Error::new(std::io::ErrorKind::Other, "Read error: -1")),
                }
            }
        }

        impl Write for FakeZedmonInterface {
            fn write(&mut self, data: &[u8]) -> std::io::Result<usize> {
                let packet_type = PacketType::from_u8(data[0]).unwrap();
                match packet_type {
                    PacketType::EnableReporting => self.next_read = Some(NextRead::Report),
                    PacketType::DisableReporting => self.next_read = None,
                    PacketType::QueryParameter => {
                        self.next_read = Some(NextRead::ParameterValue(data[1]))
                    }
                    PacketType::QueryReportFormat => {
                        self.next_read = Some(NextRead::ReportFormat(data[1]))
                    }
                    PacketType::QueryTime => self.next_read = Some(NextRead::Timestamp),
                    PacketType::SetOutput => {
                        assert_eq!(data.len(), 3);
                        self.set_output(data[1], data[2]);
                    }
                    _ => panic!("Not a valid host-to-target packet"),
                }
                Ok(data.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
    }

    fn make_report_queue(
        voltage_function: impl Fn(u64) -> (f32, f32),
        device_config: &fake_device::DeviceConfiguration,
        test_duration: Duration,
        raw_data_interval: Duration,
    ) -> VecDeque<Vec<Report>> {
        let mut report_queue = VecDeque::new();

        let mut elapsed = Duration::from_millis(0);

        // 1ms of fake time elapses between each report. Reports are batched into groups of 5,
        // the number that will fit into a single packet.
        while elapsed <= test_duration {
            let mut reports = Vec::new();
            for _ in 0..5 {
                let (v_shunt, v_bus) = (voltage_function)(elapsed.as_micros() as u64);
                reports.push(Report {
                    timestamp_micros: elapsed.as_micros() as u64,
                    values: vec![
                        Value::I16((v_shunt / device_config.v_shunt_scale) as i16),
                        Value::I16((v_bus / device_config.v_bus_scale) as i16),
                    ],
                });

                elapsed = elapsed + raw_data_interval;
                if elapsed > test_duration {
                    break;
                }
            }
            if !reports.is_empty() {
                report_queue.push_back(reports);
            }
        }

        report_queue
    }

    fn run_zedmon_reporting_with_options<
        InterfaceType: usb_bulk::Open<InterfaceType> + Read + Write,
    >(
        zedmon: &Client<InterfaceType>,
        test_duration: Duration,
        options: ReportingOptions,
    ) -> Result<Vec<u8>, Error> {
        // Implements Write by sending bytes over a channel. The holder of the channel's
        // Receiver can then inspect the data that was written to test expectations.
        struct ChannelWriter {
            sender: mpsc::Sender<Vec<u8>>,
            buffer: Vec<u8>,
        }

        impl std::io::Write for ChannelWriter {
            fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
                self.buffer.extend_from_slice(buf);
                Ok(buf.len())
            }

            fn flush(&mut self) -> std::io::Result<()> {
                let mut payload = Vec::new();
                std::mem::swap(&mut self.buffer, &mut payload);
                self.sender.send(payload).unwrap();
                Ok(())
            }
        }

        let (sender, receiver) = mpsc::channel();
        let writer = Box::new(ChannelWriter { sender, buffer: Vec::new() });
        zedmon.read_reports(writer, DurationStopper::new(test_duration), options)?;

        let mut output = Vec::new();
        while let Ok(mut buffer) = receiver.recv() {
            output.append(&mut buffer);
        }

        Ok(output)
    }

    fn run_zedmon_reporting<InterfaceType: usb_bulk::Open<InterfaceType> + Read + Write>(
        zedmon: &Client<InterfaceType>,
        test_duration: Duration,
        reporting_interval: Option<Duration>,
    ) -> Result<Vec<u8>, Error> {
        let options = ReportingOptions {
            interval: reporting_interval,
            use_host_timestamps: false,
            output_power_only: false,
        };

        run_zedmon_reporting_with_options(zedmon, test_duration, options)
    }

    // Tests that Client will disable reporting and drain enqueued packets on initialization.
    #[test]
    fn test_disable_reporting_and_drain_packets() {
        // Interface that responds to reads with Report packets until reporting is disabled and an
        // enqueued packet is drained. Afterwards, all calls are forwarded to an inner
        // FakeZedmonInterface.
        struct StillReportingInterface {
            inner: fake_device::FakeZedmonInterface,
            reporting_enabled: bool,
            packets_enqueued: usize,
        }

        impl StillReportingInterface {
            fn new(coordinator: Rc<RefCell<fake_device::Coordinator>>) -> Self {
                Self {
                    inner: fake_device::FakeZedmonInterface::new(coordinator),
                    reporting_enabled: true,
                    packets_enqueued: 1,
                }
            }

            fn make_reports(&self) -> Vec<Report> {
                let mut reports = Vec::new();
                for i in 0..5 {
                    reports.push(Report {
                        timestamp_micros: 1000 * (i as u64),
                        values: vec![Value::I16(i as i16), Value::I16(-(i as i16))],
                    });
                }
                reports
            }
        }

        impl usb_bulk::Open<StillReportingInterface> for StillReportingInterface {
            fn open<F>(_matcher: &mut F) -> Result<StillReportingInterface, usb_bulk::Error>
            where
                F: FnMut(&InterfaceInfo) -> bool,
            {
                Err(usb_bulk::Error::NoDeviceMatched)
            }
        }

        impl Read for StillReportingInterface {
            fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
                if self.reporting_enabled {
                    Ok(serialize_reports(&self.make_reports(), buffer))
                } else if self.packets_enqueued > 0 {
                    self.packets_enqueued = self.packets_enqueued - 1;
                    Ok(serialize_reports(&self.make_reports(), buffer))
                } else {
                    self.inner.read(buffer)
                }
            }
        }

        impl Write for StillReportingInterface {
            fn write(&mut self, data: &[u8]) -> std::io::Result<usize> {
                if self.reporting_enabled
                    && PacketType::from_u8(data[0]).unwrap() == PacketType::DisableReporting
                {
                    self.reporting_enabled = false;
                    Ok(1)
                } else {
                    self.inner.write(data)
                }
            }

            fn flush(&mut self) -> std::io::Result<()> {
                self.inner.flush()
            }
        }

        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 0.01,
            v_shunt_scale: 1e-5,
            v_bus_scale: 0.025,
        };
        let builder = fake_device::CoordinatorBuilder::new(device_config);
        let coordinator = builder.build();
        let interface = StillReportingInterface::new(coordinator);

        assert!(Client::new(interface).is_ok());
    }

    // Represents USB read responses enqueued by TransientFailureInterface.
    enum UsbReadResponse {
        Packet(Vec<u8>),
        Error(std::io::Error),
    }

    // Acts as an intermediary between Client and a FakeZedmonInterface, injecting
    // `num_failures` read errors at the beginning of the report stream.
    struct TransientFailureInterface {
        inner: fake_device::FakeZedmonInterface,
        num_failures: usize,
        response_queue: VecDeque<UsbReadResponse>,
    }

    impl TransientFailureInterface {
        fn new(coordinator: Rc<RefCell<fake_device::Coordinator>>, num_failures: usize) -> Self {
            Self {
                inner: fake_device::FakeZedmonInterface::new(coordinator),
                num_failures,
                response_queue: VecDeque::new(),
            }
        }
    }

    impl usb_bulk::Open<TransientFailureInterface> for TransientFailureInterface {
        fn open<F>(_matcher: &mut F) -> Result<TransientFailureInterface, usb_bulk::Error>
        where
            F: FnMut(&InterfaceInfo) -> bool,
        {
            Err(usb_bulk::Error::NoDeviceMatched)
        }
    }

    impl Read for TransientFailureInterface {
        fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
            let mut packet = vec![0; MAX_PACKET_SIZE];
            match self.inner.read(packet.as_mut_slice()) {
                Ok(num_bytes) => {
                    packet.truncate(num_bytes);
                    #[allow(clippy::if_same_then_else)] // TODO(fxbug.dev/95036)
                    if num_bytes == 0 {
                        // This case corresponds to exhaustion of `inner`s report queue.
                        self.response_queue.push_back(UsbReadResponse::Packet(packet));
                    } else if packet[0] == PacketType::Report as u8 {
                        // Enqueue a report packet for later.
                        self.response_queue.push_back(UsbReadResponse::Packet(packet));
                    } else {
                        // Any non-Report packets are passed through directly.
                        (&mut buffer[..num_bytes]).copy_from_slice(&packet);
                        return Ok(num_bytes);
                    }
                }
                Err(e) => self.response_queue.push_back(UsbReadResponse::Error(e)),
            };

            // If any failures remain, inject one; otherwise, return an enqueued Report.
            if self.num_failures > 0 {
                self.num_failures -= 1;
                Err(std::io::Error::new(std::io::ErrorKind::Other, "Read error: -1"))
            } else {
                let response = self.response_queue.pop_front().unwrap();
                match response {
                    UsbReadResponse::Packet(packet) => {
                        let num_bytes = packet.len();
                        (&mut buffer[..num_bytes]).copy_from_slice(&packet);
                        Ok(num_bytes)
                    }
                    UsbReadResponse::Error(e) => Err(e),
                }
            }
        }
    }

    impl Write for TransientFailureInterface {
        fn write(&mut self, data: &[u8]) -> std::io::Result<usize> {
            self.inner.write(data)
        }

        fn flush(&mut self) -> std::io::Result<()> {
            self.inner.flush()
        }
    }

    #[test]
    fn test_usb_read_failures() {
        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 0.01,
            v_shunt_scale: 1e-5,
            v_bus_scale: 0.025,
        };
        let test_duration = Duration::from_secs(1);
        let raw_data_interval = Duration::from_millis(100);

        let report_queue =
            make_report_queue(|_| (0.0025, 1.0), &device_config, test_duration, raw_data_interval);

        let run_reporting = |num_failures| {
            let coordinator = fake_device::CoordinatorBuilder::new(device_config.clone())
                .with_report_queue(report_queue.clone())
                .build();
            let interface = TransientFailureInterface::new(coordinator, num_failures);

            let zedmon = Client::new(interface).expect("Error building zedmon client");

            run_zedmon_reporting(&zedmon, test_duration, None)
        };

        let max_failures = Client::<TransientFailureInterface>::num_usb_read_retries();

        // Test that reporting proceeds in the event of a few consecutive USB read failures.
        let result = run_reporting(max_failures);
        assert!(result.is_ok());
        let output = result.unwrap();
        let mut reader =
            csv::ReaderBuilder::new().has_headers(false).from_reader(output.as_slice());
        assert_eq!(reader.deserialize::<Record>().count(), 11);

        // Test that reporting terminates with an error if too many consecutive read failures occur.
        let result = run_reporting(max_failures + 1);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("USB read failures"));
    }

    #[test]
    fn test_read_reports() -> Result<(), Error> {
        // The voltages used are simply time-dependent signals that, combined with shunt_resistance
        // below, yield power on the order of a few Watts.
        fn get_voltages(micros: u64) -> (f32, f32) {
            let seconds = micros as f32 / 1e6;
            let v_shunt = 1e-3 + 2e-4 * (std::f32::consts::PI * seconds).cos();
            let v_bus = 20.0 + 3.0 * (std::f32::consts::PI * seconds).sin();
            (v_shunt, v_bus)
        }

        // These values are in the same ballpark as those used on Zedmon 2.1. The test shouldn't be
        // sensitive to them.
        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 0.01,
            v_shunt_scale: 1e-5,
            v_bus_scale: 0.025,
        };
        let test_duration = Duration::from_secs(10);
        let raw_data_interval = Duration::from_millis(1);

        let report_queue =
            make_report_queue(get_voltages, &device_config, test_duration, raw_data_interval);

        let coordinator = fake_device::CoordinatorBuilder::new(device_config.clone())
            .with_report_queue(report_queue.clone())
            .build();
        let interface = fake_device::FakeZedmonInterface::new(coordinator);
        let zedmon = Client::new(interface).expect("Error building zedmon client");

        let output = run_zedmon_reporting(&zedmon, test_duration, None)?;
        let mut reader =
            csv::ReaderBuilder::new().has_headers(false).from_reader(output.as_slice());

        let mut num_records = 0;
        for result in reader.deserialize::<Record>() {
            let record = result?;
            num_records = num_records + 1;

            let (expected_shunt_voltage, expected_bus_voltage) =
                get_voltages(record.timestamp_micros);
            assert_near!(record.shunt_voltage, expected_shunt_voltage, device_config.v_shunt_scale);
            assert_near!(record.bus_voltage, expected_bus_voltage, device_config.v_bus_scale);
            assert_near!(
                record.power,
                record.shunt_voltage * record.bus_voltage / device_config.shunt_resistance,
                1e-6
            );
        }

        assert_eq!(num_records, test_duration.as_millis() / raw_data_interval.as_millis() + 1);

        Ok(())
    }

    #[test]
    fn test_read_reports_downsampled() -> Result<(), Error> {
        // The voltages and device config are all completely made up for mathematical convenience.
        // They are chosen so that v_shunt, v_bus, and power are easy to integrate analytically over
        // the downsampling intervals.
        fn get_voltages(micros: u64) -> (f32, f32) {
            let seconds = micros as f32 / 1e6;
            let v_shunt = seconds;
            let v_bus = seconds.powi(2);
            (v_shunt, v_bus)
        }

        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 2.0,
            v_shunt_scale: 1e-4,
            v_bus_scale: 1e-4,
        };

        let test_duration = Duration::from_secs(1);
        let raw_data_interval = Duration::from_millis(1);
        let reporting_interval = Duration::from_millis(100);

        // Interval-average formulas follow from the definition
        //   avg(y(t), [t_low, t_high]) := 1 / (t_high - t_low) * \int_{t_low}^{t_high} y(s) ds.
        // Input times are in seconds.
        let v_shunt_integral = |t1: f32, t2: f32| (t2.powi(2) - t1.powi(2)) / (2.0 * (t2 - t1));
        let v_bus_integral = |t1: f32, t2: f32| (t2.powi(3) - t1.powi(3)) / (3.0 * (t2 - t1));
        let power_integral = |t1: f32, t2: f32| {
            (t2.powi(4) - t1.powi(4)) / (4.0 * device_config.shunt_resistance * (t2 - t1))
        };

        let report_queue =
            make_report_queue(get_voltages, &device_config, test_duration, raw_data_interval);

        let coordinator = fake_device::CoordinatorBuilder::new(device_config.clone())
            .with_report_queue(report_queue.clone())
            .build();
        let interface = fake_device::FakeZedmonInterface::new(coordinator);
        let zedmon = Client::new(interface).expect("Error building zedmon client");

        let output = run_zedmon_reporting(&zedmon, test_duration, Some(reporting_interval))?;
        let mut reader =
            csv::ReaderBuilder::new().has_headers(false).from_reader(output.as_slice());

        // Both v_shunt and v_bus have a max value of 1. Since
        //     power = v_shunt * v_bus / shunt_resistance,
        // the maximum error in a raw power measurement should be roughly
        //     (v_shunt_scale + v_bus_scale) / shunt_resistance.
        let power_tolerance = (device_config.v_shunt_scale + device_config.v_bus_scale)
            / device_config.shunt_resistance;

        let mut num_records = 0;
        let mut prev_timestamp = 0;
        for result in reader.deserialize::<Record>() {
            let record = result?;

            num_records = num_records + 1;

            let t = record.timestamp_micros as f32 / 1e6;
            let t_prev = prev_timestamp as f32 / 1e6;

            assert_near!(
                record.shunt_voltage,
                v_shunt_integral(t_prev, t),
                device_config.v_shunt_scale
            );
            assert_near!(record.bus_voltage, v_bus_integral(t_prev, t), device_config.v_bus_scale);
            assert_near!(record.power, power_integral(t_prev, t), power_tolerance);

            prev_timestamp = record.timestamp_micros;
        }

        assert_eq!(num_records, test_duration.as_millis() / reporting_interval.as_millis());

        Ok(())
    }

    #[test]
    fn test_get_time_offset_micros() -> Result<(), Error> {
        // Test a few different zedmon offsets across a 1-microsecond range to exercise rounding
        // effects. The base offset is the Unix epoch at a particular time on 2021-03-15 PDT.
        let base_offset = Duration::from_nanos(1_615_859_483_501_877_405);
        let deltas = vec![0u64, 250, 500, 750].into_iter().map(Duration::from_nanos);

        for delta in deltas.into_iter() {
            let zedmon_offset = base_offset + delta;

            // Values are not used by this test.
            let device_config = fake_device::DeviceConfiguration {
                shunt_resistance: 0.0,
                v_shunt_scale: 0.0,
                v_bus_scale: 0.0,
            };

            let builder =
                fake_device::CoordinatorBuilder::new(device_config).with_offset_time(zedmon_offset);
            let coordinator = builder.build();
            let interface = fake_device::FakeZedmonInterface::new(coordinator);
            let zedmon = Client::new(interface)?;

            let (reported_offset, uncertainty) = zedmon.get_time_offset_micros()?;

            // Since get_time_offset_micros incorporates the worst case nanos-to-micros error in
            // the uncertainty, any sane rounding method here should suffice.
            let expected_offset = zedmon_offset.as_nanos() as i64 / 1_000;
            assert_near!(expected_offset, reported_offset, uncertainty);
        }

        Ok(())
    }

    #[test]
    fn test_set_relay() -> Result<(), Error> {
        // Values are not used by this test.
        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 0.0,
            v_shunt_scale: 0.0,
            v_bus_scale: 0.0,
        };

        let builder = fake_device::CoordinatorBuilder::new(device_config).with_relay_enabled(false);
        let coordinator = builder.build();
        let interface = fake_device::FakeZedmonInterface::new(coordinator.clone());
        let zedmon = Client::new(interface)?;

        // Test true->false and false->true transitions, and no-ops in each state.
        zedmon.set_relay(true)?;
        assert_eq!(coordinator.borrow().relay_enabled(), true);
        zedmon.set_relay(true)?;
        assert_eq!(coordinator.borrow().relay_enabled(), true);
        zedmon.set_relay(false)?;
        assert_eq!(coordinator.borrow().relay_enabled(), false);
        zedmon.set_relay(false)?;
        assert_eq!(coordinator.borrow().relay_enabled(), false);
        zedmon.set_relay(true)?;
        assert_eq!(coordinator.borrow().relay_enabled(), true);

        Ok(())
    }

    // Runs a test of Downsampler parameterized by the start time.
    fn run_downsampler_test(t_start_micros: u64) {
        // Total duration of the test scenario.
        let duration_micros = 1_000_000;

        // Interval of the raw signal (100 Hz).
        let raw_interval_micros = 10_000;

        // Downsample to 80 Hz. This lets us test both when the downsampling interval coincides with
        // the end of a raw sample interval (t=25,000us, 50,000us, 75,000us, ...) and when it
        // does not (t=12,500us, 37,500us, 62,500us, ...).
        let downsampling_interval_micros = 12_500;
        let mut downsampler = Downsampler::new(downsampling_interval_micros);

        // Functions defining the raw and average values for each quantity. These are made up, and
        // there is no relationship between the voltages and power in Downsampler's context.
        //
        // Interval-average formulas follow from the definition
        //   avg(y(t), [t_low, t_high]) := 1 / (t_high - t_low) * \int_{t_low}^{t_high} y(s) ds.
        //
        // With times measured in seconds,
        //   shunt voltage: raw: t-t0, average: 1/(t2-t1) * 1/2 * ((t2-t0)^2 - (t1-t0)^2
        //   bus voltage: raw: (t-t0)^2, average: 1/(t2-t1) * 1/3 * ((t2-t0)^3 - (t1-t0)^3
        //   power: raw: (t-t0)^3, average: 1/(t2-t1) * 1/4 * ((t2-t0)^4 - (t1-t0)^4)
        let to_seconds = |micros: u64| micros as f32 * 1e-6;
        let shunt_voltage_raw = |t_micros: u64| to_seconds(t_micros - t_start_micros);
        let shunt_voltage_average = |t1_micros: u64, t2_micros: u64| {
            let dt1 = to_seconds(t1_micros - t_start_micros);
            let dt2 = to_seconds(t2_micros - t_start_micros);
            (dt2.powi(2) - dt1.powi(2)) / (2.0 * (dt2 - dt1))
        };

        let bus_voltage_raw = |t_micros: u64| to_seconds(t_micros - t_start_micros).powi(2);
        let bus_voltage_average = |t1_micros: u64, t2_micros: u64| {
            let dt1 = to_seconds(t1_micros - t_start_micros);
            let dt2 = to_seconds(t2_micros - t_start_micros);
            (dt2.powi(3) - dt1.powi(3)) / (3.0 * (dt2 - dt1))
        };

        let power_raw = |t_micros: u64| to_seconds(t_micros - t_start_micros).powi(3);
        let power_average = |t1_micros: u64, t2_micros: u64| {
            let dt1 = to_seconds(t1_micros - t_start_micros);
            let dt2 = to_seconds(t2_micros - t_start_micros);
            (dt2.powi(4) - dt1.powi(4)) / (4.0 * (dt2 - dt1))
        };

        // Collect raw samples from t=0s to t=1s.
        let mut t_micros = t_start_micros;
        let mut records_out = Vec::new();
        while t_micros <= t_start_micros + duration_micros {
            let record = Record {
                timestamp_micros: t_micros,
                shunt_voltage: shunt_voltage_raw(t_micros),
                bus_voltage: bus_voltage_raw(t_micros),
                power: power_raw(t_micros),
            };
            match downsampler.process(record) {
                Some(r) => records_out.push(r),
                None => {}
            }

            t_micros += raw_interval_micros;
        }

        // Confirm expectations.
        assert_eq!(records_out.len(), (duration_micros / downsampling_interval_micros) as usize);

        for (i, record) in records_out.into_iter().enumerate() {
            // Endpoints of the interval for this sample, in microseconds.
            let t1 = t_start_micros + (i as u64) * downsampling_interval_micros;
            let t2 = t1 + downsampling_interval_micros;
            assert_eq!(record.timestamp_micros, t2);
            assert_near!(record.shunt_voltage, shunt_voltage_average(t1, t2), 1e-4);
            assert_near!(record.bus_voltage, bus_voltage_average(t1, t2), 1e-4);
            assert_near!(record.power, power_average(t1, t2), 1e-4);
        }
    }

    #[test]
    fn test_downsampler_nominal() {
        run_downsampler_test(0);
    }

    // Using timestamps near the Unix epoch can lead to lossy calculations between u64 timestamps
    // and f32 time deltas if appropriate care is not taken.
    #[test]
    fn test_downsampler_large_timestamps() {
        let t_start_micros = 1_615_848_933_000_000; // Sometime on 2021-03-15 PDT
        run_downsampler_test(t_start_micros);
    }

    #[test]
    fn test_downsampler_with_data_gap() {
        // Run for 1 second with a raw data interval of 10ms and a downsampling interval of 100ms,
        // with an outage in raw data from 230ms to 440ms, inclusive. Downsampling will skip outputs
        // at 300ms and 400ms and reinitialize at 450ms, so we should see a gap in timestamps of the
        // output samples between 200ms and 550ms.
        let duration_micros = 1_000_000;
        let raw_interval_micros = 10_000;
        let downsampling_interval_micros = 100_000;
        let raw_data_gap_micros = [230_000, 440_000];

        let mut downsampler = Downsampler::new(downsampling_interval_micros);

        let mut t_micros = 0;
        let mut records_out = Vec::new();
        while t_micros <= duration_micros {
            if t_micros < raw_data_gap_micros[0] || t_micros > raw_data_gap_micros[1] {
                let record = Record {
                    timestamp_micros: t_micros,
                    shunt_voltage: 1.0,
                    bus_voltage: 1.0,
                    power: 1.0,
                };
                match downsampler.process(record) {
                    Some(r) => records_out.push(r),
                    None => {}
                }
            }

            t_micros += raw_interval_micros;
        }

        let timestamps: Vec<u64> = records_out.into_iter().map(|r| r.timestamp_micros).collect();
        assert_eq!(timestamps, vec![100_000, 200_000, 550_000, 650_000, 750_000, 850_000, 950_000]);
    }

    #[test]
    fn test_record_power_only() {
        let device_config = fake_device::DeviceConfiguration {
            shunt_resistance: 0.01,
            v_shunt_scale: 1e-4,
            v_bus_scale: 1e-4,
        };

        let get_voltages = |_| (0.0025, 1.0); // v_shunt, v_bus
        let test_duration = Duration::from_secs(1);
        let raw_data_interval = Duration::from_secs(1);
        let report_queue =
            make_report_queue(get_voltages, &device_config, test_duration, raw_data_interval);

        let builder = fake_device::CoordinatorBuilder::new(device_config);
        let coordinator = builder.with_report_queue(report_queue.clone()).build();
        let interface = fake_device::FakeZedmonInterface::new(coordinator);
        let zedmon = Client::new(interface).expect("Error building zedmon client");

        let reporting_options = ReportingOptions {
            interval: None,
            use_host_timestamps: false,
            output_power_only: true,
        };
        let output = run_zedmon_reporting_with_options(&zedmon, test_duration, reporting_options)
            .expect("Error running zedmon reporting");
        let mut reader =
            csv::ReaderBuilder::new().has_headers(false).from_reader(output.as_slice());
        let mut deserialized = reader.deserialize::<f32>();
        assert_eq!(
            deserialized.next().expect("iter returned None").expect("deserialize error"),
            0.25
        );
        assert_eq!(
            deserialized.next().expect("iter returned None").expect("deserialize error"),
            0.25
        );
        assert!(deserialized.next().is_none());
    }
}
