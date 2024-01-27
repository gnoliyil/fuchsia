// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;

use anyhow::Context as _;
use fidl::endpoints::{ControlHandle as _, RequestStream as _};
use fidl_fuchsia_factory_lowpan::{
    FactoryDriverMarker, FactoryDriverProxy, FactoryLookupRequest, FactoryLookupRequestStream,
    FactoryRegisterRequest, FactoryRegisterRequestStream,
};
use fidl_fuchsia_lowpan_driver::{
    DriverMarker, DriverProxy, Protocols, RegisterRequest, RegisterRequestStream,
};
use fuchsia_syslog::macros::*;
use futures::prelude::*;
use futures::task::{Spawn, SpawnExt};
use lowpan_driver_common::lowpan_fidl::{
    CountersConnectorRequest, CountersConnectorRequestStream, DatasetConnectorRequest,
    DatasetConnectorRequestStream, DeviceConnectorRequest, DeviceConnectorRequestStream,
    DeviceExtraConnectorRequest, DeviceExtraConnectorRequestStream, DeviceRouteConnectorRequest,
    DeviceRouteConnectorRequestStream, DeviceRouteExtraConnectorRequest,
    DeviceRouteExtraConnectorRequestStream, DeviceTestConnectorRequest,
    DeviceTestConnectorRequestStream, DeviceWatcherRequest, DeviceWatcherRequestStream,
    EnergyScanConnectorRequest, EnergyScanConnectorRequestStream,
    ExperimentalDeviceConnectorRequest, ExperimentalDeviceConnectorRequestStream,
    ExperimentalDeviceExtraConnectorRequest, ExperimentalDeviceExtraConnectorRequestStream,
    FeatureConnectorRequestStream, LegacyJoiningConnectorRequest,
    LegacyJoiningConnectorRequestStream, MeshcopConnectorRequest, MeshcopConnectorRequestStream,
    TelemetryProviderConnectorRequest, TelemetryProviderConnectorRequestStream, MAX_LOWPAN_DEVICES,
};
use lowpan_driver_common::{AsyncCondition, ZxStatus};
use parking_lot::Mutex;
use regex::Regex;
use std::collections::HashMap;
use std::sync::Arc;

lazy_static::lazy_static! {
    static ref DEVICE_NAME_REGEX: Regex = Regex::new("^[a-z_][-_.+0-9a-z]{1,31}$")
        .expect("Device name regex failed to compile");
}

pub struct LowpanService<S> {
    pub devices: Arc<Mutex<HashMap<String, DriverProxy>>>,
    pub devices_factory: Arc<Mutex<HashMap<String, FactoryDriverProxy>>>,
    pub added_removed_cond: Arc<AsyncCondition>,
    pub spawner: S,
}

impl<S: Spawn> LowpanService<S> {
    pub fn with_spawner(spawner: S) -> LowpanService<S> {
        LowpanService {
            devices: Default::default(),
            devices_factory: Default::default(),
            added_removed_cond: Default::default(),
            spawner,
        }
    }
}

impl<S> LowpanService<S> {
    pub fn lookup(&self, name: &str) -> Result<DriverProxy, ZxStatus> {
        let devices = self.devices.lock();
        if let Some(device) = devices.get(name) {
            Ok(device.clone())
        } else {
            Err(ZxStatus::NOT_FOUND)
        }
    }

    pub fn get_devices(&self) -> Vec<String> {
        let devices = self.devices.lock();
        devices.keys().into_iter().map(|x| x.to_string()).collect()
    }
}

impl<S: Spawn> LowpanService<S> {
    pub fn register(
        &self,
        name: &str,
        driver: fidl::endpoints::ClientEnd<DriverMarker>,
    ) -> Result<(), ZxStatus> {
        let driver = driver.into_proxy().map_err(|_| ZxStatus::INVALID_ARGS)?;

        if !DEVICE_NAME_REGEX.is_match(name) {
            fx_log_err!("Attempted to register LoWPAN device with invalid name {:?}", name);
            return Err(ZxStatus::INVALID_ARGS);
        }

        let name = name.to_string();

        {
            // Lock the device list.
            let mut devices = self.devices.lock();

            // Check to make sure there already aren't too many devices.
            if devices.len() >= MAX_LOWPAN_DEVICES as usize {
                return Err(ZxStatus::NO_RESOURCES);
            }

            // Check for existing devices with the same name.
            if devices.contains_key(&name) {
                return Err(ZxStatus::ALREADY_EXISTS);
            }

            // Insert the new device into the list.
            devices.insert(name.clone(), driver.clone());
        }

        // Indicate that a new device was added.
        self.added_removed_cond.trigger();

        let devices = self.devices.clone();
        let devices_factory = self.devices_factory.clone();
        let added_removed_cond = self.added_removed_cond.clone();

        // The following code provides a way to automatically
        // remove a device when the connection to the LoWPAN Driver
        // is lost.
        let cleanup_task = driver
            .take_event_stream()
            .for_each(|_| futures::future::ready(()))
            .inspect(move |_: &()| {
                fx_log_info!("Removing device {:?}", &name);

                devices.lock().remove(&name);
                devices_factory.lock().remove(&name);

                // Indicate that the device was removed.
                added_removed_cond.trigger();
            });

        self.spawner.spawn(cleanup_task).expect("Unable to spawn cleanup task");

        Ok(())
    }
}

