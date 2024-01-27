// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::format_err;
use bt_rfcomm::frame::mux_commands::*;
use bt_rfcomm::frame::{CommandResponse, Frame, FrameData, FrameParseError, UIHData, UserData};
use bt_rfcomm::{Role, ServerChannel, DLCI};
use fidl_fuchsia_bluetooth::ErrorCode;
use fuchsia_async as fasync;
use fuchsia_bluetooth::types::{Channel, PeerId};
use fuchsia_inspect as inspect;
use fuchsia_inspect_derive::{AttachError, Inspect};
use futures::channel::{mpsc, oneshot};
use futures::future::{BoxFuture, Shared};
use futures::lock::Mutex;
use futures::{select, FutureExt, SinkExt, StreamExt};
use packet_encoding::Encodable;
use std::collections::{hash_map::Entry, HashMap};
use std::{convert::TryInto, sync::Arc};
use tracing::{error, info, trace, warn};

/// RFCOMM channels used to communicate with profile clients.
pub mod channel;

/// The multiplexer that manages RFCOMM channels for this session.
pub mod multiplexer;

use self::{
    channel::{Credits, FlowControlMode, FlowControlledData},
    multiplexer::{SessionMultiplexer, SessionParameters},
};
use crate::rfcomm::inspect::SessionInspect;
use crate::rfcomm::types::{Error, SignaledTask};

/// A function used to relay an opened inbound RFCOMM channel to a local client.
type ChannelOpenedFn = Box<
    dyn Fn(ServerChannel, Channel) -> BoxFuture<'static, Result<(), anyhow::Error>> + Send + Sync,
>;

/// Represents the callback for a pending open channel request initiated by a local client.
type ChannelRequestFn =
    Box<dyn FnOnce(Result<Channel, ErrorCode>) -> Result<(), anyhow::Error> + Send + Sync>;

/// Maintains the set of outstanding frames that have been sent to the remote peer.
/// Provides an API for inserting and removing sent Frames that expect a response.
struct OutstandingFrames {
    /// Outstanding command frames that have been sent to the remote peer and are awaiting
    /// responses. These are non-UIH frames. Per GSM 7.10 5.4.4.1, there shall only be one
    /// such outstanding command frame per DLCI.
    commands: HashMap<DLCI, Frame>,

    /// Outstanding mux command frames that have been sent to the remote peer and are awaiting
    /// responses. Per RFCOMM 5.5, there can be multiple outstanding mux command frames
    /// awaiting responses. However, there can only be one of each type per DLCI. Some
    /// MuxCommands are associated with a DLCI - we uniquely identify such a command by it's
    /// optional DLCI and command type. See the `mux_commands` mod for more details.
    mux_commands: HashMap<MuxCommandIdentifier, MuxCommand>,
}

impl OutstandingFrames {
    fn new() -> Self {
        Self { commands: HashMap::new(), mux_commands: HashMap::new() }
    }

    /// Potentially registers a new `frame` with the `OutstandingFrames` manager. Returns
    /// true if the frame requires a response and is registered, false if no response
    /// is needed, or an error if the frame was unable to be processed.
    fn register_frame(&mut self, frame: &Frame) -> Result<bool, Error> {
        // We don't care about Response frames as we don't expect a response for them.
        if frame.command_response == CommandResponse::Response {
            return Ok(false);
        }

        // MuxCommands are a special case. Namely, there cannot be multiple outstanding
        // MuxCommands of the same type on the same DLCI. Our implementation will not
        // attempt to send duplicate MuxCommands.
        if let FrameData::UnnumberedInfoHeaderCheck(UIHData::Mux(data)) = &frame.data {
            return match self.mux_commands.entry(data.identifier()) {
                Entry::Occupied(_) => Err(Error::Other(format_err!("MuxCommand outstanding"))),
                Entry::Vacant(entry) => {
                    let _ = entry.insert(data.clone());
                    Ok(true)
                }
            };
        }

        // There can be multiple outstanding user data frames as no response is required.
        if let FrameData::UnnumberedInfoHeaderCheck(UIHData::User(_)) = &frame.data {
            return Ok(false);
        }

        // Otherwise, it's a non-UIH frame. We only care about frames that require a
        // response (i.e Command frames with the P bit set).
        // See GSM 5.4.4.1 and 5.4.4.2 for the exact interpretation of the poll_final bit.
        if frame.poll_final {
            if self.commands.contains_key(&frame.dlci) {
                // There can only be one outstanding command frame with P/F = 1 per
                // DLCI.
                // TODO(fxbug.dev/60900): Our implementation should never try to send
                // more than one command frame on the same DLCI. However, it may make
                // sense to make this more intelligent and queue for later.
                return Err(Error::Other(format_err!("Command Frame outstanding")));
            }
            let _ = self.commands.insert(frame.dlci, frame.clone());
            return Ok(true);
        }
        Ok(false)
    }

    /// Attempts to find and remove the outstanding command frame associated with
    /// the provided `dlci`. Returns None if no such frame exists.
    fn remove_frame(&mut self, dlci: &DLCI) -> Option<Frame> {
        self.commands.remove(dlci)
    }

    /// Attempts to find and remove the outstanding MuxCommand associated with the
    /// provided `key`. Returns None if no such MuxCommand exists.
    fn remove_mux_command(&mut self, key: &MuxCommandIdentifier) -> Option<MuxCommand> {
        self.mux_commands.remove(key)
    }
}

/// An RFCOMM Session that multiplexes multiple channels over a single channel. This object
/// handles the business logic for an RFCOMM Session. Namely, it parses and handles RFCOMM
/// frames, modifies the state and role of the Session, and multiplexes any opened
/// RFCOMM channels.
///
/// `SessionInner::process_incoming_frames` is the data path for any incoming packets
/// received from the remote peer connected to this session. An owner of the `SessionInner`
/// should use `SessionInner::process_incoming_frames` to start processing the aforementioned
/// data. Any frames to be sent to the peer will be relayed using the `outgoing_frame_sender`
/// provided in `SessionInner::create`.
pub struct SessionInner {
    /// The session multiplexer that manages the current state of the session and any opened
    /// RFCOMM channels.
    multiplexer: SessionMultiplexer,
    /// Outstanding frames that have been sent to the remote peer and are awaiting responses.
    outstanding_frames: OutstandingFrames,
    /// Open channel requests that are waiting for either multiplexer startup, parameter
    /// negotiation, or channel establishment to complete.
    pending_channels: HashMap<ServerChannel, ChannelRequestFn>,
    /// Sender used to relay outgoing frames to be sent to the remote peer.
    outgoing_frame_sender: mpsc::Sender<Frame>,
    /// The channel opened callback that is called anytime a new RFCOMM channel is opened. The
    /// `SessionInner` will relay the client end of the channel to this closure.
    channel_opened_fn: ChannelOpenedFn,
    /// The inspect node for this object.
    inspect: SessionInspect,
}

impl Inspect for &mut SessionInner {
    fn iattach(self, parent: &inspect::Node, name: impl AsRef<str>) -> Result<(), AttachError> {
        self.inspect.iattach(parent, name)?;
        self.multiplexer.iattach(self.inspect.node(), "multiplexer")
    }
}

impl SessionInner {
    /// Creates and returns an RFCOMM SessionInner that represents a Session between this device
    /// and a remote peer.
    /// `outgoing_frame_sender` is used to relay RFCOMM frames to be sent to the remote peer.
    /// `channel_opened_fn` is used by the `SessionInner` to relay peer-opened RFCOMM channels to
    /// local clients.
    fn create(
        id: PeerId,
        outgoing_frame_sender: mpsc::Sender<Frame>,
        channel_opened_fn: ChannelOpenedFn,
    ) -> Self {
        Self {
            multiplexer: SessionMultiplexer::create(),
            outstanding_frames: OutstandingFrames::new(),
            pending_channels: HashMap::new(),
            outgoing_frame_sender,
            channel_opened_fn,
            inspect: SessionInspect::new(id),
        }
    }

    fn multiplexer(&mut self) -> &mut SessionMultiplexer {
        &mut self.multiplexer
    }

    fn role(&self) -> Role {
        self.multiplexer.role()
    }

    /// Returns true if credit-based flow control is enabled for this session.
    fn credit_based_flow(&self) -> bool {
        self.multiplexer.credit_based_flow()
    }

    #[cfg(test)]
    fn session_parameters(&self) -> SessionParameters {
        self.multiplexer.parameters()
    }

    #[cfg(test)]
    fn session_parameters_negotiated(&self) -> bool {
        self.multiplexer.parameters_negotiated()
    }

    /// Establishes the SessionChannel for the provided `dlci`. Returns true if establishment
    /// is successful.
    /// `initiator` indicates if this session initiated the connection.
    async fn establish_session_channel(&mut self, dlci: DLCI) -> bool {
        let user_data_sender = self.outgoing_frame_sender.clone();
        match self.multiplexer().establish_session_channel(dlci, user_data_sender) {
            Ok(channel) => {
                let server_channel = dlci.try_into().unwrap();
                let result = if dlci.initiator(self.role()).expect("should be valid") {
                    self.relay_outbound_channel_result_to_client(server_channel, Ok(channel))
                } else {
                    self.relay_inbound_channel_to_client(server_channel, channel).await
                };
                if let Err(e) = result {
                    warn!("Couldn't relay channel to client: {e:?}");
                    // Close the local end of the RFCOMM channel.
                    let _ = self.multiplexer().close_session_channel(&dlci);
                    return false;
                }
                trace!("Established RFCOMM Channel with DLCI: {dlci:?}");
                true
            }
            Err(e) => {
                warn!("Couldn't establish DLCI {dlci:?}: {e:?}");
                false
            }
        }
    }

    /// Processes the pending open channel request for the provided `server_channel`.
    async fn process_channel_pending_parameter_negotiation(
        &mut self,
        server_channel: ServerChannel,
    ) -> Result<(), Error> {
        if !self.multiplexer().started() {
            return Err(Error::MultiplexerNotStarted);
        }

        if let Some(channel_open_fn) = self.pending_channels.remove(&server_channel) {
            if let Err(e) = self.open_remote_channel(server_channel, channel_open_fn).await {
                warn!("Error opening outbound remote channel {server_channel:?}: {e:?}");
            }
        }
        Ok(())
    }

    /// Processes all of the pending open channel requests that are waiting for multiplexer startup
    /// to complete.
    async fn process_channels_pending_startup(&mut self) -> Result<(), Error> {
        if !self.multiplexer().started() {
            return Err(Error::MultiplexerNotStarted);
        }

        let outstanding_channels = std::mem::take(&mut self.pending_channels);
        for (server_channel, channel_open_fn) in outstanding_channels {
            trace!("Processing outstanding open channel request: {server_channel:?}");
            if let Err(e) = self.open_remote_channel(server_channel, channel_open_fn).await {
                warn!("Error opening remote channel {server_channel:?}: {e:?}");
            }
        }
        Ok(())
    }

    /// Cancels all pending channel requests that are waiting for multiplexer startup,
    /// parameter negotiation, or establishment to complete.
    fn cancel_pending_channels(&mut self) {
        let outstanding_requests = std::mem::take(&mut self.pending_channels);
        for (_, callback) in outstanding_requests {
            // Result of the callback irrelevant as it means there is an issue with the client.
            let _ = callback(Err(ErrorCode::Canceled));
        }
    }

    /// Finishes parameter negotiation for the Session with the provided `params` and
    /// reserves the specified DLCI.
    fn finish_parameter_negotiation(&mut self, params: &ParameterNegotiationParams) {
        // Update the session-specific parameters - currently only credit-based flow control
        // and max frame size are negotiated.
        let requested_parameters = SessionParameters {
            credit_based_flow: params.credit_based_flow(),
            max_frame_size: usize::from(params.max_frame_size),
        };
        let updated_parameters = self.multiplexer().negotiate_parameters(requested_parameters);

        // Reserve the DLCI if it doesn't exist.
        let _ = self.multiplexer().find_or_create_session_channel(params.dlci);

        // Set the flow control method depending on the negotiated parameters.
        let flow_control = if updated_parameters.credit_based_flow() {
            // The credits provided in the peer's response `params` is our (local) credit count.
            // `DEFAULT_INITIAL_CREDITS` is always assigned as the peer's (remote) credit count.
            let credits = Credits::new(
                usize::from(params.initial_credits),
                usize::from(DEFAULT_INITIAL_CREDITS),
            );
            FlowControlMode::CreditBased(credits)
        } else {
            FlowControlMode::None
        };
        // The result is irrelevant because the DLCI was just created and can't be established
        // already. Setting the initial credits should always succeed.
        if let Err(e) = self.multiplexer().set_flow_control(params.dlci, flow_control) {
            warn!("Setting flow control failed: {e:?}");
        }
    }

