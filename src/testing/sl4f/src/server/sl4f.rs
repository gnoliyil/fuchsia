// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context as _, Error};
use fidl_fuchsia_testing_sl4f::{
    FacadeIteratorMarker, FacadeIteratorSynchronousProxy, FacadeProviderMarker, FacadeProviderProxy,
};
use fuchsia_component::client::connect_to_protocol;
use fuchsia_zircon as zx;
use maplit::{convert_args, hashmap};
use parking_lot::RwLock;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use tracing::{error, info, warn};

// Standardized sl4f types and constants
use crate::{
    bluetooth::avrcp_facade::AvrcpFacade,
    server::sl4f_types::{
        AsyncCommandRequest, AsyncRequest, ClientData, CommandRequest, CommandResponse, Facade,
        MethodId, RequestId,
    },
};

// Audio related includes
use crate::audio::commands::AudioFacade;

// Session related includes
use crate::modular::facade::ModularFacade;

// Bluetooth related includes
use crate::bluetooth::avdtp_facade::AvdtpFacade;
use crate::bluetooth::ble_advertise_facade::BleAdvertiseFacade;
use crate::bluetooth::bt_sys_facade::BluetoothSysFacade;
use crate::bluetooth::gatt_client_facade::GattClientFacade;
use crate::bluetooth::gatt_server_facade::GattServerFacade;
use test_call_manager::TestCallManager as HfpFacade;
use test_rfcomm_client::RfcommManager as RfcommFacade;

use crate::bluetooth::profile_server_facade::ProfileServerFacade;

// Common
use crate::common_utils::common::{read_json_from_vmo, write_json_to_vmo};
use crate::common_utils::error::Sl4fError;

// Component related includes
use crate::component::facade::ComponentFacade;

// Device related includes
use crate::device::facade::DeviceFacade;

// Diagnostics related includes
use crate::diagnostics::facade::DiagnosticsFacade;

// Factory reset related includes
use crate::factory_reset::facade::FactoryResetFacade;

// Factory related includes
use crate::factory_store::facade::FactoryStoreFacade;

// Feedback related includes
use crate::feedback_data_provider::facade::FeedbackDataProviderFacade;

// File related includes
use crate::file::facade::FileFacade;

use crate::flatland_example::facade::FlatlandExampleFacade;

// Device Manager related includes
use crate::hardware_power_statecontrol::facade::HardwarePowerStatecontrolFacade;

// Hwinfo related includes
use crate::hwinfo::facade::HwinfoFacade;

// Input related includes
use crate::input::facade::InputFacade;

// Location related includes
use crate::location::emergency_provider_facade::EmergencyProviderFacade;
use crate::location::regulatory_region_facade::RegulatoryRegionFacade;

// Logging related includes
use crate::logging::facade::LoggingFacade;

// Netstack related includes
use crate::netstack::facade::NetstackFacade;

// Paver related includes
use crate::paver::facade::PaverFacade;

// Power related includes
use crate::power::facade::PowerFacade;

// Proxy related includes
use crate::proxy::facade::ProxyFacade;

// Scenic related includes
use crate::scenic::facade::ScenicFacade;

// SetUi related includes
use crate::setui::facade::SetUiFacade;

// System Metrics related includes
use crate::system_metrics::facade::SystemMetricsFacade;

// Temperature related includes
use crate::temperature::facade::TemperatureFacade;

// Time related includes
use crate::time::facade::TimeFacade;

// Traceutil related includes
use crate::traceutil::facade::TraceutilFacade;

// Tracing related includes
use crate::tracing::facade::TracingFacade;

// Virtual Camera Device related includes
use crate::virtual_camera::facade::VirtualCameraFacade;

// Weave related includes
use crate::weave::facade::WeaveFacade;

// Webdriver related includes
use crate::webdriver::facade::WebdriverFacade;

// Wlan related includes
use crate::wlan::facade::WlanFacade;

// Wlan DeprecatedConfiguration related includes
use crate::wlan_deprecated::facade::WlanDeprecatedConfigurationFacade;

// WlanPhy related includes
use crate::wlan_phy::facade::WlanPhyFacade;

// Wlan Policy related includes
use crate::wlan_policy::ap_facade::WlanApPolicyFacade;
use crate::wlan_policy::facade::WlanPolicyFacade;

