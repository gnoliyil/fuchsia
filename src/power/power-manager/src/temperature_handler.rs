// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::error::PowerManagerError;
use crate::log_if_err;
use crate::message::{Message, MessageReturn};
use crate::node::Node;
use crate::ok_or_default_err;
use crate::types::{Celsius, Nanoseconds, Seconds};
use crate::utils::result_debug_panic::ResultDebugPanic;
use anyhow::{format_err, Error};
use async_trait::async_trait;
use async_utils::event::Event as AsyncEvent;
use fidl_fuchsia_hardware_temperature as ftemperature;
use fuchsia_async as fasync;
use fuchsia_inspect::{self as inspect, NumericProperty, Property};
use fuchsia_inspect_contrib::{inspect_log, nodes::BoundedListNode};
use fuchsia_zircon as zx;
use log::*;
use serde_derive::Deserialize;
use serde_json as json;
use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

/// Node: TemperatureHandler
///
/// Summary: Responds to temperature requests from other nodes by polling the specified driver
///          using the temperature FIDL protocol. May be configured to cache the polled temperature
///          for a while to prevent excessive polling of the sensor. (Polling errors are not
///          cached.)
///
/// Handles Messages:
///     - ReadTemperature
///
/// Sends Messages: N/A
///
/// FIDL dependencies:
///     - fuchsia.hardware.temperature: the node uses this protocol to query the temperature driver
///       specified by `driver_path` in the TemperatureHandler constructor

/// A builder for constructing the TemperatureHandler node
pub struct TemperatureHandlerBuilder<'a> {
    driver_path: Option<String>,
    driver_proxy: Option<ftemperature::DeviceProxy>,
    cache_duration: Option<zx::Duration>,
    inspect_root: Option<&'a inspect::Node>,
}

impl<'a> TemperatureHandlerBuilder<'a> {
    #[cfg(test)]
    pub fn new() -> Self {
        Self {
            driver_path: Some("/test/driver/path".to_string()),
            driver_proxy: None,
            cache_duration: None,
            inspect_root: None,
        }
    }

    #[cfg(test)]
    pub fn driver_proxy(mut self, proxy: ftemperature::DeviceProxy) -> Self {
        self.driver_proxy = Some(proxy);
        self
    }

    #[cfg(test)]
    pub fn cache_duration(mut self, duration: zx::Duration) -> Self {
        self.cache_duration = Some(duration);
        self
    }

    #[cfg(test)]
    fn inspect_root(mut self, inspect_root: &'a inspect::Node) -> Self {
        self.inspect_root = Some(inspect_root);
        self
    }

    pub fn new_from_json(json_data: json::Value, _nodes: &HashMap<String, Rc<dyn Node>>) -> Self {
        #[derive(Deserialize)]
        struct Config {
            driver_path: String,
            cache_duration_ms: u32,
        }

        #[derive(Deserialize)]
        struct JsonData {
            config: Config,
        }

        let data: JsonData = json::from_value(json_data).unwrap();
        Self {
            driver_path: Some(data.config.driver_path),
            driver_proxy: None,
            cache_duration: Some(zx::Duration::from_millis(data.config.cache_duration_ms as i64)),
            inspect_root: None,
        }
    }

    pub fn build(self) -> Result<Rc<TemperatureHandler>, Error> {
        let driver_path = ok_or_default_err!(self.driver_path).or_debug_panic()?;

        // Default `cache_duration`: 0
        let cache_duration = self.cache_duration.unwrap_or(zx::Duration::from_millis(0));

        let mutable_inner = MutableInner {
            last_temperature: Celsius(std::f64::NAN),
            last_poll_time: fasync::Time::INFINITE_PAST,
            driver_proxy: self.driver_proxy,
            debug_temperature_override: None,
        };

        // Optionally use the default inspect root node
        let inspect_root = self.inspect_root.unwrap_or(inspect::component::inspector().root());

        Ok(Rc::new(TemperatureHandler {
            init_done: AsyncEvent::new(),
            driver_path: driver_path.clone(),
            mutable_inner: RefCell::new(mutable_inner),
            cache_duration,
            inspect: InspectData::new(
                inspect_root,
                format!("TemperatureHandler ({})", driver_path),
            ),
        }))
    }

    #[cfg(test)]
    pub async fn build_and_init(self) -> Rc<TemperatureHandler> {
        let node = self.build().unwrap();
        node.init().await.unwrap();
        node
    }
}