    /// Relays the outbound `channel_result` for the provided `server_channel` to the local client
    /// who requested it. Returns an error if delivery fails or if there is no such client.
    fn relay_outbound_channel_result_to_client(
        &mut self,
        server_channel: ServerChannel,
        channel_result: Result<Channel, ErrorCode>,
    ) -> Result<(), Error> {
        if let Some(callback) = self.pending_channels.remove(&server_channel) {
            return callback(channel_result).map_err(|e| Error::Other(format_err!("{e:?}").into()));
        }
        Err(Error::Other(format_err!("No outstanding client for: {server_channel:?}").into()))
    }

    /// Relays the inbound `channel` opened for the provided `server_channel` to the local clients
    /// of the session. Returns the status of the delivery.
    async fn relay_inbound_channel_to_client(
        &self,
        server_channel: ServerChannel,
        channel: Channel,
    ) -> Result<(), Error> {
        (self.channel_opened_fn)(server_channel, channel)
            .await
            .map_err(|e| format_err!("{e:?}").into())
    }

    /// Attempts to initiate multiplexer startup by sending an SABM command over the
    /// Mux Control DLCI.
    async fn start_multiplexer(&mut self) -> Result<(), Error> {
        if self.multiplexer().started() || self.role() == Role::Negotiating {
            warn!("StartMultiplexer request when multiplexer has role: {:?}", self.role());
            return Err(Error::MultiplexerAlreadyStarted);
        }
        self.multiplexer().set_role(Role::Negotiating);

        // Send the SABM command to initiate mux startup with the remote peer.
        self.send_sabm_command(DLCI::MUX_CONTROL_DLCI).await;
        Ok(())
    }

    /// Attempts to initiate the parameter negotiation (PN) procedure as defined in RFCOMM 5.5.3
    /// for the given `dlci`.
    async fn start_parameter_negotiation(&mut self, dlci: DLCI) -> Result<(), Error> {
        if !self.multiplexer().started() {
            warn!("ParameterNegotiation request before multiplexer startup");
            return Err(Error::MultiplexerNotStarted);
        }

        let pn_params = ParameterNegotiationParams::default_command(dlci);
        let pn_command = MuxCommand {
            params: MuxCommandParams::ParameterNegotiation(pn_params),
            command_response: CommandResponse::Command,
        };
        let pn_frame = Frame::make_mux_command(self.role(), pn_command);
        self.send_frame(pn_frame).await;
        Ok(())
    }

    /// Cancels parameter negotiation for the provided `dlci`. The `dlci` should be a valid
    /// user DLCI.
    fn cancel_parameter_negotiation(&mut self, dlci: DLCI) {
        if !dlci.is_user() {
            return;
        }

        // Notify the client of the canceled request.
        let _ = self.relay_outbound_channel_result_to_client(
            dlci.try_into().unwrap(),
            Err(ErrorCode::Canceled),
        );
    }

    /// Attempts to open an RFCOMM channel for the provided `server_channel`. The result
    /// of the operation is relayed using the `channel_request_fn`.
    async fn open_remote_channel(
        &mut self,
        server_channel: ServerChannel,
        channel_request_fn: ChannelRequestFn,
    ) -> Result<(), Error> {
        // There can only be one outstanding request per `server_channel`.
        if self.pending_channels.contains_key(&server_channel) {
            let _ = channel_request_fn(Err(ErrorCode::Failed));
            return Err(Error::Other(format_err!("Request in progress").into()));
        }

        // If the multiplexer has not started yet, save the open channel request and
        // attempt to start the multiplexer.
        if !self.multiplexer().started() {
            let _ = self.pending_channels.insert(server_channel, channel_request_fn);

            // Only attempt to start the multiplexer if we're not already negotiating.
            if self.multiplexer().role() == Role::Unassigned {
                self.start_multiplexer().await?;
            }
            return Ok(());
        }

        // When opening a remote channel, the DLCI is formed by taking the ServerChannel
        // and the opposite of our role. See RFCOMM 5.4.
        let dlci = server_channel.to_dlci(self.role().opposite_role())?;

        // If the DLC parameters have not been negotiated yet, save the open channel
        // request and attempt to negotiate the parameters. Per RFCOMM 5.5.3, PN should occur
        // at least once before creation of the first DLC. This implementation chooses to do
        // PN before the creation of every DLC.
        if !self.multiplexer().dlc_parameters_negotiated(&dlci) {
            let _ = self.pending_channels.insert(server_channel, channel_request_fn);
            self.start_parameter_negotiation(dlci).await?;
            return Ok(());
        }

        if self.multiplexer().dlci_established(&dlci) {
            let _ = channel_request_fn(Err(ErrorCode::Canceled));
            return Err(Error::ChannelAlreadyEstablished(dlci));
        }

        // Otherwise, save the pending channel request and send the SABM Command to begin
        // channel establishment.
        let _ = self.pending_channels.insert(server_channel, channel_request_fn);
        self.send_sabm_command(dlci).await;
        Ok(())
    }

    /// Attempts to close the established RFCOMM Session with the remote peer by sending
    /// the Disconnect command.
    async fn close(&mut self) -> Result<(), Error> {
        // There's nothing to close if the Session Multiplexer has not been started.
        if !self.multiplexer().started() {
            return Err(Error::MultiplexerNotStarted);
        }

        // Send the disconnect command to the peer. The session will shut down when we receive
        // the acknowledgement from the peer (either UA or DM response).
        self.send_disc_command(DLCI::MUX_CONTROL_DLCI).await;
        Ok(())
    }

    /// Attempts to send a Remote Line `status` update to the remote peer. The status is associated
    /// with the established RFCOMM channel identified by the provided `server_channel` number.
    ///
    /// Returns Error if the multiplexer hasn't started or if there is no such established channel.
    async fn send_remote_line_status(
        &mut self,
        server_channel: ServerChannel,
        status: Option<RlsError>,
    ) -> Result<(), Error> {
        // It's invalid to report the line status if the multiplexer has not started.
        if !self.multiplexer().started() {
            return Err(Error::MultiplexerNotStarted);
        }

        // The RLS command refers to the status of a local channel. Therefore, the DLCI is formed
        // by taking the channel number and _our_ role.
        let dlci = server_channel.to_dlci(self.role())?;

        // Updating the line status is only valid if the DLCI has been established.
        if !self.multiplexer().dlci_established(&dlci) {
            return Err(Error::ChannelNotEstablished(dlci));
        }

        self.send_remote_line_status_command(dlci, status).await;
        Ok(())
    }

    /// Handles an SABM command over the given `dlci` and sends a response frame to the remote peer.
    ///
    /// There are two important cases:
    /// 1) Mux Control DLCI - indicates request to start up the session multiplexer.
    /// 2) User DLCI - indicates request to establish up an RFCOMM channel over the provided `dlci`.
    async fn handle_sabm_command(&mut self, dlci: DLCI) {
        trace!("Handling SABM with DLCI: {dlci:?}");
        if dlci.is_mux_control() {
            match &self.role() {
                Role::Unassigned => {
                    // Remote device has requested to start up the multiplexer, respond positively
                    // and assume the Responder role.
                    match self.multiplexer().start(Role::Responder) {
                        Ok(_) => self.send_ua_response(dlci).await,
                        Err(e) => {
                            warn!("Mux startup failed: {e:?}");
                            self.send_dm_response(dlci).await;
                        }
                    }
                }
                Role::Negotiating => {
                    // We're currently negotiating the multiplexer role. We should send a DM, and
                    // attempt to restart the multiplexer after a random interval. See RFCOMM 5.2.1
                    self.send_dm_response(dlci).await
                    // TODO(fxbug.dev/61852): When we support the INT role, we should attempt to
                    // restart the multiplexer.
                }
                _role => {
                    // Remote device incorrectly trying to start up the multiplexer when it has
                    // already started. This is invalid - send a DM to respond negatively.
                    warn!("Received SABM when multiplexer already started");
                    self.send_dm_response(dlci).await;
                }
            }
            return;
        }

        // Otherwise, it's a request to open a user channel. Attempt to establish the session
        // channel for the given DLCI. If this fails, reply with a DM response for the `dlci`.
        match dlci.validate(self.role()) {
            Err(e) => {
                warn!("Received SABM with invalid DLCI: {e:?}");
                self.send_dm_response(dlci).await;
            }
            Ok(_) => {
                if self.establish_session_channel(dlci).await {
                    self.send_ua_response(dlci).await;
                    // After positively acknowledging the established channel. Report our
                    // current modem signals to indicate we are ready.
                    self.send_modem_status_command(dlci).await;
                } else {
                    self.send_dm_response(dlci).await;
                }
            }
        }
    }

    /// Handles a multiplexer command over the Mux Control DLCI. Potentially sends a response
    /// frame to the remote peer. Returns an error if the `mux_command` couldn't be handled.
    async fn handle_mux_command(&mut self, mux_command: &MuxCommand) -> Result<(), Error> {
        trace!("Handling MuxCommand: {:?}", mux_command);

        // For responses, validate that we were expecting the response and finish the operation.
        if mux_command.command_response == CommandResponse::Response {
            return match self.outstanding_frames.remove_mux_command(&mux_command.identifier()) {
                Some(_) => {
                    match &mux_command.params {
                        MuxCommandParams::ParameterNegotiation(pn_response) => {
                            // Finish parameter negotiation based on remote peer's response.
                            self.finish_parameter_negotiation(&pn_response);
                            // Process the open channel request for the DLCI. The DLCI is guaranteed
                            // to be a valid user DLCI since we initiated the PN.
                            let server_channel = pn_response.dlci.try_into().unwrap();
                            self.process_channel_pending_parameter_negotiation(server_channel)
                                .await?;
                            Ok(())
                        }
                        MuxCommandParams::ModemStatus(_)
                        | MuxCommandParams::RemoteLineStatus(_) => Ok(()),
                        _ => {
                            // TODO(fxbug.dev/59585): We currently don't send any other mux commands,
                            // add other handlers here when implemented.
                            Err(Error::NotImplemented)
                        }
                    }
                }
                None => {
                    warn!("Received unexpected MuxCommand response: {:?}", mux_command);
                    Err(Error::Other(format_err!("Unexpected response").into()))
                }
            };
        }

        let mux_response = match &mux_command.params {
            MuxCommandParams::ParameterNegotiation(pn_command) => {
                if !pn_command.dlci.is_user() {
                    warn!("Received PN command over invalid DLCI: {:?}", pn_command.dlci);
                    self.send_dm_response(pn_command.dlci).await;
                    return Ok(());
                }

                // Update the session specific parameters.
                self.finish_parameter_negotiation(&pn_command);

                // Reply back with the negotiated parameters as a response - most parameters are
                // simply echoed.
                // Session-wide parameters: Credit-based flow & max frame size are negotiated.
                // DLC-specific parameters: Initial credit count is set to a default value.
                let mut pn_response = pn_command.clone();
                let updated_parameters = self.multiplexer().parameters();
                pn_response.credit_based_flow_handshake = if updated_parameters.credit_based_flow()
                {
                    CreditBasedFlowHandshake::SupportedResponse
                } else {
                    CreditBasedFlowHandshake::Unsupported
                };
                pn_response.max_frame_size = updated_parameters.max_frame_size() as u16;
                pn_response.initial_credits = DEFAULT_INITIAL_CREDITS;
                MuxCommandParams::ParameterNegotiation(pn_response)
            }
            MuxCommandParams::RemotePortNegotiation(command) => {
                MuxCommandParams::RemotePortNegotiation(command.response())
            }
            command => {
                // All other Mux Commands can be echoed back.
                command.clone()
            }
        };
        let response =
            MuxCommand { params: mux_response, command_response: CommandResponse::Response };
        self.send_frame(Frame::make_mux_command(self.role(), response)).await;
        Ok(())
    }