// Wpan related includes
use crate::wpan::facade::WpanFacade;

/// Sl4f stores state for all facades and has access to information for all connected clients.
///
/// To add support for a new Facade implementation, see the hashmap in `Sl4f::new`.
#[derive(Debug)]
pub struct Sl4f {
    // facades: Mapping of method prefix to object implementing that facade's API.
    facades: HashMap<String, Arc<dyn Facade>>,

    // NOTE: facade_provider and proxied_facades will eventually become a map from proxied facade
    // to `FacadeProvider` client once we have support for multiple `FacadeProvider` instances.
    // facade_provider: Channel to the `FacadeProvider` instance hosting private facades.
    facade_provider: FacadeProviderProxy,

    // proxied_facades: Set of facades hosted by facade_provider. May be empty.
    proxied_facades: HashSet<String>,

    // connected clients
    clients: Arc<RwLock<Sl4fClients>>,
}

impl Sl4f {
    pub fn new(clients: Arc<RwLock<Sl4fClients>>) -> Result<Sl4f, Error> {
        fn to_arc_trait_object<'a, T: Facade + 'a>(facade: T) -> Arc<dyn Facade + 'a> {
            Arc::new(facade) as Arc<dyn Facade>
        }
        // To add support for a new facade, define a new submodule with the Facade implementation
        // and construct an instance and include it in the mapping below. The key is used to route
        // requests to the appropriate Facade. Facade constructors should generally not fail, as a
        // facade that returns an error here will prevent sl4f from starting.
        let facades = convert_args!(
            keys = String::from,
            values = to_arc_trait_object,
            hashmap!(
                "audio_facade" => AudioFacade::new()?,
                "avdtp_facade" => AvdtpFacade::new(),
                "avrcp_facade" => AvrcpFacade::new(),
                // TODO(fxbug.dev/77551): Remove basemgr_facade in favor of modular_facade
                "basemgr_facade" => ModularFacade::new(),
                "modular_facade" => ModularFacade::new(),
                "ble_advertise_facade" => BleAdvertiseFacade::new(),
                "bt_sys_facade" => BluetoothSysFacade::new(),
                "component_facade" => ComponentFacade::new(),
                "diagnostics_facade" => DiagnosticsFacade::new(),
                "device_facade" => DeviceFacade::new(),
                "factory_reset_facade" => FactoryResetFacade::new(),
                "factory_store_facade" => FactoryStoreFacade::new(),
                "feedback_data_provider_facade" => FeedbackDataProviderFacade::new(),
                "file_facade" => FileFacade::new(),
                "flatland_example_facade" => FlatlandExampleFacade::new(),
                "gatt_client_facade" => GattClientFacade::new(),
                "gatt_server_facade" => GattServerFacade::new(),
                "hardware_power_statecontrol_facade" => HardwarePowerStatecontrolFacade::new(),
                "hfp_facade" => HfpFacade::new(),
                "hwinfo_facade" => HwinfoFacade::new(),
                "input_facade" => InputFacade::new(),
                "location_emergency_provider_facade" => EmergencyProviderFacade::new()?,
                "location_regulatory_region_facade" => RegulatoryRegionFacade::new()?,
                "logging_facade" => LoggingFacade::new(),
                "netstack_facade" => NetstackFacade::default(),
                "rfcomm_facade" => RfcommFacade::new()?,
                "paver" => PaverFacade::new(),
                "power_facade" => PowerFacade::new(),
                "profile_server_facade" => ProfileServerFacade::new(),
                "proxy_facade" => ProxyFacade::new(),
                "scenic_facade" => ScenicFacade::new(),
                "setui_facade" => SetUiFacade::new(),
                "system_metrics_facade" => SystemMetricsFacade::new(),
                "temperature_facade" => TemperatureFacade::new(),
                "time_facade" => TimeFacade::new(),
                "traceutil_facade" => TraceutilFacade::new(),
                "tracing_facade" => TracingFacade::new(),
                "virtual_camera_facade" => VirtualCameraFacade::new(),
                "weave_facade" => WeaveFacade::new(),
                "webdriver_facade" => WebdriverFacade::new(),
                "wlan" => WlanFacade::new()?,
                "wlan_ap_policy" => WlanApPolicyFacade::new()?,
                "wlan_deprecated" => WlanDeprecatedConfigurationFacade::new()?,
                "wlan_phy" => WlanPhyFacade::new()?,
                "wlan_policy" => WlanPolicyFacade::new()?,
                "wpan_facade" => WpanFacade::new(),
            )
        );

