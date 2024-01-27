// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::types::{ConnectivityState, MacAddressFilterSettingsDto, NeighborInfoDto};
use crate::common_utils::lowpan_context::LowpanContext;
use anyhow::Error;
use fidl_fuchsia_lowpan_device::ConnectivityState as lowpan_ConnectivityState;
use fidl_fuchsia_lowpan_device::{DeviceExtraProxy, DeviceProxy};
use fidl_fuchsia_lowpan_test::DeviceTestProxy;
use parking_lot::{RwLock, RwLockUpgradableReadGuard};

/// Perform Wpan FIDL operations.
///
/// Note this object is shared among all threads created by server.
#[derive(Debug)]
pub struct WpanFacade {
    /// The proxy to access the lowpan Device service.
    device: RwLock<Option<DeviceProxy>>,
    /// The proxy to access the lowpan DeviceTest service.
    device_test: RwLock<Option<DeviceTestProxy>>,
    /// The proxy to access the lowpan DeviceExtra service.
    device_extra: RwLock<Option<DeviceExtraProxy>>,
}

impl WpanFacade {
    pub fn new() -> WpanFacade {
        WpanFacade {
            device: RwLock::new(None),
            device_test: RwLock::new(None),
            device_extra: RwLock::new(None),
        }
    }

    /// Returns the DeviceTestManager proxy provided on instantiation
    /// or establishes a new connection.
    pub async fn initialize_proxies(&self) -> Result<(), Error> {
        let (device, device_extra, device_test) = match LowpanContext::new(None) {
            Ok(low_pan_context) => low_pan_context.get_default_device_proxies().await?,
            _ => bail!("Error retrieving default device proxies"),
        };
        let device_rw = self.device.upgradable_read();
        let device_extra_rw = self.device_extra.upgradable_read();
        let device_test_rw = self.device_test.upgradable_read();
        *RwLockUpgradableReadGuard::upgrade(device_rw) = Some(device.clone());
        *RwLockUpgradableReadGuard::upgrade(device_extra_rw) = Some(device_extra.clone());
        *RwLockUpgradableReadGuard::upgrade(device_test_rw) = Some(device_test.clone());
        Ok(())
    }

