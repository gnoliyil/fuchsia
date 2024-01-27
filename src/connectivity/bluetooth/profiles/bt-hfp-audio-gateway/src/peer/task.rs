// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::format_err,
    async_utils::hanging_get::client::HangingGetStream,
    bt_rfcomm::profile::server_channel_from_protocol,
    fidl_fuchsia_bluetooth_bredr as bredr,
    fidl_fuchsia_bluetooth_hfp::{NetworkInformation, PeerHandlerProxy},
    fuchsia_async::Task,
    fuchsia_bluetooth::{
        profile::{Attribute, ProtocolDescriptor},
        types::PeerId,
    },
    fuchsia_inspect::{self as inspect, Property},
    fuchsia_inspect_derive::{AttachError, Inspect},
    fuchsia_zircon as zx,
    futures::{
        channel::mpsc::{self, Sender},
        future::{self, Either, Future},
        select,
        stream::{empty, Empty},
        FutureExt, SinkExt, StreamExt,
    },
    parking_lot::Mutex,
    profile_client::ProfileEvent,
    std::{convert::TryInto, fmt, sync::Arc},
    tracing::{error, info, warn},
    vigil::{DropWatch, Vigil},
};

use super::{
    calls::{Call, CallAction, Calls},
    gain_control::GainControl,
    indicators::{AgIndicator, AgIndicators, HfIndicator},
    procedure::ProcedureMarker,
    ringer::Ringer,
    sco_state::{InspectableScoState, ScoActive, ScoState},
    service_level_connection::ServiceLevelConnection,
    slc_request::SlcRequest,
    update::AgUpdate,
    ConnectionBehavior, PeerRequest,
};

use crate::{
    a2dp,
    audio::AudioControl,
    config::AudioGatewayFeatureSupport,
    error::Error,
    features::CodecId,
    hfp,
    inspect::PeerTaskInspect,
    sco_connector::{ScoConnection, ScoConnector},
};

const CONNECTION_INIT_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(10);

const DEFAULT_CODECS: &[CodecId] = &[CodecId::CVSD];

pub(super) struct PeerTask {
    id: PeerId,
    local_config: AudioGatewayFeatureSupport,
    connection_behavior: ConnectionBehavior,
    profile_proxy: bredr::ProfileProxy,
    handler: Option<PeerHandlerProxy>,
    network: NetworkInformation,
    network_updates: Either<
        HangingGetStream<PeerHandlerProxy, NetworkInformation>,
        Empty<Result<NetworkInformation, fidl::Error>>,
    >,
    battery_level: u8,
    calls: Calls,
    gain_control: GainControl,
    connection: ServiceLevelConnection,
    a2dp_control: a2dp::Control,
    sco_connector: ScoConnector,
    sco_state: InspectableScoState,
    ringer: Ringer,
    audio_control: Arc<Mutex<Box<dyn AudioControl>>>,
    hfp_sender: Sender<hfp::Event>,
    manager_id: Option<hfp::ManagerConnectionId>,
    inspect: PeerTaskInspect,
}

impl Inspect for &mut PeerTask {
    fn iattach(self, parent: &inspect::Node, name: impl AsRef<str>) -> Result<(), AttachError> {
        let _ = self.inspect.iattach(parent, name)?;
        self.sco_state.iattach(self.inspect.node(), "sco_connection")?;
        Ok(())
    }
}

impl PeerTask {
    pub fn new(
        id: PeerId,
        profile_proxy: bredr::ProfileProxy,
        audio_control: Arc<Mutex<Box<dyn AudioControl>>>,
        local_config: AudioGatewayFeatureSupport,
        connection_behavior: ConnectionBehavior,
        hfp_sender: Sender<hfp::Event>,
        sco_connector: ScoConnector,
    ) -> Result<Self, Error> {
        let connection = ServiceLevelConnection::with_init_timeout(fuchsia_async::Timer::new(
            CONNECTION_INIT_TIMEOUT,
        ));
        let a2dp_control = a2dp::Control::connect();
        Ok(Self {
            id,
            local_config,
            connection_behavior,
            profile_proxy,
            handler: None,
            network: NetworkInformation::default(),
            network_updates: empty().right_stream(),
            // Default to a battery level of 5 (max). This will be updated when we receive
            // a battery information update.
            battery_level: 5,
            calls: Calls::new(None),
            gain_control: GainControl::new()?,
            connection,
            a2dp_control,
            sco_connector,
            sco_state: InspectableScoState::default(),
            ringer: Ringer::default(),
            audio_control,
            hfp_sender,
            manager_id: None,
            inspect: PeerTaskInspect::new(id),
        })
    }

    pub fn spawn(
        id: PeerId,
        profile_proxy: bredr::ProfileProxy,
        audio_control: Arc<Mutex<Box<dyn AudioControl>>>,
        local_config: AudioGatewayFeatureSupport,
        connection_behavior: ConnectionBehavior,
        hfp_sender: Sender<hfp::Event>,
        in_band_sco: bool,
        inspect: &inspect::Node,
    ) -> Result<(Task<()>, Sender<PeerRequest>), Error> {
        let (sender, receiver) = mpsc::channel(0);
        let sco_connector = ScoConnector::build(profile_proxy.clone(), in_band_sco);
        let mut peer = Self::new(
            id,
            profile_proxy,
            audio_control,
            local_config,
            connection_behavior,
            hfp_sender,
            sco_connector,
        )?;
        if let Err(e) = peer.iattach(inspect, "task") {
            warn!("Failed to attach PeerTaskInspect to provided inspect node: {}", e)
        }
        let task = Task::local(peer.run(receiver).map(|_| ()));
        Ok((task, sender))
    }

    fn set_handler(&mut self, handler: Option<PeerHandlerProxy>) {
        info!(id = %self.id, "Replacing {:?} with new handler", self.handler);
        self.handler = handler;
        self.inspect.connected_peer_handler.set(self.handler.is_some());
    }

    /// Always give preference to connection requests received from the peer device.
    async fn on_connection_request(
        &mut self,
        _protocol: Vec<ProtocolDescriptor>,
        channel: fuchsia_bluetooth::types::Channel,
    ) -> Result<(), Error> {
        if self.connection.connected() {
            info!("Overwriting existing connection to {}", self.id);
        } else {
            info!("Connection request from {}", self.id);
        }
        self.connection.connect(channel);
        if let Err(e) = self.connection.iattach(self.inspect.node(), "service_level_connection") {
            warn!("Failed to attach ServiceLevelConnection to PeerTaskInspect: {}", e)
        }
        if let Some(id) = self.manager_id {
            self.notify_peer_connected(id).await;
            self.setup_handler().await?;
        }
        Ok(())
    }

    /// Make a connection to the peer only if there is not an existing connection.
    async fn connect(&mut self, params: bredr::ConnectParameters) -> Result<(), anyhow::Error> {
        if self.connection.connected() {
            info!("Already connected to peer: {}.", self.id);
            return Ok(());
        }
        info!("Initiating connection to peer: {}", self.id);
        let channel = self
            .profile_proxy
            .connect(&self.id.into(), &params)
            .await?
            .map_err(|e| format_err!("Profile connection request error: {:?}", e))?;
        self.connection.connect(channel.try_into()?);
        if let Err(e) = self.connection.iattach(self.inspect.node(), "service_level_connection") {
            warn!("Failed to attach ServiceLevelConnection to PeerTaskInspect: {}", e)
        }
        if let Some(id) = self.manager_id {
            self.notify_peer_connected(id).await;
            self.setup_handler()
                .await
                .map_err(|e| format_err!("Error setting up peer handler: {}", e))?;
        }
        Ok(())
    }

    async fn on_search_result(
        &mut self,
        protocol: Option<Vec<ProtocolDescriptor>>,
        _attributes: Vec<Attribute>,
    ) {
        info!("Search results received for {}", self.id);

        let server_channel = match protocol.as_ref().map(server_channel_from_protocol) {
            Some(Some(sc)) => sc,
            _ => {
                info!("Search result received for non-RFCOMM protocol: {:?}", protocol);
                return;
            }
        };
        let params = bredr::ConnectParameters::Rfcomm(bredr::RfcommParameters {
            channel: Some(server_channel.into()),
            ..Default::default()
        });

        if self.connection_behavior.autoconnect {
            if let Err(e) = self.connect(params).await {
                info!("Error inititating connecting to peer {}: {:?}", self.id, e);
            }
        }
    }

    async fn notify_peer_connected(&mut self, manager_id: hfp::ManagerConnectionId) {
        let (proxy, handle) =
            fidl::endpoints::create_proxy().expect("Cannot create required fidl handle");
        self.hfp_sender
            .send(hfp::Event::PeerConnected { peer_id: self.id, manager_id, handle })
            .await
            .expect("Cannot communicate with main Hfp task");
        self.set_handler(Some(proxy));
    }

    async fn setup_handler(&mut self) -> Result<(), Error> {
        info!("Got request to handle peer {} headset using handler", self.id);
        let Some(handler) = self.handler.clone() else {
            return Ok(());
        };
        // Getting the network information the first time should always return a complete table.
        // If the call returns an error, do not set up the handler.
        let info = match handler.watch_network_information().await {
            Ok(info) => info,
            Err(fidl::Error::ClientChannelClosed { status: zx::Status::PEER_CLOSED, .. }) => {
                return Ok(());
            }
            Err(e) => {
                warn!("Error handling peer request: {}", e);
                return Ok(());
            }
        };

        self.handle_network_update(info).await;

        let client_end = self.gain_control.get_client_end()?;
        if let Err(e) = handler.gain_control(client_end) {
            warn!("Error setting gain control for peer {}: {}", self.id, e);
            return Ok(());
        }

        self.calls = Calls::new(Some(handler.clone()));
        if let Err(e) = self.calls.iattach(self.inspect.node(), "calls") {
            warn!("Failed to attach Calls to PeerTaskInspect: {}", e)
        }

        self.create_network_updates_stream(handler);
        Ok(())
    }