        // Attempt to connect to the single `FacadeProvider` instance.
        let mut proxied_facades = HashSet::<String>::new();
        let facade_provider = match connect_to_protocol::<FacadeProviderMarker>() {
            Ok(proxy) => proxy,
            Err(error) => {
                error!(%error, "Failed to connect to FacadeProvider");
                return Err(error.into());
            }
        };
        // Get the names of the facades hosted by the `FacadeProvider`.
        // NOTE: Due to the inability to actively verify that connection succeeds, there are
        // multiple layers of error checking at which a PEER_CLOSED means that there never was a
        // `FacadeProvider` to connect to.
        let (client_end, server_end) = fidl::endpoints::create_endpoints::<FacadeIteratorMarker>()?;
        match facade_provider.get_facades(server_end) {
            Ok(_) => {
                let facade_iter = FacadeIteratorSynchronousProxy::new(client_end.into_channel());
                loop {
                    match facade_iter.get_next(zx::Time::INFINITE) {
                        Ok(facades) if facades.is_empty() => break, // Indicates completion.
                        Ok(facades) => proxied_facades.extend(facades.into_iter()),
                        // A PEER_CLOSED error before any facades are read indicates that there was
                        // never a successful connection.
                        Err(error) if error.is_closed() && proxied_facades.is_empty() => {
                            break;
                        }
                        Err(error) => {
                            error!(%error, "Failed to get proxied facade list");
                            proxied_facades.clear();
                            break;
                        }
                    };
                }
            }
            // The channel's server end was closed due to no `FacadeProvider` instance.
            Err(error) if error.is_closed() => (),
            Err(error) => {
                error!(%error, "Failed to get FacadeIterator");
                return Err(error.into());
            }
        };

        Ok(Sl4f { facades, facade_provider, proxied_facades, clients })
    }

    /// Gets the facade registered with the given name, if one exists.
    pub fn get_facade(&self, name: &str) -> Option<Arc<dyn Facade>> {
        self.facades.get(name).map(Arc::clone)
    }

    /// Implement the Facade trait method cleanup() to clean up state when "/cleanup" is queried.
    pub async fn cleanup(&self) {
        for facade in self.facades.values() {
            facade.cleanup();
        }
        // If there are any proxied facades, make a synchronous request to cleanup transient state.
        if !self.proxied_facades.is_empty() {
            if let Err(error) = self.facade_provider.cleanup().await {
                error!(%error, "Failed to execute Cleanup()");
            }
        }
        self.clients.write().cleanup_clients();
    }

    pub fn print_clients(&self) {
        self.clients.read().print_clients();
    }

    /// Implement the Facade trait method print() to log state when "/print" is queried.
    pub async fn print(&self) {
        for facade in self.facades.values() {
            facade.print();
        }
        // If there are any proxied facades, make a synchronous request to print state.
        if !self.proxied_facades.is_empty() {
            if let Err(error) = self.facade_provider.print().await {
                error!(%error, "Failed to execute Print()");
            }
        }
    }

    /// Returns true if the facade with the given name is hosted by a registered `FacadeProvider`.
    /// # Arguments
    /// * 'name' - A string representing the name of the facade.
    pub fn has_proxy_facade(&self, name: &str) -> bool {
        self.proxied_facades.contains(name)
    }

    /// Sends a request on a facade hosted by a registered `FacadeProvider` and waits
    /// asynchronously for the response.
    /// # Arguments
    /// * 'facade' - A string representing the name of the facade.
    /// * 'command' - A string representing the command to execute on the facade.
    /// * 'args' - An arbitrary JSON Value containing any arguments to the command.
    pub async fn handle_proxy_request(
        &self,
        facade: String,
        command: String,
        args: Value,
    ) -> Result<Value, Error> {
        // Populate a new VMO with a JSON blob containing the arguments.
        let encode_params = async {
            let params_blob = zx::Vmo::create_with_opts(zx::VmoOptions::RESIZABLE, 0)?;
            write_json_to_vmo(&params_blob, &args)?;
            Ok::<zx::Vmo, Error>(params_blob)
        };
        let params_blob = match encode_params.await {
            Ok(params_blob) => params_blob,
            Err(error) => {
                return Err(
                    Sl4fError::new(&format!("Failed to write params with: {}", error)).into()
                );
            }
        };

        // Forward the request to the `FacadeProvider`.
        match self.facade_provider.execute(&facade, &command, params_blob).await {
            // Success with no response.
            Ok((None, None)) => Ok(Value::Null),
            // Success with response. The JSON blob must be read out from the returned VMO.
            Ok((Some(vmo), None)) => match read_json_from_vmo(&vmo) {
                Ok(result) => Ok(result),
                Err(error) => {
                    Err(Sl4fError::new(&format!("Failed to read result with: {}", error)).into())
                }
            },
            // The command failed. Return the error string.
            Ok((_, Some(string))) => Err(Sl4fError::new(&string).into()),
            Err(error) => {
                Err(Sl4fError::new(&format!("Failed to send command with {}", error)).into())
            }
        }
    }
}

