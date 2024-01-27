// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::wlan::types;
use anyhow::{Context as _, Error};
use fidl_fuchsia_wlan_device_service::{DeviceMonitorMarker, DeviceMonitorProxy};
use fidl_fuchsia_wlan_internal as fidl_internal;
use fuchsia_component::client::connect_to_protocol;
use fuchsia_zircon as zx;
use ieee80211::Ssid;
use parking_lot::RwLock;
use std::collections::HashMap;
use std::convert::{TryFrom, TryInto};
use wlan_common::scan::ScanResult;

// WlanFacade: proxies commands from sl4f test to proper fidl APIs
//
// This object is shared among all threads created by server.  The inner object is the facade
// itself.  Callers interact with a wrapped version of the facade that enforces read/write
// protection.
//
// Use: Create once per server instantiation.
#[derive(Debug)]
struct InnerWlanFacade {
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    scan_results: bool,
}

#[derive(Debug)]
pub(crate) struct WlanFacade {
    monitor_svc: DeviceMonitorProxy,
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    inner: RwLock<InnerWlanFacade>,
}

impl WlanFacade {
    pub fn new() -> Result<WlanFacade, Error> {
        let monitor_svc = connect_to_protocol::<DeviceMonitorMarker>()?;

        Ok(WlanFacade { monitor_svc, inner: RwLock::new(InnerWlanFacade { scan_results: false }) })
    }

    /// Gets the list of wlan interface IDs.
    pub async fn get_iface_id_list(&self) -> Result<Vec<u16>, Error> {
        let wlan_iface_ids = wlan_service_util::get_iface_list(&self.monitor_svc)
            .await
            .context("Get Iface Id List: failed to get wlan iface list")?;
        Ok(wlan_iface_ids)
    }

    /// Gets the list of wlan interface IDs.
    pub async fn get_phy_id_list(&self) -> Result<Vec<u16>, Error> {
        let wlan_phy_ids = wlan_service_util::get_phy_list(&self.monitor_svc)
            .await
            .context("Get Phy Id List: failed to get wlan phy list")?;
        Ok(wlan_phy_ids)
    }

    pub async fn scan(&self) -> Result<Vec<String>, Error> {
        // get the first client interface
        let sme_proxy = wlan_service_util::client::get_first_sme(&self.monitor_svc)
            .await
            .context("Scan: failed to get client iface sme proxy")?;

        // start the scan
        let scan_result_list =
            wlan_service_util::client::passive_scan(&sme_proxy).await.context("Scan failed")?;

        // send the ssids back to the test
        let mut ssid_list = Vec::new();
        for scan_result in &scan_result_list {
            let scan_result: ScanResult = scan_result.clone().try_into()?;
            let ssid = String::from_utf8_lossy(&scan_result.bss_description.ssid).into_owned();
            ssid_list.push(ssid);
        }
        Ok(ssid_list)
    }

    async fn passive_scan(
        &self,
    ) -> Result<impl IntoIterator<Item = Result<ScanResult, Error>>, Error> {
        // get the first client interface
        let sme_proxy = wlan_service_util::client::get_first_sme(&self.monitor_svc)
            .await
            .context("Scan: failed to get client iface sme proxy")?;
        // start the scan
        Ok(wlan_service_util::client::passive_scan(&sme_proxy)
            .await
            .context("Scan failed")?
            .into_iter()
            .map(ScanResult::try_from))
    }

    pub async fn scan_for_bss_info(
        &self,
    ) -> Result<HashMap<String, Vec<Box<types::BssDescriptionDef>>>, Error> {
        let mut scan_results_by_ssid_string = HashMap::new();
        for scan_result in self.passive_scan().await? {
            let scan_result = scan_result.context("Failed to convert scan result")?;
            let entry = scan_results_by_ssid_string
                .entry(String::from(scan_result.bss_description.ssid.to_string_not_redactable()))
                .or_insert(vec![]);

            let fidl_bss_desc: fidl_internal::BssDescription = scan_result.bss_description.into();
            entry.push(Box::new(fidl_bss_desc.into()));
        }
        Ok(scan_results_by_ssid_string)
    }

    pub async fn connect(
        &self,
        target_ssid: Ssid,
        target_pwd: Vec<u8>,
        target_bss_desc: fidl_internal::BssDescription,
    ) -> Result<bool, Error> {
        let sme_proxy = wlan_service_util::client::get_first_sme(&self.monitor_svc)
            .await
            .context("Connect: failed to get client iface sme proxy")?;
        wlan_service_util::client::connect(&sme_proxy, target_ssid, target_pwd, target_bss_desc)
            .await
    }

    /// Destroys a WLAN interface by input interface ID.
    ///
    /// # Arguments
    /// * `iface_id` - The u16 interface id.
    pub async fn destroy_iface(&self, iface_id: u16) -> Result<(), Error> {
        wlan_service_util::destroy_iface(&self.monitor_svc, iface_id)
            .await
            .context("Destroy: Failed to destroy iface")
    }

    pub async fn disconnect(&self) -> Result<(), Error> {
        wlan_service_util::client::disconnect_all(&self.monitor_svc)
            .await
            .context("Disconnect: Failed to disconnect ifaces")
    }

    pub async fn status(&self) -> Result<types::ClientStatusResponseWrapper, Error> {
        // get the first client interface
        let sme_proxy = wlan_service_util::client::get_first_sme(&self.monitor_svc)
            .await
            .context("Status: failed to get iface sme proxy")?;

        let rsp = sme_proxy.status().await.context("failed to get status from sme_proxy")?;

        Ok(types::ClientStatusResponseWrapper(rsp))
    }

    pub async fn query_iface(
        &self,
        iface_id: u16,
    ) -> Result<types::QueryIfaceResponseWrapper, Error> {
        let iface_info = self
            .monitor_svc
            .query_iface(iface_id)
            .await
            .context("Failed to query iface information")?
            .map_err(|e| zx::Status::from_raw(e));

        match iface_info {
            Ok(info) => Ok(types::QueryIfaceResponseWrapper(info.into())),
            Err(zx::Status::NOT_FOUND) => {
                Err(format_err!("no iface information for ID: {}", iface_id))
            }
            Err(e) => Err(e.into()),
        }
    }
}
