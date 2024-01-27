// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use async_utils::hanging_get::client::HangingGetStream;
use fidl::endpoints::{Proxy, RequestStream};
use fidl_fuchsia_bluetooth::PeerId;
use fidl_fuchsia_bluetooth_sys::{
    AccessMarker, AccessProxy, BondableMode, ConfigurationMarker, ConfigurationProxy, HostInfo,
    HostWatcherMarker, HostWatcherProxy, InputCapability, OutputCapability, PairingDelegateMarker,
    PairingDelegateRequest, PairingDelegateRequestStream, PairingMarker, PairingMethod,
    PairingOptions, PairingProxy, PairingSecurityLevel, Peer, ProcedureTokenProxy, Settings,
    TechnologyType,
};
use fuchsia_async::{self as fasync, DurationExt, TimeoutExt};
use fuchsia_bluetooth::types::Address;
use fuchsia_component as component;
use fuchsia_zircon::{self as zx, DurationNum};
use tracing::{error, info};

use parking_lot::RwLock;
use std::collections::HashMap;

use crate::bluetooth::types::SerializablePeer;
use crate::common_utils::common::macros::{fx_err_and_bail, with_line};

use futures::channel::mpsc;
use futures::stream::StreamExt;

use derivative::Derivative;

static ERR_NO_ACCESS_PROXY_DETECTED: &'static str = "No Bluetooth Access Proxy detected.";

#[derive(Derivative)]
#[derivative(Debug)]
struct InnerBluetoothSysFacade {
    /// The current Bluetooth Access Interface Proxy
    access_proxy: Option<AccessProxy>,

    /// The connection to the Bluetooth Pairing interface.
    pairing_proxy: Option<PairingProxy>,

    /// The current fuchsia.bluetooth.sys.Configuration Proxy
    config_proxy: Option<ConfigurationProxy>,

    /// The MPSC Sender object for sending the pin to the pairing delegate.
    client_pin_sender: Option<mpsc::Sender<String>>,

    /// The MPSC Receiver object for sending the pin out from the pairing delegate.
    client_pin_receiver: Option<mpsc::Receiver<String>>,

    /// Discovered device list
    discovered_device_list: HashMap<u64, SerializablePeer>,

    /// Discoverable token
    discoverable_token: Option<ProcedureTokenProxy>,

    /// Discovery token
    discovery_token: Option<ProcedureTokenProxy>,

    /// Peer Watcher Stream for incomming and dropped peers
    #[derivative(Debug = "ignore")]
    peer_watcher_stream: Option<HangingGetStream<AccessProxy, (Vec<Peer>, Vec<PeerId>)>>,

    /// Host Watcher Stream for watching hosts
    #[derivative(Debug = "ignore")]
    host_watcher_stream: Option<HangingGetStream<HostWatcherProxy, Vec<HostInfo>>>,

    /// Current active BT address
    active_bt_address: Option<String>,
}

#[derive(Debug)]
pub struct BluetoothSysFacade {
    initialized: RwLock<bool>,
    inner: RwLock<InnerBluetoothSysFacade>,
}

/// Perform Bluetooth Access operations.
///
/// Note this object is shared among all threads created by server.
impl BluetoothSysFacade {
    pub fn new() -> BluetoothSysFacade {
        BluetoothSysFacade {
            initialized: RwLock::new(false),
            inner: RwLock::new(InnerBluetoothSysFacade {
                access_proxy: None,
                pairing_proxy: None,
                config_proxy: None,
                client_pin_sender: None,
                client_pin_receiver: None,
                discovered_device_list: HashMap::new(),
                discoverable_token: None,
                discovery_token: None,
                peer_watcher_stream: None,
                host_watcher_stream: None,
                active_bt_address: None,
            }),
        }
    }