/// Metadata for clients utilizing the /init API.
#[derive(Debug)]
pub struct Sl4fClients {
    // clients: map of clients that are connected to the sl4f server.
    // key = session_id (unique for every ACTS instance) and value = Data about client (see
    // sl4f_types.rs)
    clients: HashMap<String, Vec<ClientData>>,
}

impl Sl4fClients {
    pub fn new() -> Self {
        Self { clients: HashMap::new() }
    }

    /// Registers a new connected client. Returns true if the client was already initialized.
    fn init_client(&mut self, id: String) -> bool {
        use std::collections::hash_map::Entry::*;
        match self.clients.entry(id) {
            Occupied(entry) => {
                warn!(tag = "client_init", "Key: {:?} already exists in clients. ", entry.key());
                true
            }
            Vacant(entry) => {
                entry.insert(Vec::new());
                info!(tag = "client_init", "Updated clients: {:?}", self.clients);
                false
            }
        }
    }

    fn cleanup_clients(&mut self) {
        self.clients.clear();
    }

    fn print_clients(&self) {
        info!("SL4F Clients: {:?}", self.clients);
    }
}

fn json<T>(content: &T) -> hyper::Response<hyper::Body>
where
    T: serde::Serialize,
{
    use std::convert::TryInto as _;

    let application_json = "application/json".try_into().expect("json header value");
    let data = serde_json::to_string(content).expect("encode json");

    let mut response = hyper::Response::new(data.into());
    assert_eq!(response.headers_mut().insert(hyper::header::CONTENT_TYPE, application_json), None);
    response
}

/// Handles all incoming requests to SL4F server, routes accordingly
pub async fn serve(
    request: hyper::Request<hyper::Body>,
    clients: Arc<RwLock<Sl4fClients>>,
    sender: async_channel::Sender<AsyncRequest>,
) -> hyper::Response<hyper::Body> {
    use hyper::Method;

    match (request.method(), request.uri().path()) {
        (&Method::GET, "/") => {
            // Parse the command request
            info!(tag = "serve", "Received command request via GET.");
            client_request(request, &sender).await
        }
        (&Method::POST, "/") => {
            // Parse the command request
            info!(tag = "serve", "Received command request via POST.");
            client_request(request, &sender).await
        }
        (&Method::GET, "/init") => {
            // Initialize a client
            info!(tag = "serve", "Received init request.");
            client_init(request, &clients).await
        }
        (&Method::GET, "/print_clients") => {
            // Print information about all clients
            info!(tag = "serve", "Received print client request.");
            const PRINT_ACK: &str = "Successfully printed clients.";
            json(&PRINT_ACK)
        }
        (&Method::GET, "/cleanup") => {
            info!(tag = "serve", "Received server cleanup request.");
            server_cleanup(request, &sender).await
        }
        _ => {
            error!(tag = "serve", "Received unknown server request.");
            const FAIL_REQUEST_ACK: &str = "Unknown GET request.";
            let res = CommandResponse::new(json!(""), None, Some(FAIL_REQUEST_ACK.to_string()));
            json(&res)
        }
    }
}

