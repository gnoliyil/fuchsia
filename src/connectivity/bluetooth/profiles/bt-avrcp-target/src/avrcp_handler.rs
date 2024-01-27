// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::anyhow,
    async_helpers::component_lifecycle::ComponentLifecycleServer,
    fidl::endpoints::create_request_stream,
    fidl_fuchsia_bluetooth_avrcp::{
        PeerManagerMarker, TargetAvcError, TargetHandlerMarker, TargetHandlerRequest,
        TargetHandlerRequestStream, TargetPassthroughError,
    },
    fidl_fuchsia_bluetooth_component::LifecycleState,
    fuchsia_component::client::connect_to_protocol,
    futures::TryStreamExt,
    std::sync::Arc,
    tracing::{trace, warn},
};

use crate::media::media_sessions::MediaSessions;

/// Fulfills an AVRCP Target request.
async fn handle_target_request(
    request: TargetHandlerRequest,
    media_sessions: Arc<MediaSessions>,
) -> Result<(), fidl::Error> {
    trace!("Received target request: {:?}", request);
    match request {
        TargetHandlerRequest::GetEventsSupported { responder } => {
            // Send a static response of TG supported events.
            responder.send(&mut Ok(media_sessions.get_supported_notification_events()))?;
        }
        TargetHandlerRequest::GetPlayStatus { responder } => {
            let mut response = media_sessions.get_active_session().map_or(
                Err(TargetAvcError::RejectedNoAvailablePlayers),
                |state| {
                    let play_status = state.session_info().get_play_status().clone();
                    Ok(play_status.into())
                },
            );
            responder.send(&mut response)?;
        }
        TargetHandlerRequest::GetMediaAttributes { responder } => {
            let mut response = media_sessions.get_active_session().map_or(
                Err(TargetAvcError::RejectedNoAvailablePlayers),
                |state| {
                    let media_attributes = state.session_info().get_media_info().clone();
                    Ok(media_attributes.into())
                },
            );
            responder.send(&mut response)?;
        }
        TargetHandlerRequest::SendCommand { command, pressed, responder } => {
            if let Ok(state) = media_sessions.get_active_session() {
                responder.send(state.handle_avc_passthrough_command(command, pressed).await)?;
            } else {
                responder.send(Err(TargetPassthroughError::CommandRejected))?;
            }
        }
        TargetHandlerRequest::ListPlayerApplicationSettingAttributes { responder } => {
            // Send back the static list of Media supported PlayerApplicationSettingAttributes.
            let mut response = media_sessions
                .get_active_session()
                .map_or(Err(TargetAvcError::RejectedNoAvailablePlayers), |state| {
                    Ok(state.get_supported_player_application_setting_attributes())
                });
            responder.send(&mut response)?;
        }
        TargetHandlerRequest::GetPlayerApplicationSettings { attribute_ids, responder } => {
            let mut response = media_sessions.get_active_session().map_or(
                Err(TargetAvcError::RejectedNoAvailablePlayers),
                |state| {
                    state
                        .session_info()
                        .get_player_application_settings(attribute_ids)
                        .map(Into::into)
                },
            );
            responder.send(&mut response)?;
        }
        TargetHandlerRequest::SetPlayerApplicationSettings { requested_settings, responder } => {
            if let Ok(state) = media_sessions.get_active_session() {
                let set_settings =
                    state.handle_set_player_application_settings(requested_settings.into()).await;
                responder.send(&mut set_settings.map(Into::into))?;
            } else {
                responder.send(&mut Err(TargetAvcError::RejectedNoAvailablePlayers))?;
            }
        }
        TargetHandlerRequest::GetNotification { event_id, responder } => {
            let mut response = media_sessions.get_active_session().map_or(
                Err(TargetAvcError::RejectedNoAvailablePlayers),
                |state| {
                    let notification = state.session_info().get_notification_value(&event_id);
                    notification.map(Into::into)
                },
            );
            responder.send(&mut response)?;
        }
        TargetHandlerRequest::WatchNotification {
            event_id,
            current,
            pos_change_interval,
            responder,
        } => {
            // Add the notification responder to our notifications map.
            // A FIDL response will be sent when the notification specified by `event_id` is triggered.
            let _ = media_sessions.register_notification(
                event_id,
                current.into(),
                pos_change_interval,
                responder,
            );
        }
        TargetHandlerRequest::SetAddressedPlayer { player_id, responder } => {
            let response = media_sessions.set_addressed_player(player_id);
            responder.send(response)?;
        }
        TargetHandlerRequest::GetMediaPlayerItems { responder } => {
            let mut response = media_sessions.get_media_player_items();
            responder.send(&mut response)?;
        }
    }

    Ok(())
}

/// Process and fulfill incoming TargetHandler requests.
pub(crate) async fn handle_target_requests(
    mut target_request_stream: TargetHandlerRequestStream,
    media_sessions: Arc<MediaSessions>,
) -> Result<(), anyhow::Error> {
    while let Some(req) = target_request_stream.try_next().await? {
        let fut = handle_target_request(req, media_sessions.clone());
        if let Err(e) = fut.await {
            warn!("Error handling request: {:?}", e);
        }
    }

    Err(anyhow!("AVRCP TargetHandler dropped."))
}

/// Set up the AVRCP Service and register the target handler.
/// Spin up task for handling incoming TargetHandler requests.
pub(crate) async fn process_avrcp_requests(
    media_sessions: Arc<MediaSessions>,
    mut lifecycle: ComponentLifecycleServer,
) -> Result<(), anyhow::Error> {
    // AVRCP Service Setup
    // Register this target handler with the AVRCP component.
    let avrcp_svc = connect_to_protocol::<PeerManagerMarker>()
        .expect("Failed to connect to Bluetooth AVRCP interface");
    let (target_client, request_stream) =
        create_request_stream::<TargetHandlerMarker>().expect("Error creating Controller endpoint");
    if let Err(e) = avrcp_svc.register_target_handler(target_client).await? {
        return Err(anyhow!("Error registering target handler: {:?}", e));
    }
    // End AVRCP Service Setup

    lifecycle.set(LifecycleState::Ready).await.expect("lifecycle server to set value");

    handle_target_requests(request_stream, media_sessions).await
}