pub struct TemperatureHandler {
    /// Signalled after `init()` has completed. Used to ensure node doesn't process messages until
    /// its `init()` has completed.
    init_done: AsyncEvent,

    /// Path to the temperature driver that this node reads from.
    driver_path: String,

    /// Mutable inner state.
    mutable_inner: RefCell<MutableInner>,

    /// Duration for which a polled temperature is cached. This prevents excessive polling of the
    /// sensor.
    cache_duration: zx::Duration,

    /// A struct for managing Component Inspection data
    inspect: InspectData,
}

impl TemperatureHandler {
    fn handle_get_driver_path(&self) -> Result<MessageReturn, PowerManagerError> {
        Ok(MessageReturn::GetDriverPath(self.driver_path.clone()))
    }

    async fn handle_read_temperature(&self) -> Result<MessageReturn, PowerManagerError> {
        fuchsia_trace::duration!(
            "power_manager",
            "TemperatureHandler::handle_read_temperature",
            "driver" => self.driver_path.as_str()
        );

        self.init_done.wait().await;

        if let Some(temperature) = self.mutable_inner.borrow().debug_temperature_override {
            return Ok(MessageReturn::ReadTemperature(temperature));
        }

        let last_poll_time = self.mutable_inner.borrow().last_poll_time;
        let last_temperature = self.mutable_inner.borrow().last_temperature;

        // If the last temperature value is sufficiently fresh, return it instead of polling.
        // Note that if the previous poll generated an error, `last_poll_time` was not updated,
        // and (barring clock glitches) a new poll will occur.
        if fasync::Time::now() <= last_poll_time + self.cache_duration {
            return Ok(MessageReturn::ReadTemperature(last_temperature));
        }

        let result = self.read_temperature().await;
        log_if_err!(
            result,
            format!("Failed to read temperature from {}", self.driver_path).as_str()
        );
        fuchsia_trace::instant!(
            "power_manager",
            "TemperatureHandler::read_temperature_result",
            fuchsia_trace::Scope::Thread,
            "driver" => self.driver_path.as_str(),
            "result" => format!("{:?}", result).as_str()
        );

        match result {
            Ok(temperature) => {
                let mut inner = self.mutable_inner.borrow_mut();
                inner.last_temperature = temperature;
                inner.last_poll_time = fasync::Time::now();
                self.inspect.log_temperature_reading(temperature);
                Ok(MessageReturn::ReadTemperature(temperature))
            }
            Err(e) => {
                self.inspect.read_errors.add(1);
                self.inspect.last_read_error.set(format!("{}", e).as_str());
                Err(e.into())
            }
        }
    }

    async fn read_temperature(&self) -> Result<Celsius, Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "TemperatureHandler::read_temperature",
            "driver" => self.driver_path.as_str()
        );

        // Extract `driver_proxy` from `mutable_inner`, returning an error (or asserting in debug)
        // if the proxy is missing
        let driver_proxy = self
            .mutable_inner
            .borrow()
            .driver_proxy
            .as_ref()
            .ok_or(format_err!("Missing driver_proxy"))
            .or_debug_panic()?
            .clone();

        let (status, temperature) = driver_proxy.get_temperature_celsius().await.map_err(|e| {
            format_err!("{}: get_temperature_celsius IPC failed: {}", self.name(), e)
        })?;
        zx::Status::ok(status).map_err(|e| {
            format_err!("{}: get_temperature_celsius driver returned error: {}", self.name(), e)
        })?;
        Ok(Celsius(temperature.into()))
    }

    fn handle_debug_message(
        &self,
        command: &String,
        args: &Vec<String>,
    ) -> Result<MessageReturn, PowerManagerError> {
        let print_help = || {
            info!("Supported commands: set_temperature, clear_temperature");
        };

        match command.as_str() {
            "set_temperature_override" => {
                let temperature = Celsius(
                    args.get(0)
                        .ok_or(PowerManagerError::InvalidArgument(format!(
                            "Must specify exactly one arg"
                        )))?
                        .parse::<f64>()
                        .map_err(|_| {
                            PowerManagerError::InvalidArgument(format!(
                                "Couldn't parse f64 from arg {}",
                                args[0]
                            ))
                        })?,
                );
                info!("Overriding temperature to {} C", temperature.0);
                self.mutable_inner.borrow_mut().debug_temperature_override = Some(temperature);
            }
            "clear_temperature_override" => {
                info!("Clearing temperature override");
                self.mutable_inner.borrow_mut().debug_temperature_override = None;
            }
            "help" => {
                print_help();
            }
            e => {
                error!("Unsupported command {}", e);
                print_help();
                return Err(PowerManagerError::Unsupported);
            }
        }

        Ok(MessageReturn::Debug)
    }
}