    /// Handles a Disconnect command over the provided `dlci`. Returns a flag indicating
    /// session termination.
    async fn handle_disconnect_command(&mut self, dlci: DLCI) -> bool {
        trace!("Received Disconnect for DLCI {:?}", dlci);

        let terminate_session = if dlci.is_user() {
            let pn_identifier =
                MuxCommandIdentifier(Some(dlci), MuxCommandMarker::ParameterNegotiation);
            // Peer rejected our request to negotiate parameters for the `dlci`. Cancel the
            // PN and respond positively. See RFCOMM 5.5.3.
            if self.outstanding_frames.remove_mux_command(&pn_identifier).is_some() {
                self.cancel_parameter_negotiation(dlci);
                self.send_ua_response(dlci).await;
                return false;
            }

            // Otherwise, it's a request to close the DLC.
            if !self.multiplexer().close_session_channel(&dlci) {
                warn!("Received Disc command for unopened DLCI: {:?}", dlci);
                self.send_dm_response(dlci).await;
                return false;
            }
            false
        } else {
            // Disconnect over the Mux Control DLCI; we should terminate the session.
            true
        };
        // The default response for Disconnect is a UA. See RFCOMM 5.2.2 and GSM 7.10 Section 5.3.4.
        self.send_ua_response(dlci).await;
        terminate_session
    }

    /// Handles a received user `user_data` payload with optional `credits` and routes to the RFCOMM
    /// channel specified by `dlci`.
    ///
    /// If routing fails, sends a DM response over the provided `dlci`.
    async fn handle_user_data(&mut self, dlci: DLCI, user_data: UserData, credits: Option<u8>) {
        // In general, UserData frames do not need to be acknowledged.
        if let Err(e) = self
            .multiplexer()
            .receive_user_data(dlci, FlowControlledData { user_data, credits })
            .await
        {
            // If there was an error sending the user data for any reason, we reply with
            // a DM to indicate failure.
            warn!("Couldn't relay user data: {e:?}");
            self.send_dm_response(dlci).await;
        }
    }

    /// Handles an UnnumberedAcknowledgement response over the provided `dlci`.
    /// Returns a flag indicating session termination.
    async fn handle_ua_response(&mut self, dlci: DLCI) -> bool {
        // TODO(fxbug.dev/63104): Handle UA responses for Disconnect frames sent via
        // an individual SessionChannel.
        match self.outstanding_frames.remove_frame(&dlci).map(|frame| frame.data) {
            Some(FrameData::SetAsynchronousBalancedMode) if dlci.is_mux_control() => {
                // If we are not negotiating anymore, mux startup was either canceled
                // or completed. No need to do anything.
                if self.role() != Role::Negotiating {
                    trace!(
                        "Received response when mux startup was either canceled or completed: {:?}",
                        self.role()
                    );
                    return false;
                }
                // Otherwise, assume the initiator role and complete startup. Starting should never
                // fail because we are guaranteed to be in the Negotiating role.
                self.multiplexer().start(Role::Initiator).unwrap();
                // Process any pending channels awaiting startup.
                let _ = self.process_channels_pending_startup().await;
            }
            Some(FrameData::SetAsynchronousBalancedMode) => {
                // Positive acknowledgement to open a remote channel on a user DLCI.
                if self.establish_session_channel(dlci).await {
                    // After successfully opening the RFCOMM channel, report our
                    // current Modem signals to indicate readiness.
                    self.send_modem_status_command(dlci).await;
                }
            }
            Some(FrameData::Disconnect) if dlci.is_mux_control() => {
                info!("Received UA response to Disconnect of RFCOMM Session. Shutting down...");
                return true;
            }
            Some(_) | None => warn!("Received unexpected UA response over DLCI: {:?}", dlci),
        }
        false
    }

    /// Handles a DisconnectedMode response over the provided `dlci`.
    /// Returns a flag indicating session termination.
    async fn handle_dm_response(&mut self, dlci: DLCI) -> bool {
        // See GSM 7.10 Section 5.5.3 for the usage of the DM response.
        // TODO(fxbug.dev/63104): Handle DM responses for Disconnect frames sent via
        // an individual SessionChannel.
        match self.outstanding_frames.remove_frame(&dlci).map(|frame| frame.data) {
            Some(FrameData::SetAsynchronousBalancedMode) if dlci.is_mux_control() => {
                // Peer rejected our request to start the Session multiplexer - reset and
                // let the peer retry.
                self.multiplexer().reset();
            }
            Some(FrameData::SetAsynchronousBalancedMode) => {
                // Peer rejected our request to open a user DLC - close the DLC, notify the client
                // of failure, and let the peer retry.
                let _ = self.multiplexer().close_session_channel(&dlci);
                let _ = self.relay_outbound_channel_result_to_client(
                    dlci.try_into().unwrap(),
                    Err(ErrorCode::Canceled),
                );
            }
            Some(FrameData::Disconnect) if dlci.is_mux_control() => {
                // A negative response to a Disconnect command typically means the peer is in
                // a different logical state than us. This is fatal, and we should shut down
                // the RFCOMM session.
                info!("Received DM response to Disconnect of RFCOMM Session. Shutting down...");
                return true;
            }
            Some(frame_data) => warn!("Unexpected DM for {:?}", frame_data),
            None => {
                let pn_identifier =
                    MuxCommandIdentifier(Some(dlci), MuxCommandMarker::ParameterNegotiation);
                // Special case: Peer rejected our request to negotiate parameters for the `dlci`.
                // See RFCOMM 5.5.3. Cancel the PN.
                if self.outstanding_frames.remove_mux_command(&pn_identifier).is_some() {
                    self.cancel_parameter_negotiation(dlci);
                }
            }
        }
        false
    }

    /// Handles an incoming Frame received from the peer. Returns a flag indicating whether
    /// the session should terminate, or an error if the frame was unable to be handled.
    async fn handle_frame(&mut self, frame: Frame) -> Result<bool, Error> {
        let (dlci, credits) = (frame.dlci, frame.credits);
        match frame.data {
            FrameData::SetAsynchronousBalancedMode => {
                self.handle_sabm_command(dlci).await;
            }
            FrameData::UnnumberedAcknowledgement => {
                return Ok(self.handle_ua_response(dlci).await);
            }
            FrameData::DisconnectedMode => {
                return Ok(self.handle_dm_response(dlci).await);
            }
            FrameData::Disconnect => return Ok(self.handle_disconnect_command(dlci).await),
            FrameData::UnnumberedInfoHeaderCheck(UIHData::Mux(data)) => {
                self.handle_mux_command(&data).await?;
            }
            FrameData::UnnumberedInfoHeaderCheck(UIHData::User(data)) => {
                self.handle_user_data(dlci, data, credits).await;
            }
        }
        Ok(false)
    }

    /// Handles the error case when parsing a frame and sends an optional response frame if
    /// needed.
    async fn handle_frame_parse_error(&mut self, e: FrameParseError) {
        warn!("Error parsing frame: {e:?}");
        // Currently, the only frame parsing error that requires a response is the MuxCommand
        // parsing error.
        match e {
            FrameParseError::UnsupportedMuxCommandType(val) => {
                let non_supported_response = Frame::make_mux_command(
                    self.role(),
                    MuxCommand {
                        params: MuxCommandParams::NonSupported(NonSupportedCommandParams {
                            cr_bit: true,
                            non_supported_command: val,
                        }),
                        command_response: CommandResponse::Response,
                    },
                );
                self.send_frame(non_supported_response).await;
            }
            _ => {}
        }
    }

    /// Sends a RLS command for the provided `dlci`.
    async fn send_remote_line_status_command(&mut self, dlci: DLCI, status: Option<RlsError>) {
        let mux_command = MuxCommand {
            params: MuxCommandParams::RemoteLineStatus(RemoteLineStatusParams::new(dlci, status)),
            command_response: CommandResponse::Command,
        };
        self.send_frame(Frame::make_mux_command(self.role(), mux_command)).await;
    }

    /// Sends a Modem Status command for the provided `dlci`.
    async fn send_modem_status_command(&mut self, dlci: DLCI) {
        let mux_command = MuxCommand {
            params: MuxCommandParams::ModemStatus(ModemStatusParams::default(dlci)),
            command_response: CommandResponse::Command,
        };
        self.send_frame(Frame::make_mux_command(self.role(), mux_command)).await;
    }

    /// Sends an SABM command over the provided `dlci`.
    async fn send_sabm_command(&mut self, dlci: DLCI) {
        self.send_frame(Frame::make_sabm_command(self.role(), dlci)).await
    }

    /// Sends a UA response over the provided `dlci`.
    async fn send_ua_response(&mut self, dlci: DLCI) {
        self.send_frame(Frame::make_ua_response(self.role(), dlci)).await
    }

    /// Sends a DM response over the provided `dlci`.
    async fn send_dm_response(&mut self, dlci: DLCI) {
        self.send_frame(Frame::make_dm_response(self.role(), dlci)).await
    }

    /// Sends a Disc command over the provided `dlci`.
    async fn send_disc_command(&mut self, dlci: DLCI) {
        self.send_frame(Frame::make_disc_command(self.role(), dlci)).await
    }

    /// Sends the `frame` to the remote peer using the `outgoing_frame_sender`.
    async fn send_frame(&mut self, frame: Frame) {
        // Potentially save the frame-to-be-sent in the OutstandingFrames manager. This
        // bookkeeping step will allow us to easily match received responses to our sent
        // commands.
        if let Err(e) = self.outstanding_frames.register_frame(&frame) {
            warn!("Couldn't send frame: {e:?}");
            return;
        }

        // Result of this send doesn't matter since failure indicates
        // peer disconnection.
        let _ = self.outgoing_frame_sender.send(frame).await;
    }

    /// Starts the data processing task for this RFCOMM Session.
    /// `data_receiver` is a stream of incoming data packets received from the remote peer.
    ///
    /// The lifetime of this task is tied to the `data_receiver`.
    async fn process_incoming_frames(
        inner: Arc<Mutex<Self>>,
        mut data_receiver: mpsc::Receiver<Vec<u8>>,
    ) -> Result<(), anyhow::Error> {
        while let Some(bytes) = data_receiver.next().await {
            let mut w_inner = inner.lock().await;
            match Frame::parse(w_inner.role().opposite_role(), w_inner.credit_based_flow(), &bytes)
            {
                Ok(f) => {
                    trace!("Parsed frame from peer: {f:?}");
                    match w_inner.handle_frame(f).await {
                        Ok(true) => break,
                        Ok(false) => {}
                        Err(e) => warn!("Error handling RFCOMM frame: {e:?}"),
                    }
                }
                Err(e) => {
                    w_inner.handle_frame_parse_error(e).await;
                }
            };
        }
        // The `data_receiver` has closed, indicating peer disconnection.
        let mut w_inner = inner.lock().await;
        w_inner.cancel_pending_channels();
        w_inner.inspect.disconnect();
        w_inner.outgoing_frame_sender.close_channel();
        Ok(())
    }
}

/// The maximum number of concurrent frames that can be accepted by this Session.
/// This value is chosen arbitrarily, and is not defined in the GSM or RFCOMM specifications.
/// This value is chosen as a number significantly more than the expected number of RFCOMM frames in
/// a single transaction (typically, we expect to receive 1-2 RFCOMM frames from a remote device
/// before responding).
const MAX_CONCURRENT_FRAMES: usize = 100;

/// An RFCOMM Session that multiplexes multiple channels over a single L2CAP channel.
///
/// A `Session` is represented by a processing task which processes incoming bytes
/// from the remote peer. Any multiplexed RFCOMM channels will be delivered to the
/// `clients` of the Session.
#[derive(Inspect)]
pub struct Session {
    _task: fasync::Task<()>,
    #[inspect(forward)]
    inner: Arc<Mutex<SessionInner>>,
    /// Shared termination future.
    terminated: Shared<BoxFuture<'static, ()>>,
}

impl Session {
    /// Creates a new RFCOMM Session with peer `id` over the `l2cap_channel`. Any multiplexed
    /// RFCOMM channels will be relayed using the `channel_opened_callback`.
    pub fn create(
        id: PeerId,
        l2cap_channel: Channel,
        channel_opened_callback: ChannelOpenedFn,
    ) -> Self {
        // The `session_inner` relays outgoing packets (to be sent to the remote peer) to the
        // `Session` using this mpsc::channel.
        let (frames_to_peer_sender, frame_receiver) = mpsc::channel(MAX_CONCURRENT_FRAMES);
        let session_inner = Arc::new(Mutex::new(SessionInner::create(
            id,
            frames_to_peer_sender,
            channel_opened_callback,
        )));
        let (termination_sender, receiver) = oneshot::channel();
        let terminated = receiver.map(|_| ()).boxed().shared();
        let _task = fasync::Task::spawn(Session::session_task(
            id,
            l2cap_channel,
            session_inner.clone(),
            frame_receiver,
            termination_sender,
        ));
        Self { _task, inner: session_inner, terminated }
    }