    /// A new call manager is connected.
    async fn on_manager_connected(&mut self, id: hfp::ManagerConnectionId) -> Result<(), Error> {
        self.manager_id = Some(id);
        if !self.connection.connected() {
            // If there is no peer connection, there should not be a handler.
            self.set_handler(None);
            return Ok(());
        }
        self.notify_peer_connected(id).await;
        self.setup_handler().await?;
        Ok(())
    }

    /// When a new handler is received, the state is not known. It might be stale because the
    /// system is asynchronous and the PeerHandler connection has two sides. The handler will only
    /// be stored if all the associated fidl calls are successful. Otherwise it is dropped without
    /// being set.
    async fn on_peer_handler(&mut self, handler: PeerHandlerProxy) -> Result<(), Error> {
        info!("Got request to handle peer {} headset using handler", self.id);
        // Getting the network information the first time should always return a complete table.
        // If the call returns an error, do not set up the handler.
        let info = match handler.watch_network_information().await {
            Ok(info) => info,
            Err(fidl::Error::ClientChannelClosed { status: zx::Status::PEER_CLOSED, .. }) => {
                return Ok(());
            }
            Err(e) => {
                warn!("Error handling peer request: {}", e);
                return Ok(());
            }
        };

        self.handle_network_update(info).await;

        let client_end = self.gain_control.get_client_end()?;
        if let Err(e) = handler.gain_control(client_end) {
            warn!("Error setting gain control for peer {}: {}", self.id, e);
            return Ok(());
        }

        self.calls = Calls::new(Some(handler.clone()));
        if let Err(e) = self.calls.iattach(self.inspect.node(), "calls") {
            warn!("Failed to attach Calls to PeerTaskInspect: {}", e)
        }

        self.create_network_updates_stream(handler.clone());
        self.set_handler(Some(handler));

        Ok(())
    }

    /// Set a new HangingGetStream to watch for network information updates.
    fn create_network_updates_stream(&mut self, handler: PeerHandlerProxy) {
        self.network_updates =
            HangingGetStream::new_with_fn_ptr(handler, PeerHandlerProxy::watch_network_information)
                .left_stream();
    }

    async fn peer_request(&mut self, request: PeerRequest) -> Result<(), Error> {
        match request {
            PeerRequest::Profile(ProfileEvent::PeerConnected { protocol, channel, id: _ }) => {
                let protocol = protocol.iter().map(ProtocolDescriptor::from).collect();
                self.on_connection_request(protocol, channel).await?;
            }
            PeerRequest::Profile(ProfileEvent::SearchResult { protocol, attributes, id: _ }) => {
                let protocol = protocol.map(|p| p.iter().map(ProtocolDescriptor::from).collect());
                let attributes = attributes.iter().map(Attribute::from).collect();
                self.on_search_result(protocol, attributes).await;
            }
            PeerRequest::ManagerConnected { id } => self.on_manager_connected(id).await?,
            PeerRequest::Handle(handler) => self.on_peer_handler(handler).await?,
            PeerRequest::BatteryLevel(level) => {
                self.battery_level = level;
                let status = AgIndicator::BatteryLevel(self.battery_level);
                self.phone_status_update(status).await;
            }
            PeerRequest::Behavior(behavior) => {
                self.connection_behavior = behavior;
            }
        }
        Ok(())
    }