struct MutableInner {
    /// Last temperature returned by the handler.
    last_temperature: Celsius,

    /// Time of the last temperature poll, for determining cache freshness.
    last_poll_time: fasync::Time,

    /// Proxy to the temperature driver. Populated during `init()` unless previously supplied (in a
    /// test).
    driver_proxy: Option<ftemperature::DeviceProxy>,

    /// Allow the debug service to set an override temperature. If set, `ReadTemperature` will
    /// always respond with this temperature.
    debug_temperature_override: Option<Celsius>,
}

#[async_trait(?Send)]
impl Node for TemperatureHandler {
    fn name(&self) -> String {
        format!("TemperatureHandler ({})", self.driver_path)
    }

    /// Initializes internal state.
    ///
    /// Connects to the temperature driver unless a proxy was already provided (in a test).
    async fn init(&self) -> Result<(), Error> {
        fuchsia_trace::duration!("power_manager", "TemperatureHandler::init");

        // Connect to the temperature driver. Typically this is None, but it may be set by tests.
        let driver_proxy = match &self.mutable_inner.borrow().driver_proxy {
            Some(p) => p.clone(),
            None => fuchsia_component::client::connect_to_protocol_at_path::<
                ftemperature::DeviceMarker,
            >(&self.driver_path)?,
        };

        self.mutable_inner.borrow_mut().driver_proxy = Some(driver_proxy);
        self.init_done.signal();

        Ok(())
    }

    async fn handle_message(&self, msg: &Message) -> Result<MessageReturn, PowerManagerError> {
        match msg {
            Message::ReadTemperature => self.handle_read_temperature().await,
            Message::GetDriverPath => self.handle_get_driver_path(),
            Message::Debug(command, args) => self.handle_debug_message(command, args),
            _ => Err(PowerManagerError::Unsupported),
        }
    }
}

struct InspectData {
    temperature_readings: RefCell<BoundedListNode>,
    read_errors: inspect::UintProperty,
    last_read_error: inspect::StringProperty,
}

impl InspectData {
    /// Number of inspect samples to store in the `temperature_readings` BoundedListNode.
    // Store the last 60 seconds of temperature readings (fxbug.dev/59774)
    const NUM_INSPECT_TEMPERATURE_SAMPLES: usize = 60;

    fn new(parent: &inspect::Node, name: String) -> Self {
        // Create a local root node and properties
        let root = parent.create_child(name);
        let temperature_readings = RefCell::new(BoundedListNode::new(
            root.create_child("temperature_readings"),
            Self::NUM_INSPECT_TEMPERATURE_SAMPLES,
        ));
        let read_errors = root.create_uint("read_temperature_error_count", 0);
        let last_read_error = root.create_string("last_read_error", "");

        // Pass ownership of the new node to the parent node, otherwise it'll be dropped
        parent.record(root);

        InspectData { temperature_readings, read_errors, last_read_error }
    }

