// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl::endpoints::Proxy, fidl_fuchsia_bluetooth_avrcp::*, futures::TryFutureExt,
    parking_lot::Mutex, std::sync::Arc,
};

use crate::types::PeerError as Error;

/// Delegates commands received on any peer channels to the currently registered target handler and
/// absolute volume handler.
/// If no target handler or absolute volume handler is registered with the service, this delegate
/// will return appropriate stub responses.
/// If a target handler is changed or closed at any point, this delegate will handle the state
/// transitions for any outstanding and pending registered notifications.
#[derive(Debug)]
pub struct TargetDelegate {
    inner: Arc<Mutex<TargetDelegateInner>>,
}

#[derive(Debug)]
struct TargetDelegateInner {
    target_handler: Option<TargetHandlerProxy>,
    absolute_volume_handler: Option<AbsoluteVolumeHandlerProxy>,
}

impl TargetDelegate {
    pub fn new() -> TargetDelegate {
        TargetDelegate {
            inner: Arc::new(Mutex::new(TargetDelegateInner {
                target_handler: None,
                absolute_volume_handler: None,
            })),
        }
    }

    // Retrieves a clone of the current volume handler, if there is one, otherwise returns None.
    fn absolute_volume_handler(&self) -> Option<AbsoluteVolumeHandlerProxy> {
        let mut guard = self.inner.lock();
        if guard.absolute_volume_handler.as_ref().map_or(false, Proxy::is_closed) {
            guard.absolute_volume_handler = None;
        }
        guard.absolute_volume_handler.clone()
    }

    // Retrieves a clone of the current target handler, if there is one, otherwise returns None.
    fn target_handler(&self) -> Option<TargetHandlerProxy> {
        let mut guard = self.inner.lock();
        if guard.target_handler.as_ref().map_or(false, Proxy::is_closed) {
            guard.target_handler = None;
        }
        guard.target_handler.clone()
    }

    /// Sets the target delegate. Resets any pending registered notifications.
    /// If the delegate is already set, returns an Error.
    pub fn set_target_handler(&self, target_handler: TargetHandlerProxy) -> Result<(), Error> {
        let mut guard = self.inner.lock();
        if guard.target_handler.as_ref().map_or(false, |p| !p.is_closed()) {
            return Err(Error::TargetBound);
        }
        guard.target_handler = Some(target_handler);
        Ok(())
    }

    /// Sets the absolute volume handler delegate. Returns an error if one is currently active.
    pub fn set_absolute_volume_handler(
        &self,
        absolute_volume_handler: AbsoluteVolumeHandlerProxy,
    ) -> Result<(), Error> {
        let mut guard = self.inner.lock();
        if guard.absolute_volume_handler.as_ref().map_or(false, |p| !p.is_closed()) {
            return Err(Error::TargetBound);
        }

        guard.absolute_volume_handler = Some(absolute_volume_handler);
        Ok(())
    }

    /// Send a passthrough panel command
    pub async fn send_passthrough_command(
        &self,
        command: AvcPanelCommand,
        pressed: bool,
    ) -> Result<(), TargetPassthroughError> {
        let target_handler =
            self.target_handler().ok_or(TargetPassthroughError::CommandRejected)?;

        // if we have a FIDL error, reject the passthrough command
        target_handler
            .send_command(command, pressed)
            .await
            .map_err(|_| TargetPassthroughError::CommandRejected)?
    }