macro_rules! impl_serve_to_driver {
    ($request_stream:ty, $request:ident, $protocol_member:ident) => {
        #[async_trait::async_trait()]
        impl<S: Sync> ServeTo<$request_stream> for LowpanService<S> {
            async fn serve_to(&self, request_stream: $request_stream) -> anyhow::Result<()> {
                request_stream
                    .err_into::<Error>()
                    .try_for_each_concurrent(MAX_CONCURRENT, |command| async {
                        match command {
                            $request::Connect { name, server_end, .. } => {
                                if let Err(err) = match self.lookup(&name) {
                                    Ok(dev) => dev.get_protocols(Protocols {
                                        $protocol_member: Some(server_end),
                                        ..Protocols::EMPTY
                                    }),
                                    Err(err) => server_end.close_with_epitaph(err.into()),
                                } {
                                    fx_log_warn!("{:?}", err);
                                }
                            }
                        }
                        Result::<(), anyhow::Error>::Ok(())
                    })
                    .inspect_err(|e| fx_log_err!("{:?}", e))
                    .await
            }
        }
    };
}

impl_serve_to_driver!(DeviceConnectorRequestStream, DeviceConnectorRequest, device);
impl_serve_to_driver!(DeviceExtraConnectorRequestStream, DeviceExtraConnectorRequest, device_extra);
impl_serve_to_driver!(DeviceRouteConnectorRequestStream, DeviceRouteConnectorRequest, device_route);
impl_serve_to_driver!(
    DeviceRouteExtraConnectorRequestStream,
    DeviceRouteExtraConnectorRequest,
    device_route_extra
);
impl_serve_to_driver!(CountersConnectorRequestStream, CountersConnectorRequest, counters);
impl_serve_to_driver!(DeviceTestConnectorRequestStream, DeviceTestConnectorRequest, device_test);
impl_serve_to_driver!(DatasetConnectorRequestStream, DatasetConnectorRequest, thread_dataset);
impl_serve_to_driver!(
    LegacyJoiningConnectorRequestStream,
    LegacyJoiningConnectorRequest,
    thread_legacy_joining
);
impl_serve_to_driver!(MeshcopConnectorRequestStream, MeshcopConnectorRequest, meshcop);
impl_serve_to_driver!(EnergyScanConnectorRequestStream, EnergyScanConnectorRequest, energy_scan);

impl_serve_to_driver!(
    ExperimentalDeviceConnectorRequestStream,
    ExperimentalDeviceConnectorRequest,
    experimental_device
);
impl_serve_to_driver!(
    ExperimentalDeviceExtraConnectorRequestStream,
    ExperimentalDeviceExtraConnectorRequest,
    experimental_device_extra
);
impl_serve_to_driver!(
    TelemetryProviderConnectorRequestStream,
    TelemetryProviderConnectorRequest,
    telemetry_provider
);
impl_serve_to_driver!(FeatureConnectorRequestStream, FeatureConnectorRequest, thread_feature);