    fn log_temperature_reading(&self, temperature: Celsius) {
        inspect_log!(self.temperature_readings.borrow_mut(), temperature: temperature.0);
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use async_utils::PollExt as _;
    use fuchsia_inspect::testing::TreeAssertion;
    use futures::poll;
    use futures::TryStreamExt;
    use inspect::assert_data_tree;
    use std::task::Poll;

    /// Spawns a new task that acts as a fake temperature driver for testing purposes. Each
    /// GetTemperatureCelsius call responds with a value provided by the supplied `get_temperature`
    /// closure.
    pub fn fake_temperature_driver(
        mut get_temperature: impl FnMut() -> Celsius + 'static,
    ) -> ftemperature::DeviceProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<ftemperature::DeviceMarker>().unwrap();
        fasync::Task::local(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(ftemperature::DeviceRequest::GetTemperatureCelsius { responder }) => {
                        let _ =
                            responder.send(zx::Status::OK.into_raw(), get_temperature().0 as f32);
                    }
                    _ => assert!(false),
                }
            }
        })
        .detach();

        proxy
    }

    /// Tests that the node can handle the 'ReadTemperature' message as expected. The test
    /// checks for the expected temperature value which is returned by the fake temperature driver.
    #[fasync::run_singlethreaded(test)]
    async fn test_read_temperature() {
        // Readings for the fake temperature driver.
        let temperature_readings = vec![1.2, 3.4, 5.6, 7.8, 9.0];

        // Readings piped through the fake driver will be cast to f32 and back to f64.
        let expected_readings: Vec<f64> =
            temperature_readings.iter().map(|x| *x as f32 as f64).collect();

        // Each ReadTemperature request will respond with the next element from
        // `temperature_readings`, wrapping back around when the end of the vector is reached.
        let mut index = 0;
        let get_temperature = move || {
            let value = temperature_readings[index];
            index = (index + 1) % temperature_readings.len();
            Celsius(value)
        };
        let node = TemperatureHandlerBuilder::new()
            .driver_proxy(fake_temperature_driver(get_temperature))
            .build_and_init()
            .await;

        // Send ReadTemperature message and check for expected value.
        for expected_reading in expected_readings {
            let result = node.handle_message(&Message::ReadTemperature).await;
            let temperature = result.unwrap();
            if let MessageReturn::ReadTemperature(t) = temperature {
                assert_eq!(t.0, expected_reading);
            } else {
                assert!(false);
            }
        }
    }

    #[test]
    fn test_caching() -> Result<(), Error> {
        let mut executor = fasync::TestExecutor::new_with_fake_time();
        executor.set_fake_time(Seconds(0.0).into());

        let sensor_temperature = Rc::new(Cell::new(Celsius(0.0)));

        let sensor_temperature_clone = sensor_temperature.clone();
        let get_temperature = move || sensor_temperature_clone.get();
        let node = executor
            .run_until_stalled(&mut Box::pin(
                TemperatureHandlerBuilder::new()
                    .driver_proxy(fake_temperature_driver(get_temperature))
                    .cache_duration(zx::Duration::from_millis(500))
                    .build_and_init(),
            ))
            .unwrap();

        let run = move |executor: &mut fasync::TestExecutor, duration_ms: i64| {
            executor.set_fake_time(executor.now() + zx::Duration::from_millis(duration_ms));

            let poll =
                executor.run_until_stalled(&mut node.handle_message(&Message::ReadTemperature));
            if let Poll::Ready(Ok(MessageReturn::ReadTemperature(temperature))) = poll {
                temperature
            } else {
                panic!("Unexpected poll: {:?}", poll);
            }
        };

        // When advancing longer than the cache duration, we'll always poll the sensor.
        sensor_temperature.set(Celsius(20.0));
        assert_eq!(run(&mut executor, 1000), Celsius(20.0));
        sensor_temperature.set(Celsius(21.0));
        assert_eq!(run(&mut executor, 1000), Celsius(21.0));

        // If insufficient time has passed, we'll see the cached value.
        sensor_temperature.set(Celsius(22.0));
        assert_eq!(run(&mut executor, 200), Celsius(21.0));
        assert_eq!(run(&mut executor, 200), Celsius(21.0));
        assert_eq!(run(&mut executor, 200), Celsius(22.0));

        Ok(())
    }

    /// Tests that an unsupported message is handled gracefully and an error is returned.
    #[fasync::run_singlethreaded(test)]
    async fn test_unsupported_msg() {
        let node = TemperatureHandlerBuilder::new()
            .driver_proxy(fake_temperature_driver(|| Celsius(0.0)))
            .build_and_init()
            .await;
        match node.handle_message(&Message::GetCpuLoads).await {
            Err(PowerManagerError::Unsupported) => {}
            e => panic!("Unexpected return value: {:?}", e),
        }
    }

    /// Tests for the presence and correctness of dynamically-added inspect data
    #[fasync::run_singlethreaded(test)]
    async fn test_inspect_data() {
        let inspector = inspect::Inspector::default();

        let node = TemperatureHandlerBuilder::new()
            .driver_proxy(fake_temperature_driver(|| Celsius(30.0)))
            .inspect_root(inspector.root())
            .build_and_init()
            .await;

        // The node will read the current temperature and log the sample into Inspect. Read enough
        // samples to test that the correct number of samples are logged and older ones are dropped.
        for _ in 0..InspectData::NUM_INSPECT_TEMPERATURE_SAMPLES + 10 {
            node.handle_message(&Message::ReadTemperature).await.unwrap();
        }

        let mut root = TreeAssertion::new("TemperatureHandler (/test/driver/path)", false);
        let mut temperature_readings = TreeAssertion::new("temperature_readings", true);

        // Since we read 10 more samples than our limit allows, the first 10 should be dropped. So
        // test that the sample numbering starts at 10 and continues for the expected number of
        // samples.
        for i in 10..InspectData::NUM_INSPECT_TEMPERATURE_SAMPLES + 10 {
            let mut sample_child = TreeAssertion::new(&i.to_string(), true);
            sample_child.add_property_assertion("temperature", Box::new(30.0));
            sample_child.add_property_assertion("@time", Box::new(inspect::testing::AnyProperty));
            temperature_readings.add_child_assertion(sample_child);
        }

        root.add_child_assertion(temperature_readings);
        assert_data_tree!(inspector, root: { root, });
    }

    /// Tests that well-formed configuration JSON does not panic the `new_from_json` function.
    #[fasync::run_singlethreaded(test)]
    async fn test_new_from_json() {
        let json_data = json::json!({
            "type": "TemperatureHandler",
            "name": "temperature",
            "config": {
                "driver_path": "/dev/class/temperature/000",
                "cache_duration_ms": 1000
            }
        });
        let _ = TemperatureHandlerBuilder::new_from_json(json_data, &HashMap::new());
    }

    /// Tests that the node correctly reports its driver path.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_driver_path() {
        let node = TemperatureHandlerBuilder::new()
            .driver_proxy(fake_temperature_driver(|| Celsius(0.0)))
            .build_and_init()
            .await;

        let driver_path = match node.handle_message(&Message::GetDriverPath).await.unwrap() {
            MessageReturn::GetDriverPath(driver_path) => driver_path,
            e => panic!("Unexpected message response: {:?}", e),
        };

        // "TestTemperatureHandler" is the driver path assigned in
        // `TemperatureHandlerBuilder::new()`
        assert_eq!(driver_path, "/test/driver/path".to_string());
    }

    /// Tests that messages sent to the node are asynchronously blocked until the node's `init()`
    /// has completed.
    #[fasync::run_singlethreaded(test)]
    async fn test_require_init() {
        // Create the node without `init()`
        let node = TemperatureHandlerBuilder::new()
            .driver_proxy(fake_temperature_driver(|| Celsius(0.0)))
            .build()
            .unwrap();

        // Future to send the node a message
        let mut message_future = node.handle_message(&Message::ReadTemperature);

        assert!(poll!(&mut message_future).is_pending());
        assert_matches!(node.init().await, Ok(()));
        assert_matches!(message_future.await, Ok(MessageReturn::ReadTemperature(Celsius(_))));
    }

    /// Tests that the debug temperature override command correctly overrides temperature and can be
    /// cleared as expected.
    #[fasync::run_singlethreaded(test)]
    async fn test_debug_temperature_override() {
        let node = TemperatureHandlerBuilder::new()
            .driver_proxy(fake_temperature_driver(|| Celsius(10.0)))
            .build_and_init()
            .await;

        assert_matches!(
            node.handle_message(&Message::ReadTemperature).await.unwrap(),
            MessageReturn::ReadTemperature(Celsius(t)) if t == 10.0
        );

        assert_matches!(
            node.handle_message(&Message::Debug(
                "set_temperature_override".into(),
                vec!["20.0".into()]
            ))
            .await
            .unwrap(),
            MessageReturn::Debug
        );

        assert_matches!(
            node.handle_message(&Message::ReadTemperature).await.unwrap(),
            MessageReturn::ReadTemperature(Celsius(t)) if t == 20.0
        );

        assert_matches!(
            node.handle_message(&Message::Debug("clear_temperature_override".into(), vec![]))
                .await
                .unwrap(),
            MessageReturn::Debug
        );

        assert_matches!(
            node.handle_message(&Message::ReadTemperature).await.unwrap(),
            MessageReturn::ReadTemperature(Celsius(t)) if t == 10.0
        );
    }
}