    /// Processing task that drives the work for an RFCOMM Session with a peer.
    ///
    /// 1) Drives the RFCOMM SessionInner task - this task is responsible for
    ///    RFCOMM related functionality: parsing & handling frames, modifying internal state, and
    ///    multiplexing RFCOMM channels. Any outgoing frames intended for the peer will be sent to
    ///    the `frame_receiver`.
    /// 2) Drives the peer processing task which handles incoming packets from the `l2cap` channel.
    ///    This task will relay these received packets to the `session_inner`. The task also
    ///    receives packets from the `frame_receiver` and sends them to the remote peer.
    ///
    /// The lifetime of this task is tied to the provided `l2cap` channel. When the remote peer
    /// disconnects, the `l2cap` channel will close, and therefore the task will terminate.
    async fn session_task(
        id: PeerId,
        l2cap: Channel,
        session_inner: Arc<Mutex<SessionInner>>,
        frame_receiver: mpsc::Receiver<Frame>,
        termination_sender: oneshot::Sender<()>,
    ) {
        // `Session::peer_processing_task()` uses this mpsc::channel to relay data received from the
        // peer to the `session_inner`.
        let (data_sender, data_from_peer_receiver) = mpsc::channel(MAX_CONCURRENT_FRAMES);

        // Business logic of the RFCOMM session - parsing and handling frames, modifying the state
        // of the session, and multiplexing RFCOMM channels.
        let session_inner_task =
            SessionInner::process_incoming_frames(session_inner, data_from_peer_receiver)
                .boxed()
                .fuse();
        // Processes packets of data to/from the remote peer.
        let peer_processing_task =
            Session::peer_processing_task(l2cap, frame_receiver, data_sender).boxed().fuse();

        // If the `peer_processing_task` terminates first, then the peer disconnected unexpectedly.
        // In this case, we expect the `session_inner_task` will clean up and terminate.
        // If the `session_inner_task` terminates first then a Disconnect frame was received (in
        // either direction). In this case, the finishing of the `session_inner_task` will result
        // in the `peer_processing_task` to close.
        let _ = futures::future::join(session_inner_task, peer_processing_task).await;

        // Session has finished; notify any subscribed clients.
        info!(%id, "Session with peer ended");
        let _ = termination_sender.send(());
    }

    /// Processes incoming data from the `l2cap_channel` connected to the remote peer and
    /// relays it using the `data_sender`.
    /// Processes frames-to-be-sent from the `pending_writes` queue and sends them to the
    /// remote peer.
    async fn peer_processing_task(
        mut l2cap_channel: Channel,
        mut pending_writes: mpsc::Receiver<Frame>,
        mut data_sender: mpsc::Sender<Vec<u8>>,
    ) {
        loop {
            select! {
                incoming_bytes = l2cap_channel.next().fuse() => {
                    match incoming_bytes {
                        Some(Ok(bytes)) => {
                            trace!("Received packet from peer: {:?}", bytes);
                            if let Err(_) = data_sender.send(bytes).await {
                                warn!("Couldn't send bytes to main run task");
                            }
                        },
                        Some(Err(e)) => {
                            error!("Error reading bytes from l2cap channel: {e:?}");
                        },
                        None => {
                            info!("Peer closed L2CAP connection, exiting");
                            return;
                        }
                    }
                }
                frame_to_be_written = pending_writes.next() => {
                    let frame = match frame_to_be_written {
                        Some(frame) => frame,
                        None => {
                            // The SessionInner task finished and closed its end of the channel.
                            // This means a Disconnect frame was received (in either direction), and
                            // we can terminate.
                            trace!("SessionInner task finished, closing peer_processing_task.");
                            return;
                        }
                    };
                    trace!("Sending frame to remote: {:?}", frame);
                    let mut buf = vec![0; frame.encoded_len()];
                    if let Err(e) = frame.encode(&mut buf[..]) {
                        warn!("Couldn't encode frame: {e:?}");
                        continue;
                    }
                    // The result of this send is irrelevant, as failure would
                    // indicate peer disconnection.
                    let _ = l2cap_channel.as_ref().write(&buf);
                }
                complete => { return; }
            }
        }
    }

    /// A test-only hook to initiate multiplexer startup without needing to request
    /// to open a specific RFCOMM channel.
    #[cfg(test)]
    async fn initiate_multiplexer_startup(&self) {
        let mut w_inner = self.inner.lock().await;
        if let Err(e) = w_inner.start_multiplexer().await {
            warn!("Couldn't start session multiplexer: {:?}", e);
        }
    }

    /// Requests to open a new RFCOMM channel for the provided `server_channel`.
    pub async fn open_rfcomm_channel(
        &self,
        server_channel: ServerChannel,
        channel_opened_cb: ChannelRequestFn,
    ) {
        let mut w_inner = self.inner.lock().await;
        if let Err(e) = w_inner.open_remote_channel(server_channel, channel_opened_cb).await {
            warn!("Couldn't open RFCOMM channel: {e:?}");
        }
    }

    /// Request to close the RFCOMM session.
    pub async fn close(&self) {
        let mut w_inner = self.inner.lock().await;
        if let Err(e) = w_inner.close().await {
            warn!("Couldn't close RFCOMM session: {e:?}");
        }
    }

    /// Request to send an RLS frame to the remote peer.
    pub async fn send_remote_line_status(
        &self,
        server_channel: ServerChannel,
        status: Option<RlsError>,
    ) {
        let mut w_inner = self.inner.lock().await;
        if let Err(e) = w_inner.send_remote_line_status(server_channel, status).await {
            warn!("Couldn't close RFCOMM session: {e:?}");
        }
    }
}

impl SignaledTask for Session {
    fn finished(&self) -> BoxFuture<'static, ()> {
        self.terminated.clone().boxed()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use assert_matches::assert_matches;
    use async_utils::PollExt;
    use fuchsia_async as fasync;
    use fuchsia_inspect::testing::AnyProperty;
    use futures::{pin_mut, task::Poll, Future};
    use std::convert::TryFrom;

    use crate::{rfcomm::session::multiplexer::ParameterNegotiationState, rfcomm::test_util::*};

    /// Makes a DLC PN frame with arbitrary command parameters.
    /// `command_response` indicates whether the frame should be a command or response.
    /// `dlci` is the DLCI being negotiated.
    /// `credit_flow` indicates whether credit-based flow control should be set or not.
    /// `max_frame_size` indicates the max frame size to use for the PN.
    fn make_dlc_pn_frame(
        command_response: CommandResponse,
        dlci: DLCI,
        credit_flow: bool,
        max_frame_size: u16,
    ) -> Frame {
        let credit_based_flow_handshake = match (credit_flow, command_response) {
            (false, _) => CreditBasedFlowHandshake::Unsupported,
            (true, CommandResponse::Command) => CreditBasedFlowHandshake::SupportedRequest,
            (true, CommandResponse::Response) => CreditBasedFlowHandshake::SupportedResponse,
        };
        let pn_command = MuxCommand {
            params: MuxCommandParams::ParameterNegotiation(ParameterNegotiationParams {
                dlci,
                credit_based_flow_handshake,
                priority: 12,
                max_frame_size,
                initial_credits: DEFAULT_INITIAL_CREDITS,
            }),
            command_response,
        };
        Frame {
            role: Role::Initiator,
            dlci: DLCI::MUX_CONTROL_DLCI,
            data: FrameData::UnnumberedInfoHeaderCheck(UIHData::Mux(pn_command)),
            poll_final: false,
            command_response,
            credits: None,
        }
    }

    /// Creates and returns the SessionInner processing task. Uses a channel_opened_fn that
    /// indiscriminately accepts all opened RFCOMM channels.
    fn setup_session_task() -> (impl Future<Output = ()>, Channel) {
        let id = PeerId(987);
        let (local, remote) = Channel::create();
        let channel_opened_fn = Box::new(|_server_channel, _channel| async { Ok(()) }.boxed());
        let (frame_sender, frame_receiver) = mpsc::channel(0);
        let session_inner =
            Arc::new(Mutex::new(SessionInner::create(id, frame_sender, channel_opened_fn)));
        let (sender, _receiver) = oneshot::channel();
        let session_fut =
            Session::session_task(PeerId(1), local, session_inner, frame_receiver, sender);

        (session_fut, remote)
    }

    /// Creates a ChannelOpenedFn that relays inbound RFCOMM channels using the `channel_sender`.
    /// Tests should use the returned Receiver to assert on the delivery of opened RFCOMM channels.
    fn create_inbound_relay() -> (ChannelOpenedFn, mpsc::Receiver<Result<Channel, ErrorCode>>) {
        let (channel_sender, channel_receiver) = mpsc::channel(0);
        let f = Box::new(move |_server_channel, channel| {
            let mut sender = channel_sender.clone();
            async move {
                assert!(sender.send(Ok(channel)).await.is_ok());
                Ok(())
            }
            .boxed()
        });
        (f, channel_receiver)
    }

    /// Creates a ChannelRequestFn that relays outbound RFCOMM channels using the `channel_sender`.
    /// Tests should use the returned Receiver to assert on delivery of outbound channels.
    fn create_outbound_relay() -> (ChannelRequestFn, mpsc::Receiver<Result<Channel, ErrorCode>>) {
        let (channel_sender, channel_receiver) = mpsc::channel(0);
        let f = Box::new(move |channel: Result<Channel, ErrorCode>| {
            let mut sender = channel_sender.clone();
            assert!(sender.try_send(channel).is_ok());
            Ok(())
        });
        (f, channel_receiver)
    }

    /// Creates and returns 1) A SessionInner 2) A stream of outgoing frames to be sent to the
    /// remote peer. Use this to validate SessionInner behavior and 3) A stream of opened RFCOMM
    /// channels. Use this to validate channel establishment.
    fn setup_session(
    ) -> (SessionInner, mpsc::Receiver<Frame>, mpsc::Receiver<Result<Channel, ErrorCode>>) {
        let id = PeerId(5);
        let (channel_opened_fn, channel_receiver) = create_inbound_relay();
        let (outgoing_frame_sender, outgoing_frames) = mpsc::channel(0);
        let session = SessionInner {
            multiplexer: SessionMultiplexer::create(),
            outstanding_frames: OutstandingFrames::new(),
            pending_channels: HashMap::new(),
            outgoing_frame_sender,
            channel_opened_fn,
            inspect: SessionInspect::new(id),
        };
        (session, outgoing_frames, channel_receiver)
    }

    /// Handles the provided `frame` and expects the `expected` frame data to be sent to
    /// the provided `outgoing_frames` receiver.
    #[track_caller]
    fn handle_and_expect_frame(
        exec: &mut fasync::TestExecutor,
        session: &mut SessionInner,
        outgoing_frames: &mut mpsc::Receiver<Frame>,
        frame: Frame,
        expected: FrameData,
    ) {
        let mut handle_fut = Box::pin(session.handle_frame(frame));
        exec.run_until_stalled(&mut handle_fut).expect_pending("waiting for outgoing frame");
        expect_frame(exec, outgoing_frames, expected, None);
        assert!(exec.run_until_stalled(&mut handle_fut).is_ready());
    }

    /// Expects and returns the `channel` from the provided `receiver`.
    #[track_caller]
    fn expect_channel(
        exec: &mut fasync::TestExecutor,
        receiver: &mut mpsc::Receiver<Result<Channel, ErrorCode>>,
    ) -> Channel {
        let mut channel_fut = Box::pin(receiver.next());
        match exec.run_until_stalled(&mut channel_fut) {
            Poll::Ready(Some(Ok(channel))) => channel,
            x => panic!("Expected a channel but got {:?}", x),
        }
    }

    /// Expects a cancellation Error over the provided `receiver`.
    #[track_caller]
    fn expect_channel_error(
        exec: &mut fasync::TestExecutor,
        receiver: &mut mpsc::Receiver<Result<Channel, ErrorCode>>,
        expected_error: ErrorCode,
    ) {
        let mut channel_fut = Box::pin(receiver.next());
        match exec.run_until_stalled(&mut channel_fut) {
            Poll::Ready(Some(Err(e))) => assert_eq!(e, expected_error),
            x => panic!("Expected ready error but got {:?}", x),
        }
    }