    pub fn init_proxies(&self) -> Result<(), Error> {
        if *self.initialized.read() {
            return Ok(());
        }
        *self.initialized.write() = true;

        let tag = "BluetoothSysFacade::init_proxies";
        let mut inner = self.inner.write();
        let access_proxy = match inner.access_proxy.clone() {
            Some(proxy) => {
                info!(tag = &with_line!(tag), "Current access proxy: {:?}", proxy);
                Ok(proxy)
            }
            None => {
                info!(tag = &with_line!(tag), "Setting new access proxy");
                let proxy = component::client::connect_to_protocol::<AccessMarker>();
                if let Err(err) = proxy {
                    fx_err_and_bail!(
                        &with_line!(tag),
                        format_err!("Failed to create access proxy: {:?}", err)
                    );
                }
                proxy
            }
        };

        let access_proxy = access_proxy.unwrap();
        inner.access_proxy = Some(access_proxy.clone());

        inner.peer_watcher_stream =
            Some(HangingGetStream::new_with_fn_ptr(access_proxy, AccessProxy::watch_peers));

        if inner.pairing_proxy.as_ref().map_or(true, |p| p.is_closed()) {
            info!(tag = &with_line!(tag), "Setting new Pairing proxy");
            let proxy = component::client::connect_to_protocol::<PairingMarker>();
            if let Err(err) = proxy {
                fx_err_and_bail!(
                    &with_line!(tag),
                    format_err!("Failed to create Pairing proxy: {:?}", err)
                );
            }
            inner.pairing_proxy = Some(proxy.expect("is Ok"));
        }

        let host_watcher_proxy = match component::client::connect_to_protocol::<HostWatcherMarker>()
        {
            Ok(proxy) => proxy,
            Err(err) => fx_err_and_bail!(
                &with_line!(tag),
                format_err!("Failed to connect to HostWatcher: {}", err)
            ),
        };

        inner.host_watcher_stream =
            Some(HangingGetStream::new_with_fn_ptr(host_watcher_proxy, HostWatcherProxy::watch));

        let configuration_proxy =
            match component::client::connect_to_protocol::<ConfigurationMarker>() {
                Ok(proxy) => proxy,
                Err(err) => fx_err_and_bail!(
                    &with_line!(tag),
                    format_err!("Failed to connect to configuration service: {}", err)
                ),
            };
        inner.config_proxy = Some(configuration_proxy);

        Ok(())
    }