/// Contains both the raw and filtered temperature values returned from the TemperatureFilter
/// `get_temperature` function.
#[derive(PartialEq, Debug)]
pub struct TemperatureReadings {
    pub raw: Celsius,
    pub filtered: Celsius,
}

/// Wrapper for reading and filtering temperature samples from a TemperatureHandler node.
pub struct TemperatureFilter {
    /// Filter time constant. A value of 0 effectively disables filtering.
    time_constant: Seconds,

    /// Previous sample temperature
    prev_temperature: Cell<Option<Celsius>>,

    /// Previous sample timestamp
    prev_timestamp: Cell<Nanoseconds>,

    /// TemperatureHandler node that is used to read temperature
    temperature_handler: Rc<dyn Node>,
}

impl TemperatureFilter {
    /// Constucts a new TemperatureFilter with the specified TemperatureHandler node and filter time
    /// constant.
    pub fn new(temperature_handler: Rc<dyn Node>, time_constant: Seconds) -> Self {
        Self {
            time_constant,
            prev_temperature: Cell::new(None),
            prev_timestamp: Cell::new(Nanoseconds(0)),
            temperature_handler,
        }
    }

    /// Reads a new temperature sample and returns a Temperature instance containing both the raw
    /// and filtered temperature values.
    pub async fn get_temperature(
        &self,
        timestamp: Nanoseconds,
    ) -> Result<TemperatureReadings, Error> {
        fuchsia_trace::duration!("power_manager", "TemperatureFilter::get_temperature");

        let raw_temperature = self.read_temperature().await?;
        let filtered_temperature = match self.prev_temperature.get() {
            Some(prev_temperature) => Self::low_pass_filter(
                raw_temperature,
                prev_temperature,
                (timestamp - self.prev_timestamp.get()).into(),
                self.time_constant,
            ),
            None => raw_temperature,
        };

        self.prev_temperature.set(Some(filtered_temperature));
        self.prev_timestamp.set(timestamp);

        Ok(TemperatureReadings { raw: raw_temperature, filtered: filtered_temperature })
    }