#[async_trait::async_trait()]
impl<S: Sync> ServeTo<DeviceWatcherRequestStream> for LowpanService<S> {
    async fn serve_to(&self, request_stream: DeviceWatcherRequestStream) -> anyhow::Result<()> {
        use futures::lock::Mutex;
        let last_device_list: Mutex<Option<Vec<String>>> = Mutex::new(None);

        request_stream
            .err_into::<Error>()
            .try_for_each_concurrent(MAX_CONCURRENT, |command| async {
                match command {
                    DeviceWatcherRequest::WatchDevices { responder } => {
                        let mut locked_device_list =
                            last_device_list.try_lock().ok_or(format_err!(
                                "No more than 1 outstanding call to watch_devices is allowed"
                            ))?;

                        if locked_device_list.is_none() {
                            // This is the first call to WatchDevices,
                            // so we return the whole list.

                            *locked_device_list = Some(self.get_devices());

                            let (added, removed) = (locked_device_list.clone().unwrap(), vec![]);
                            responder
                                .send(
                                    &mut added.iter().map(String::as_str),
                                    &mut removed.iter().map(String::as_str),
                                )
                                .context("error sending response")?;
                        } else {
                            // This is a follow-up call.
                            let current_devices = loop {
                                let wait = self.added_removed_cond.wait();

                                let current_devices = self.get_devices();

                                // Note that this should work even though the returned
                                // list of interfaces isn't sorted. As long as the
                                // keys aren't intentionally shuffled when nothing
                                // has changed then this check should work just fine.
                                if current_devices != *locked_device_list.as_ref().unwrap() {
                                    break current_devices;
                                }

                                // We wait here for something to change.
                                wait.await;
                            };

                            // Devices have been added or removed, let's sort them out.

                            // Calculate devices added.
                            // This mechanism is O(n^2), but in reality n is going to
                            // almost always be 1---so it makes sense to prioritize
                            // convenience. It may even be slower to try to optimize
                            // this.
                            let added = current_devices
                                .iter()
                                .filter_map(|name| {
                                    if !locked_device_list.as_ref().unwrap().contains(name) {
                                        Some(name.clone())
                                    } else {
                                        None
                                    }
                                })
                                .collect::<Vec<_>>();

                            // Calculate devices removed.
                            // This mechanism is O(n^2), but in reality n is going to
                            // almost always be 1---so it makes sense to prioritize
                            // convenience. It may even be slower to try to optimize
                            // this.
                            let removed = locked_device_list
                                .as_ref()
                                .unwrap()
                                .iter()
                                .filter_map(|name| {
                                    if !current_devices.contains(name) {
                                        Some(name.clone())
                                    } else {
                                        None
                                    }
                                })
                                .collect::<Vec<_>>();

                            // Save our current list of devices so that we
                            // can use it the next time this method is called.
                            *locked_device_list = Some(current_devices);

                            responder
                                .send(
                                    &mut added.iter().map(String::as_str),
                                    &mut removed.iter().map(String::as_str),
                                )
                                .context("error sending response")?;
                        }
                    }
                }
                Result::<(), anyhow::Error>::Ok(())
            })
            .inspect_err(|e| fx_log_err!("{:?}", e))
            .await
    }
}

#[async_trait::async_trait()]
impl<S: Spawn + Sync> ServeTo<RegisterRequestStream> for LowpanService<S> {
    async fn serve_to(&self, request_stream: RegisterRequestStream) -> anyhow::Result<()> {
        let control_handle = request_stream.control_handle();
        request_stream
            .err_into::<Error>()
            .try_for_each_concurrent(MAX_CONCURRENT, |command| async {
                match command {
                    RegisterRequest::RegisterDevice { name, driver, .. } => {
                        fx_log_info!("Received register request for {:?}", name);

                        if let Err(err) = self.register(&name, driver) {
                            control_handle.shutdown_with_epitaph(err);
                        }
                    }
                }
                Result::<(), anyhow::Error>::Ok(())
            })
            .inspect_err(|e| fx_log_err!("{:?}", e))
            .await
    }
}

mod factory {
    use super::*;

    impl<S: Spawn + Sync> LowpanService<S> {
        pub fn lookup_factory(&self, name: &str) -> Result<FactoryDriverProxy, ZxStatus> {
            let devices = self.devices_factory.lock();
            if let Some(device) = devices.get(name) {
                Ok(device.clone())
            } else {
                Err(ZxStatus::NOT_FOUND)
            }
        }

        pub fn register_factory(
            &self,
            name: &str,
            driver: fidl::endpoints::ClientEnd<FactoryDriverMarker>,
        ) -> Result<(), ZxStatus> {
            let driver = driver.into_proxy().map_err(|_| ZxStatus::INVALID_ARGS)?;

            if !DEVICE_NAME_REGEX.is_match(name) {
                fx_log_err!("Attempted to register LoWPAN device with invalid name {:?}", name);
                return Err(ZxStatus::INVALID_ARGS);
            }

            let name = name.to_string();

            // Lock the device list.
            let mut devices = self.devices_factory.lock();

            // Check to make sure there already aren't too many devices.
            if devices.len() >= MAX_LOWPAN_DEVICES as usize {
                return Err(ZxStatus::NO_RESOURCES);
            }

            // Check for existing devices with the same name.
            if devices.contains_key(&name) {
                return Err(ZxStatus::ALREADY_EXISTS);
            }

            // Insert the new device into the list.
            devices.insert(name.clone(), driver.clone());

            Ok(())
        }
    }

