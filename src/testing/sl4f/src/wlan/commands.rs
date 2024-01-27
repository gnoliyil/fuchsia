// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::server::Facade;
use anyhow::{format_err, Error};
use async_trait::async_trait;
use ieee80211::Ssid;
use serde_json::{from_value, to_value, Value};
use std::convert::TryFrom;
use tracing::*;

// Testing helper methods
use crate::wlan::{facade::WlanFacade, types};

use crate::common_utils::common::parse_u64_identifier;

#[async_trait(?Send)]
impl Facade for WlanFacade {
    async fn handle_request(&self, method: String, args: Value) -> Result<Value, Error> {
        match method.as_ref() {
            "scan" => {
                info!(tag = "WlanFacade", "performing wlan scan");
                let results = self.scan().await?;
                info!(tag = "WlanFacade", "received {:?} scan results", results.len());
                to_value(results).map_err(|e| format_err!("error handling scan results: {}", e))
            }
            "scan_for_bss_info" => {
                info!(tag = "WlanFacade", "performing wlan scan");
                let results = self.scan_for_bss_info().await?;
                info!(tag = "WlanFacade", "received {:?} scan results", results.len());
                to_value(results).map_err(|e| format_err!("error handling scan results: {}", e))
            }
            "connect" => {
                let target_ssid = match args.get("target_ssid") {
                    Some(ssid_value) => {
                        let ssid = match ssid_value.as_str() {
                            Some(ssid_value) => Ssid::try_from(ssid_value)?,
                            None => {
                                return Err(format_err!("Please provide a target ssid"));
                            }
                        };
                        ssid
                    }
                    None => return Err(format_err!("Please provide a target ssid")),
                };

                let target_pwd = match args.get("target_pwd") {
                    Some(pwd) => match pwd.clone().as_str() {
                        Some(pwd) => pwd.as_bytes().to_vec(),
                        None => {
                            info!(tag = "WlanFacade", "Please check provided password");
                            vec![0; 0]
                        }
                    },
                    _ => vec![0; 0],
                };

                let target_bss_desc: types::BssDescriptionDef = match args.get("target_bss_desc") {
                    Some(target_bss_desc) => from_value(target_bss_desc.clone())?,
                    None => return Err(format_err!("Please provide a target BSS description")),
                };

                info!(tag = "WlanFacade", "performing wlan connect to SSID: {:?}", target_ssid);
                let results = self.connect(target_ssid, target_pwd, target_bss_desc.into()).await?;
                to_value(results)
                    .map_err(|e| format_err!("error handling connection result: {}", e))
            }
            "get_iface_id_list" => {
                info!(tag = "WlanFacade", "Getting the interface id list.");
                let result = self.get_iface_id_list().await?;
                to_value(result).map_err(|e| format_err!("error handling get_iface_id_list: {}", e))
            }
            "get_phy_id_list" => {
                info!(tag = "WlanFacade", "Getting the phy id list.");
                let result = self.get_phy_id_list().await?;
                to_value(result).map_err(|e| format_err!("error handling get_phy_id_list: {}", e))
            }
            "destroy_iface" => {
                info!(tag = "WlanFacade", "Performing wlan destroy_iface");
                let iface_id = parse_u64_identifier(args.clone())?;
                self.destroy_iface(iface_id as u16).await?;
                to_value(true).map_err(|e| format_err!("error handling destroy_iface: {}", e))
            }
            "disconnect" => {
                info!(tag = "WlanFacade", "performing wlan disconnect");
                self.disconnect().await?;
                to_value(true).map_err(|e| format_err!("error handling disconnect: {}", e))
            }
            "query_iface" => {
                let iface_id = match args.get("iface_id") {
                    Some(iface_id) => match iface_id.as_u64() {
                        Some(iface_id) => iface_id as u16,
                        None => return Err(format_err!("Could not parse iface id")),
                    },
                    None => return Err(format_err!("Please provide target iface id")),
                };

                info!(tag = "WlanFacade", "performing wlan query iface");
                let result = self.query_iface(iface_id).await?;
                to_value(result).map_err(|e| format_err!("error handling query iface: {}", e))
            }
            "status" => {
                info!(tag = "WlanFacade", "fetching connection status");
                let result = self.status().await?;
                to_value(result).map_err(|e| format_err!("error handling connection status: {}", e))
            }
            _ => return Err(format_err!("unsupported command!")),
        }
    }
}