    /// Queries the current temperature from the temperature handler node
    async fn read_temperature(&self) -> Result<Celsius, Error> {
        fuchsia_trace::duration!("power_manager", "TemperatureFilter::read_temperature");
        match self.temperature_handler.handle_message(&Message::ReadTemperature).await {
            Ok(MessageReturn::ReadTemperature(t)) => Ok(t),
            Ok(r) => Err(format_err!("ReadTemperature had unexpected return value: {:?}", r)),
            Err(e) => Err(format_err!("ReadTemperature failed: {:?}", e)),
        }
    }

    /// Filters the input temperature value using the specified previous temperature value `y_prev`,
    /// `time_delta`, and `time_constant`.
    fn low_pass_filter(
        y: Celsius,
        y_prev: Celsius,
        time_delta: Seconds,
        time_constant: Seconds,
    ) -> Celsius {
        if time_constant == Seconds(0.0) {
            y
        } else {
            Celsius(y_prev.0 + (time_delta.0 / time_constant.0) * (y.0 - y_prev.0))
        }
    }
}

#[cfg(test)]
mod temperature_filter_tests {
    use super::*;
    use crate::test::mock_node::{MessageMatcher, MockNodeMaker};
    use crate::{msg_eq, msg_ok_return};
    use fuchsia_async as fasync;

    /// Tests the low_pass_filter function for correctness.
    #[test]
    fn test_low_pass_filter() {
        let y_0 = Celsius(0.0);
        let y_1 = Celsius(10.0);
        let time_delta = Seconds(1.0);
        let time_constant = Seconds(10.0);

        assert_eq!(
            TemperatureFilter::low_pass_filter(y_1, y_0, time_delta, time_constant),
            Celsius(1.0)
        );

        // Filter constant of 0 is valid and should be treated as "no filtering"
        assert_eq!(TemperatureFilter::low_pass_filter(y_1, y_0, time_delta, Seconds(0.0)), y_1);
    }

    /// Tests that the TemperatureFilter `get_temperature` function queries the TemperatureHandler
    /// node and returns the expected raw and filtered temperature values.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_temperature() {
        let mut mock_maker = MockNodeMaker::new();
        let temperature_node = mock_maker.make(
            "Temperature",
            vec![
                (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(50.0)))),
                (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(80.0)))),
            ],
        );

        // Create a TemperatureFilter instance using 5 seconds as the filter constant
        let filter = TemperatureFilter::new(temperature_node, Seconds(5.0));

        // The first reading should return identical raw/filtered values
        assert_eq!(
            filter.get_temperature(Nanoseconds(0)).await.unwrap(),
            TemperatureReadings { raw: Celsius(50.0), filtered: Celsius(50.0) }
        );

        // The next reading should return the raw value (80C) along with the calculated filtered
        // value for the given elapsed time (1 second)
        assert_eq!(
            filter.get_temperature(Seconds(1.0).into()).await.unwrap(),
            TemperatureReadings { raw: Celsius(80.0), filtered: Celsius(56.0) }
        );
    }
}