    /// Returns the thread rloc from the DeviceTest proxy service.
    pub async fn get_thread_rloc16(&self) -> Result<u16, Error> {
        let thread_rloc16 = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_thread_rloc16().await?,
            _ => bail!("DeviceTest proxy is not set"),
        };
        Ok(thread_rloc16)
    }

    /// Returns the current mac address (thread random mac address) from the DeviceTest
    /// proxy service.
    pub async fn get_ncp_mac_address(&self) -> Result<[u8; 8], Error> {
        let current_mac_address = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_current_mac_address().await?,
            _ => bail!("DeviceTest proxy is not set"),
        };
        Ok(current_mac_address.octets)
    }

    /// Returns the ncp channel from the DeviceTest proxy service.
    pub async fn get_ncp_channel(&self) -> Result<u16, Error> {
        let current_channel = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_current_channel().await?,
            _ => bail!("DeviceTest proxy is not set"),
        };
        Ok(current_channel)
    }

    /// Returns the current rssi from the DeviceTest proxy service.
    pub async fn get_ncp_rssi(&self) -> Result<i32, Error> {
        let ncp_rssi = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_current_rssi().await?,
            _ => bail!("DeviceTest proxy is not set"),
        };
        Ok(ncp_rssi.into())
    }

    /// Returns the factory mac address from the DeviceTest proxy service.
    pub async fn get_weave_node_id(&self) -> Result<[u8; 8], Error> {
        let factory_mac_address = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_factory_mac_address().await?,
            _ => bail!("DeviceTest proxy is not set"),
        };
        Ok(factory_mac_address.octets)
    }

    /// Returns the network name from the DeviceExtra proxy service.
    pub async fn get_network_name(&self) -> Result<Vec<u8>, Error> {
        let raw_name = match self.device_extra.read().as_ref() {
            Some(device_extra) => device_extra.watch_identity().await?.raw_name,
            _ => bail!("DeviceExtra proxy is not set"),
        };
        match raw_name {
            Some(raw_name) => Ok(raw_name),
            None => bail!("Network name is not specified!"),
        }
    }

    /// Returns the partition id from the DeviceTest proxy service.
    pub async fn get_partition_id(&self) -> Result<u32, Error> {
        let partition_id = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_partition_id().await?,
            _ => bail!("DeviceTest proxy is not set"),
        };
        Ok(partition_id)
    }

    /// Returns the thread router id from the DeviceTest proxy service.
    pub async fn get_thread_router_id(&self) -> Result<u8, Error> {
        let router_id = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_thread_router_id().await?,
            _ => bail!("DeviceTest proxy is not set"),
        };
        Ok(router_id)
    }

    /// Returns the connectivity state from the DeviceTest proxy service.
    pub async fn get_ncp_state(&self) -> Result<ConnectivityState, Error> {
        let device_state = match self.device.read().as_ref() {
            Some(device) => device.watch_device_state().await?.connectivity_state,
            _ => bail!("DeviceTest proxy is not set"),
        };
        match device_state {
            Some(connectivity_state) => Ok(WpanFacade::to_connectivity_state(connectivity_state)),
            None => bail!("Device state is not defined!"),
        }
    }

    /// Returns true if the connectivity state is commissioned.
    pub async fn get_is_commissioned(&self) -> Result<bool, Error> {
        let ncp_state = self.get_ncp_state().await?;
        let is_commissioned = match ncp_state {
            ConnectivityState::Attached
            | ConnectivityState::Attaching
            | ConnectivityState::Isolated
            | ConnectivityState::Ready => true,
            _ => false,
        };
        Ok(is_commissioned)
    }

    /// Returns the panid from the DeviceExtra proxy service.
    pub async fn get_panid(&self) -> Result<u16, Error> {
        match self.device_extra.read().as_ref() {
            Some(device_extra) => match device_extra.watch_identity().await?.panid {
                Some(panid) => Ok(panid),
                None => bail!("Pan id is not specified!"),
            },
            _ => bail!("DeviceExtra proxy is not set"),
        }
    }

    /// Returns the mac address filter settings from the DeviceTest proxy service.
    pub async fn get_mac_address_filter_settings(
        &self,
    ) -> Result<MacAddressFilterSettingsDto, Error> {
        let settings = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_mac_address_filter_settings().await?,
            _ => bail!("DeviceTest proxy is not set!"),
        };
        Ok(settings.into())
    }

    /// Replaces the mac address filter settings on the DeviceTest proxy service.
    pub async fn replace_mac_address_filter_settings(
        &self,
        settings: MacAddressFilterSettingsDto,
    ) -> Result<(), Error> {
        match self.device_test.read().as_ref() {
            Some(device_test) => {
                device_test.replace_mac_address_filter_settings(settings.into()).await?
            }
            None => bail!("DeviceTest proxy is not set!"),
        }
        Ok(())
    }

    ///Returns the thread neighbor table from the DeviceTest proxy service.
    pub async fn get_neighbor_table(&self) -> Result<Vec<NeighborInfoDto>, Error> {
        let settings = match self.device_test.read().as_ref() {
            Some(device_test) => device_test.get_neighbor_table().await?,
            _ => bail!("DeviceTest proxy is not set!"),
        };
        Ok(settings.into_iter().map(|setting| setting.into()).collect())
    }

    fn to_connectivity_state(connectivity_state: lowpan_ConnectivityState) -> ConnectivityState {
        match connectivity_state {
            lowpan_ConnectivityState::Inactive => ConnectivityState::Inactive,
            lowpan_ConnectivityState::Ready => ConnectivityState::Ready,
            lowpan_ConnectivityState::Offline => ConnectivityState::Offline,
            lowpan_ConnectivityState::Attaching => ConnectivityState::Attaching,
            lowpan_ConnectivityState::Attached => ConnectivityState::Attached,
            lowpan_ConnectivityState::Isolated => ConnectivityState::Isolated,
            lowpan_ConnectivityState::Commissioning => ConnectivityState::Commissioning,
            _ => ConnectivityState::Unknown,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wpan::types::{MacAddressFilterItemDto, MacAddressFilterModeDto};
    use fidl::endpoints::ProtocolMarker;
    use fidl_fuchsia_lowpan_device::{DeviceExtraMarker, DeviceMarker};
    use fidl_fuchsia_lowpan_test::DeviceTestMarker;
    use fuchsia_async as fasync;
    use futures::prelude::*;
    use lazy_static::lazy_static;
    use lowpan_driver_common::DummyDevice;
    use lowpan_driver_common::ServeTo;

    lazy_static! {
        static ref MOCK_TESTER: MockTester = MockTester::new();
    }

    struct MockTester {
        dummy_device: DummyDevice,
    }
    impl MockTester {
        fn new() -> Self {
            Self { dummy_device: DummyDevice::default() }
        }

        fn create_endpoints<T: ProtocolMarker>() -> (RwLock<Option<T::Proxy>>, T::RequestStream) {
            let (client_ep, server_ep) = fidl::endpoints::create_endpoints::<T>();
            (RwLock::new(Some(client_ep.into_proxy().unwrap())), server_ep.into_stream().unwrap())
        }

        pub fn create_facade_and_serve(
            &'static self,
        ) -> (
            WpanFacade,
            (
                impl Future<Output = anyhow::Result<()>>,
                impl Future<Output = anyhow::Result<()>>,
                impl Future<Output = anyhow::Result<()>>,
            ),
        ) {
            let (device_proxy, device_server) = MockTester::create_endpoints::<DeviceMarker>();
            let (device_test_proxy, device_test_server) =
                MockTester::create_endpoints::<DeviceTestMarker>();
            let (device_extra_proxy, device_extra_server) =
                MockTester::create_endpoints::<DeviceExtraMarker>();

            let facade = WpanFacade {
                device: device_proxy,
                device_test: device_test_proxy,
                device_extra: device_extra_proxy,
            };

            (
                facade,
                (
                    self.dummy_device.serve_to(device_server),
                    self.dummy_device.serve_to(device_test_server),
                    self.dummy_device.serve_to(device_extra_server),
                ),
            )
        }

        pub async fn assert_wpan_fn<TResult>(
            func: impl Future<Output = Result<TResult, Error>>,
            server_future: (
                impl Future<Output = anyhow::Result<()>>,
                impl Future<Output = anyhow::Result<()>>,
                impl Future<Output = anyhow::Result<()>>,
            ),
        ) {
            let facade_fut = async move {
                let awaiting = func.await;
                awaiting.expect("No value returned!");
            };
            futures::select! {
                err = server_future.0.fuse() => panic!("Server task stopped: {:?}", err),
                err = server_future.1.fuse() => panic!("Server task stopped: {:?}", err),
                err = server_future.2.fuse() => panic!("Server task stopped: {:?}", err),
                _ = facade_fut.fuse() => (),
            }
        }
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_thread_rloc16() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_thread_rloc16(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_ncp_channel() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_ncp_channel(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_ncp_mac_address() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_ncp_mac_address(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_ncp_rssi() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_ncp_rssi(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_weave_node_id() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_weave_node_id(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_network_name() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_network_name(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_partition_id() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_partition_id(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_thread_router_id() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_thread_router_id(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_ncp_state() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_ncp_state(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_is_commissioned() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_is_commissioned(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_panid() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_panid(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_mac_address_filter_settings() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_mac_address_filter_settings(), facade.1).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_replace_mac_address_filter_settings() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(
            facade.0.replace_mac_address_filter_settings(MacAddressFilterSettingsDto {
                mode: Some(MacAddressFilterModeDto::Allow),
                items: Some(vec![MacAddressFilterItemDto {
                    mac_address: Some([0, 1, 2, 3, 4, 5, 6, 7]),
                    rssi: None,
                }]),
            }),
            facade.1,
        )
        .await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_get_neigbor_table() {
        let facade = MOCK_TESTER.create_facade_and_serve();
        MockTester::assert_wpan_fn(facade.0.get_neighbor_table(), facade.1).await;
    }
}