    /// Processes a `request` for information from an HFP procedure.
    async fn procedure_request(&mut self, request: SlcRequest) {
        info!("HF procedure request ({}): {:?}", self.id, request);
        let marker = (&request).into();
        match request {
            SlcRequest::GetAgFeatures { response } => {
                let features = (&self.local_config).into();
                // Update the procedure with the retrieved AG update.
                self.connection.receive_ag_request(marker, response(features)).await;
            }
            SlcRequest::GetSubscriberNumberInformation { response } => {
                let result = if let Some(handler) = &mut self.handler {
                    handler.subscriber_number_information().await.ok().unwrap_or_else(Vec::new)
                } else {
                    vec![]
                };
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::GetAgIndicatorStatus { response } => {
                let call_ind = self.calls.indicators();
                let status = AgIndicators {
                    service: self.network.service_available.unwrap_or(false),
                    call: call_ind.call,
                    callsetup: call_ind.callsetup,
                    callheld: call_ind.callheld,
                    signal: self.network.signal_strength.map(|ss| ss as u8).unwrap_or(0),
                    roam: self.network.roaming.unwrap_or(false),
                    battchg: self.battery_level,
                };
                // Update the procedure with the retrieved AG update.
                self.connection.receive_ag_request(marker, response(status)).await;
            }
            SlcRequest::GetNetworkOperatorName { response } => {
                let format = self.connection.network_operator_name_format();
                let name = match &self.handler {
                    Some(h) => {
                        let result = h.query_operator().await;
                        if let Err(err) = &result {
                            warn!(
                                "Got error attempting to retrieve operator name from AG: {:} for peer {:}",
                                err, self.id
                            );
                        };
                        result.ok().flatten()
                    }
                    None => None,
                };
                let name_option = match (name, format) {
                    (Some(n), Some(_)) => Some(n),
                    _ => None, // The format must be set before getting the network name.
                };
                // Update the procedure with the result of retrieving the AG network name.
                self.connection.receive_ag_request(marker, response(name_option)).await;
            }
            SlcRequest::SendDtmf { code, response } => {
                let result = self.calls.send_dtmf_code(code).await;
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::SendHfIndicator { indicator, response } => {
                self.hf_indicator_update(indicator);
                self.connection.receive_ag_request(marker, response()).await;
            }
            SlcRequest::SetNrec { enable, response } => {
                let result = if let Some(handler) = &mut self.handler {
                    if let Ok(Ok(())) = handler.set_nrec_mode(enable).await {
                        Ok(())
                    } else {
                        Err(())
                    }
                } else {
                    Err(())
                };
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::SpeakerVolumeSynchronization { level, response } => {
                self.gain_control.report_speaker_gain(level);
                self.connection.receive_ag_request(marker, response()).await;
            }
            SlcRequest::MicrophoneVolumeSynchronization { level, response } => {
                self.gain_control.report_microphone_gain(level);
                self.connection.receive_ag_request(marker, response()).await;
            }
            SlcRequest::QueryCurrentCalls { response } => {
                let result = self.calls.current_calls();
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::Answer { response } => {
                let result = self.calls.answer().map_err(|e| {
                    warn!(peer = %self.id, %e, "Unexpected Answer from Hands Free");
                });
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::HangUp { response } => {
                let result = self.calls.hang_up().map_err(|e| {
                    warn!(peer = %self.id, %e, "Unexpected Hang Up from Hands Free");
                });
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::Hold { command, response } => {
                let result = self.calls.hold(command).map_err(|e| {
                    warn!(peer = %self.id, %e, ?command, "Unexpected Action from Hands Free");
                });
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::InitiateCall { call_action, response } => {
                let result = self.handle_initiate_call(call_action).await;
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::SynchronousConnectionSetup { response } => {
                if self.sco_state.is_active() {
                    warn!(peer = %self.id, "SCO setup request when SCO state was active");
                    // Drop existing SCO connection.
                    self.sco_state.iset(ScoState::SettingUp);
                }
                // TODO(fxbug.dev/72681): Because we may need to send an OK response to the HF
                // just before setting up the synchronous connection, we send it here by routing
                // through the procedure.
                self.connection.receive_ag_request(marker, AgUpdate::Ok).await;

                let codecs = self.get_codecs();
                let setup_result =
                    self.sco_connector.connect(self.id.clone(), codecs.clone()).await;
                let finish_result = match setup_result {
                    Ok(conn) => self.finish_sco_connection(conn).await,
                    Err(err) => {
                        if !codecs.contains(&CodecId::CVSD) {
                            // Try again with selecting CVSD
                            return self.initiate_codec_negotiation(Some(CodecId::CVSD)).await;
                        } else {
                            Err(err.into())
                        }
                    }
                };
                let result =
                    finish_result.map_err(|e| warn!(?e, "Error setting up audio connection"));
                self.connection.receive_ag_request(marker, response(result)).await;
            }
            SlcRequest::RestartCodecConnectionSetup { response } => {
                self.connection.receive_ag_request(marker, response()).await;
                // Start CodecConnectionSetup running again.
                self.initiate_codec_negotiation(None).await;
            }
        };
    }

    pub async fn handle_initiate_call(&mut self, call_action: CallAction) -> Result<(), ()> {
        let three_way_calling = self.connection.three_way_calling();
        let call_active = self.calls.is_call_active();
        let handler = self.handler.as_ref().ok_or(())?;

        if call_active && !three_way_calling {
            warn!("Attempting to initiate unsupported outgoing call during active call.");
            return Err(());
        }

        if call_active && three_way_calling {
            // Hold calls, and return Err(_) if it fails.
            self.calls.hold_active().map_err(|e| {
                warn!(peer = %self.id, %e, "Failed to hold active call when making outgoing call");
            })?;
        }

        match handler.request_outgoing_call(&call_action.into()).await {
            Ok(Ok(())) => Ok(()),
            err => {
                warn!(peer = %self.id, ?err, "Error initiating outgoing call");
                Err(())
            }
        }
    }

    pub async fn run(mut self, mut task_channel: mpsc::Receiver<PeerRequest>) -> Self {
        loop {
            let mut active_sco_closed_fut = self.on_active_sco_closed().fuse();
            info!(peer = %self.id, sco_state = ?self.sco_state, "Beginning select");
            let mut sco_state = self.sco_state.as_mut();
            select! {
                // Wait until the HF sets up a SCO connection.
                conn_res = sco_state.on_connected() => {
                    drop(sco_state);
                    info!(peer = %self.id, "Handling SCO Connection accepted");
                    match conn_res {
                        Ok(sco) if !sco.is_closed() => {
                            let finish_sco_res = self.finish_sco_connection(sco).await;
                            if let Err(err) = finish_sco_res {
                                warn!(peer = %self.id, ?err, "Failed to finish SCO connection");
                            }
                            let call_transfer_res = self.calls.transfer_to_hf();
                            if let Err(err) = call_transfer_res {
                                warn!(peer = %self.id, ?err, "Transfer to HF failed");
                            }
                        },
                        // This can occur if the HF opens and closes a SCO connection immediately.
                        Ok(_) => warn!(peer = %self.id, "Got already closed SCO connection"),
                        Err(err) => {
                            warn!(peer = %self.id, %err, "Got error waiting for SCO connection");
                            break;
                        }
                    }
                }
                // New request coming from elsewhere in the component
                request = task_channel.next() => {
                    drop(sco_state);
                    info!(peer = %self.id, ?request, "Handling peer request");
                    if let Some(request) = request {
                        if let Err(e) = self.peer_request(request).await {
                            warn!(peer = %self.id, %e, "Error handling peer request");
                            break;
                        }
                    } else {
                        info!(peer = %self.id, "Peer task channel closed");
                        break;
                    }
                }
                // New request on the gain control protocol
                request = self.gain_control.select_next_some() => {
                    info!(peer = %self.id, ?request, "Handling gain control");
                    self.connection.receive_ag_request(ProcedureMarker::VolumeControl, request.into()).await;
                },
                // A new call state has been received from the call service
                update = self.calls.select_next_some() => {
                    drop(sco_state);
                    info!(peer = %self.id, ?update, "Handling call");
                    // TODO(fxbug.dev/75538): for in-band ring  setup audio if should_ring is true
                    self.ringer.ring(self.calls.should_ring());
                    if update.callwaiting {
                        if let Some(call) = self.calls.waiting() {
                            self.call_waiting_update(call).await;
                        }
                    }
                    for status in update.to_vec() {
                        self.phone_status_update(status).await;
                    }
                   // Sync the SCO connection state to the call state.
                   // The error is already logged and we can't do anything.
                   let _ = self.update_sco_state().await;
                }
                // SCO connection has closed.
                _ = active_sco_closed_fut => {
                    drop(sco_state);
                    info!(peer = %self.id, "Handling SCO Connection closed, transferring call to AG");
                    self.sco_state.iset(ScoState::TearingDown);
                    let call_transfer_res = self.calls.transfer_to_ag();
                    if let Err(err) = call_transfer_res {
                        warn!("Transfer to AG failed with {:} for peer {}", err, self.id)
                    }
                }
                request = self.connection.next() => {
                    drop(sco_state);
                    info!("Handling SLC request {:?} for peer {}.", request, self.id);
                    if let Some(request) = request {
                        match request {
                            Ok(r) => self.procedure_request(r).await,
                            Err(e) => {
                                warn!("SLC stream error {:?} for peer {}", e, self.id);
                                break;
                            }
                        }
                    } else {
                        info!("Peer task channel closed for peer {}", self.id);
                        break;
                    }
                }
                update = self.network_updates.next() => {
                    drop(sco_state);
                    info!("Handling network update {:?} for peer {}", update, self.id);
                    if let Some(update) = stream_item_map_or_log(update, "PeerHandler::WatchNetworkUpdate", &self.id) {
                        self.handle_network_update(update).await
                    } else {
                        break;
                    }
                }
                _ = self.ringer.select_next_some() => {
                    drop(sco_state);
                    info!("Handling ring for peer {}.", self.id);
                    if let Some(call) = self.calls.ringing() {
                        self.ring_update(call).await;
                    } else {
                        self.ringer.ring(false);
                    }
                }
                complete => break,
            }
        }

        info!("Stopping task for peer {}", self.id);

        self
    }

    /// Sends an HF Indicator update to the client.
    fn hf_indicator_update(&mut self, indicator: HfIndicator) {
        match indicator {
            ind @ HfIndicator::EnhancedSafety(_) => {
                info!("Received EnhancedSafety HF Indicator update {:?} for peer {}", ind, self.id);
            }
            HfIndicator::BatteryLevel(v) => {
                if let Some(handler) = &mut self.handler {
                    if let Err(e) = handler.report_headset_battery_level(v) {
                        warn!("Couldn't report headset battery level {:?} for peer {}", e, self.id);
                    }
                }
                self.inspect.set_hf_battery_level(v);
            }
        }
    }

    /// Start the Codec Connection procedure if no sco connection exists.
    /// This procedure will negotiate a codec and eventually call
    /// `sco_connector.connect`. If `force_codec` is set, select that codec specifically instead of
    /// the preferred one.
    async fn initiate_codec_negotiation(&mut self, force_codec: Option<CodecId>) {
        if self.sco_state.is_active() {
            return;
        }
        self.connection
            .receive_ag_request(
                ProcedureMarker::CodecConnectionSetup,
                AgUpdate::CodecSetup(force_codec),
            )
            .await;
    }

    /// Update the SCO connection state to reflect the call state, creating a
    /// a future to wait for incoming SCO connections if necessary.
    async fn update_sco_state(&mut self) -> Result<(), ()> {
        let call_active = self.calls.is_call_active();
        let call_transferred = self.calls.is_call_transferred_to_ag();
        if call_active && call_transferred {
            error!("Call both active and transferred for peer {}", self.id);
            return Err(());
        }

        let previous_sco_state = &*self.sco_state;

        info!(
            %self.id,
            ?previous_sco_state,
            "update_sco_state: active: {}, transferred: {}",
            call_active,
            call_transferred
        );

        if call_active {
            match previous_sco_state {
                // A call just started, so set up SCO.
                ScoState::Inactive
                // A call was just transferred to the HF, so set up SCO.
                | ScoState::AwaitingRemote(_)
                => {
                    self.initiate_codec_negotiation(None).await;
                    self.sco_state.iset(ScoState::SettingUp);
                },
                // We are negotiating codecs; wait for that to finish before starting the SCO
                // connection, so do nothing.
                ScoState::SettingUp
                // The SCO connection was closed by the peer and we requested the call manager set
                // that call as transferred to AG,  but we are waiting on the call manager to do
                // so, so do nothing.
                | ScoState::TearingDown
                // A call is active and we have a SCO connection, so do nothing.
                | ScoState::Active(_) => {},
            }
        } else if call_transferred {
            // If a call is transferred to the AG, we should be waiting for a connection from the
            // HF to transfer it back.
            if let ScoState::AwaitingRemote(_) = previous_sco_state {
                // Already waiting for a SCO connection, nothing to do.
                info!("update_sco_state: already awaiting");
                return Ok(());
            }
            let fut = self.sco_connector.accept(self.id.clone(), self.get_codecs());
            self.sco_state.iset(ScoState::AwaitingRemote(Box::pin(fut)));
        } else {
            /* No call in progress */
            self.sco_state.iset(ScoState::Inactive);
        };

        info!(%self.id, ?self.sco_state, "update_sco_state: finished");

        Ok(())
    }

    async fn finish_sco_connection(&mut self, sco_connection: ScoConnection) -> Result<(), Error> {
        let peer_id = self.id.clone();
        info!(%peer_id, "Finishing SCO connection");
        let res = self.a2dp_control.pause(Some(peer_id)).await;
        let pause_token = match res {
            Err(e) => {
                warn!(%peer_id, ?e, "Couldn't pause A2DP Audio");
                None
            }
            Ok(token) => {
                info!(%peer_id, "Successfully paused audio");
                token
            }
        };
        let vigil = Vigil::new(ScoActive::new(&sco_connection, pause_token));
        {
            let mut audio = self.audio_control.lock();
            let codec_id = self.codec_for_parameter_set(&sco_connection.params.parameter_set);
            if let Err(e) = audio.start(self.id.clone(), sco_connection, codec_id) {
                // Cancel the SCO connection, we can't send audio.
                // TODO(fxbug.dev/79784): this probably means we should just cancel out of HFP and
                // this peer's connection entirely.
                warn!(%peer_id, ?e, "Couldn't start Audio - dropping audio connection");
                return Err(Error::system(format!("Couldn't start audio DAI"), e));
            } else {
                info!(%peer_id, "Successfully started Audio DAI");
            }
        }
        Vigil::watch(&vigil, {
            let control = self.audio_control.clone();
            move |_| match control.lock().stop() {
                Err(e) => warn!(%peer_id, ?e,  "Couldn't stop audio"),
                Ok(()) => info!(%peer_id, "Stopped HFP Audio"),
            }
        });
        info!(%peer_id, ?vigil, "Done finish_sco_connection");
        self.sco_state.iset(ScoState::Active(vigil));

        Ok(())
    }

    fn codec_for_parameter_set(&self, param_set: &bredr::HfpParameterSet) -> CodecId {
        use bredr::HfpParameterSet::*;
        match param_set {
            MsbcT2 | MsbcT1 => CodecId::MSBC,
            _ => CodecId::CVSD,
        }
    }

    fn on_active_sco_closed(&self) -> impl Future<Output = ()> + 'static {
        match &*self.sco_state {
            ScoState::Active(connection) => connection.on_closed().left_future(),
            _ => future::pending().right_future(),
        }
    }

    /// Request to send the phone `status` by initiating the Phone Status Indicator
    /// procedure.
    async fn ring_update(&mut self, call: Call) {
        self.connection.receive_ag_request(ProcedureMarker::Ring, AgUpdate::Ring(call)).await;
    }

    /// Request to send a Call Waiting Notification.
    async fn call_waiting_update(&mut self, call: Call) {
        self.connection
            .receive_ag_request(
                ProcedureMarker::CallWaitingNotifications,
                AgUpdate::CallWaiting(call),
            )
            .await;
    }

    /// Request to send the phone `status` by initiating the Phone Status Indicator
    /// procedure.
    async fn phone_status_update(&mut self, status: AgIndicator) {
        self.connection.receive_ag_request(ProcedureMarker::PhoneStatus, status.into()).await;
    }

    /// Update the network information with the provided `update` value.
    async fn handle_network_update(&mut self, update: NetworkInformation) {
        if update_table_entry(&mut self.network.service_available, &update.service_available) {
            let status = AgIndicator::Service(self.network.service_available.unwrap() as u8);
            self.phone_status_update(status).await;
        }
        if update_table_entry(&mut self.network.signal_strength, &update.signal_strength) {
            let status = AgIndicator::Signal(self.network.signal_strength.unwrap() as u8);
            self.phone_status_update(status).await;
        }
        if update_table_entry(&mut self.network.roaming, &update.roaming) {
            let status = AgIndicator::Roam(self.network.roaming.unwrap() as u8);
            self.phone_status_update(status).await;
        }
        self.inspect.network.update(&self.network);
    }

    fn get_codecs(&self) -> Vec<CodecId> {
        self.connection.get_selected_codec().map_or(DEFAULT_CODECS.to_vec(), |c| vec![c])
    }
}

/// Table entries are all optional fields. Update `dst` if `src` is present and differs from `dst`.
///
/// Return true if the update occurred.
fn update_table_entry<T: PartialEq + Clone>(dst: &mut Option<T>, src: &Option<T>) -> bool {
    if src.is_some() && src != dst {
        *dst = src.clone();
        true
    } else {
        false
    }
}

/// Take an item from a stream the produces results and return an `Option<T>` when available.
/// Log a message including the `stream_name` when the stream produces an error or is exhausted.
///
/// This is useful when dealing with streams where the only meaningful difference between
/// a terminated stream and an error value is how it should be logged.
fn stream_item_map_or_log<T, E: fmt::Debug>(
    item: Option<Result<T, E>>,
    stream_name: &str,
    peer_id: &PeerId,
) -> Option<T> {
    match item {
        Some(Ok(value)) => Some(value),
        Some(Err(e)) => {
            warn!("Error on stream {} for peer {:}: {:?}", stream_name, peer_id, e);
            None
        }
        None => {
            info!("Stream {} closed for peer {:}", stream_name, peer_id);
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        assert_matches::assert_matches,
        async_test_helpers::run_while,
        async_utils::PollExt,
        at_commands::{self as at, SerDe},
        bt_rfcomm::{profile::build_rfcomm_protocol, ServerChannel},
        core::task::Poll,
        fidl::AsHandleRef,
        fidl_fuchsia_bluetooth_bredr::{ProfileMarker, ProfileRequestStream, ScoErrorCode},
        fidl_fuchsia_bluetooth_hfp::{
            CallDirection, CallRequest, CallRequestStream, CallState, NextCall, PeerHandlerMarker,
            PeerHandlerRequest, PeerHandlerRequestStream, PeerHandlerWatchNextCallResponder,
            SignalStrength,
        },
        fuchsia_async as fasync,
        fuchsia_bluetooth::types::Channel,
        futures::{
            future::ready,
            pin_mut,
            stream::{FusedStream, Stream},
            SinkExt,
        },
        proptest::prelude::*,
        std::convert::TryFrom,
    };

    use crate::{
        audio::TestAudioControl,
        features::{AgFeatures, HfFeatures},
        peer::{
            calls::Number,
            indicators::{AgIndicatorsReporting, HfIndicators},
            service_level_connection::{
                tests::{
                    create_and_connect_slc, create_and_initialize_slc,
                    expect_data_received_by_peer, expect_peer_ready, serialize_at_response,
                },
                SlcState,
            },
        },
        sco_connector::tests::connection_for_codec,
    };

    fn arb_signal() -> impl Strategy<Value = Option<SignalStrength>> {
        proptest::option::of(prop_oneof![
            Just(SignalStrength::None),
            Just(SignalStrength::VeryLow),
            Just(SignalStrength::Low),
            Just(SignalStrength::Medium),
            Just(SignalStrength::High),
            Just(SignalStrength::VeryHigh),
        ])
    }

    prop_compose! {
        fn arb_network()(
            service_available in any::<Option<bool>>(),
            signal_strength in arb_signal(),
            roaming in any::<Option<bool>>()
        ) -> NetworkInformation {
            NetworkInformation {
                service_available,
                roaming,
                signal_strength,
                ..Default::default()
            }
        }
    }

    fn setup_peer_task(
        connection: Option<ServiceLevelConnection>,
    ) -> (PeerTask, Sender<PeerRequest>, mpsc::Receiver<PeerRequest>, ProfileRequestStream) {
        let (sender, receiver) = mpsc::channel(1);
        let (proxy, stream) = fidl::endpoints::create_proxy_and_stream::<ProfileMarker>().unwrap();
        let audio: Arc<Mutex<Box<dyn AudioControl>>> =
            Arc::new(Mutex::new(Box::new(TestAudioControl::default())));
        let sco_connector = ScoConnector::build(proxy.clone(), false);
        let mut task = PeerTask::new(
            PeerId(1),
            proxy,
            audio,
            AudioGatewayFeatureSupport::default(),
            ConnectionBehavior::default(),
            mpsc::channel(1).0,
            sco_connector,
        )
        .expect("Could not create PeerTask");
        if let Some(conn) = connection {
            task.connection = conn;
        }
        (task, sender, receiver, stream)
    }

    proptest! {
        #![proptest_config(ProptestConfig{
            // Disable persistence to avoid the warning for not running in the
            // source code directory (since we're running on a Fuchsia target)
            failure_persistence: None,
            .. ProptestConfig::default()
        })]
        #[test]
        fn updates(a in arb_network(), b in arb_network()) {
            let mut exec = fasync::TestExecutor::new();
            let mut task = setup_peer_task(None).0;

            task.network = a.clone();
            exec.run_singlethreaded(task.handle_network_update(b.clone()));

            let c = task.network.clone();

            // Check that the `service_available` field is correct.
            if b.service_available.is_some() {
                assert_eq!(c.service_available, b.service_available);
            } else if a.service_available.is_some() {
                assert_eq!(c.service_available, a.service_available);
            } else {
                assert_eq!(c.service_available, None);
            }

            // Check that the `signal_strength` field is correct.
            if b.signal_strength.is_some() {
                assert_eq!(c.signal_strength, b.signal_strength);
            } else if a.signal_strength.is_some() {
                assert_eq!(c.signal_strength, a.signal_strength);
            } else {
                assert_eq!(c.signal_strength, None);
            }

            // Check that the `roaming` field is correct.
            if b.roaming.is_some() {
                assert_eq!(c.roaming, b.roaming);
            } else if a.roaming.is_some() {
                assert_eq!(c.roaming, a.roaming);
            } else {
                assert_eq!(c.roaming, None);
            }
        }
    }

    #[fuchsia::test]
    fn handle_peer_request_stores_peer_handler_proxy() {
        let mut exec = fasync::TestExecutor::new();
        let mut peer = setup_peer_task(None).0;
        assert!(peer.handler.is_none());
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();

        {
            let request_fut = peer.peer_request(PeerRequest::Handle(proxy));
            pin_mut!(request_fut);

            let (result, request_fut) = run_while(&mut exec, request_fut, stream.next());
            match result {
                Some(Ok(PeerHandlerRequest::WatchNetworkInformation { responder })) => {
                    responder
                        .send(&NetworkInformation::default())
                        .expect("Successfully send network information");
                }
                x => panic!("Expected watch network information request: {:?}", x),
            };

            // Request future should finish.
            let request_result = exec.run_singlethreaded(request_fut);
            assert!(request_result.is_ok());
        }

        assert!(peer.handler.is_some());
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn handle_peer_request_decline_to_handle() {
        let mut peer = setup_peer_task(None).0;
        assert!(peer.handler.is_none());
        let (proxy, server_end) = fidl::endpoints::create_proxy::<PeerHandlerMarker>().unwrap();

        // close the PeerHandler channel by dropping the server endpoint.
        drop(server_end);

        peer.peer_request(PeerRequest::Handle(proxy)).await.expect("request to succeed");
        assert!(peer.handler.is_none());
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn task_runs_until_all_event_sources_close() {
        let (peer, sender, receiver, _) = setup_peer_task(None);
        // Peer will stop running when all event sources are closed.
        // Close all sources
        drop(sender);

        // Test that `run()` completes.
        let _ = peer.run(receiver).await;
    }

    /// Expects a message to be received by the peer. This expectation does not validate the
    /// contents of the data received.
    #[track_caller]
    fn expect_message_received_by_peer(exec: &mut fasync::TestExecutor, remote: &mut Channel) {
        let mut vec = Vec::new();
        let mut remote_fut = Box::pin(remote.read_datagram(&mut vec));
        assert!(exec.run_until_stalled(&mut remote_fut).is_ready());
    }

    #[fuchsia::test]
    fn peer_task_drives_procedure() {
        let mut exec = fasync::TestExecutor::new();
        let (mut peer, _sender, receiver, _profile) = setup_peer_task(None);

        // Set up the RFCOMM connection.
        let (local, mut remote) = Channel::create();
        exec.run_singlethreaded(peer.on_connection_request(vec![], local))
            .expect("Connection request handling to succeed");

        let mut peer_task_fut = Box::pin(peer.run(receiver));
        assert!(exec.run_until_stalled(&mut peer_task_fut).is_pending());

        // Simulate remote peer (HF) sending AT command to start the SLC Init Procedure.
        let features = HfFeatures::empty();
        let command = format!("AT+BRSF={}\r", features.bits()).into_bytes();
        let _ = remote.as_ref().write(&command);
        let _ = exec.run_until_stalled(&mut peer_task_fut);
        // We then expect an outgoing message to the peer.
        expect_message_received_by_peer(&mut exec, &mut remote);
    }

    #[fuchsia::test]
    fn network_information_updates_are_relayed_to_peer() {
        // This test produces the following two network updates. Each update is expected to
        // be sent to the remote peer.
        let network_update_1 = NetworkInformation {
            signal_strength: Some(SignalStrength::Low),
            roaming: Some(false),
            ..Default::default()
        };
        // Expect to send the Signal and Roam indicators to the peer.
        let expected_data1 = vec![AgIndicator::Signal(3).into(), AgIndicator::Roam(0).into()];

        let network_update_2 = NetworkInformation {
            service_available: Some(true),
            roaming: Some(true),
            ..Default::default()
        };
        // Expect to send the Service and Roam indicators to the peer.
        let expected_data2 = vec![AgIndicator::Service(1).into(), AgIndicator::Roam(1).into()];

        // The value after the updates are applied is expected to be the following
        let expected_network = NetworkInformation {
            service_available: Some(true),
            roaming: Some(true),
            signal_strength: Some(SignalStrength::Low),
            ..Default::default()
        };

        // Set up the executor, peer, and background call manager task
        let mut exec = fasync::TestExecutor::new();
        let state = SlcState {
            ag_indicator_events_reporting: AgIndicatorsReporting::new_enabled(),
            ..SlcState::default()
        };
        let (connection, mut remote) = create_and_initialize_slc(state);
        let (peer, mut sender, receiver, _profile) = setup_peer_task(Some(connection));

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();

        // A vec to hold all the stream items we don't care about for this test.
        let mut junk_drawer = vec![];

        // Filter out all items that are irrelevant to this particular test, placing them in
        // the junk_drawer.
        let mut stream = stream.filter_map(move |item| {
            let item = match item {
                Ok(PeerHandlerRequest::WatchNetworkInformation { responder }) => Some(responder),
                x => {
                    junk_drawer.push(x);
                    None
                }
            };
            ready(item)
        });

        // Pass in the client end connected to the call manager
        let result = exec.run_singlethreaded(sender.send(PeerRequest::Handle(proxy)));
        assert!(result.is_ok());

        // The stream should produce the network update while the peer runs in the background.
        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // Send the first network update - should be relayed to the peer.
        let (responder, run_fut) = run_while(&mut exec, run_fut, stream.next());
        responder.unwrap().send(&network_update_1).expect("Successfully send network information");
        let ((), run_fut) = run_while(
            &mut exec,
            run_fut,
            expect_data_received_by_peer(&mut remote, expected_data1),
        );

        // Send the second network update - should be relayed to the peer.
        let (responder, run_fut) = run_while(&mut exec, run_fut, stream.next());
        responder.unwrap().send(&network_update_2).expect("Successfully send network information");
        let ((), mut run_fut) = run_while(
            &mut exec,
            run_fut,
            expect_data_received_by_peer(&mut remote, expected_data2),
        );

        // Drop the peer task sender to force the PeerTask's run future to complete
        drop(sender);
        let task = exec.run_singlethreaded(&mut run_fut);

        // Check that the task's network information contains the expected values
        // based on the updates provided by the call manager task.
        assert_eq!(task.network, expected_network);
    }

    #[fuchsia::test]
    fn terminated_slc_ends_peer_task() {
        let mut exec = fasync::TestExecutor::new();
        let (connection, remote) = create_and_initialize_slc(SlcState::default());
        let (peer, _sender, receiver, _profile) = setup_peer_task(Some(connection));

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // The peer task is pending with no futher work to do at this time.
        let result = exec.run_until_stalled(&mut run_fut);
        assert!(result.is_pending());

        // Closing the SLC connection will result in the completion of the peer task.
        drop(remote);

        let result = exec.run_until_stalled(&mut run_fut);
        let peer = result.expect("run to complete");
        assert!(peer.connection.is_terminated());
    }

    #[fuchsia::test]
    fn error_in_slc_ends_peer_task() {
        let mut exec = fasync::TestExecutor::new();
        let (connection, remote) = create_and_initialize_slc(SlcState::default());
        let (peer, _sender, receiver, _profile) = setup_peer_task(Some(connection));

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // Produces an error when polling the ServiceLevelConnection stream by disabling write
        // on the remote socket and read on the local socket.
        let status = unsafe {
            zx::sys::zx_socket_set_disposition(
                remote.as_ref().raw_handle(),
                zx::sys::ZX_SOCKET_DISPOSITION_WRITE_DISABLED,
                0,
            )
        };
        zx::Status::ok(status).unwrap();

        // Error on the SLC connection will result in the completion of the peer task.
        let result = exec.run_until_stalled(&mut run_fut);
        assert!(result.is_ready());
    }

    /// Transform `stream` into a Stream of WatchNextCall responders.
    async fn wait_for_call_stream(
        stream: PeerHandlerRequestStream,
    ) -> impl Stream<Item = PeerHandlerWatchNextCallResponder> {
        filtered_stream(stream, |item| match item {
            PeerHandlerRequest::WatchNextCall { responder } => Ok(responder),
            x => Err(x),
        })
        .await
    }

    /// Transform `stream` into a Stream of `T`.
    ///
    /// `f` is a function that takes a PeerHandlerRequest and either returns an Ok if the request
    /// is relevant to the test or Err if the request irrelevant.
    ///
    /// This test helper function can be used in the common case where the test interacts
    /// with a particular kind of PeerHandlerRequest.
    ///
    /// Initial setup of the handler is done, then a filtered stream is produced which
    /// outputs items based on the result of `f`. Ok return values from `f` are returned from the
    /// `filtered_stream`. Err return values from `f` are not returned from the `filtered_stream`.
    /// Instead they are stored within `filtered_stream` until `filtered_stream` is dropped
    /// so that they do not cause the underlying fidl channel to be closed.
    async fn filtered_stream<T>(
        mut stream: PeerHandlerRequestStream,
        f: impl Fn(PeerHandlerRequest) -> Result<T, PeerHandlerRequest>,
    ) -> impl Stream<Item = T> {
        // Send the network information immediately so the peer can make progress.
        match stream.next().await {
            Some(Ok(PeerHandlerRequest::WatchNetworkInformation { responder })) => {
                responder
                    .send(&NetworkInformation::default())
                    .expect("Successfully send network information");
            }
            x => panic!("Expected watch network information request: {:?}", x),
        };

        // A vec to hold all the stream items we don't care about for this test.
        let mut junk_drawer = vec![];

        // Filter out all items, placing them in the junk_drawer.
        stream.filter_map(move |item| {
            let item = match item {
                Ok(item) => match f(item) {
                    Ok(t) => Some(t),
                    Err(x) => {
                        junk_drawer.push(x);
                        None
                    }
                },
                _ => None,
            };
            ready(item)
        })
    }

    #[fuchsia::test]
    fn call_updates_update_ringer_state() {
        // Set up the executor, peer, and background call manager task
        let mut exec = fasync::TestExecutor::new();

        // Setup the peer task with the specified SlcState to enable indicator events.
        let state = SlcState {
            ag_indicator_events_reporting: AgIndicatorsReporting::new_enabled(),
            ..SlcState::default()
        };
        let (connection, mut remote) = create_and_initialize_slc(state);
        let (peer, mut sender, receiver, _profile) = setup_peer_task(Some(connection));

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();

        // Pass in the client end connected to the call manager
        let result = exec.run_singlethreaded(sender.send(PeerRequest::Handle(proxy)));
        assert!(result.is_ok());

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);
        let (mut stream, run_fut) = run_while(&mut exec, run_fut, wait_for_call_stream(stream));

        // Send the incoming call
        let (responder, run_fut) = run_while(&mut exec, run_fut, stream.next());
        let (client_end, _call_stream) = fidl::endpoints::create_request_stream().unwrap();
        let next_call = NextCall {
            call: Some(client_end),
            remote: Some("1234567".to_string()),
            state: Some(CallState::IncomingRinging),
            direction: Some(CallDirection::MobileTerminated),
            ..Default::default()
        };
        responder.unwrap().send(next_call).expect("Successfully send call information");

        let expected_data = vec![AgIndicator::CallSetup(1).into()];
        let ((), mut run_fut) =
            run_while(&mut exec, run_fut, expect_data_received_by_peer(&mut remote, expected_data));

        // Drop the peer task sender to force the PeerTask's run future to complete
        drop(sender);
        let task = exec.run_until_stalled(&mut run_fut).expect("run_fut to complete");

        // Check that the task's ringer has an active call with the expected call index.
        assert!(task.ringer.ringing());
    }

    #[fuchsia::test]
    fn transfers_change_sco_state() {
        // Set up the executor, peer, and background call manager task
        let mut exec = fasync::TestExecutor::new();

        // Setup the peer task.
        let (connection, _remote) = create_and_initialize_slc(SlcState::default());
        let (peer, mut sender, receiver, mut profile) = setup_peer_task(Some(connection));

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();

        // Pass in the client end connected to the call manager
        let result = exec.run_singlethreaded(sender.send(PeerRequest::Handle(proxy)));
        assert!(result.is_ok());

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);
        let (mut stream, run_fut) = run_while(&mut exec, run_fut, wait_for_call_stream(stream));

        // Send the incoming call
        let (responder, run_fut) = run_while(&mut exec, run_fut, stream.next());
        let (client_end, mut call_stream) = fidl::endpoints::create_request_stream().unwrap();
        let next_call = NextCall {
            call: Some(client_end),
            remote: Some("1234567".to_string()),
            state: Some(CallState::IncomingRinging),
            direction: Some(CallDirection::MobileTerminated),
            ..Default::default()
        };
        responder.unwrap().send(next_call).expect("Successfully send call information");

        // Answer call.
        let (request, run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        let state_responder = match request {
            Some(Ok(CallRequest::WatchState { responder })) => responder,
            req => panic!("Expected WatchState, got {:?}", req),
        };
        state_responder.send(CallState::OngoingActive).expect("Sent OngoingActive.");
        let (sco, mut run_fut) =
            run_while(&mut exec, run_fut, expect_sco_connection(&mut profile, true, Ok(())));
        while let None = exec.run_one_step(&mut run_fut) {}
        let mut sco = sco.unwrap();

        // Call is transferred to AG by AG and SCO is torn down.
        let (request, run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        let state_responder = match request {
            Some(Ok(CallRequest::WatchState { responder })) => responder,
            req => panic!("Expected WatchState, got {:?}", req),
        };
        state_responder.send(CallState::TransferredToAg).expect("Sent TransferredToAg.");
        let (request, mut run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        while let None = exec.run_one_step(&mut run_fut) {}
        let (sco_result, run_fut) = run_while(&mut exec, run_fut, &mut sco.next());
        assert_matches!(sco_result, None);

        // Call is transferred to HF by AG and SCO is set up.
        let state_responder = match request {
            Some(Ok(CallRequest::WatchState { responder })) => responder,
            req => panic!("Expected WatchState, got {:?}", req),
        };
        state_responder.send(CallState::OngoingActive).expect("Sent OngoingActive.");
        // Don't send a SCO connection until they are trying to connect.
        let (sco, mut run_fut) =
            run_while(&mut exec, run_fut, expect_sco_connection(&mut profile, true, Ok(())));
        let _ = exec.run_one_step(&mut run_fut);
        let sco = sco.expect("SCO Connection.");
        // Run until the connection is handled by the task.  This avoids a race where the
        // the incoming SCO connection is closed before it's received, in which case a call
        // is never set to active.
        let _ = exec.run_one_step(&mut run_fut);

        // SCO is torn down by HF and call is transferred to AG
        drop(sco);
        let (watch_state_req, run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        let (req, run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        assert_matches!(req, Some(Ok(CallRequest::RequestTransferAudio { .. })));
        let state_responder = match watch_state_req {
            Some(Ok(CallRequest::WatchState { responder })) => responder,
            req => panic!("Expected WatchState, got {:?}", req),
        };
        state_responder.send(CallState::TransferredToAg).expect("Sent TransferredToAg");

        // SCO is set up by HF and call is transferred to HF
        let (_sco, mut run_fut) =
            run_while(&mut exec, run_fut, expect_sco_connection(&mut profile, false, Ok(())));
        while let None = exec.run_one_step(&mut run_fut) {}
        let (_watch_state_req, run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        let (req, mut run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        assert_matches!(req, Some(Ok(CallRequest::RequestActive { .. })));

        // Drop the peer task sender to force the PeerTask's run future to complete
        drop(sender);
        while let None = exec.run_one_step(&mut run_fut) {}
    }

    #[fuchsia::test]
    fn incoming_hf_indicator_battery_level_is_propagated_to_peer_handler_stream() {
        // Set up the executor, peer, and background call manager task
        let mut exec = fasync::TestExecutor::new();

        // Setup the peer task with the specified SlcState to enable the battery level HF indicator.
        let mut hf_indicators = HfIndicators::default();
        hf_indicators.enable_indicators(vec![at::BluetoothHFIndicator::BatteryLevel]);
        let state = SlcState { hf_indicators, ..SlcState::default() };
        let (connection, mut remote) = create_and_initialize_slc(state);
        let (peer, mut sender, receiver, _profile) = setup_peer_task(Some(connection));

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();
        // The battery level that will be reported by the peer.
        let expected_level = 4;

        // Pass in the client end connected to the call manager
        let result = exec.run_singlethreaded(sender.send(PeerRequest::Handle(proxy)));
        assert!(result.is_ok());

        // Run the PeerTask.
        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        let headset_level_stream_fut = filtered_stream(stream, |item| match item {
            PeerHandlerRequest::ReportHeadsetBatteryLevel { level, .. } => Ok(level),
            x => Err(x),
        });

        let (mut stream, run_fut) = run_while(&mut exec, run_fut, headset_level_stream_fut);

        // Peer sends us a battery level HF indicator update.
        let battery_level_cmd = at::Command::Biev {
            anum: at::BluetoothHFIndicator::BatteryLevel,
            value: expected_level as i64,
        };
        let mut buf = Vec::new();
        at::Command::serialize(&mut buf, &vec![battery_level_cmd]).expect("serialization is ok");
        let _ = remote.as_ref().write(&buf[..]).expect("channel write is ok");

        // Run the main future - the task should receive the HF indicator and report it.
        let (battery_level, _run_fut) = run_while(&mut exec, run_fut, stream.next());
        assert_eq!(battery_level, Some(expected_level));

        // Since we (the AG) received a valid HF indicator, we expect to send an OK back to the peer.
        expect_peer_ready(&mut exec, &mut remote, Some(serialize_at_response(at::Response::Ok)));
    }

    #[fuchsia::test]
    fn local_battery_level_change_initiates_phone_status_procedure() {
        let mut exec = fasync::TestExecutor::new();

        // Setup the peer task with the specified SlcState to enable the battery level indicator on
        // both the HF and the AG.
        let mut hf_indicators = HfIndicators::default();
        hf_indicators.enable_indicators(vec![at::BluetoothHFIndicator::BatteryLevel]);
        let ag_indicator_events_reporting = AgIndicatorsReporting::new_enabled();
        let state =
            SlcState { hf_indicators, ag_indicator_events_reporting, ..SlcState::default() };
        let (connection, mut remote) = create_and_initialize_slc(state);
        let (peer, mut sender, receiver, _profile) = setup_peer_task(Some(connection));

        // Run the PeerTask.
        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // Receive a local update from the Fuchsia Battery Manager about a battery level change.
        let request = PeerRequest::BatteryLevel(3);
        let send_request_fut = sender.send(request);
        let (send_result, run_fut) = run_while(&mut exec, run_fut, send_request_fut);
        assert_matches!(send_result, Ok(()));

        // We expect the peer (HF) to receive the PhoneStatus update AT command.
        let expected_ciev = vec![AgIndicator::BatteryLevel(3).into()];
        let ((), _run_fut) =
            run_while(&mut exec, run_fut, expect_data_received_by_peer(&mut remote, expected_ciev));
    }

    #[fuchsia::test]
    fn call_updates_produce_call_waiting() {
        // Set up the executor, peer, and background call manager task
        let mut exec = fasync::TestExecutor::new();

        let raw_number = "1234567";
        let number = Number::from(raw_number);
        let expected_ccwa =
            vec![at::success(at::Success::Ccwa { ty: number.type_(), number: number.into() })];
        let expected_ciev = vec![AgIndicator::CallSetup(1).into()];

        // Setup the peer task with the specified SlcState to enable indicator events.
        let state = SlcState {
            call_waiting_notifications: true,
            ag_indicator_events_reporting: AgIndicatorsReporting::new_enabled(),
            ..SlcState::default()
        };
        let (connection, mut remote) = create_and_initialize_slc(state);
        let (peer, mut sender, receiver, _profile) = setup_peer_task(Some(connection));

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // Pass in the client end connected to the call manager
        let result = exec.run_singlethreaded(sender.send(PeerRequest::Handle(proxy)));
        assert!(result.is_ok());

        let (mut stream, run_fut) = run_while(&mut exec, run_fut, wait_for_call_stream(stream));

        // Send the incoming waiting call.
        let (responder, run_fut) = run_while(&mut exec, run_fut, stream.next());
        let (client_end, _call_stream) = fidl::endpoints::create_request_stream().unwrap();
        let next_call = NextCall {
            call: Some(client_end),
            remote: Some(raw_number.to_string()),
            state: Some(CallState::IncomingWaiting),
            direction: Some(CallDirection::MobileTerminated),
            ..Default::default()
        };
        responder.unwrap().send(next_call).expect("Successfully send call information");

        let ((), run_fut) =
            run_while(&mut exec, run_fut, expect_data_received_by_peer(&mut remote, expected_ccwa));
        let ((), mut run_fut) =
            run_while(&mut exec, run_fut, expect_data_received_by_peer(&mut remote, expected_ciev));

        // Drop the peer task sender to force the PeerTask's run future to complete
        drop(sender);
        let _ = exec.run_until_stalled(&mut run_fut).expect("run_fut to complete");
    }

    #[fuchsia::test]
    fn outgoing_call_holds_active() {
        // Set up the executor.
        let mut exec = fasync::TestExecutor::new();

        // Setup the peer task with the specified SlcState to enable three way calling.
        let mut ag_features = AgFeatures::default();
        ag_features.set(AgFeatures::THREE_WAY_CALLING, true);
        let mut hf_features = HfFeatures::default();
        hf_features.set(HfFeatures::THREE_WAY_CALLING, true);
        let state = SlcState {
            ag_features,
            hf_features,
            selected_codec: Some(CodecId::MSBC),
            ..SlcState::default()
        };
        let (connection, remote) = create_and_initialize_slc(state);
        let (peer, mut sender, receiver, mut profile) = setup_peer_task(Some(connection));

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // Pass in the client end connected to the call manager
        exec.run_singlethreaded(sender.send(PeerRequest::Handle(proxy))).expect("Connecting peer");

        let (mut stream, run_fut) = run_while(&mut exec, run_fut, wait_for_call_stream(stream));

        // Send the incoming waiting call.
        let (responder, run_fut) = run_while(&mut exec, run_fut, stream.next());
        let (client_end, mut call_stream) = fidl::endpoints::create_request_stream().unwrap();
        let next_call = NextCall {
            call: Some(client_end),
            remote: Some("1234567".to_string()),
            state: Some(CallState::IncomingWaiting),
            direction: Some(CallDirection::MobileTerminated),
            ..Default::default()
        };
        responder.unwrap().send(next_call).expect("Successfully send call information");

        // Answer call.
        let (request, run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        let state_responder = match request {
            Some(Ok(CallRequest::WatchState { responder })) => responder,
            req => panic!("Expected WatchState, got {:?}", req),
        };
        state_responder.send(CallState::OngoingActive).expect("Sent OngoingActive.");
        let (sco, run_fut) =
            run_while(&mut exec, run_fut, expect_sco_connection(&mut profile, true, Ok(())));
        let _sco = sco.expect("SCO Connection.");

        // Make outgoing call from HF.
        let dial_cmd = at::Command::AtdNumber { number: String::from("7654321") };
        let mut buf = Vec::new();
        at::Command::serialize(&mut buf, &vec![dial_cmd]).expect("serialization is ok");
        let _ = remote.as_ref().write(&buf[..]).expect("channel write is ok");

        let (watch_state_req, run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        let _watch_state_resp = match watch_state_req {
            Some(Ok(CallRequest::WatchState { responder, .. })) => responder,
            req => panic!("Expected WatchState, got {:?}", req),
        };

        // Receive hold request from first call.
        let (hold_req, _run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        match hold_req {
            Some(Ok(CallRequest::RequestHold { .. })) => {}
            req => panic!("Expected RequestHold, got {:?}", req),
        };
    }

    #[fuchsia::test]
    fn connection_behavior_request_updates_state() {
        let mut exec = fasync::TestExecutor::new();
        let (peer, mut sender, receiver, mut profile) = setup_peer_task(None);

        let _peer_task = fasync::Task::local(peer.run(receiver));

        // First check that a connection is made when search results are received.
        // Send a valid search result.
        let random_channel_number = ServerChannel::try_from(4).expect("valid server channel");
        let protocol =
            Some(build_rfcomm_protocol(random_channel_number).iter().map(Into::into).collect());
        let event = ProfileEvent::SearchResult {
            id: PeerId(1),
            protocol: protocol.clone(),
            attributes: vec![],
        };
        exec.run_singlethreaded(sender.send(PeerRequest::Profile(event))).expect("Send to succeed");

        // Get a connection request on the `profile` stream.
        let result = exec.run_until_stalled(&mut profile.next());
        let _channel = match result {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Connect {
                connection:
                    bredr::ConnectParameters::Rfcomm(bredr::RfcommParameters {
                        channel: Some(sc), ..
                    }),
                responder,
                ..
            }))) => {
                assert_eq!(sc, u8::from(random_channel_number));
                let (local, remote) = Channel::create();
                let local = local.try_into().unwrap();
                responder.send(&mut Ok(local)).unwrap();

                remote
            }
            x => panic!("unexpected result: {:?}", x),
        };

        // Disable auto connect behavior
        exec.run_singlethreaded(
            sender.send(PeerRequest::Behavior(ConnectionBehavior { autoconnect: false })),
        )
        .expect("Send to succeed");

        // Connection is not made after autoconnect is disabled
        // Send search results.
        let event = ProfileEvent::SearchResult { id: PeerId(1), protocol, attributes: vec![] };
        exec.run_singlethreaded(sender.send(PeerRequest::Profile(event))).expect("Send to succeed");

        // No request is received on stream.
        let result = exec.run_until_stalled(&mut profile.next());
        assert!(result.is_pending());
    }

    #[fuchsia::test]
    fn non_rfcomm_search_result_is_ignored() {
        let mut exec = fasync::TestExecutor::new();
        let (peer, mut sender, receiver, mut profile) = setup_peer_task(None);

        let _peer_task = fasync::Task::local(peer.run(receiver));

        // No connection should be made for a non RFCOMM search result.
        // Send a valid search result with some random L2CAP protocol.
        let protocol = vec![bredr::ProtocolDescriptor {
            protocol: bredr::ProtocolIdentifier::L2Cap,
            params: vec![bredr::DataElement::Uint16(25)],
        }];
        let event = ProfileEvent::SearchResult {
            id: PeerId(1),
            protocol: Some(protocol),
            attributes: vec![],
        };
        exec.run_singlethreaded(sender.send(PeerRequest::Profile(event))).expect("Send to succeed");

        // No connection request on the `profile` stream.
        assert!(exec.run_until_stalled(&mut profile.next()).is_pending());
    }

    #[fuchsia::test]
    fn connect_request_triggers_connection() {
        let mut exec = fasync::TestExecutor::new();
        let connection = ServiceLevelConnection::new();
        let (local, mut remote) = Channel::create();
        let (peer, mut sender, receiver, _profile) = setup_peer_task(Some(connection));

        assert!(!peer.connection.connected());

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        let event_fut = sender.send(PeerRequest::Profile(ProfileEvent::PeerConnected {
            id: PeerId(0),
            protocol: vec![],
            channel: local,
        }));
        exec.run_singlethreaded(event_fut).unwrap();

        // The peer task is pending with no further work to do at this time.
        let result = exec.run_until_stalled(&mut run_fut);
        assert!(result.is_pending());

        // Closing the SLC connection will result in the completion of the peer task.
        drop(sender);

        let result = exec.run_until_stalled(&mut run_fut);
        let peer = result.expect("run to complete");
        assert!(peer.connection.connected());
        assert!(exec.run_until_stalled(&mut remote.next()).is_pending());
    }

    #[fuchsia::test]
    fn connect_request_replaces_connection() {
        let mut exec = fasync::TestExecutor::new();
        // SLC is connected at the start of the test.
        let (connection, mut old_remote) = create_and_connect_slc();
        let (peer, mut sender, receiver, _profile) = setup_peer_task(Some(connection));

        assert!(peer.connection.connected());

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // create a new connection for the SLC
        let (local, mut new_remote) = Channel::create();
        let event_fut = sender.send(PeerRequest::Profile(ProfileEvent::PeerConnected {
            id: PeerId(0),
            protocol: vec![],
            channel: local,
        }));
        exec.run_singlethreaded(event_fut).unwrap();

        // The peer task is pending with no further work to do at this time.
        let result = exec.run_until_stalled(&mut run_fut);
        assert!(result.is_pending());

        // Closing the SLC connection will result in the completion of the peer task.
        drop(sender);

        let result = exec.run_until_stalled(&mut run_fut);
        let peer = result.expect("run to complete");
        assert!(peer.connection.connected());
        let result = exec.run_until_stalled(&mut old_remote.next());
        // old_remote is closed
        assert_matches!(result, Poll::Ready(None));
        // new_remote is open
        assert!(exec.run_until_stalled(&mut new_remote.next()).is_pending());
    }

    /// Run a PeerTask until it has no more work left to do, then return it.
    /// This function generates its own PeerTask channel for convenience. This means that
    /// the channel cannot be used to send messages to a running PeerTask.
    #[track_caller]
    fn run_peer_until_stalled(exec: &mut fasync::TestExecutor, peer: PeerTask) -> PeerTask {
        let (_sender, receiver) = mpsc::channel(0);
        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);
        exec.run_until_stalled(&mut run_fut)
            .expect_pending("shouldn't be done while _sender is live");
        drop(_sender);
        exec.run_until_stalled(&mut run_fut).unwrap()
    }

    async fn expect_sco_connection(
        profile_requests: &mut ProfileRequestStream,
        expected_initiator: bool,
        result: Result<(), ScoErrorCode>,
    ) -> Option<bredr::ScoConnectionRequestStream> {
        // All parameter sets accepted
        use bredr::HfpParameterSet::*;
        let accept_parameter_sets = vec![MsbcT2, MsbcT1, CvsdS4, CvsdS1, CvsdD1, CvsdD0];
        expect_sco_connection_with_parameters(
            profile_requests,
            expected_initiator,
            &accept_parameter_sets,
            result,
        )
        .await
    }

    async fn expect_sco_connection_with_parameters(
        profile_requests: &mut ProfileRequestStream,
        expected_initiator: bool,
        accept_parameter_sets: &Vec<bredr::HfpParameterSet>,
        result: Result<(), ScoErrorCode>,
    ) -> Option<bredr::ScoConnectionRequestStream> {
        // Sometimes dropping the SCO connection accept future doesn't cancel the
        // existing request before we want to connect a new one.  In this case, we
        // may get *two* requests for a SCO connection, and we want the second one,
        // which has the expected direction.
        loop {
            tracing::info!("Waiting for a SCO connection");
            let (proxy, params) = match profile_requests.next().await.expect("request").unwrap() {
                bredr::ProfileRequest::ConnectSco { receiver, params, initiator, .. } => {
                    if initiator != expected_initiator {
                        tracing::warn!("Skipping because we expect {initiator} to match expected {expected_initiator}");
                        continue;
                    };
                    assert!(params.len() >= 1);
                    (receiver.into_proxy().unwrap(), params)
                }
                x => panic!("Unexpected request to profile stream: {:?}", x),
            };
            tracing::info!("Got a SCO connection: {params:?}");
            match result {
                Ok(()) => {
                    let (client, request_stream) =
                        fidl::endpoints::create_request_stream::<bredr::ScoConnectionMarker>()
                            .expect("request stream");
                    let accepted_params = params.into_iter().find(|p| {
                        p.parameter_set
                            .map(|set| accept_parameter_sets.contains(&set))
                            .unwrap_or(false)
                    });
                    if let Some(connect_params) = accepted_params {
                        proxy.connected(client, &connect_params).unwrap();
                        return Some(request_stream);
                    } else {
                        proxy.error(bredr::ScoErrorCode::ParametersRejected).unwrap();
                        return None;
                    }
                }
                Err(code) => {
                    proxy.error(code).unwrap();
                    return None;
                }
            }
        }
    }

    /// Setup a new audio connection between the PeerTask and an upstream ProfileRequestStream.
    /// This helper function asserts that the SCO connection is created and that the audio
    /// connection has started up.
    #[track_caller]
    fn setup_audio(
        exec: &mut fasync::TestExecutor,
        peer: &mut PeerTask,
        profile_requests: &mut ProfileRequestStream,
    ) -> bredr::ScoConnectionRequestStream {
        let codecs = peer.get_codecs();
        let sco_connector = peer.sco_connector.clone();
        let audio_connection_fut = sco_connector.connect(peer.id.clone(), codecs).fuse();
        pin_mut!(audio_connection_fut);

        exec.run_until_stalled(&mut audio_connection_fut).expect_pending("shouldn't be done yet");

        // Expect a sco connection, and have it succeed.
        let sco_complete_fut = expect_sco_connection(profile_requests, true, Ok(()));
        pin_mut!(sco_complete_fut);
        let result = exec.run_singlethreaded(&mut futures::future::select(
            audio_connection_fut,
            sco_complete_fut,
        ));

        let (remote_sco, mut audio_connection_fut) = match result {
            Either::Right(r) => r,
            Either::Left(_) => panic!("Audio connection future shouldn't have finished"),
        };

        let res = exec.run_until_stalled(&mut audio_connection_fut).expect("should be done");
        let local_sco = res.expect("should have started up okay");
        let audio_connection_fut2 = peer.finish_sco_connection(local_sco);
        pin_mut!(audio_connection_fut2);
        exec.run_singlethreaded(&mut audio_connection_fut2).expect("finished");

        remote_sco.unwrap()
    }

    #[fuchsia::test]
    fn setup_audio_connection_connects_and_starts_audio() {
        let mut exec = fasync::TestExecutor::new();
        // SLC is connected at the start of the test.
        let (connection, _old_remote) = create_and_connect_slc();
        let (mut peer, _sender, _receiver, mut profile_requests) =
            setup_peer_task(Some(connection));

        assert!(peer.connection.connected());

        let audio_control = peer.audio_control.clone();

        let _remote_sco = setup_audio(&mut exec, &mut peer, &mut profile_requests);

        // Should have started up the test audio control. Test by trying to start it again, it
        // should be an error.
        {
            let mut lock = audio_control.lock();
            let (connection, _stream) = connection_for_codec(CodecId::CVSD, false);
            let _ = lock
                .start(PeerId(0), connection, CodecId::CVSD)
                .expect_err("shouldn't be able to start, already started");
        }
    }

    #[fuchsia::test]
    fn renegotiaties_when_hq_sco_fails_to_connect() {
        // Set up the executor.
        let mut exec = fasync::TestExecutor::new();

        // Setup the peer task with the specified SlcState to enable codec negotiation and WBS
        let mut ag_features = AgFeatures::default();
        ag_features.set(AgFeatures::CODEC_NEGOTIATION, true);
        let mut hf_features = HfFeatures::default();
        hf_features.set(HfFeatures::CODEC_NEGOTIATION, true);
        let state = SlcState {
            ag_features,
            hf_features,
            hf_supported_codecs: Some(vec![CodecId::CVSD, CodecId::MSBC]),
            ..SlcState::default()
        };
        let (connection, mut remote) = create_and_initialize_slc(state);
        let (peer, mut sender, receiver, mut profile) = setup_peer_task(Some(connection));

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();

        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // Pass in the client end connected to the call manager
        exec.run_singlethreaded(sender.send(PeerRequest::Handle(proxy))).expect("Connecting peer");

        let (mut stream, run_fut) = run_while(&mut exec, run_fut, wait_for_call_stream(stream));

        // Send the incoming waiting call.
        let (responder, run_fut) = run_while(&mut exec, run_fut, stream.next());
        let (client_end, mut call_stream) = fidl::endpoints::create_request_stream().unwrap();
        let next_call = NextCall {
            call: Some(client_end),
            remote: Some("1234567".to_string()),
            state: Some(CallState::IncomingWaiting),
            direction: Some(CallDirection::MobileTerminated),
            ..Default::default()
        };
        responder.unwrap().send(next_call).expect("Successfully send call information");

        // Answer call to initiate audio connection.
        let (request, run_fut) = run_while(&mut exec, run_fut, &mut call_stream.next());
        let state_responder = match request {
            Some(Ok(CallRequest::WatchState { responder })) => responder,
            req => panic!("Expected WatchState, got {:?}", req),
        };
        state_responder.send(CallState::OngoingActive).expect("Sent OngoingActive.");

        // We expect to choose MSBC first.
        let choose_msbc = vec![at::success(at::Success::Bcs { codec: CodecId::MSBC.into() })];
        let ((), run_fut) =
            run_while(&mut exec, run_fut, expect_data_received_by_peer(&mut remote, choose_msbc));

        let codec_confirm_cmd = at::Command::Bcs { codec: CodecId::MSBC.into() };
        let mut buf = Vec::new();
        at::Command::serialize(&mut buf, &vec![codec_confirm_cmd]).expect("serialization is ok");
        let _ = remote.as_ref().write(&buf[..]).expect("channel write is ok");

        // First SCO connections fail (T2 and T1), causing a re-negotiation of the codec state.
        let accepted_paramset = vec![bredr::HfpParameterSet::CvsdS4];
        let (sco, run_fut) = run_while(
            &mut exec,
            run_fut,
            expect_sco_connection_with_parameters(&mut profile, true, &accepted_paramset, Ok(())),
        );
        assert!(sco.is_none(), "expected sco to fail");

        // Now, we re-negotiate with CVSD.
        // "OK" is from previous choice (+BCS=2, AT+BCS=2, OK)
        let choose_cvsd =
            vec![at::Response::Ok, at::success(at::Success::Bcs { codec: CodecId::CVSD.into() })];
        let ((), run_fut) =
            run_while(&mut exec, run_fut, expect_data_received_by_peer(&mut remote, choose_cvsd));

        let codec_confirm_cmd = at::Command::Bcs { codec: CodecId::CVSD.into() };
        let mut buf = Vec::new();
        at::Command::serialize(&mut buf, &vec![codec_confirm_cmd]).expect("serialization is ok");
        let _ = remote.as_ref().write(&buf[..]).expect("channel write is ok");

        // Expect a connection with the CVSD params.
        let (sco, mut run_fut) = run_while(
            &mut exec,
            run_fut,
            expect_sco_connection_with_parameters(&mut profile, true, &accepted_paramset, Ok(())),
        );
        assert!(sco.is_some(), "expected sco to succeed with CVSD");

        // Drop the peer task sender to force the PeerTask's run future to complete
        drop(sender);
        while let None = exec.run_one_step(&mut run_fut) {}
    }

    #[fuchsia::test]
    fn audio_is_stopped_when_sco_connection_closes() {
        let mut exec = fasync::TestExecutor::new();
        // SLC is connected at the start of the test.
        let (connection, _old_remote) = create_and_connect_slc();
        let (mut peer, _sender, receiver, mut profile_requests) = setup_peer_task(Some(connection));

        assert!(peer.connection.connected());

        let audio_control = peer.audio_control.clone();

        let remote_sco = setup_audio(&mut exec, &mut peer, &mut profile_requests);

        // Should have started up the test audio control.
        {
            let mut lock = audio_control.lock();
            let (connection, _stream) = connection_for_codec(CodecId::CVSD, false);
            let _ = lock
                .start(PeerId(0), connection, CodecId::CVSD)
                .expect_err("shouldn't be able to start, already started");
        }

        // Set up the run task.
        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);
        let _ = exec.run_until_stalled(&mut run_fut);

        drop(remote_sco);

        // Spin the run task to notice that the SCO connection has failed, and drop
        // the audio / stop the audio.
        let _ = exec.run_until_stalled(&mut run_fut);

        // Should have stopped the audio - check by trying to stop it again, it should be an error
        let mut lock = audio_control.lock();
        let _ = lock.stop().expect_err("should already be stopped");
    }

    #[fuchsia::test]
    fn sco_connection_closed_when_call_ends() {
        let mut exec = fasync::TestExecutor::new();
        // SLC is connected at the start of the test.
        let (connection, _old_remote) = create_and_connect_slc();
        let (mut peer, _sender, _receiver, mut profile_requests) =
            setup_peer_task(Some(connection));

        assert!(peer.connection.connected());

        let _remote_sco = setup_audio(&mut exec, &mut peer, &mut profile_requests);

        // Run the PeerTask to handle all audio setup tasks
        let peer = run_peer_until_stalled(&mut exec, peer);

        assert!(peer.sco_state.is_active());

        // Create the Call Manager side of a PeerHandler to send a call state update to the
        // `OngoingHeld` state in order to tear down the active sco connection.
        // Once call_manager does the work required by the test, it completes, returning items that
        // should be kept alive for the remainder of the test.
        async fn call_manager(
            stream: PeerHandlerRequestStream,
        ) -> (impl Stream<Item = PeerHandlerWatchNextCallResponder>, CallRequestStream) {
            let mut stream = wait_for_call_stream(stream).await;

            // Send the held call response
            let responder = stream.next().await.unwrap();
            let (client_end, call_stream) = fidl::endpoints::create_request_stream().unwrap();
            let next_call = NextCall {
                call: Some(client_end),
                remote: Some("1234567".to_string()),
                state: Some(CallState::OngoingHeld),
                direction: Some(CallDirection::MobileTerminated),
                ..Default::default()
            };
            responder.send(next_call).expect("Successfully send call information");

            (stream, call_stream)
        }

        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<PeerHandlerMarker>().unwrap();
        let call_manager_fut = call_manager(stream);
        pin_mut!(call_manager_fut);

        let (mut sender, receiver) = mpsc::channel(0);
        // Wire up the HFP side of PeerHandler by passing the proxy into the PeerTask.
        let handle_fut = sender.send(PeerRequest::Handle(proxy));

        // Join the futures that are responsible for sending events _into_ the PeerTask.
        let join_fut = futures::future::join(call_manager_fut, handle_fut);

        // Create the run future to drive the PeerTask forward.
        let run_fut = peer.run(receiver);
        pin_mut!(run_fut);

        // Run until `join_fut` completes.
        let (_stream, mut run_fut) = run_while(&mut exec, run_fut, join_fut);

        // Make sure all work that is pending in the PeerTask `run` future completes,
        // forcing the future to complete and return the `PeerTask` object.
        assert!(exec.run_until_stalled(&mut run_fut).is_pending());
        // Force `run_fut` to complete by dropping the `sender`.
        drop(sender);
        let peer = exec.run_singlethreaded(run_fut);

        // The SCO connection should be removed after the PeerTask has handled the Terminated call
        // update.
        assert!(!peer.sco_state.is_active());
    }
}
