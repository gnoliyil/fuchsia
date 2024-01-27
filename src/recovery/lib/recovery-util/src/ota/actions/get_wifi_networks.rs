// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::ota::controller::SendEvent;
use crate::ota::state_machine::Event;
use crate::wlan::{WifiConnect, WifiConnectImpl};
use fuchsia_async::Task;

pub struct GetWifiNetworksAction {}

impl GetWifiNetworksAction {
    pub fn run(event_sender: Box<dyn SendEvent>) {
        // This is split into two parts for testing. It allowing a fake wifi service to be injected.
        Self::run_with_wifi_service(event_sender, Box::new(WifiConnectImpl::new()));
    }

    // Gets a list of local WiFi networks.
    // Asynchronously generates a Networks event which could be empty if there is an error
    fn run_with_wifi_service(
        mut event_sender: Box<dyn SendEvent>,
        wifi_service: Box<dyn WifiConnect>,
    ) {
        let task = async move {
            println!("Getting networks");
            let networks = wifi_service.scan_for_networks().await.unwrap_or_else(|_| Vec::new());
            #[cfg(feature = "debug_logging")]
            println! {"====== Received {:?}", networks};
            event_sender.send(Event::Networks(networks));
        };
        Task::local(task).detach();
    }
}

#[cfg(test)]
mod tests {
    use super::GetWifiNetworksAction;
    use crate::ota::controller::{MockSendEvent, SendEvent};
    use crate::ota::state_machine::{Event, NetworkInfos};
    use crate::wlan::{NetworkInfo, WifiConnect};
    use anyhow::{anyhow, Error};
    use async_trait::async_trait;
    use fidl_fuchsia_wlan_policy::{NetworkConfig, SecurityType};
    use fuchsia_async::{self as fasync};
    use futures::future;

    fn create_test_networks() -> Vec<NetworkInfo> {
        let info0 =
            NetworkInfo { ssid: "info0".to_string(), rssi: 10, security_type: SecurityType::None };

        let info1 =
            NetworkInfo { ssid: "info1".to_string(), rssi: 20, security_type: SecurityType::Wpa2 };

        let info2 =
            NetworkInfo { ssid: "info2".to_string(), rssi: 30, security_type: SecurityType::Wpa3 };

        vec![info0, info1, info2]
    }

    struct FakeWifiConnectImpl {
        scan_returns_networks: bool,
        networks: NetworkInfos,
    }

    impl FakeWifiConnectImpl {
        fn new(scan_returns_networks: bool, networks: NetworkInfos) -> Self {
            Self { scan_returns_networks, networks }
        }
    }

    #[async_trait(? Send)]
    impl WifiConnect for FakeWifiConnectImpl {
        async fn scan_for_networks(&self) -> Result<NetworkInfos, Error> {
            if self.scan_returns_networks {
                Ok(self.networks.clone())
            } else {
                Err(anyhow!("Failed"))
            }
        }

        // Not used in these tests
        async fn connect(&self, _network: NetworkConfig) -> Result<(), Error> {
            Err(Error::msg("This should not be called"))
        }
    }

    #[fuchsia::test]
    fn test_get_networks_populated() {
        let mut exec = fasync::TestExecutor::new();
        let infos = create_test_networks();
        let infos_check = create_test_networks();
        let infos_len = infos.len();
        let mut event_sender = MockSendEvent::new();
        event_sender
            .expect_send()
            .withf(move |event| {
                if let Event::Networks(received_infos) = event {
                    received_infos.len() == infos_len
                        && received_infos
                            .iter()
                            .zip(infos_check.iter())
                            .filter(|&(a, b)| a == b)
                            .count()
                            == infos_len
                } else {
                    false
                }
            })
            .times(1)
            .return_const(());
        let event_sender: Box<dyn SendEvent> = Box::new(event_sender);
        GetWifiNetworksAction::run_with_wifi_service(
            event_sender,
            // We will return infos in this test
            Box::new(FakeWifiConnectImpl::new(true, infos)),
        );
        // Make sure the task under test runs to its finish
        let _ = exec.run_until_stalled(&mut future::pending::<()>());
    }

    #[fuchsia::test]
    fn test_get_networks_fail_to_gather_empty_vec_returned() {
        let mut exec = fasync::TestExecutor::new();
        let infos = create_test_networks();
        let mut event_sender = MockSendEvent::new();
        event_sender
            .expect_send()
            .withf(move |event| {
                if let Event::Networks(received_infos) = event {
                    received_infos.len() == 0
                } else {
                    false
                }
            })
            .times(1)
            .return_const(());
        let event_sender: Box<dyn SendEvent> = Box::new(event_sender);
        GetWifiNetworksAction::run_with_wifi_service(
            event_sender,
            // We will not return infos in this test
            Box::new(FakeWifiConnectImpl::new(false, infos.clone())),
        );
        // Make sure the task under test runs to its finish
        let _ = exec.run_until_stalled(&mut future::pending::<()>());
    }
}
