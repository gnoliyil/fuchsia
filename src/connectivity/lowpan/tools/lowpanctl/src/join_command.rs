// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::context::LowpanCtlContext;
use crate::prelude::*;
use fidl_fuchsia_lowpan_device::{Credential, Identity, ProvisioningParams};
use fidl_fuchsia_lowpan_experimental::{
    JoinParams, JoinerCommissioningParams, ProvisioningMonitorMarker, ProvisioningProgress,
};
use hex;

const PROVISION_CMD_NAME_LEN: usize = 63;
const PROVISION_CMD_CRED_MASTER_LEY_LEN: &[usize] = &[16, 32];

/// Contains the arguments decoded for the `join` command.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "join")]
pub struct JoinCommand {
    /// name
    #[argh(option)]
    pub name: Option<String>,

    /// extended PANID, input hex string
    #[argh(option)]
    pub xpanid: Option<String>,

    /// string identifying the type of network
    #[argh(option)]
    pub net_type: Option<String>,

    /// channel index
    #[argh(option)]
    pub channel: Option<u16>,

    /// panid for 802.14.5-based networks (or the equivalent)
    #[argh(option)]
    pub panid: Option<u16>,

    /// credential master key
    #[argh(option)]
    pub credential_master_key: Option<String>,

    /// pskd, optional
    #[argh(option)]
    pub pskd: Option<String>,

    /// provisioning url, optional
    #[argh(option)]
    pub provisioning_url: Option<String>,

    /// vendor name, optional
    #[argh(option)]
    pub vendor_name: Option<String>,

    /// vendor model, optional
    #[argh(option)]
    pub vendor_model: Option<String>,

    /// vendor sw version, optional
    #[argh(option)]
    pub vendor_sw_version: Option<String>,

    /// vendor data string, optional
    #[argh(option)]
    pub vendor_data_string: Option<String>,
}

impl JoinCommand {
    fn get_name_vec_from_str(&self) -> Result<Option<Vec<u8>>, Error> {
        if self.name.clone().unwrap().as_bytes().len() > PROVISION_CMD_NAME_LEN {
            return Err(format_err!("name should be less or equal to 63 bytes"));
        }
        Ok(Some(self.name.clone().unwrap().as_bytes().to_vec()))
    }

    fn get_xpanid(&self) -> Result<Option<[u8; 8]>, Error> {
        self.xpanid
            .as_ref()
            .map(|value| {
                let vec = hex::decode(value.to_string())?;
                let octets = vec
                    .try_into()
                    .map_err(|_: Vec<u8>| format_err!("malformed xpanid {}", value))?;
                Ok(octets)
            })
            .transpose()
    }

    fn get_cred_master_key_vec_from_str(&self) -> Result<Option<Vec<u8>>, Error> {
        self.credential_master_key
            .as_ref()
            .map(|value| {
                let res = hex::decode(value.to_string())?;
                if res.len() != PROVISION_CMD_CRED_MASTER_LEY_LEN[0]
                    && res.len() != PROVISION_CMD_CRED_MASTER_LEY_LEN[1]
                {
                    return Err(format_err!("credential master key must be 16 or 32 bytes"));
                }
                Ok(res)
            })
            .transpose()
    }

    fn get_identity(&self) -> Result<Identity, Error> {
        Ok(Identity {
            raw_name: self.get_name_vec_from_str()?,
            xpanid: self.get_xpanid()?,
            xpanid_deprecated: self.get_xpanid()?.map(|array| array.to_vec()),
            net_type: self.net_type.clone(),
            channel: self.channel.clone(),
            panid: self.panid.clone(),
            ..Identity::EMPTY
        })
    }

    fn get_credential(&self) -> Result<Option<Box<Credential>>, Error> {
        let cred_master_key_vec = self.get_cred_master_key_vec_from_str()?;
        Ok(cred_master_key_vec.map(|value| Box::new(Credential::NetworkKey(value))))
    }

    fn get_joiner_params(&self) -> JoinerCommissioningParams {
        JoinerCommissioningParams {
            pskd: self.pskd.clone(),
            provisioning_url: self.provisioning_url.clone(),
            vendor_name: self.vendor_name.clone(),
            vendor_model: self.vendor_model.clone(),
            vendor_sw_version: self.vendor_sw_version.clone(),
            vendor_data_string: self.vendor_data_string.clone(),
            ..JoinerCommissioningParams::EMPTY
        }
    }

    fn get_join_params(&self) -> Result<JoinParams, Error> {
        if let Some(_) = self.name.clone() {
            return Ok(JoinParams::ProvisioningParameter(ProvisioningParams {
                identity: self.get_identity()?,
                credential: self.get_credential()?,
            }));
        }
        if !self.pskd.clone().unwrap().is_empty() {
            return Ok(JoinParams::JoinerParameter(self.get_joiner_params()));
        }
        Err(format_err!("invalid parameter: one of the following needs to be set: name or pskd"))
    }

    pub async fn exec(&self, context: &mut LowpanCtlContext) -> Result<(), Error> {
        let mut join_args = self.get_join_params()?;
        let device_extra = context
            .get_default_experimental_device_extra()
            .await
            .context("Unable to get device instance")?;
        let (client_end, server_end) = create_endpoints::<ProvisioningMonitorMarker>()?;
        let monitor = client_end.into_proxy()?;
        device_extra
            .join_network(&mut join_args, server_end)
            .context("Unable to send join command")?;
        loop {
            match monitor.watch_progress().await? {
                Ok(ProvisioningProgress::Progress(x)) => {
                    println!("Joining network... {}%", x * 100.0);
                }
                Ok(ProvisioningProgress::Identity(x)) => {
                    println!("Completed.\nIdentity: {:?}", x);
                    break;
                }
                Ok(_) => {
                    // Don't care about other progress updates.
                }
                Err(e) => {
                    return Err(format_err!("error monitoring progress: {:?}", e));
                }
            }
        }
        Ok(())
    }
}