    #[async_trait::async_trait()]
    impl<S: Spawn + Sync> ServeTo<FactoryLookupRequestStream> for LowpanService<S> {
        async fn serve_to(&self, request_stream: FactoryLookupRequestStream) -> anyhow::Result<()> {
            request_stream
                .err_into::<Error>()
                .try_for_each_concurrent(MAX_CONCURRENT, |command| async {
                    match command {
                        FactoryLookupRequest::Lookup { name, device_factory, .. } => {
                            fx_log_info!("Received lookup factory request for {:?}", name);

                            if let Err(err) = match self.lookup_factory(&name) {
                                Ok(dev) => dev.get_factory_device(device_factory),
                                Err(err) => device_factory.close_with_epitaph(err.into()),
                            } {
                                fx_log_err!("{:?}", err);
                            }
                        }
                    }
                    Result::<(), anyhow::Error>::Ok(())
                })
                .inspect_err(|e| fx_log_err!("{:?}", e))
                .await
        }
    }

    #[async_trait::async_trait()]
    impl<S: Spawn + Sync> ServeTo<FactoryRegisterRequestStream> for LowpanService<S> {
        async fn serve_to(
            &self,
            request_stream: FactoryRegisterRequestStream,
        ) -> anyhow::Result<()> {
            let control_handle = request_stream.control_handle();
            request_stream
                .err_into::<Error>()
                .try_for_each_concurrent(MAX_CONCURRENT, |command| async {
                    match command {
                        FactoryRegisterRequest::Register { name, driver, .. } => {
                            fx_log_info!("Received register factory request for {:?}", name);

                            if let Err(err) = self.register_factory(&name, driver) {
                                control_handle.shutdown_with_epitaph(err);
                            }
                        }
                    }
                    Result::<(), anyhow::Error>::Ok(())
                })
                .inspect_err(|e| fx_log_err!("{:?}", e))
                .await
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl::endpoints::create_endpoints;
    use fuchsia_async as fasync;

    #[fasync::run_until_stalled(test)]
    async fn test_interface_name_check() {
        let service = LowpanService::with_spawner(FuchsiaGlobalExecutor);

        let (client_ep, _) = create_endpoints::<DriverMarker>().unwrap();
        assert_eq!(service.register("lowpan0", client_ep), Ok(()));

        let (client_ep, _) = create_endpoints::<DriverMarker>().unwrap();
        assert_eq!(service.register("low pan 0", client_ep), Err(ZxStatus::INVALID_ARGS));

        let (client_ep, _) = create_endpoints::<DriverMarker>().unwrap();
        assert_eq!(service.register("0lowpan", client_ep), Err(ZxStatus::INVALID_ARGS));

        let (client_ep, _) = create_endpoints::<DriverMarker>().unwrap();
        assert_eq!(service.register("l", client_ep), Err(ZxStatus::INVALID_ARGS));
    }

    #[fasync::run_until_stalled(test)]
    async fn test_factory_interface() {
        let service = LowpanService::with_spawner(FuchsiaGlobalExecutor);

        let (client_ep, _) = create_endpoints::<FactoryDriverMarker>().unwrap();
        assert_eq!(service.register_factory("lowpan0", client_ep), Ok(()));
    }

    #[fasync::run_until_stalled(test)]
    async fn test_interface_added_notifications() {
        let service = LowpanService::with_spawner(FuchsiaGlobalExecutor);

        let waiter = service.added_removed_cond.wait();

        let (driver_client_ep, _) = create_endpoints::<DriverMarker>().unwrap();
        assert_eq!(service.register("lowpan0", driver_client_ep), Ok(()));

        waiter.await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_interface_removed_notifications() {
        let service = LowpanService::with_spawner(FuchsiaGlobalExecutor);

        let mut waiter = service.added_removed_cond.wait();
        {
            let (driver_client_ep, _) = create_endpoints::<DriverMarker>().unwrap();
            assert_eq!(service.register("lowpan0", driver_client_ep), Ok(()));

            waiter.await;

            waiter = service.added_removed_cond.wait();
        }
        waiter.await;
    }
}