    #[test]
    fn test_outstanding_frame_manager() {
        let mut outstanding_frames = OutstandingFrames::new();

        // Always sets poll_final = true, and therefore requires a response.
        let sabm_command = Frame::make_sabm_command(Role::Unassigned, DLCI::MUX_CONTROL_DLCI);
        assert_matches!(outstanding_frames.register_frame(&sabm_command), Ok(true));
        // Inserting the same frame on the same DLCI should be rejected since
        // there is already one outstanding.
        assert_matches!(outstanding_frames.register_frame(&sabm_command), Err(_));

        // Inserting same type of frame, but different DLCI is OK.
        let user_sabm = Frame::make_sabm_command(Role::Initiator, DLCI::try_from(3).unwrap());
        assert_matches!(outstanding_frames.register_frame(&user_sabm), Ok(true));

        // Response frames shouldn't be registered.
        let ua_response = Frame::make_ua_response(Role::Responder, DLCI::MUX_CONTROL_DLCI);
        assert_matches!(outstanding_frames.register_frame(&ua_response), Ok(false));

        // Random DLCI - no frame should exist.
        let random_dlci = DLCI::try_from(8).unwrap();
        assert_eq!(outstanding_frames.remove_frame(&random_dlci), None);
        // SABM command should be retrievable.
        assert_eq!(outstanding_frames.remove_frame(&DLCI::MUX_CONTROL_DLCI), Some(sabm_command));

        // User data frames shouldn't ever be registered as they require no response. In particular,
        // the `poll_final` bit is redefined for UserData frames.
        let user_data = Frame::make_user_data_frame(
            Role::Initiator,
            random_dlci,
            UserData { information: vec![] },
            Some(10), // Random amount of credits
        );
        assert_matches!(outstanding_frames.register_frame(&user_data), Ok(false));

        // Two different MuxCommands on the same DLCI is OK.
        let data = MuxCommand {
            params: MuxCommandParams::FlowControlOff(FlowControlParams {}),
            command_response: CommandResponse::Command,
        };
        let mux_command = Frame::make_mux_command(Role::Initiator, data);
        assert_matches!(outstanding_frames.register_frame(&mux_command), Ok(true));
        let data2 = MuxCommand {
            params: MuxCommandParams::FlowControlOn(FlowControlParams {}),
            command_response: CommandResponse::Command,
        };
        let mux_command2 = Frame::make_mux_command(Role::Initiator, data2.clone());
        assert_matches!(outstanding_frames.register_frame(&mux_command2), Ok(true));
        // Removing MuxCommand is OK.
        assert_eq!(outstanding_frames.remove_mux_command(&data2.identifier()), Some(data2));

        // Same MuxCommand but on different DLCIs is OK.
        let user_dlci1 = DLCI::try_from(10).unwrap();
        let data1 = MuxCommand {
            params: MuxCommandParams::ParameterNegotiation(
                ParameterNegotiationParams::default_command(user_dlci1),
            ),
            command_response: CommandResponse::Command,
        };
        let mux_command1 = Frame::make_mux_command(Role::Initiator, data1);
        assert_matches!(outstanding_frames.register_frame(&mux_command1), Ok(true));
        let user_dlci2 = DLCI::try_from(15).unwrap();
        let data2 = MuxCommand {
            params: MuxCommandParams::ParameterNegotiation(
                ParameterNegotiationParams::default_command(user_dlci2),
            ),
            command_response: CommandResponse::Command,
        };
        let mux_command2 = Frame::make_mux_command(Role::Initiator, data2);
        assert_matches!(outstanding_frames.register_frame(&mux_command2), Ok(true));
    }

    #[test]
    fn test_session_inner_inspect() {
        let mut exec = fasync::TestExecutor::new();
        let inspect = inspect::Inspector::new();

        // Setup SessionInner with inspect.
        let (data_sender, data_receiver) = mpsc::channel(0);
        let (mut inner, _outgoing_frames, _inbound_channels) = setup_session();
        inner.iattach(inspect.root(), "session_test").expect("should attach to inspect tree");
        let session = Arc::new(Mutex::new(inner));
        // Default inspect tree.
        fuchsia_inspect::assert_data_tree!(inspect, root: {
            session_test: contains {
                peer_id: AnyProperty,
                connected: "Connected",
            },
        });

        // Run the Session task.
        let mut session_task =
            Box::pin(SessionInner::process_incoming_frames(session.clone(), data_receiver));
        exec.run_until_stalled(&mut session_task)
            .expect_pending("shouldn't be done while data_sender is live");

        // Simulate peer disconnection.
        drop(data_sender);
        assert_matches!(exec.run_until_stalled(&mut session_task), Poll::Ready(Ok(_)));
        // Inspect when Session is not active.
        fuchsia_inspect::assert_data_tree!(inspect, root: {
            session_test: contains {
                peer_id: AnyProperty,
                connected: "Disconnected",
            }
        });
    }

    #[test]
    fn test_register_l2cap_channel() {
        let mut exec = fasync::TestExecutor::new();

        let (processing_fut, remote) = setup_session_task();
        pin_mut!(processing_fut);
        exec.run_until_stalled(&mut processing_fut)
            .expect_pending("shouldn't be done while remote is live");

        drop(remote);
        exec.run_until_stalled(&mut processing_fut).expect("should be done");
    }

    #[test]
    fn test_receiving_data_is_ok() {
        let mut exec = fasync::TestExecutor::new();

        let (processing_fut, remote) = setup_session_task();
        pin_mut!(processing_fut);
        exec.run_until_stalled(&mut processing_fut)
            .expect_pending("shouldn't be done while remote is live");

        // Remote sends us some data. Even though this is an invalid Frame,
        // the `processing_fut` should still be OK.
        let frame_bytes = [0x03, 0x3F, 0x01, 0x1C];
        assert_eq!(remote.as_ref().write(&frame_bytes[..]), Ok(4));

        exec.run_until_stalled(&mut processing_fut)
            .expect_pending("shouldn't be done while remote is live");
    }

    #[test]
    fn test_peer_disconnection_notifies_termination_future() {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(992);
        let (local, remote) = Channel::create();
        let channel_opened_fn = Box::new(|_server_channel, _channel| async { Ok(()) }.boxed());
        let session = Session::create(id, local, channel_opened_fn);

        // Session should still be active.
        let mut closed_fut = session.finished();
        exec.run_until_stalled(&mut closed_fut)
            .expect_pending("shouldn't be done while remote is live");

        // Peer disconnects - the termination future should resolve.
        drop(remote);
        assert!(exec.run_until_stalled(&mut closed_fut).is_ready());

        // Trying to check again if the Session has terminated is OK - should resolve immediately.
        let mut closed_fut2 = session.finished();
        assert!(exec.run_until_stalled(&mut closed_fut2).is_ready());

        // Although unlikely, checking after the Session has gone out of scope resolves immediately.
        let mut closed_fut3 = session.finished();
        drop(session);
        assert!(exec.run_until_stalled(&mut closed_fut3).is_ready());
    }

    #[test]
    fn test_receiving_user_sabm_before_mux_startup_is_rejected() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();
        assert_eq!(session.role(), Role::Unassigned);