    /// Get the supported events from the target handler or a default set of events if no
    /// target handler is set.
    /// This function always returns a result and not an error as this call may happen before a target handler is set.
    pub async fn get_supported_events(&self) -> Vec<NotificationEvent> {
        // Spec requires that we reply with at least two notification types.
        // Reply we support volume change always and if there is no absolute volume handler, we
        // respond with an error of no players available.
        // Also respond we have address player change so we can synthesize that event.
        let mut default_events =
            vec![NotificationEvent::VolumeChanged, NotificationEvent::AddressedPlayerChanged];

        let cmd_fut = match self.target_handler() {
            None => return default_events,
            Some(x) => x.get_events_supported(),
        };

        if let Ok(Ok(mut events)) = cmd_fut.await {
            // We always also support volume change and addressed player changed.
            events.append(&mut default_events);
            events.sort_unstable();
            events.dedup();
            events
        } else {
            // Ignore FIDL errors and errors from the target handler and return the default set of notifications.
            default_events
        }
    }

    pub async fn send_get_play_status_command(&self) -> Result<PlayStatus, TargetAvcError> {
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;

        // if we have a FIDL error, return no players available
        target_handler
            .get_play_status()
            .await
            .map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)?
    }

    /// Send a set absolute volume command to the absolute volume handler.
    pub async fn send_set_absolute_volume_command(&self, volume: u8) -> Result<u8, TargetAvcError> {
        let abs_vol_handler =
            self.absolute_volume_handler().ok_or(TargetAvcError::RejectedInvalidParameter)?;
        abs_vol_handler
            .set_volume(volume)
            .map_err(|_| TargetAvcError::RejectedInvalidParameter)
            .await
    }

    /// Get current value of the notification
    pub async fn send_get_notification(
        &self,
        event: NotificationEvent,
    ) -> Result<Notification, TargetAvcError> {
        if event == NotificationEvent::VolumeChanged {
            let abs_vol_handler =
                self.absolute_volume_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;
            // if we have a FIDL error return no players
            let volume = abs_vol_handler
                .get_current_volume()
                .map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)
                .await?;

            return Ok(Notification { volume: Some(volume), ..Default::default() });
        }
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;
        // if we have a FIDL error, return no players available
        target_handler
            .get_notification(event)
            .await
            .map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)?
    }

    /// Watch for the change of the notification value
    // TODO(fxbug.dev/54002): Instead of cloning the AbsoluteVolumeHandlerProxy and then
    // sending a new `on_volume_changed()` hanging-get request, AVRCP should
    // monitor any outstanding request, and subscribe each peer to the result
    // of the single outstanding request.
    pub async fn send_watch_notification(
        &self,
        event: NotificationEvent,
        current_value: Notification,
        pos_change_interval: u32,
    ) -> Result<Notification, TargetAvcError> {
        if event == NotificationEvent::VolumeChanged {
            let abs_vol_handler = self
                .absolute_volume_handler()
                .ok_or(TargetAvcError::RejectedAddressedPlayerChanged)?;
            let volume = abs_vol_handler
                .on_volume_changed()
                .map_err(|_| TargetAvcError::RejectedAddressedPlayerChanged)
                .await?;

            return Ok(Notification { volume: Some(volume), ..Default::default() });
        }
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedAddressedPlayerChanged)?;
        // if we have a FIDL error, send that the players changed
        target_handler
            .watch_notification(event, current_value, pos_change_interval)
            .await
            .map_err(|_| TargetAvcError::RejectedAddressedPlayerChanged)?
    }

    pub async fn send_get_media_attributes_command(
        &self,
    ) -> Result<MediaAttributes, TargetAvcError> {
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;
        target_handler
            .get_media_attributes()
            .await
            .map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)?
    }

    pub async fn send_list_player_application_setting_attributes_command(
        &self,
    ) -> Result<Vec<PlayerApplicationSettingAttributeId>, TargetAvcError> {
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;
        target_handler
            .list_player_application_setting_attributes()
            .await
            .map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)?
    }

    pub async fn send_get_player_application_settings_command(
        &self,
        attributes: Vec<PlayerApplicationSettingAttributeId>,
    ) -> Result<PlayerApplicationSettings, TargetAvcError> {
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;
        let send_command_fut = target_handler.get_player_application_settings(&attributes);
        send_command_fut.await.map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)?
    }

    pub async fn send_set_player_application_settings_command(
        &self,
        requested_settings: PlayerApplicationSettings,
    ) -> Result<PlayerApplicationSettings, TargetAvcError> {
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;
        target_handler
            .set_player_application_settings(requested_settings)
            .await
            .map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)?
    }

    pub async fn send_get_media_player_items_command(
        &self,
    ) -> Result<Vec<MediaPlayerItem>, TargetAvcError> {
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;
        target_handler
            .get_media_player_items()
            .await
            .map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)?
    }

    pub async fn send_set_addressed_player_command(
        &self,
        mut player_id: AddressedPlayerId,
    ) -> Result<(), TargetAvcError> {
        let target_handler =
            self.target_handler().ok_or(TargetAvcError::RejectedNoAvailablePlayers)?;
        target_handler
            .set_addressed_player(&mut player_id)
            .await
            .map_err(|_| TargetAvcError::RejectedNoAvailablePlayers)?
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use assert_matches::assert_matches;
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_bluetooth_avrcp::{
        Equalizer, PlayerApplicationSettings, ShuffleMode, TargetHandlerMarker,
        TargetHandlerRequest,
    };
    use fuchsia_async as fasync;
    use futures::StreamExt;
    use std::task::Poll;

    // This also gets tested at a service level. Test that we get a TargetBound error on double set.
    // Test that we can set again after the target handler has closed.
    #[test]
    fn set_target_test() {
        let mut exec = fasync::TestExecutor::new();
        let (target_proxy_1, target_stream_1) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Error creating TargetHandler endpoint");
        let (target_proxy_2, _target_stream_2) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Error creating TargetHandler endpoint");

        let target_delegate = TargetDelegate::new();
        assert_matches!(target_delegate.set_target_handler(target_proxy_1), Ok(()));
        assert_matches!(
            target_delegate.set_target_handler(target_proxy_2.clone()),
            Err(Error::TargetBound)
        );

        // After the target handler is dropped, we should be able to set the handler.
        drop(target_stream_1);
        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());

        assert_matches!(target_delegate.set_target_handler(target_proxy_2), Ok(()));
    }

    #[test]
    // Test getting correct response from a get_media_attributes command.
    fn test_get_media_attributes() {
        let mut exec = fasync::TestExecutor::new();
        let target_delegate = TargetDelegate::new();

        let (target_proxy, mut target_stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Error creating TargetHandler endpoint");
        assert_matches!(target_delegate.set_target_handler(target_proxy), Ok(()));

        let get_media_attr_fut = target_delegate.send_get_media_attributes_command();
        pin_utils::pin_mut!(get_media_attr_fut);
        assert!(exec.run_until_stalled(&mut get_media_attr_fut).is_pending());

        let select_next_some_fut = target_stream.select_next_some();
        pin_utils::pin_mut!(select_next_some_fut);
        match exec.run_until_stalled(&mut select_next_some_fut) {
            Poll::Ready(Ok(TargetHandlerRequest::GetMediaAttributes { responder })) => {
                assert!(responder.send(&mut Ok(MediaAttributes::default())).is_ok());
            }
            _ => assert!(false, "unexpected stream state"),
        };

        match exec.run_until_stalled(&mut get_media_attr_fut) {
            Poll::Ready(attributes) => {
                assert_eq!(attributes, Ok(MediaAttributes::default()));
            }
            _ => assert!(false, "unexpected state"),
        }
    }

    #[test]
    // Test getting correct response from a list_player_application_settings command.
    fn test_list_player_application_settings() {
        let mut exec = fasync::TestExecutor::new();
        let target_delegate = TargetDelegate::new();

        let (target_proxy, mut target_stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Error creating TargetHandler endpoint");
        assert_matches!(target_delegate.set_target_handler(target_proxy), Ok(()));

        let list_pas_fut =
            target_delegate.send_list_player_application_setting_attributes_command();
        pin_utils::pin_mut!(list_pas_fut);
        assert!(exec.run_until_stalled(&mut list_pas_fut).is_pending());

        let select_next_some_fut = target_stream.select_next_some();
        pin_utils::pin_mut!(select_next_some_fut);
        match exec.run_until_stalled(&mut select_next_some_fut) {
            Poll::Ready(Ok(TargetHandlerRequest::ListPlayerApplicationSettingAttributes {
                responder,
            })) => {
                assert!(responder.send(&mut Ok(vec![])).is_ok());
            }
            _ => assert!(false, "unexpected stream state"),
        };

        match exec.run_until_stalled(&mut list_pas_fut) {
            Poll::Ready(attributes) => {
                assert_eq!(attributes, Ok(vec![]));
            }
            _ => assert!(false, "unexpected state"),
        }
    }

    #[test]
    // Test getting correct response from a get_player_application_settings command.
    fn test_get_player_application_settings() {
        let mut exec = fasync::TestExecutor::new();
        let target_delegate = TargetDelegate::new();

        let (target_proxy, mut target_stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Error creating TargetHandler endpoint");
        assert_matches!(target_delegate.set_target_handler(target_proxy), Ok(()));

        let attributes = vec![PlayerApplicationSettingAttributeId::ShuffleMode];
        let get_pas_fut = target_delegate.send_get_player_application_settings_command(attributes);
        pin_utils::pin_mut!(get_pas_fut);
        assert!(exec.run_until_stalled(&mut get_pas_fut).is_pending());

        let select_next_some_fut = target_stream.select_next_some();
        pin_utils::pin_mut!(select_next_some_fut);
        match exec.run_until_stalled(&mut select_next_some_fut) {
            Poll::Ready(Ok(TargetHandlerRequest::GetPlayerApplicationSettings {
                responder,
                ..
            })) => {
                assert!(responder
                    .send(&mut Ok(PlayerApplicationSettings {
                        shuffle_mode: Some(ShuffleMode::Off),
                        ..Default::default()
                    }))
                    .is_ok());
            }
            _ => assert!(false, "unexpected stream state"),
        };

        match exec.run_until_stalled(&mut get_pas_fut) {
            Poll::Ready(attributes) => {
                assert_eq!(
                    attributes,
                    Ok(PlayerApplicationSettings {
                        shuffle_mode: Some(ShuffleMode::Off),
                        ..Default::default()
                    })
                );
            }
            _ => assert!(false, "unexpected state"),
        }
    }

    #[test]
    // Test getting correct response from a get_player_application_settings command.
    fn test_set_player_application_settings() {
        let mut exec = fasync::TestExecutor::new();
        let target_delegate = TargetDelegate::new();

        let (target_proxy, mut target_stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Error creating TargetHandler endpoint");
        assert_matches!(target_delegate.set_target_handler(target_proxy), Ok(()));

        // Current media doesn't support Equalizer.
        let attributes =
            PlayerApplicationSettings { equalizer: Some(Equalizer::Off), ..Default::default() };
        let set_pas_fut = target_delegate.send_set_player_application_settings_command(attributes);
        pin_utils::pin_mut!(set_pas_fut);
        assert!(exec.run_until_stalled(&mut set_pas_fut).is_pending());

        let select_next_some_fut = target_stream.select_next_some();
        pin_utils::pin_mut!(select_next_some_fut);
        match exec.run_until_stalled(&mut select_next_some_fut) {
            Poll::Ready(Ok(TargetHandlerRequest::SetPlayerApplicationSettings {
                responder,
                ..
            })) => {
                assert!(responder.send(&mut Ok(PlayerApplicationSettings::default())).is_ok());
            }
            _ => assert!(false, "unexpected stream state"),
        };

        // We expect the returned `set_settings` to be empty, since we requested an
        // unsupported application setting.
        match exec.run_until_stalled(&mut set_pas_fut) {
            Poll::Ready(attr) => {
                assert_eq!(attr, Ok(PlayerApplicationSettings::default()));
            }
            _ => assert!(false, "unexpected state"),
        }
    }

    #[test]
    // Test getting correct response from a get_media_player_items request.
    fn test_get_media_player_items() {
        let mut exec = fasync::TestExecutor::new();
        let target_delegate = TargetDelegate::new();

        let (target_proxy, mut target_stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Error creating TargetHandler endpoint");
        assert_matches!(target_delegate.set_target_handler(target_proxy), Ok(()));

        let get_media_fut = target_delegate.send_get_media_player_items_command();
        pin_utils::pin_mut!(get_media_fut);
        assert!(exec.run_until_stalled(&mut get_media_fut).is_pending());

        let select_next_some_fut = target_stream.select_next_some();
        pin_utils::pin_mut!(select_next_some_fut);
        match exec.run_until_stalled(&mut select_next_some_fut) {
            Poll::Ready(Ok(TargetHandlerRequest::GetMediaPlayerItems { responder, .. })) => {
                assert!(responder
                    .send(&mut Ok(vec![MediaPlayerItem {
                        player_id: Some(1),
                        ..Default::default()
                    }]))
                    .is_ok());
            }
            _ => assert!(false, "unexpected stream state"),
        };

        match exec.run_until_stalled(&mut get_media_fut) {
            Poll::Ready(items) => {
                assert!(items.is_ok());
                let items = items.expect("Just checked");
                assert_eq!(items.len(), 1);
                assert_eq!(items[0].player_id, Some(1));
            }
            _ => assert!(false, "unexpected state"),
        }
    }

    // test we get the default response before a target handler is set and test we get the response
    // from a target handler that we expected.
    #[test]
    fn target_handler_response() {
        let mut exec = fasync::TestExecutor::new();

        let target_delegate = TargetDelegate::new();
        {
            // try without a target handler
            let get_supported_events_fut = target_delegate.get_supported_events();
            pin_utils::pin_mut!(get_supported_events_fut);
            match exec.run_until_stalled(&mut get_supported_events_fut) {
                Poll::Ready(events) => {
                    assert_eq!(
                        events,
                        vec![
                            NotificationEvent::VolumeChanged,
                            NotificationEvent::AddressedPlayerChanged
                        ]
                    );
                }
                _ => assert!(false, "wrong default value"),
            };
        }

        {
            // try with a target handler
            let (target_proxy, mut target_stream) =
                create_proxy_and_stream::<TargetHandlerMarker>()
                    .expect("Error creating TargetHandler endpoint");
            assert_matches!(target_delegate.set_target_handler(target_proxy), Ok(()));

            let get_supported_events_fut = target_delegate.get_supported_events();
            pin_utils::pin_mut!(get_supported_events_fut);
            assert!(exec.run_until_stalled(&mut get_supported_events_fut).is_pending());

            let select_next_some_fut = target_stream.select_next_some();
            pin_utils::pin_mut!(select_next_some_fut);
            match exec.run_until_stalled(&mut select_next_some_fut) {
                Poll::Ready(Ok(TargetHandlerRequest::GetEventsSupported { responder })) => {
                    assert!(responder
                        .send(&mut Ok(vec![
                            NotificationEvent::VolumeChanged,
                            NotificationEvent::TrackPosChanged,
                        ]))
                        .is_ok());
                }
                _ => assert!(false, "unexpected stream state"),
            };

            match exec.run_until_stalled(&mut get_supported_events_fut) {
                Poll::Ready(events) => {
                    assert!(events.contains(&NotificationEvent::VolumeChanged));
                    assert!(events.contains(&NotificationEvent::AddressedPlayerChanged));
                    assert!(events.contains(&NotificationEvent::TrackPosChanged));
                }
                _ => assert!(false, "unexpected state"),
            }
        }
    }
}