    pub async fn monitor_pairing_delegate_request_stream(
        mut stream: PairingDelegateRequestStream,
        mut pin_receiver: mpsc::Receiver<String>,
        mut pin_sender: mpsc::Sender<String>,
    ) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::monitor_pairing_delegate_request_stream";
        while let Some(request) = stream.next().await {
            match request {
                Ok(r) => match r {
                    PairingDelegateRequest::OnPairingComplete {
                        id,
                        success,
                        control_handle: _,
                    } => {
                        let status = match success {
                            true => "Success",
                            false => "Failure",
                        };
                        info!(
                            tag = &with_line!(tag),
                            id = id.value,
                            status,
                            "Pairing complete for peer",
                        );
                    }
                    PairingDelegateRequest::OnPairingRequest {
                        peer,
                        method,
                        displayed_passkey,
                        responder,
                    } => {
                        let _res = pin_sender.try_send(displayed_passkey.to_string());

                        let address = match &peer.address {
                            Some(address) => Address::from(address).to_string(),
                            None => "Unknown Address".to_string(),
                        };
                        info!(
                            tag = &with_line!(tag),
                            "Pairing request from peer: {}",
                            match &peer.name {
                                Some(name) => format!("{} ({})", name, address),
                                None => address.clone(),
                            }
                        );
                        let consent = true;
                        let default_passkey = "000000".to_string();
                        let (confirm, entered_passkey) = match method {
                            PairingMethod::Consent => (consent, None),
                            PairingMethod::PasskeyComparison => (consent, None),
                            PairingMethod::PasskeyDisplay => {
                                info!(
                                    "Passkey {:?} provided for 'Passkey Display`.",
                                    displayed_passkey
                                );
                                (true, None)
                            }
                            PairingMethod::PasskeyEntry => {
                                let timeout = 30.seconds(); // Spec defined timeout
                                let pin = match pin_receiver
                                    .next()
                                    .on_timeout(timeout.after_now(), || None)
                                    .await
                                {
                                    Some(p) => p,
                                    _ => {
                                        error!(
                                            tag = &with_line!(tag),
                                            "No pairing pin found from remote host."
                                        );
                                        default_passkey
                                    }
                                };

                                (consent, Some(pin))
                            }
                        };
                        let _ = responder.send(
                            confirm,
                            match entered_passkey {
                                Some(passkey) => passkey.parse::<u32>().unwrap(),
                                None => 0u32,
                            },
                        );
                    }
                    PairingDelegateRequest::OnRemoteKeypress {
                        id,
                        keypress,
                        control_handle: _,
                    } => {
                        info!(
                            tag = &with_line!(tag),
                            id = id.value,
                            ?keypress,
                            "Unhandled OnRemoteKeypress for Device"
                        );
                    }
                },
                Err(r) => return Err(format_err!("Error during handling request stream: {:?}", r)),
            };
        }
        Ok(())
    }

    /// Starts the pairing delegate with I/O Capabilities as required inputs.
    ///
    /// # Arguments
    /// * `input` - A String representing the input capability.
    ///       Available values: NONE, CONFIRMATION, KEYBOARD
    /// * `output` - A String representing the output capability
    ///       Available values: NONE, DISPLAY
    pub async fn accept_pairing(&self, input: &str, output: &str) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::accept_pairing";
        let input_capability = match input {
            "NONE" => InputCapability::None,
            "CONFIRMATION" => InputCapability::Confirmation,
            "KEYBOARD" => InputCapability::Keyboard,
            _ => {
                fx_err_and_bail!(&with_line!(tag), format!("Invalid Input Capability {:?}", input))
            }
        };
        let output_capability = match output {
            "NONE" => OutputCapability::None,
            "DISPLAY" => OutputCapability::Display,
            _ => fx_err_and_bail!(
                &with_line!(tag),
                format!("Invalid Output Capability {:?}", output)
            ),
        };

        info!(tag = &with_line!(tag), "Accepting pairing");
        let (delegate_local, delegate_remote) = zx::Channel::create();
        let delegate_local = fasync::Channel::from_channel(delegate_local)?;
        let delegate_ptr =
            fidl::endpoints::ClientEnd::<PairingDelegateMarker>::new(delegate_remote);
        let _result = match &self.inner.read().pairing_proxy {
            Some(p) => p.set_pairing_delegate(input_capability, output_capability, delegate_ptr),
            None => fx_err_and_bail!(&with_line!(tag), "No Bluetooth Pairing Proxy Set."),
        };
        let delegate_request_stream = PairingDelegateRequestStream::from_channel(delegate_local);

        let (sender, pin_receiver) = mpsc::channel(10);
        let (pin_sender, receiever) = mpsc::channel(10);
        let pairing_delegate_fut = BluetoothSysFacade::monitor_pairing_delegate_request_stream(
            delegate_request_stream,
            pin_receiver,
            pin_sender,
        );

        self.inner.write().client_pin_sender = Some(sender);
        self.inner.write().client_pin_receiver = Some(receiever);

        let fut = async {
            let result = pairing_delegate_fut.await;
            if let Err(error) = result {
                error!(
                    tag = &with_line!("BluetoothSysFacade::accept_pairing"),
                    ?error,
                    "Failed to create or monitor the pairing service delegate",
                );
            }
        };
        fasync::Task::spawn(fut).detach();

        Ok(())
    }

    /// Sets an access proxy to use if one is not already in use.
    pub async fn init_access_proxy(&self) -> Result<(), Error> {
        self.init_proxies()
    }

    pub async fn input_pairing_pin(&self, pin: String) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::input_pairing_pin";
        match self.inner.read().client_pin_sender.clone() {
            Some(mut sender) => sender.try_send(pin)?,
            None => {
                let err_msg = "No sender setup for pairing delegate.".to_string();
                fx_err_and_bail!(&with_line!(tag), err_msg)
            }
        };
        Ok(())
    }

    pub async fn get_pairing_pin(&self) -> Result<String, Error> {
        let tag = "BluetoothSysFacade::get_pairing_pin";
        let pin = match &mut self.inner.write().client_pin_receiver {
            Some(receiever) => match receiever.try_next() {
                Ok(value) => match value {
                    Some(v) => v,
                    None => return Err(format_err!("Error getting pin from pairing delegate.")),
                },
                Err(_e) => {
                    let err_msg = "No pairing pin sent from the pairing delegate.".to_string();
                    fx_err_and_bail!(&with_line!(tag), err_msg)
                }
            },
            None => {
                let err_str = "No receiever setup for pairing delegate.".to_string();
                error!(tag = &with_line!(tag), "{}", err_str);
                bail!(err_str)
            }
        };
        Ok(pin)
    }

    /// Sets the current access proxy to be discoverable.
    ///
    /// # Arguments
    /// * 'discoverable' - A bool object for setting Bluetooth device discoverable or not.
    pub async fn set_discoverable(&self, discoverable: bool) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::set_discoverable";

        if !discoverable {
            self.inner.write().discoverable_token = None;
        } else {
            let token = match &self.inner.read().access_proxy {
                Some(proxy) => {
                    let (token, token_server) = fidl::endpoints::create_proxy()?;
                    let resp = proxy.make_discoverable(token_server).await?;
                    if let Err(err) = resp {
                        let err_msg = format_err!("Error: {:?}", err);
                        fx_err_and_bail!(&with_line!(tag), err_msg)
                    }
                    token
                }
                None => fx_err_and_bail!(
                    &with_line!(tag),
                    format!("{:?}", ERR_NO_ACCESS_PROXY_DETECTED.to_string())
                ),
            };
            self.inner.write().discoverable_token = Some(token);
        }
        Ok(())
    }

    /// Sets the current access proxy name.
    ///
    /// # Arguments
    /// * 'name' - A String object representing the name to set.
    pub async fn set_name(&self, name: String) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::set_name";
        match &self.inner.read().access_proxy {
            Some(proxy) => {
                let resp = proxy.set_local_name(&name);
                if let Err(err) = resp {
                    let err_msg = format_err!("Error: {:?}", err);
                    fx_err_and_bail!(&with_line!(tag), err_msg)
                }
                Ok(())
            }
            None => fx_err_and_bail!(
                &with_line!(tag),
                format!("{:?}", ERR_NO_ACCESS_PROXY_DETECTED.to_string())
            ),
        }
    }

    /// Starts discovery on the Bluetooth Access Proxy.
    ///
    /// # Arguments
    /// * 'discovery' - A bool representing starting and stopping discovery.
    pub async fn start_discovery(&self, discovery: bool) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::start_discovery";
        if !discovery {
            self.inner.write().discovery_token = None;
            Ok(())
        } else {
            let token = match &self.inner.read().access_proxy {
                Some(proxy) => {
                    let (token, token_server) = fidl::endpoints::create_proxy()?;
                    let resp = proxy.start_discovery(token_server).await?;
                    if let Err(err) = resp {
                        let err_msg = format_err!("Error: {:?}", err);
                        fx_err_and_bail!(&with_line!(tag), err_msg)
                    }
                    token
                }
                None => fx_err_and_bail!(
                    &with_line!(tag),
                    format!("{:?}", ERR_NO_ACCESS_PROXY_DETECTED.to_string())
                ),
            };
            self.inner.write().discovery_token = Some(token);
            Ok(())
        }
    }

    /// Returns a hashmap of the known devices on the Bluetooth Access proxy.
    pub async fn get_known_remote_devices(&self) -> Result<HashMap<u64, SerializablePeer>, Error> {
        let tag = "BluetoothSysFacade::get_known_remote_devices";

        loop {
            let (discovered_devices, removed_peers) = match &mut self
                .inner
                .write()
                .peer_watcher_stream
            {
                Some(stream) => {
                    match stream.next().on_timeout(100.millis().after_now(), || None).await {
                        Some(Ok(d)) => d,
                        Some(Err(e)) => fx_err_and_bail!(
                            &with_line!(tag),
                            format!("{:?}", format!("Peer Watcher Stream failed with: {:?}", e))
                        ),
                        None => break,
                    }
                }
                None => fx_err_and_bail!(
                    &with_line!(tag),
                    format!("{:?}", "Peer Watcher Stream not available")
                ),
            };

            let serialized_peers_map: HashMap<u64, SerializablePeer> =
                discovered_devices.iter().map(|d| (d.id.unwrap().value, d.into())).collect();

            self.inner.write().discovered_device_list.extend(serialized_peers_map);

            let mut known_devices = self.inner.write().discovered_device_list.clone();
            for peer_id in removed_peers {
                if known_devices.contains_key(&peer_id.value) {
                    info!(tag, "Peer {:?} removed.", peer_id);
                    known_devices.remove(&peer_id.value);
                }
            }
            self.inner.write().discovered_device_list = known_devices;
        }

        Ok(self.inner.read().discovered_device_list.clone())
    }

    /// Forgets (Unbonds) an input device ID.
    ///
    /// # Arguments
    /// * `id` - A u64 representing the device ID.
    pub async fn forget(&self, id: u64) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::forget";
        match &self.inner.read().access_proxy {
            Some(proxy) => {
                let resp = proxy.forget(&mut PeerId { value: id }).await?;
                if let Err(err) = resp {
                    let err_msg = format_err!("Error: {:?}", err);
                    fx_err_and_bail!(&with_line!(tag), err_msg)
                }
                Ok(())
            }
            None => fx_err_and_bail!(
                &with_line!(tag),
                format!("{:?}", ERR_NO_ACCESS_PROXY_DETECTED.to_string())
            ),
        }
    }

    /// Connects over BR/EDR to an input device ID.
    ///
    /// # Arguments
    /// * `id` - A u64 representing the device ID.
    pub async fn connect(&self, id: u64) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::connect";
        match &self.inner.read().access_proxy {
            Some(proxy) => {
                let resp = proxy.connect(&mut PeerId { value: id }).await?;
                if let Err(err) = resp {
                    let err_msg = format_err!("Error: {:?}", err);
                    fx_err_and_bail!(&with_line!(tag), err_msg)
                }
                Ok(())
            }
            None => fx_err_and_bail!(
                &with_line!(tag),
                format!("{:?}", ERR_NO_ACCESS_PROXY_DETECTED.to_string())
            ),
        }
    }

    /// Sends an outgoing pairing request over BR/EDR or LE to an input device ID.
    ///
    /// # Arguments
    /// * `id` - A u64 representing the device ID.
    /// * `pairing_security_level_value` - The security level required for this pairing request
    ///        represented as a u64. (Only for LE pairing)
    ///        Available Values
    ///        1 - ENCRYPTED: Encrypted without MITM protection (unauthenticated)
    ///        2 - AUTHENTICATED: Encrypted with MITM protection (authenticated).
    ///        None: Used for BR/EDR
    /// * `bondable` - A bool representing whether the pairing mode is bondable or not. None is
    ///        also accepted. False if non bondable, True if bondable.
    /// * `transport_value` - A u64 representing the transport type.
    ///        Available Values
    ///        1 - BREDR: Classic BR/EDR transport
    ///        2 - LE: Bluetooth Low Energy Transport
    pub async fn pair(
        &self,
        id: u64,
        pairing_security_level_value: Option<u64>,
        bondable: Option<bool>,
        transport_value: u64,
    ) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::pair";

        let pairing_security_level = match pairing_security_level_value {
            Some(value) => match value {
                1 => Some(PairingSecurityLevel::Encrypted),
                2 => Some(PairingSecurityLevel::Authenticated),
                _ => fx_err_and_bail!(
                    &with_line!(tag),
                    format!(
                        "Invalid pairing security level provided: {:?}",
                        pairing_security_level_value
                    )
                ),
            },
            None => None,
        };

        let transport = match transport_value {
            1 => TechnologyType::Classic,
            2 => TechnologyType::LowEnergy,
            _ => fx_err_and_bail!(
                &with_line!(tag),
                format!("Invalid transport provided: {:?}", transport_value)
            ),
        };

        let bondable_mode = match bondable {
            Some(v) => match v {
                false => BondableMode::NonBondable,
                true => BondableMode::Bondable,
            },
            None => BondableMode::Bondable,
        };

        let pairing_options = PairingOptions {
            le_security_level: pairing_security_level,
            bondable_mode: Some(bondable_mode),
            transport: Some(transport),
            ..Default::default()
        };

        let proxy = match &self.inner.read().access_proxy {
            Some(p) => p.clone(),
            None => fx_err_and_bail!(
                &with_line!(tag),
                format!("{:?}", ERR_NO_ACCESS_PROXY_DETECTED.to_string())
            ),
        };
        let fut = async move {
            let result = proxy.pair(&mut PeerId { value: id }, pairing_options).await;
            if let Err(err) = result {
                error!(tag = &with_line!("BluetoothSysFacade::pair"), ?err, "Failed to pair with",);
            }
        };
        fasync::Task::spawn(fut).detach();
        Ok(())
    }

    /// Disconnects an active BR/EDR connection by input device ID.
    ///
    /// # Arguments
    /// * `id` - A u64 representing the device ID.
    pub async fn disconnect(&self, id: u64) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::disconnect";
        match &self.inner.read().access_proxy {
            Some(proxy) => {
                let resp = proxy.disconnect(&mut PeerId { value: id }).await?;
                if let Err(err) = resp {
                    let err_msg = format_err!("Error: {:?}", err);
                    fx_err_and_bail!(&with_line!(tag), err_msg)
                }
                Ok(())
            }
            None => fx_err_and_bail!(
                &with_line!(tag),
                format!("{:?}", ERR_NO_ACCESS_PROXY_DETECTED.to_string())
            ),
        }
    }

    /// Updates the configuration of the active host device
    ///
    /// # Arguments
    /// * `settings` - The table of settings. Any settings that are not present will not be changed.
    pub async fn update_settings(&self, settings: Settings) -> Result<(), Error> {
        let tag = "BluetoothSysFacade::update_settings";
        match &self.inner.read().config_proxy {
            Some(proxy) => {
                let new_settings = proxy.update(settings).await?;
                info!("new core stack settings: {:?}", new_settings);
                Ok(())
            }
            None => {
                fx_err_and_bail!(&with_line!(tag), "No Bluetooth Configuration Proxy detected.")
            }
        }
    }

    /// Returns the current Active Adapter's Address.
    pub async fn get_active_adapter_address(&self) -> Result<String, Error> {
        let tag = "BluetoothSysFacade::get_active_adapter_address";

        let host_info_list = match &mut self.inner.write().host_watcher_stream {
            Some(stream) => {
                match stream.next().on_timeout(1.seconds().after_now(), || None).await {
                    Some(r) => match r {
                        Ok(d) => d,
                        Err(e) => fx_err_and_bail!(
                            &with_line!(tag),
                            format!("{:?}", format!("Host Watcher Stream failed with: {:?}", e))
                        ),
                    },
                    None => {
                        match &self.inner.read().active_bt_address {
                            Some(addr) => return Ok(addr.to_string()),
                            None => fx_err_and_bail!(
                                &with_line!(tag),
                                format!(
                                    "{:?}",
                                    "No active adapter - Timed out waiting for host_watcher_stream update."
                                )
                            ),
                        };
                    }
                }
            }
            None => fx_err_and_bail!(
                &with_line!(tag),
                format!("{:?}", "Host Watcher Stream not available")
            ),
        };

        for host in host_info_list {
            let host_active = host.active.unwrap();
            if host_active {
                match host.addresses {
                    Some(a) => {
                        let public_address = Address::from(a[0]).to_string();
                        self.inner.write().active_bt_address = Some(public_address.clone());
                        return Ok(public_address);
                    }
                    None => fx_err_and_bail!(&with_line!(tag), "Host address not found."),
                }
            }
        }
        fx_err_and_bail!(&with_line!(tag), "No active host found.")
    }

    /// Cleans up objects in use.
    pub fn cleanup(&self) {
        let mut inner = self.inner.write();
        inner.access_proxy = None;
        inner.pairing_proxy = None;
        inner.client_pin_sender = None;
        inner.client_pin_receiver = None;
        inner.discovered_device_list.clear();
        inner.discoverable_token = None;
        inner.discovery_token = None;
    }

    /// Prints useful information.
    pub fn print(&self) {
        let tag = "BluetoothSysFacade::print:";
        let guard = self.inner.read();
        info!(
            tag = &with_line!(tag),
            access = ?guard.access_proxy,
            pairing = ?guard.pairing_proxy,
            discovered_device_list = ?self.inner.read().discovered_device_list
        );
    }
}