/// Given the request, map the test request to a FIDL query and execute
/// asynchronously
async fn client_request(
    request: hyper::Request<hyper::Body>,
    sender: &async_channel::Sender<AsyncRequest>,
) -> hyper::Response<hyper::Body> {
    const FAIL_TEST_ACK: &str = "Command failed";

    let (request_id, method_id, method_params) = match parse_request(request).await {
        Ok(res) => res,
        Err(error) => {
            error!(tag = "client_request", ?error, "Failed to parse request");
            return json(&FAIL_TEST_ACK);
        }
    };

    // Create channel for async thread to respond to
    // Package response and ship over JSON RPC
    let (async_sender, receiver) = futures::channel::oneshot::channel();
    let req = AsyncCommandRequest::new(async_sender, method_id.clone(), method_params);
    sender.send(AsyncRequest::Command(req)).await.expect("Failed to send request to async thread.");
    let resp = receiver.await.expect("Async thread dropped responder.");

    info!(
        tag = "client_request",
        method = ?method_id.method,
        response = ?resp,
        "Received async thread response"
    );

    // If the response has a return value, package into response, otherwise use error code
    match resp.result {
        Some(async_res) => {
            let res = CommandResponse::new(request_id.into_response_id(), Some(async_res), None);
            json(&res)
        }
        None => {
            let res = CommandResponse::new(request_id.into_response_id(), None, resp.error);
            json(&res)
        }
    }
}

/// Initializes a new client, adds to clients.
async fn client_init(
    request: hyper::Request<hyper::Body>,
    clients: &Arc<RwLock<Sl4fClients>>,
) -> hyper::Response<hyper::Body> {
    const INIT_ACK: &str = "Recieved init request.";
    const FAIL_INIT_ACK: &str = "Failed to init client.";

    let (_, _, method_params) = match parse_request(request).await {
        Ok(res) => res,
        Err(_) => return json(&FAIL_INIT_ACK),
    };

    let client_id_raw = match method_params.get("client_id") {
        Some(id) => Some(id).unwrap().clone(),
        None => return json(&FAIL_INIT_ACK),
    };

    // Initialize client with key = id, val = client data
    let client_id = client_id_raw.as_str().map(String::from).unwrap();

    if clients.write().init_client(client_id) {
        json(&FAIL_INIT_ACK)
    } else {
        json(&INIT_ACK)
    }
}

/// Given a request, grabs the method id, name, and parameters
/// Return Sl4fError if fail
async fn parse_request(
    request: hyper::Request<hyper::Body>,
) -> Result<(RequestId, MethodId, Value), Error> {
    use bytes::Buf as _;

    let body = hyper::body::aggregate(request.into_body()).await.context("read request")?;

    // Ignore the json_rpc field
    let request_data: CommandRequest = match serde_json::from_reader(body.reader()) {
        Ok(tdata) => tdata,
        Err(_) => return Err(Sl4fError::new("Failed to unpack request data.").into()),
    };

    let request_id_raw = request_data.id;
    let method_id_raw = request_data.method;
    let method_params = request_data.params;
    info!(tag = "parse_request",
        request_id = ?request_id_raw,
        name = ?method_id_raw,
        args = ?method_params
    );

    let request_id = RequestId::new(request_id_raw);
    // Separate the method_name field of the request into the method type (e.g bluetooth) and the
    // actual method name itself, defaulting to an empty method id if not formatted properly.
    let method_id = method_id_raw.parse().unwrap_or_default();
    Ok((request_id, method_id, method_params))
}

async fn server_cleanup(
    request: hyper::Request<hyper::Body>,
    sender: &async_channel::Sender<AsyncRequest>,
) -> hyper::Response<hyper::Body> {
    const FAIL_CLEANUP_ACK: &str = "Failed to cleanup SL4F resources.";
    const CLEANUP_ACK: &str = "Successful cleanup of SL4F resources.";

    info!(tag = "server_cleanup", "Cleaning up server state");
    let (request_id, _, _) = match parse_request(request).await {
        Ok(res) => res,
        Err(_) => return json(&FAIL_CLEANUP_ACK),
    };

    // Create channel for async thread to respond to
    let (async_sender, receiver) = futures::channel::oneshot::channel();

    // Cleanup all resources associated with sl4f
    sender
        .send(AsyncRequest::Cleanup(async_sender))
        .await
        .expect("Failed to send request to async thread.");
    let () = receiver.await.expect("Async thread dropped responder.");

    let ack = CommandResponse::new(request_id.into_response_id(), Some(json!(CLEANUP_ACK)), None);
    json(&ack)
}