        // Expect a DM response due to user DLCI SABM before Mux DLCI SABM.
        let sabm = Frame::make_sabm_command(Role::Initiator, DLCI::try_from(3).unwrap());
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            sabm,
            FrameData::DisconnectedMode,
        );
    }

    #[test]
    fn test_receiving_mux_sabm_starts_multiplexer_with_ua_response() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();

        // Remote sends us an SABM command - expect a positive UA response.
        let sabm = Frame::make_sabm_command(Role::Unassigned, DLCI::MUX_CONTROL_DLCI);
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            sabm,
            FrameData::UnnumberedAcknowledgement,
        );

        // The multiplexer for this session should be started and assume the Responder role.
        assert!(session.multiplexer().started());
        assert_eq!(session.role(), Role::Responder);
    }

    #[test]
    fn test_receiving_mux_sabm_after_mux_startup_is_rejected() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        // Remote sends us a SABM command on the Mux Control DLCI after the multiplexer has
        // already started. We expect to reject this with a DM response.
        let sabm = Frame::make_sabm_command(Role::Initiator, DLCI::MUX_CONTROL_DLCI);
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            sabm,
            FrameData::DisconnectedMode,
        );
    }

    #[test]
    fn test_receiving_multiple_pn_commands_results_in_set_parameters() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();
        assert!(!session.session_parameters_negotiated());
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        // Remote initiates DLC PN over a random user DLCI - expect to reply with a DLCPN response.
        let random_dlci = DLCI::try_from(3).unwrap();
        let dlcpn = make_dlc_pn_frame(CommandResponse::Command, random_dlci, false, 64);
        let expected_response =
            make_dlc_pn_frame(CommandResponse::Response, random_dlci, false, 64);
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            dlcpn,
            expected_response.data,
        );

        // The global session parameters should be set.
        let expected_parameters =
            SessionParameters { credit_based_flow: false, max_frame_size: 64 };
        assert_eq!(session.session_parameters(), expected_parameters);

        // Multiple DLC PN requests before a DLC is established is OK - new parameters.
        let dlcpn = make_dlc_pn_frame(CommandResponse::Command, random_dlci, true, 11);
        let expected_response = make_dlc_pn_frame(CommandResponse::Response, random_dlci, true, 11);
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            dlcpn,
            expected_response.data,
        );

        // The global session parameters should be updated.
        let expected_parameters = SessionParameters { credit_based_flow: true, max_frame_size: 11 };
        assert_eq!(session.session_parameters(), expected_parameters);
    }

    #[test]
    fn test_dlcpn_renegotiation_does_not_update_parameters() {
        let mut exec = fasync::TestExecutor::new();

        // Create and start a SessionInner that relays any opened RFCOMM channels.
        let (mut session, mut outgoing_frames, mut channel_receiver) = setup_session();
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        // Remote peer initiates DLC PN over a random DLCI - expect to reply with a PN.
        let random_dlci = DLCI::try_from(3).unwrap();
        let dlcpn = make_dlc_pn_frame(CommandResponse::Command, random_dlci, true, 100);
        let expected_response =
            make_dlc_pn_frame(CommandResponse::Response, random_dlci, true, 100);
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            dlcpn,
            expected_response.data,
        );

        // The global session parameters should be set.
        let expected_parameters =
            SessionParameters { credit_based_flow: true, max_frame_size: 100 };
        assert_eq!(session.session_parameters(), expected_parameters);

        // Remote peer sends SABM over a user DLCI - this will establish the DLCI.
        let generic_dlci = 6;
        let user_dlci = DLCI::try_from(generic_dlci).unwrap();
        let user_sabm = Frame::make_sabm_command(Role::Initiator, user_dlci);
        let _channel = {
            let mut handle_fut = Box::pin(session.handle_frame(user_sabm));
            exec.run_until_stalled(&mut handle_fut).expect_pending("waiting for channel delivery");
            // We expect a channel to be delivered from the`channel_opened_fn`.
            let c = expect_channel(&mut exec, &mut channel_receiver);
            // Continue to run the `handle_frame` to process the result of the channel delivery.
            exec.run_until_stalled(&mut handle_fut).expect_pending("waiting for outgoing frame");
            // We expect to respond to the peer with a positive UA.
            expect_frame(
                &mut exec,
                &mut outgoing_frames,
                FrameData::UnnumberedAcknowledgement,
                Some(user_dlci),
            );
            exec.run_until_stalled(&mut handle_fut).expect_pending("waiting for modem status");
            // We then expect to send our current Modem Signals to the peer.
            expect_mux_command(&mut exec, &mut outgoing_frames, MuxCommandMarker::ModemStatus);
            let _ = exec.run_until_stalled(&mut handle_fut).expect("should be done handling frame");
            c
        };

        // There should be an established DLC.
        assert!(session.multiplexer().dlc_established());

        // Remote tries to re-negotiate the session parameters, we expect to reply with
        // a UIH PN response with the current session parameters (max_frame_size = 100).
        let dlcpn = make_dlc_pn_frame(CommandResponse::Command, user_dlci, true, 60);
        let expected_response = make_dlc_pn_frame(CommandResponse::Response, user_dlci, true, 100);
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            dlcpn,
            expected_response.data,
        );

        // The global session parameters should not be updated since the first DLC has
        // already been established.
        assert_eq!(session.session_parameters(), expected_parameters);
    }

    #[test]
    fn test_establish_dlci_request_relays_channel_to_channel_open_fn() {
        let mut exec = fasync::TestExecutor::new();

        // Create and start a SessionInner that relays any opened RFCOMM channels.
        let (mut session, mut outgoing_frames, mut channel_receiver) = setup_session();
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        // Remote peer sends SABM over a user DLCI - we expect to reply with a UA response.
        let random_dlci = 8;
        let user_dlci = DLCI::try_from(random_dlci).unwrap();
        let user_sabm = Frame::make_sabm_command(Role::Initiator, user_dlci);
        {
            let mut handle_fut = Box::pin(session.handle_frame(user_sabm));
            exec.run_until_stalled(&mut handle_fut).expect_pending("should wait for channel");
            // We expect a channel to be delivered from the`channel_opened_fn`.
            let _c = expect_channel(&mut exec, &mut channel_receiver);
            // Continue to run the `handle_frame` to process the result of the channel delivery.
            exec.run_until_stalled(&mut handle_fut)
                .expect_pending("should wait for outgoing frame");
            // We expect to respond with a positive UA response.
            expect_frame(
                &mut exec,
                &mut outgoing_frames,
                FrameData::UnnumberedAcknowledgement,
                Some(user_dlci),
            );
            exec.run_until_stalled(&mut handle_fut).expect_pending("should wait for modem status");
            // After positively responding, we expect to send our current Modem Signals to indicate
            // readiness.
            expect_mux_command(&mut exec, &mut outgoing_frames, MuxCommandMarker::ModemStatus);
            let _ = exec.run_until_stalled(&mut handle_fut).expect("finished handling frame");
        }
    }

    #[test]
    fn test_no_registered_clients_rejects_establish_dlci_request() {
        let mut exec = fasync::TestExecutor::new();

        // Create the session - set the channel_send_fn to unanimously reject
        // channels, to simulate failure.
        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();
        session.channel_opened_fn =
            Box::new(|_, _channel| async { Err(format_err!("Always rejecting")) }.boxed());
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        // Remote peer sends SABM over a user DLCI - this should be rejected with a
        // DM response frame because channel delivery failed.
        let user_dlci = DLCI::try_from(6).unwrap();
        let user_sabm = Frame::make_sabm_command(Role::Initiator, user_dlci);
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            user_sabm,
            FrameData::DisconnectedMode,
        );
    }

    #[test]
    fn test_received_user_data_is_relayed_to_and_from_profile_client() {
        let mut exec = fasync::TestExecutor::new();

        // Create and start a SessionInner that relays any opened RFCOMM channels.
        let (mut session, mut outgoing_frames, mut channel_receiver) = setup_session();
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        // Establish a user DLCI with an adequate amount of credits - the RFCOMM channel should
        // be delivered to the channel receiver.
        let user_dlci = DLCI::try_from(8).unwrap();
        let _ = session.multiplexer().find_or_create_session_channel(user_dlci);
        assert!(session
            .multiplexer()
            .set_flow_control(user_dlci, FlowControlMode::CreditBased(Credits::new(100, 100)))
            .is_ok());
        let mut profile_client_channel = {
            let mut establish_fut = Box::pin(session.establish_session_channel(user_dlci));
            exec.run_until_stalled(&mut establish_fut).expect_pending("should wait for channel");
            let channel = expect_channel(&mut exec, &mut channel_receiver);
            assert_matches!(exec.run_until_stalled(&mut establish_fut), Poll::Ready(true));
            channel
        };

        // Remote peer sends us user data.
        let pattern = vec![0x00, 0x01, 0x02];
        {
            let user_data_frame = Frame::make_user_data_frame(
                Role::Initiator,
                user_dlci,
                UserData { information: pattern.clone() },
                Some(10), // Random amount of credits.
            );
            let mut handle_fut = Box::pin(session.handle_frame(user_data_frame));
            assert!(exec.run_until_stalled(&mut handle_fut).is_ready());

            // User data should be forwarded to the profile client channel.
            match exec.run_until_stalled(&mut profile_client_channel.next()) {
                Poll::Ready(Some(Ok(buf))) => {
                    assert_eq!(buf, pattern);
                }
                x => panic!("Expected user data but got {:?}", x),
            }
        }

        // Profile client responds with it's own data.
        let response = vec![0x09, 0x08, 0x07, 0x06];
        assert_eq!(profile_client_channel.as_ref().write(&response), Ok(4));
        // The data should be processed by the SessionChannel, packed as a user data
        // frame, and sent as an outgoing frame.
        expect_user_data_frame(
            &mut exec,
            &mut outgoing_frames,
            UserData { information: response },
            Some(156), // CREDIT_HIGH_WATER_MARK - (100 (initial credits) - 1 (received frames))
        );
    }

    #[test]
    fn test_receiving_invalid_mux_command_results_in_non_supported_command() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        let unsupported_command = 0xff;
        let mut handle_fut = Box::pin(session.handle_frame_parse_error(
            FrameParseError::UnsupportedMuxCommandType(unsupported_command),
        ));
        exec.run_until_stalled(&mut handle_fut).expect_pending("should wait for outgoing frame");

        // We expect an NSC Frame response.
        let expected = FrameData::UnnumberedInfoHeaderCheck(UIHData::Mux(MuxCommand {
            params: MuxCommandParams::NonSupported(NonSupportedCommandParams {
                cr_bit: true,
                non_supported_command: unsupported_command,
            }),
            command_response: CommandResponse::Response,
        }));
        expect_frame(&mut exec, &mut outgoing_frames, expected, None);
        assert!(exec.run_until_stalled(&mut handle_fut).is_ready());
    }

    #[test]
    fn test_disconnect_over_user_dlci_closes_session_channel() {
        let mut exec = fasync::TestExecutor::new();

        // Create and start a SessionInner that relays any opened RFCOMM channel.
        let (mut session, mut outgoing_frames, mut channel_receiver) = setup_session();
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        // Establish a random user DLCI.
        let user_dlci = DLCI::try_from(6).unwrap();
        let _channel = {
            let mut establish_fut = Box::pin(session.establish_session_channel(user_dlci));
            exec.run_until_stalled(&mut establish_fut).expect_pending("should wait for channel");
            let c = expect_channel(&mut exec, &mut channel_receiver);
            assert_matches!(exec.run_until_stalled(&mut establish_fut), Poll::Ready(true));
            c
        };
        assert!(session.multiplexer().dlc_established());

        // Receive a disconnect command - should close the channel for the provided DLCI and
        // respond with UA.
        let disc = Frame::make_disc_command(Role::Initiator, user_dlci);
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            disc.clone(),
            FrameData::UnnumberedAcknowledgement,
        );
        assert!(!session.multiplexer().dlci_established(&user_dlci));

        // Receiving a disconnect again on the already-closed DLCI should result in a DM response.
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            disc,
            FrameData::DisconnectedMode,
        );
    }

    #[fuchsia::test]
    fn test_disconnect_over_mux_control_closes_session() {
        let mut exec = fasync::TestExecutor::new();

        let (session_fut, mut remote) = setup_session_task();
        pin_mut!(session_fut);
        exec.run_until_stalled(&mut session_fut)
            .expect_pending("shouldn't be done while remote is live");

        {
            let remote_closed_fut = remote.closed();
            pin_mut!(remote_closed_fut);
            exec.run_until_stalled(&mut remote_closed_fut)
                .expect_pending("shouldn't be done while remote is live");
        }

        // Remote sends SABM to start up session multiplexer.
        let sabm = Frame::make_sabm_command(Role::Unassigned, DLCI::MUX_CONTROL_DLCI);
        send_peer_frame(remote.as_ref(), sabm);
        exec.run_until_stalled(&mut session_fut)
            .expect_pending("shouldn't be done while remote is live");
        // Expect the outgoing acknowledgement for the SABM.
        expect_frame_received_by_peer(&mut exec, &mut remote);

        // Remote sends us a disconnect frame over the Mux Control DLCI.
        let disconnect = Frame::make_disc_command(Role::Initiator, DLCI::MUX_CONTROL_DLCI);
        send_peer_frame(remote.as_ref(), disconnect);
        // Once we process the disconnect, the session should terminate.
        let _ = exec.run_until_stalled(&mut session_fut).expect("Session is done now");
        // Expect the outgoing acknowledgement for the disconnect.
        expect_frame_received_by_peer(&mut exec, &mut remote);

        // Remote should be closed, since the session has terminated.
        {
            let remote_closed_fut = remote.closed();
            pin_mut!(remote_closed_fut);
            let _ = exec
                .run_until_stalled(&mut remote_closed_fut)
                .expect("L2CAP channel should be closed now");
        }
    }

    #[test]
    fn test_start_multiplexer() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();
        assert!(!session.multiplexer().started());

        // Initiate multiplexer startup - we expect to send a SABM frame.
        {
            let mut start_mux_fut = Box::pin(session.start_multiplexer());
            exec.run_until_stalled(&mut start_mux_fut)
                .expect_pending("should wait for outgoing frame");
            // The outgoing frame should be an SABM.
            expect_frame(
                &mut exec,
                &mut outgoing_frames,
                FrameData::SetAsynchronousBalancedMode,
                Some(DLCI::MUX_CONTROL_DLCI),
            );
            assert_matches!(exec.run_until_stalled(&mut start_mux_fut), Poll::Ready(Ok(_)));
        }

        // Attempting to start the multiplexer while it's already starting should fail.
        {
            let mut start_mux_fut = Box::pin(session.start_multiplexer());
            assert_matches!(exec.run_until_stalled(&mut start_mux_fut), Poll::Ready(Err(_)));
        }

        // Simulate peer responding positively with a UA - this should complete startup.
        {
            let mut handle_fut =
                Box::pin(session.handle_frame(Frame::make_ua_response(
                    Role::Unassigned,
                    DLCI::MUX_CONTROL_DLCI,
                )));
            assert!(exec.run_until_stalled(&mut handle_fut).is_ready());
        }
        // Multiplexer startup should finish with the initiator role.
        assert!(session.multiplexer().started());
        assert_eq!(session.role(), Role::Initiator);
    }

    #[test]
    fn test_peer_rejects_multiplexer_startup() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();
        assert!(!session.multiplexer().started());
        // Initiate multiplexer startup - we expect to send a SABM frame.
        {
            let mut start_mux_fut = Box::pin(session.start_multiplexer());
            exec.run_until_stalled(&mut start_mux_fut)
                .expect_pending("should wait for outgoing frame");
            expect_frame(
                &mut exec,
                &mut outgoing_frames,
                FrameData::SetAsynchronousBalancedMode,
                Some(DLCI::MUX_CONTROL_DLCI),
            );
            assert_matches!(exec.run_until_stalled(&mut start_mux_fut), Poll::Ready(Ok(_)));
        }
        // Simulate peer responding negatively with a DM - this should cancel startup.
        {
            let mut handle_fut =
                Box::pin(session.handle_frame(Frame::make_dm_response(
                    Role::Unassigned,
                    DLCI::MUX_CONTROL_DLCI,
                )));
            assert!(exec.run_until_stalled(&mut handle_fut).is_ready());
        }
        // The multiplexer should not be started and should still be Unassigned.
        assert!(!session.multiplexer().started());
        assert_eq!(session.role(), Role::Unassigned);
    }

    #[test]
    fn test_initiating_parameter_negotiation_expects_response() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();

        // Attempting to negotiate parameters before mux startup should fail.
        assert!(!session.multiplexer().started());
        let user_dlci = DLCI::try_from(3).unwrap();
        {
            let mut pn_fut = Box::pin(session.start_parameter_negotiation(user_dlci));
            assert_matches!(exec.run_until_stalled(&mut pn_fut), Poll::Ready(Err(_)));
        }

        assert!(session.multiplexer().start(Role::Initiator).is_ok());
        // Initiating PN should be OK now. Upon receiving response, the session parameters
        // should get set.
        {
            let mut pn_fut = Box::pin(session.start_parameter_negotiation(user_dlci));
            exec.run_until_stalled(&mut pn_fut).expect_pending("should wait for outgoing frame");
            expect_mux_command(
                &mut exec,
                &mut outgoing_frames,
                MuxCommandMarker::ParameterNegotiation,
            );
            assert_matches!(exec.run_until_stalled(&mut pn_fut), Poll::Ready(Ok(_)));
        }
        // Simulate peer responding positively - the parameters should be negotiated.
        {
            let mut handle_fut = Box::pin(session.handle_frame(make_dlc_pn_frame(
                CommandResponse::Response,
                user_dlci,
                true, // Peer supports credit-based flow control.
                100,  // Peer supports max-frame-size of 100.
            )));
            assert_matches!(exec.run_until_stalled(&mut handle_fut), Poll::Ready(Ok(_)));
        }
        assert!(session.multiplexer().parameters_negotiated());
        let expected_parameters =
            SessionParameters { credit_based_flow: true, max_frame_size: 100 };
        assert_eq!(session.multiplexer().parameters(), expected_parameters);
    }

    #[test]
    fn test_peer_rejects_parameter_negotiation_with_dm() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _rfcomm_channels) = setup_session();
        let (outbound_fn, mut outbound_channels) = create_outbound_relay();
        assert!(session.multiplexer().start(Role::Initiator).is_ok());

        // Request to open a channel - should initiate parameter negotiation.
        let server_channel = ServerChannel::try_from(13).unwrap();
        let user_dlci = server_channel.to_dlci(Role::Responder).unwrap();
        {
            let mut open_fut = Box::pin(session.open_remote_channel(server_channel, outbound_fn));
            exec.run_until_stalled(&mut open_fut).expect_pending("should wait for outgoing frame");
            expect_mux_command(
                &mut exec,
                &mut outgoing_frames,
                MuxCommandMarker::ParameterNegotiation,
            );
            assert_matches!(exec.run_until_stalled(&mut open_fut), Poll::Ready(Ok(_)));
        }
        // Simulate peer responding negatively with a DM response.
        {
            let mut handle_fut =
                Box::pin(session.handle_frame(Frame::make_dm_response(Role::Responder, user_dlci)));
            assert_matches!(exec.run_until_stalled(&mut handle_fut), Poll::Ready(Ok(_)));
        }
        // The session-wide parameters should not be negotiated.
        assert_eq!(
            session.multiplexer().parameter_negotiation_state(),
            ParameterNegotiationState::NotNegotiated
        );
        // Client should be notified of cancellation.
        expect_channel_error(&mut exec, &mut outbound_channels, ErrorCode::Canceled);
    }

    #[test]
    fn test_peer_rejects_parameter_negotiation_with_disc() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, mut outgoing_frames, _inbound_channels) = setup_session();
        let (outbound_fn, mut outbound_channels) = create_outbound_relay();
        assert!(session.multiplexer().start(Role::Initiator).is_ok());

        // Request to open a channel - should initiate parameter negotiation.
        let server_channel = ServerChannel::try_from(13).unwrap();
        let user_dlci = server_channel.to_dlci(Role::Responder).unwrap();
        {
            let mut open_fut = Box::pin(session.open_remote_channel(server_channel, outbound_fn));
            exec.run_until_stalled(&mut open_fut).expect_pending("should wait for outgoing frame");
            expect_mux_command(
                &mut exec,
                &mut outgoing_frames,
                MuxCommandMarker::ParameterNegotiation,
            );
            assert_matches!(exec.run_until_stalled(&mut open_fut), Poll::Ready(Ok(_)));
        }
        // Simulate peer responding negatively with a Disconnect command - we expect to positively
        // reply with a UA.
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            Frame::make_disc_command(Role::Responder, user_dlci),
            FrameData::UnnumberedAcknowledgement,
        );
        // The session-wide parameters should not be negotiated.
        assert_eq!(
            session.multiplexer().parameter_negotiation_state(),
            ParameterNegotiationState::NotNegotiated
        );
        // Client should be notified of cancellation.
        expect_channel_error(&mut exec, &mut outbound_channels, ErrorCode::Canceled);
    }

    #[test]
    fn test_open_channel_request_establishes_channel_after_mux_startup_and_pn() {
        let mut exec = fasync::TestExecutor::new();

        // Create and start a SessionInner that relays any opened RFCOMM channels.
        let (mut session, mut outgoing_frames, _inbound_channels) = setup_session();
        let (outbound_fn, mut outbound_channels) = create_outbound_relay();

        // Initiate an open RFCOMM channel request with a random valid ServerChannel.
        let server_channel = ServerChannel::try_from(5).unwrap();
        {
            let mut open_fut = Box::pin(session.open_remote_channel(server_channel, outbound_fn));
            exec.run_until_stalled(&mut open_fut).expect_pending("should wait for outgoing frame");
            // Since the multiplexer has not started, we first expect to send an SABM over the
            // MUX Control DLCI to the remote peer.
            expect_frame(
                &mut exec,
                &mut outgoing_frames,
                FrameData::SetAsynchronousBalancedMode,
                Some(DLCI::MUX_CONTROL_DLCI),
            );
            assert_matches!(exec.run_until_stalled(&mut open_fut), Poll::Ready(Ok(_)));
        }
        {
            // Simulate peer responding positively with a UA.
            let mut handle_fut = Box::pin(
                session
                    .handle_frame(Frame::make_ua_response(Role::Responder, DLCI::MUX_CONTROL_DLCI)),
            );
            exec.run_until_stalled(&mut handle_fut)
                .expect_pending("should wait for outgoing frame");
            // We then expect the session to initiate a Parameter Negotiation request, since
            // the session has not negotiated parameters.
            expect_mux_command(
                &mut exec,
                &mut outgoing_frames,
                MuxCommandMarker::ParameterNegotiation,
            );
            assert_matches!(exec.run_until_stalled(&mut handle_fut), Poll::Ready(Ok(_)));
        }

        let expected_dlci = server_channel.to_dlci(Role::Responder).unwrap();
        {
            // Simulate the peer's positive response to the DLC PN request. We then expect an
            // outgoing SABM command to establish the user channel.
            handle_and_expect_frame(
                &mut exec,
                &mut session,
                &mut outgoing_frames,
                make_dlc_pn_frame(
                    CommandResponse::Response,
                    expected_dlci,
                    true, // Supports credit-based flow control.
                    100,  // Supports max frame size of 100.
                ),
                FrameData::SetAsynchronousBalancedMode, // Outgoing SABM.
            );
        }

        {
            // Remote peer replies positively to the open channel request.
            let mut handle_fut = Box::pin(
                session.handle_frame(Frame::make_ua_response(Role::Responder, expected_dlci)),
            );
            exec.run_until_stalled(&mut handle_fut).expect_pending("should wait for channel");
            // We then expect to open a local RFCOMM channel to be relayed to a profile client.
            let _channel = expect_channel(&mut exec, &mut outbound_channels);
            exec.run_until_stalled(&mut handle_fut)
                .expect_pending("should wait for outgoing frame");
            // Upon successful channel delivery, we expect an outgoing ModemStatus frame to
            // be sent.
            expect_mux_command(&mut exec, &mut outgoing_frames, MuxCommandMarker::ModemStatus);
            assert_matches!(exec.run_until_stalled(&mut handle_fut), Poll::Ready(Ok(_)));
        }

        // The DLCI should be established.
        assert!(session.multiplexer().dlci_established(&expected_dlci));
    }

    #[test]
    fn test_open_channel_request_rejected_by_peer() {
        let mut exec = fasync::TestExecutor::new();

        // Start SessionInner - don't expect any relayed channels.
        let (mut session, mut outgoing_frames, _inbound_channels) = setup_session();
        let (outbound_fn, mut outbound_channels) = create_outbound_relay();
        session.channel_opened_fn =
            Box::new(|_, _channel| async { panic!("Don't expect channels!") }.boxed());
        assert!(session.multiplexer().start(Role::Initiator).is_ok());

        let server_channel = ServerChannel::try_from(5).unwrap();
        let expected_dlci = server_channel.to_dlci(Role::Responder).unwrap();
        // Simulate PN finishing ahead of time so that we can directly test the rejection case.
        session.finish_parameter_negotiation(&ParameterNegotiationParams::default_command(
            expected_dlci,
        ));
        // Initiate an open RFCOMM channel request with a random valid ServerChannel. Expect
        // an outgoing SABM.
        {
            let mut open_fut = Box::pin(session.open_remote_channel(server_channel, outbound_fn));
            exec.run_until_stalled(&mut open_fut).expect_pending("should wait for outgoing frame");
            expect_frame(
                &mut exec,
                &mut outgoing_frames,
                FrameData::SetAsynchronousBalancedMode,
                Some(expected_dlci),
            );
            assert_matches!(exec.run_until_stalled(&mut open_fut), Poll::Ready(Ok(_)));
        }
        {
            // Simulate peer responding negatively with a DM.
            let mut handle_fut = Box::pin(
                session.handle_frame(Frame::make_dm_response(Role::Responder, expected_dlci)),
            );
            assert_matches!(exec.run_until_stalled(&mut handle_fut), Poll::Ready(Ok(_)));
        }
        // The DLCI should not be established due to peer rejection - client should be notified.
        assert!(!session.multiplexer().dlci_established(&expected_dlci));
        expect_channel_error(&mut exec, &mut outbound_channels, ErrorCode::Canceled);
    }

    #[test]
    fn test_cancellation_during_open_channel_notifies_client() {
        let mut exec = fasync::TestExecutor::new();

        let (local, mut remote) = Channel::create();
        let (channel_open_fn, _inbound_channels) = create_inbound_relay();
        let session = Session::create(PeerId(42), local, channel_open_fn);
        let (outbound_fn, mut outbound_channels) = create_outbound_relay();

        // Local profile client requests to open an RFCOMM channel.
        let server_channel = ServerChannel::try_from(2).unwrap();
        {
            let mut open_fut = Box::pin(session.open_rfcomm_channel(server_channel, outbound_fn));
            assert!(exec.run_until_stalled(&mut open_fut).is_ready());
        }

        // Remote should receive an RFCOMM frame to start up the multiplexer.
        expect_frame_received_by_peer(&mut exec, &mut remote);
        // Remote responds positively.
        let ua = Frame::make_ua_response(Role::Unassigned, DLCI::MUX_CONTROL_DLCI);
        send_peer_frame(remote.as_ref(), ua);

        // Remote should receive an RFCOMM frame to negotiate parameters.
        expect_frame_received_by_peer(&mut exec, &mut remote);
        // Remote disconnects - run any background tasks to completion.
        drop(remote);
        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());
        // Client should be notified of cancellation.
        expect_channel_error(&mut exec, &mut outbound_channels, ErrorCode::Canceled);
    }

    #[fuchsia::test]
    fn session_close_request_before_mux_startup_is_no_op() {
        let mut exec = fasync::TestExecutor::new();

        let (local, remote) = Channel::create();
        let (channel_open_fn, _inbound_channels) = create_inbound_relay();
        let session = Session::create(PeerId(52), local, channel_open_fn);

        // Some local client (via the RfcommTest API) requests to close the RFCOMM session.
        {
            let mut close_fut = Box::pin(session.close());
            assert!(exec.run_until_stalled(&mut close_fut).is_ready());
        }

        // Because the RFCOMM session hasn't been established yet, the close request is a
        // no-op. The underlying L2CAP channel should still be open.
        let mut channel_closed_fut = Box::pin(remote.closed());
        exec.run_until_stalled(&mut channel_closed_fut)
            .expect_pending("shouldn't be done while session active");
    }

    #[fuchsia::test]
    fn session_close_request_results_in_disconnect_frame_to_peer() {
        let mut exec = fasync::TestExecutor::new();

        let (local, mut remote) = Channel::create();
        let (channel_open_fn, _inbound_channels) = create_inbound_relay();
        let session = Session::create(PeerId(52), local, channel_open_fn);

        // Manually start the multiplexer.
        {
            let mut start_fut = Box::pin(session.initiate_multiplexer_startup());
            assert!(exec.run_until_stalled(&mut start_fut).is_ready());
        }

        // Expect outgoing SABM and simulate peer positive response.
        expect_frame_received_by_peer(&mut exec, &mut remote);
        let ua = Frame::make_ua_response(Role::Unassigned, DLCI::MUX_CONTROL_DLCI);
        send_peer_frame(remote.as_ref(), ua);
        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());

        // Some client (presumably via the RfcommTest API) requests to close the RFCOMM session.
        {
            let mut close_fut = Box::pin(session.close());
            assert!(exec.run_until_stalled(&mut close_fut).is_ready());
        }

        // Remote should receive an RFCOMM frame to Disconnect.
        expect_frame_received_by_peer(&mut exec, &mut remote);
        {
            // The session (and therefore L2CAP channel) should only close after the
            // peer acknowledges.
            let mut channel_closed_fut = Box::pin(remote.closed());
            exec.run_until_stalled(&mut channel_closed_fut)
                .expect_pending("shouldn't finish while session active");
        }
        // Remote responds positively.
        let ua = Frame::make_ua_response(Role::Unassigned, DLCI::MUX_CONTROL_DLCI);
        send_peer_frame(remote.as_ref(), ua);

        // At this point, the L2CAP channel should be closed.
        let mut channel_closed_fut = Box::pin(remote.closed());
        assert!(exec.run_until_stalled(&mut channel_closed_fut).is_ready());
    }

    #[test]
    fn test_open_multiple_channels_establishes_channels_after_acknowledgement() {
        let mut exec = fasync::TestExecutor::new();

        // Create and start a SessionInner that relays any opened RFCOMM channels.
        let (mut session, mut outgoing_frames, _inbound_channels) = setup_session();
        let (outbound_fn, _outbound_channels1) = create_outbound_relay();
        let (outbound_fn2, _outbound_channels2) = create_outbound_relay();

        // The session multiplexer has started.
        assert!(session.multiplexer().start(Role::Responder).is_ok());

        // Initiate an open RFCOMM channel request with a random valid ServerChannel.
        let server_channel = ServerChannel::try_from(5).unwrap();
        let expected_dlci = server_channel.to_dlci(Role::Initiator).unwrap();
        {
            let mut open_fut = Box::pin(session.open_remote_channel(server_channel, outbound_fn));
            exec.run_until_stalled(&mut open_fut).expect_pending("should wait for outgoing frame");
            // We expect the session to initiate a Parameter Negotiation request (UIH Frame),
            // for the DLCI. We do this for every DLC.
            expect_mux_command(
                &mut exec,
                &mut outgoing_frames,
                MuxCommandMarker::ParameterNegotiation,
            );
            assert_matches!(exec.run_until_stalled(&mut open_fut), Poll::Ready(Ok(_)));
        }

        // Before the peer responds, we get another request to open a different RFCOMM channel.
        let server_channel2 = ServerChannel::try_from(9).unwrap();
        let expected_dlci2 = server_channel2.to_dlci(Role::Initiator).unwrap();
        {
            // We expect the session to initiate a Parameter Negotiation request (UIH Frame),
            // for the DLCI. We do this for every DLC.
            let mut open_fut = Box::pin(session.open_remote_channel(server_channel2, outbound_fn2));
            exec.run_until_stalled(&mut open_fut).expect_pending("should wait for outgoing frame");
            expect_mux_command(
                &mut exec,
                &mut outgoing_frames,
                MuxCommandMarker::ParameterNegotiation,
            );
            assert_matches!(exec.run_until_stalled(&mut open_fut), Poll::Ready(Ok(_)));
        }

        // Simulate the peer's positive response to the first DLC PN request. We expect an
        // outgoing SABM.
        let pn_frame = make_dlc_pn_frame(
            CommandResponse::Response,
            expected_dlci,
            true, // Supports credit-based flow control.
            100,  // Supports max frame size of 100.
        );
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            pn_frame,
            FrameData::SetAsynchronousBalancedMode,
        );
        // Simulate the peer's positive response to the second DLC PN request. We expect an
        // outgoing SABM.
        let pn_frame = make_dlc_pn_frame(
            CommandResponse::Response,
            expected_dlci2,
            true, // Supports credit-based flow control.
            105,  // Supports max frame size of 105.
        );
        handle_and_expect_frame(
            &mut exec,
            &mut session,
            &mut outgoing_frames,
            pn_frame,
            FrameData::SetAsynchronousBalancedMode,
        );
    }

    #[test]
    fn test_open_rfcomm_channel_relays_channel_to_callback() {
        let mut exec = fasync::TestExecutor::new();

        let (local, mut remote) = Channel::create();
        let (channel_open_fn, _inbound_channels) = create_inbound_relay();
        let session = Session::create(PeerId(321), local, channel_open_fn);
        let (outbound_fn, mut outbound_channels) = create_outbound_relay();

        // 1. Simulate local profile client requesting to open an RFCOMM channel.
        let server_channel = ServerChannel::try_from(2).unwrap();
        let expected_dlci = server_channel.to_dlci(Role::Responder).unwrap();
        {
            let mut open_fut = Box::pin(session.open_rfcomm_channel(server_channel, outbound_fn));
            assert!(exec.run_until_stalled(&mut open_fut).is_ready());
        }

        // 2. Remote should receive an RFCOMM frame to start up the multiplexer.
        expect_frame_received_by_peer(&mut exec, &mut remote);
        // 3. Remote responds positively.
        let ua = Frame::make_ua_response(Role::Unassigned, DLCI::MUX_CONTROL_DLCI);
        send_peer_frame(remote.as_ref(), ua);

        // 4. Remote should receive an RFCOMM frame to negotiate parameters.
        expect_frame_received_by_peer(&mut exec, &mut remote);
        // 5. Remote responds positively.
        let pn_response = make_dlc_pn_frame(CommandResponse::Response, expected_dlci, true, 100);
        send_peer_frame(remote.as_ref(), pn_response);

        // 6. Remote should receive an RFCOMM frame to establish the `expected_dlci`.
        expect_frame_received_by_peer(&mut exec, &mut remote);
        // 7. Remote responds positively.
        let ua = Frame::make_ua_response(Role::Responder, expected_dlci);
        send_peer_frame(remote.as_ref(), ua);

        // Mux startup, Parameter negotiation, and channel establishment are complete. The RFCOMM
        // channel should be ready and relayed to the client.
        let _channel = expect_channel(&mut exec, &mut outbound_channels);

        // Client trying to connect again on the same channel should fail immediately.
        let (outbound_fn2, mut outbound_channels2) = create_outbound_relay();
        let mut open_fut = Box::pin(session.open_rfcomm_channel(server_channel, outbound_fn2));
        assert!(exec.run_until_stalled(&mut open_fut).is_ready());
        expect_channel_error(&mut exec, &mut outbound_channels2, ErrorCode::Canceled);
    }

    #[test]
    fn test_open_same_rfcomm_channel_fails() {
        let mut exec = fasync::TestExecutor::new();

        // Create and start a SessionInner that relays any opened RFCOMM channels.
        let (mut session, mut outgoing_frames, _inbound_channels) = setup_session();
        assert!(session.multiplexer().start(Role::Responder).is_ok());
        let (outbound_fn, mut outbound_channels1) = create_outbound_relay();
        let (outbound_fn2, mut outbound_channels2) = create_outbound_relay();

        // Initiate an open RFCOMM channel request.
        let server_channel = ServerChannel::try_from(5).unwrap();
        {
            let mut open_fut = Box::pin(session.open_remote_channel(server_channel, outbound_fn));
            exec.run_until_stalled(&mut open_fut).expect_pending("should wait for outgoing frame");
            // Expect to initiate PN.
            expect_mux_command(
                &mut exec,
                &mut outgoing_frames,
                MuxCommandMarker::ParameterNegotiation,
            );
            assert_matches!(exec.run_until_stalled(&mut open_fut), Poll::Ready(Ok(_)));
        }
        // Before peer responds, client tries to request to open the same channel - failure.
        let mut open_fut = Box::pin(session.open_remote_channel(server_channel, outbound_fn2));
        assert_matches!(exec.run_until_stalled(&mut open_fut), Poll::Ready(Err(_)));
        // Client should be notified that the second request failed.
        expect_channel_error(&mut exec, &mut outbound_channels2, ErrorCode::Failed);
        // First request should still be alive - nothing relayed.
        poll_stream(&mut exec, &mut outbound_channels1).expect_pending("nothing relayed")
    }

    #[fuchsia::test]
    fn send_rls_before_mux_startup_returns_error() {
        let mut exec = fasync::TestExecutor::new();

        let (mut session, _outgoing_frames, _inbound_channels) = setup_session();

        // Initiate an RLS update command.
        let server_channel = ServerChannel::try_from(3).unwrap();
        let mut rls_fut = Box::pin(session.send_remote_line_status(server_channel, None));
        match exec.run_until_stalled(&mut rls_fut) {
            Poll::Ready(Err(Error::MultiplexerNotStarted)) => {}
            x => panic!("Expected ready with error but got: {:?}", x),
        }
    }

    #[fuchsia::test]
    fn send_rls_before_channel_establishment_returns_error() {
        let mut exec = fasync::TestExecutor::new();

        let our_role = Role::Responder;
        let (mut session, _outgoing_frames, _inbound_channels) = setup_session();
        assert!(session.multiplexer().start(our_role).is_ok());

        // Initiate an RLS update for some channel that has not been established.
        let server_channel = ServerChannel::try_from(3).expect("should be valid server channel");
        let expected_dlci = server_channel.to_dlci(our_role).expect("should be valid dlci");

        let mut rls_fut = Box::pin(session.send_remote_line_status(server_channel, None));
        match exec.run_until_stalled(&mut rls_fut) {
            Poll::Ready(Err(Error::ChannelNotEstablished(dlci))) => {
                assert_eq!(dlci, expected_dlci);
            }
            x => panic!("Expected ready with error but got: {:?}", x),
        }
    }

    #[fuchsia::test]
    fn rls_command_received_by_peer() {
        let mut exec = fasync::TestExecutor::new();

        let our_role = Role::Responder;
        let remote_peer_role = our_role.opposite_role();
        let (mut session, mut outgoing_frames, mut channel_receiver) = setup_session();
        assert!(session.multiplexer().start(our_role).is_ok());

        // Remote peer wants to open an RFCOMM channel for some service provided by this device.
        let server_channel_number =
            ServerChannel::try_from(2).expect("valid server channel number");
        let expected_dlci = server_channel_number.to_dlci(our_role).expect("valid DLCI");

        let user_sabm = Frame::make_sabm_command(remote_peer_role, expected_dlci);
        let _rfcomm_channel = {
            let mut handle_fut = Box::pin(session.handle_frame(user_sabm));
            exec.run_until_stalled(&mut handle_fut).expect_pending("should wait for channel");
            // We expect a channel to be delivered to the local client (e.g to the `channel_receiver`).
            let _c = expect_channel(&mut exec, &mut channel_receiver);
            // After successfully delivering the channel to a local client, we expect to notify the peer with
            // a positive UA response.
            exec.run_until_stalled(&mut handle_fut)
                .expect_pending("should wait for outgoing frame");
            expect_frame(
                &mut exec,
                &mut outgoing_frames,
                FrameData::UnnumberedAcknowledgement,
                Some(expected_dlci),
            );

            // After positively responding, we expect to send our current Modem Signals to indicate
            // readiness.
            exec.run_until_stalled(&mut handle_fut)
                .expect_pending("should wait for outgoing frame");
            expect_mux_command(&mut exec, &mut outgoing_frames, MuxCommandMarker::ModemStatus);
            assert_matches!(exec.run_until_stalled(&mut handle_fut), Poll::Ready(Ok(false)));
            _c
        };

        // An RFCOMM channel at `expected_dlci` has been established. Sending an RLS update to the peer
        // should succeed.
        let error_status = Some(RlsError::Overrun);
        {
            let mut rls_fut =
                Box::pin(session.send_remote_line_status(server_channel_number, error_status));
            exec.run_until_stalled(&mut rls_fut).expect_pending("should wait for outgoing frame");
            expect_mux_command(&mut exec, &mut outgoing_frames, MuxCommandMarker::RemoteLineStatus);
            assert_matches!(exec.run_until_stalled(&mut rls_fut), Poll::Ready(Ok(_)));
        }

        // Peer would typically respond by echoing the RLS - nothing to be done thereafter.
        let mux_command = MuxCommand {
            params: MuxCommandParams::RemoteLineStatus(RemoteLineStatusParams::new(
                expected_dlci,
                error_status,
            )),
            command_response: CommandResponse::Response,
        };
        let peer_rls = Frame::make_mux_command(remote_peer_role, mux_command);
        {
            let mut handle_fut = Box::pin(session.handle_frame(peer_rls));
            assert_matches!(exec.run_until_stalled(&mut handle_fut), Poll::Ready(Ok(false)));
        }
    }
}
