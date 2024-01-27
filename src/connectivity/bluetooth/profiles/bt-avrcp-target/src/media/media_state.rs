// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_bluetooth_avrcp as fidl_avrcp;
use fidl_fuchsia_media as fidl_media_types;
use fidl_fuchsia_media_sessions2::{self as fidl_media, SessionControlProxy, SessionInfoDelta};
use tracing::{info, warn};

use crate::media::media_types::{
    avrcp_repeat_mode_to_media, avrcp_shuffle_mode_to_media, media_repeat_mode_to_avrcp,
    media_shuffle_mode_to_avrcp, MediaInfo, Notification, PlaybackRate, ValidPlayStatus,
    ValidPlayerApplicationSettings,
};

/// A fixed address for a MediaSession.
///
/// The address is fixed because we currently only support one MediaSession. This
/// addressed ID is used for AVRCP notifications.
pub(crate) const MEDIA_SESSION_ADDRESSED_PLAYER_ID: u16 = 1;

/// A fixed displayable name for a MediaSession.
///
/// The displayable name is fixed because we currently do not support multiple MediaSessions.
/// This name is used for AVRCP Browse channel related identification.
pub(crate) const MEDIA_SESSION_DISPLAYABLE_NAME: &str = "Fuchsia Media Player";

#[derive(Clone, Debug)]
pub(crate) struct MediaState {
    session_control: SessionControlProxy,
    session_info: SessionInfo,
}

impl MediaState {
    pub fn new(session_control: SessionControlProxy) -> Self {
        Self { session_control, session_info: SessionInfo::new() }
    }

    /// Updates `session_info` with the new `info` changes.
    pub fn update_session_info(&mut self, info: SessionInfoDelta) {
        self.session_info.update_session_info(info.player_status, info.metadata);
    }

    /// Updates the saved battery status with the new `status`.
    pub fn update_battery_status(&mut self, status: fidl_avrcp::BatteryStatus) {
        self.session_info.update_battery_status(status);
    }

    pub fn session_info(&self) -> SessionInfo {
        self.session_info.clone()
    }

    /// Set requested player application settings on the MediaPlayer.
    /// Returns `RejectedInvalidParameter` if an unsupported player application setting is provided.
    pub async fn handle_set_player_application_settings(
        &self,
        requested_settings: ValidPlayerApplicationSettings,
    ) -> Result<ValidPlayerApplicationSettings, fidl_avrcp::TargetAvcError> {
        // Currently, MediaSession only supports RepeatStatusMode and ShuffleMode.
        if requested_settings.unsupported_settings_set() {
            return Err(fidl_avrcp::TargetAvcError::RejectedInvalidParameter);
        }

        let mut set_settings = ValidPlayerApplicationSettings::default();
        if let Some(repeat_mode) = requested_settings.repeat_status_mode() {
            let requested_repeat_mode: fidl_media::RepeatMode =
                avrcp_repeat_mode_to_media(repeat_mode);
            match self.session_control.set_repeat_mode(requested_repeat_mode) {
                Ok(_) => {
                    set_settings
                        .set_repeat_status_mode(media_repeat_mode_to_avrcp(requested_repeat_mode));
                }
                Err(_) => return Err(fidl_avrcp::TargetAvcError::RejectedInternalError),
            }
        };

        if let Some(shuffle_mode) = requested_settings.shuffle_mode() {
            let requested_shuffle_mode: bool = avrcp_shuffle_mode_to_media(shuffle_mode);
            match self.session_control.set_shuffle_mode(requested_shuffle_mode) {
                Ok(_) => set_settings
                    .set_shuffle_mode(media_shuffle_mode_to_avrcp(requested_shuffle_mode)),
                Err(_) => return Err(fidl_avrcp::TargetAvcError::RejectedInternalError),
            }
        };

        Ok(set_settings)
    }

    /// Acknowledge key press down event.
    /// If `command` is unsupported, send a `COMMAND_NOT_IMPLEMENTED`.
    /// Otherwise, handle the command on the key release event because it will only
    /// be triggered once (whereas a long press sends pressed multiple times).
    // TODO(fxbug.dev/1216): Add support for VolumeUp/Down.
    pub async fn handle_avc_passthrough_command(
        &self,
        command: fidl_avrcp::AvcPanelCommand,
        pressed: bool,
    ) -> Result<(), fidl_avrcp::TargetPassthroughError> {
        if pressed {
            return self.is_supported_passthrough_command(command);
        }

        if let Err(_) = match command {
            fidl_avrcp::AvcPanelCommand::Play => self.session_control.play(),
            fidl_avrcp::AvcPanelCommand::Pause => self.session_control.pause(),
            fidl_avrcp::AvcPanelCommand::Stop => self.session_control.stop(),
            fidl_avrcp::AvcPanelCommand::FastForward => self.session_control.skip_forward(),
            fidl_avrcp::AvcPanelCommand::Rewind => self.session_control.skip_reverse(),
            fidl_avrcp::AvcPanelCommand::Forward => self.session_control.next_item(),
            fidl_avrcp::AvcPanelCommand::Backward => self.session_control.prev_item(),
            fidl_avrcp::AvcPanelCommand::VolumeUp | fidl_avrcp::AvcPanelCommand::VolumeDown => {
                info!("Received relative {:?} command - ignoring", command);
                Ok(())
            }
            _ => return Err(fidl_avrcp::TargetPassthroughError::CommandNotImplemented),
        } {
            return Err(fidl_avrcp::TargetPassthroughError::CommandRejected);
        }

        Ok(())
    }

    pub fn is_supported_passthrough_command(
        &self,
        command: fidl_avrcp::AvcPanelCommand,
    ) -> Result<(), fidl_avrcp::TargetPassthroughError> {
        match command {
            fidl_avrcp::AvcPanelCommand::Play
            | fidl_avrcp::AvcPanelCommand::Pause
            | fidl_avrcp::AvcPanelCommand::Stop
            | fidl_avrcp::AvcPanelCommand::FastForward
            | fidl_avrcp::AvcPanelCommand::Rewind
            | fidl_avrcp::AvcPanelCommand::Forward
            | fidl_avrcp::AvcPanelCommand::Backward
            | fidl_avrcp::AvcPanelCommand::VolumeUp
            | fidl_avrcp::AvcPanelCommand::VolumeDown => Ok(()),
            _ => Err(fidl_avrcp::TargetPassthroughError::CommandNotImplemented),
        }
    }

    pub fn get_supported_player_application_setting_attributes(
        &self,
    ) -> Vec<fidl_avrcp::PlayerApplicationSettingAttributeId> {
        vec![
            fidl_avrcp::PlayerApplicationSettingAttributeId::RepeatStatusMode,
            fidl_avrcp::PlayerApplicationSettingAttributeId::ShuffleMode,
        ]
    }
}

#[derive(Clone, Debug)]
pub(crate) struct SessionInfo {
    play_status: ValidPlayStatus,
    player_application_settings: ValidPlayerApplicationSettings,
    media_info: MediaInfo,
    playback_rate: Option<PlaybackRate>,
    battery_status: fidl_avrcp::BatteryStatus,
}

impl SessionInfo {
    pub fn new() -> Self {
        Self {
            play_status: Default::default(),
            player_application_settings: ValidPlayerApplicationSettings::new(
                None, None, None, None,
            ),
            media_info: Default::default(),
            playback_rate: None,
            battery_status: fidl_avrcp::BatteryStatus::Normal,
        }
    }

    pub fn get_play_status(&self) -> ValidPlayStatus {
        let mut play_status = self.play_status.clone();
        // Need to update the reported time, since we only update with new info from media.
        if let Some(rate) = &self.playback_rate {
            play_status.song_position = rate.current_position();
        }
        play_status
    }

    pub fn get_media_info(&self) -> &MediaInfo {
        &self.media_info
    }

    pub fn get_playback_rate(&self) -> &Option<PlaybackRate> {
        &self.playback_rate
    }

    /// Return a static value indicating the ID of the player.
    /// For the purposes of Fuchsia Media, there will only be one player, so the
    /// chosen identifier is 1.
    ///
    /// Used for AddressedPlayerChanged notifications.
    pub fn get_player_id(&self) -> u16 {
        MEDIA_SESSION_ADDRESSED_PLAYER_ID
    }

    /// If no `attribute_ids` are provided, return all of the current player
    /// application settings that are supported by the MediaPlayer.
    /// Otherwise, return the currently set settings specified by `attribute_ids`.
    /// Unsupported `attribute_ids` will return an error.
    pub fn get_player_application_settings(
        &self,
        mut attribute_ids: Vec<fidl_avrcp::PlayerApplicationSettingAttributeId>,
    ) -> Result<ValidPlayerApplicationSettings, fidl_avrcp::TargetAvcError> {
        if attribute_ids.is_empty() {
            attribute_ids = self.supported_player_application_settings();
        }

        let mut settings = ValidPlayerApplicationSettings::default();
        for id in attribute_ids {
            match id {
                fidl_avrcp::PlayerApplicationSettingAttributeId::RepeatStatusMode => {
                    settings.set_repeat_status_mode(Some(
                        self.player_application_settings.get_repeat_status_mode(),
                    ));
                }
                fidl_avrcp::PlayerApplicationSettingAttributeId::ShuffleMode => {
                    settings.set_shuffle_mode(Some(
                        self.player_application_settings.get_shuffle_mode(),
                    ));
                }
                _ => {
                    warn!(
                        "Received get request for unsupported player application setting: {:?}.",
                        id
                    );
                    return Err(fidl_avrcp::TargetAvcError::RejectedInvalidParameter);
                }
            }
        }
        Ok(settings)
    }

    /// Returns a `Notification` containing the currently set notification value specified
    /// by `event_id`.
    /// Returns `RejectedInvalidParameter` for unsupported `NotificationEvent` IDs.
    // TODO(fxbug.dev/1216): Add VolumeChanged to supported events when scoped.
    pub fn get_notification_value(
        &self,
        event_id: &fidl_avrcp::NotificationEvent,
    ) -> Result<Notification, fidl_avrcp::TargetAvcError> {
        let mut notification = Notification::default();
        match event_id {
            fidl_avrcp::NotificationEvent::PlaybackStatusChanged => {
                notification.status = Some(self.play_status.get_playback_status());
            }
            fidl_avrcp::NotificationEvent::PlayerApplicationSettingChanged => {
                notification.application_settings = Some(
                    self.get_player_application_settings(vec![])
                        .expect("Should get application settings."),
                );
            }
            fidl_avrcp::NotificationEvent::TrackChanged => {
                notification.media_info = Some(self.media_info.clone());
            }
            fidl_avrcp::NotificationEvent::TrackPosChanged => {
                notification.pos = Some(self.play_status.get_playback_position());
            }
            fidl_avrcp::NotificationEvent::AddressedPlayerChanged => {
                notification.player_id = Some(self.get_player_id());
            }
            fidl_avrcp::NotificationEvent::BattStatusChanged => {
                notification.battery_status = Some(self.battery_status);
            }
            _ => {
                warn!(
                    "Received notification request for unsupported notification event_id {:?}",
                    event_id
                );
                return Err(fidl_avrcp::TargetAvcError::RejectedInvalidParameter);
            }
        }
        Ok(notification)
    }

    pub fn supported_player_application_settings(
        &self,
    ) -> Vec<fidl_avrcp::PlayerApplicationSettingAttributeId> {
        vec![
            fidl_avrcp::PlayerApplicationSettingAttributeId::RepeatStatusMode,
            fidl_avrcp::PlayerApplicationSettingAttributeId::ShuffleMode,
        ]
    }

    /// Updates the state for the session.
    /// If `player_info` is provided, the play_status, player_application_settings,
    /// playback rate, and media_info will be updated.
    pub fn update_session_info(
        &mut self,
        player_info: Option<fidl_media::PlayerStatus>,
        metadata: Option<fidl_media_types::Metadata>,
    ) {
        if let Some(info) = player_info {
            if let Some(timeline_fn) = info.timeline_function {
                self.playback_rate = Some(timeline_fn.into());
            }
            // If the `playback_rate` or `player_state` is None, then the value is unchanged from
            // the previous value.
            // If the `duration` is None, then it is assumed to be unknown, not applicable, or
            // unchanged.
            self.play_status.update_play_status(
                info.duration,
                self.playback_rate,
                info.player_state,
            );
            self.player_application_settings
                .update_player_application_settings(info.repeat_mode, info.shuffle_on);
            self.media_info.update_media_info(info.duration, metadata);
            return;
        }
        if let Some(m) = metadata {
            self.media_info.update_metadata(m);
        }
    }

    /// Updates the saved battery level for the session.
    pub fn update_battery_status(&mut self, status: fidl_avrcp::BatteryStatus) {
        self.battery_status = status;
    }
}

impl From<SessionInfo> for Notification {
    fn from(src: SessionInfo) -> Notification {
        let mut notification = Notification::default();
        notification.status = Some(src.play_status.get_playback_status());
        notification.application_settings = Some(
            src.get_player_application_settings(vec![]).expect("Should get application settings."),
        );
        notification.media_info = Some(src.get_media_info().clone());
        notification.pos = Some(src.play_status.get_playback_position());
        notification.player_id = Some(src.get_player_id());
        notification.battery_status = Some(src.battery_status);
        notification
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use anyhow::{format_err, Error};
    use fidl::encoding::Decodable as FidlDecodable;
    use fidl::endpoints::{create_proxy, create_proxy_and_stream};
    use fidl_fuchsia_media::{self as fidl_media_types};
    use fidl_fuchsia_media_sessions2::{self as fidl_media, *};
    use fuchsia_async as fasync;
    use futures::stream::TryStreamExt;

    pub(crate) fn create_metadata() -> fidl_media_types::Metadata {
        create_metadata_title("This is a sample title".to_string())
    }

    pub(crate) fn create_metadata_title(title: String) -> fidl_media_types::Metadata {
        let mut metadata = fidl_media_types::Metadata::new_empty();
        let mut property1 = fidl_media_types::Property::new_empty();
        property1.label = fidl_media_types::METADATA_LABEL_TITLE.to_string();
        property1.value = title;
        metadata.properties = vec![property1];
        metadata
    }

    pub(crate) fn create_player_status() -> fidl_media::PlayerStatus {
        let mut player_status = fidl_media::PlayerStatus::EMPTY;

        let mut timeline_fn = fidl_media_types::TimelineFunction::new_empty();
        // Playback started at beginning of media.
        timeline_fn.subject_time = 0;
        // Monotonic clock time at beginning of media (nanos).
        timeline_fn.reference_time = 500000000;
        // Playback rate = 1, normal playback.
        timeline_fn.subject_delta = 1;
        timeline_fn.reference_delta = 1;

        player_status.player_state = Some(fidl_media::PlayerState::Playing);
        player_status.duration = Some(123456789);
        player_status.shuffle_on = Some(true);
        player_status.timeline_function = Some(timeline_fn);

        player_status
    }

    async fn wait_for_request(
        mut requests: SessionControlRequestStream,
        predicate: impl Fn(SessionControlRequest) -> bool,
    ) -> Result<(), Error> {
        while let Some(request) = requests.try_next().await? {
            if predicate(request) {
                return Ok(());
            }
        }
        return Err(format_err!("Did not receive request that matched predicate."));
    }

    #[fuchsia::test]
    fn test_is_supported_passthrough_command() {
        let _exec = fasync::TestExecutor::new();
        let (session_proxy, _) = create_proxy::<SessionControlMarker>().expect("Should work");
        let media_state = MediaState::new(session_proxy);
        assert!(media_state
            .is_supported_passthrough_command(fidl_avrcp::AvcPanelCommand::Play)
            .is_ok());
        assert!(media_state
            .is_supported_passthrough_command(fidl_avrcp::AvcPanelCommand::Pause)
            .is_ok());
        assert!(media_state
            .is_supported_passthrough_command(fidl_avrcp::AvcPanelCommand::FastForward)
            .is_ok());
        assert!(media_state
            .is_supported_passthrough_command(fidl_avrcp::AvcPanelCommand::Lock)
            .is_err());
        assert!(media_state
            .is_supported_passthrough_command(fidl_avrcp::AvcPanelCommand::Record)
            .is_err());
    }

    #[fuchsia::test]
    /// Tests updating SessionInfo with Media PlayerStatus and Metadata.
    /// 1) Tests Metadata and no PlayerStatus -> Partial updates
    /// 2) Tests PlayerStatus and no Metadata -> Partial updates
    /// 3) Tests PlayerStatus and Metadata -> Full update
    /// 4) Tests neither PlayerStatus and Metadata -> No updates
    fn test_update_session_info() {
        let exec = fasync::TestExecutor::new_with_fake_time();
        exec.set_fake_time(fasync::Time::from_nanos(555555555));

        let (session_proxy, _) = create_proxy::<SessionControlMarker>().expect("Should work");
        let mut media_state = MediaState::new(session_proxy);

        // 1. Only metadata
        let mut info = fidl_media::SessionInfoDelta::EMPTY;
        let title = "Sapphire 💖".to_string();
        info.metadata = Some(create_metadata_title(title.clone()));
        info.player_status = None;
        media_state.update_session_info(info);

        let expected_play_status = ValidPlayStatus::default();
        let expected_pas = ValidPlayerApplicationSettings::new(None, None, None, None);
        let expected_media_info =
            MediaInfo::new(Some(title.clone()), None, None, None, None, None, None);
        assert_eq!(media_state.session_info().play_status, expected_play_status);
        assert_eq!(media_state.session_info().player_application_settings, expected_pas);
        assert_eq!(media_state.session_info().media_info, expected_media_info);

        // 2. Only PlayerStatus
        exec.set_fake_time(fasync::Time::from_nanos(654321000));
        let mut info = fidl_media::SessionInfoDelta::EMPTY;
        info.metadata = None;
        info.player_status = Some(create_player_status());
        media_state.update_session_info(info);

        let expected_play_status =
            ValidPlayStatus::new(Some(123), Some(154), Some(fidl_avrcp::PlaybackStatus::Playing));
        let expected_pas = ValidPlayerApplicationSettings::new(
            None,
            None,
            Some(fidl_avrcp::ShuffleMode::AllTrackShuffle),
            None,
        );
        let expected_media_info =
            MediaInfo::new(Some(title), None, None, None, None, None, Some("123".to_string()));
        assert_eq!(media_state.session_info().play_status, expected_play_status);
        assert_eq!(media_state.session_info().player_application_settings, expected_pas);
        assert_eq!(media_state.session_info().media_info, expected_media_info);

        // 3. Both
        exec.set_fake_time(fasync::Time::from_nanos(555555555));
        let info = fidl_media::SessionInfoDelta {
            metadata: Some(create_metadata()),
            player_status: Some(create_player_status()),
            ..fidl_media::SessionInfoDelta::EMPTY
        };
        media_state.update_session_info(info);

        let expected_play_status =
            ValidPlayStatus::new(Some(123), Some(55), Some(fidl_avrcp::PlaybackStatus::Playing));
        let expected_pas = ValidPlayerApplicationSettings::new(
            None,
            None,
            Some(fidl_avrcp::ShuffleMode::AllTrackShuffle),
            None,
        );
        let expected_media_info = MediaInfo::new(
            Some("This is a sample title".to_string()),
            None,
            None,
            None,
            None,
            None,
            Some("123".to_string()),
        );
        assert_eq!(media_state.session_info().play_status, expected_play_status);
        assert_eq!(media_state.session_info().player_application_settings, expected_pas);
        assert_eq!(media_state.session_info().media_info, expected_media_info);

        // 4. Neither, values from (3) should stay the same.
        let info = fidl_media::SessionInfoDelta::EMPTY;
        media_state.update_session_info(info);

        assert_eq!(media_state.session_info().play_status, expected_play_status);
        assert_eq!(media_state.session_info().player_application_settings, expected_pas);
        assert_eq!(media_state.session_info().media_info, expected_media_info);
    }

    #[fuchsia::test]
    /// Test that given the current view of the world, getting PlayerApplicationSettings
    /// returns as expected.
    /// 1) Empty view of the world, ask for all settings.
    /// 2) Normal view of the world, ask for all settings.
    /// 3) Normal view of the world, ask for specific settings.
    /// 4) Normal view of the world, ask for unsupported settings.
    fn test_get_player_application_settings() {
        let exec = fasync::TestExecutor::new_with_fake_time();
        exec.set_fake_time(fasync::Time::from_nanos(555555555));

        let (session_proxy, _) = create_proxy::<SessionControlMarker>().expect("Should work");
        let mut media_state = MediaState::new(session_proxy);

        // 1. Query when no state updates.
        let attribute_ids = vec![];
        let settings = media_state.session_info().get_player_application_settings(attribute_ids);
        let expected_settings = ValidPlayerApplicationSettings::new(
            None,
            Some(fidl_avrcp::RepeatStatusMode::Off),
            Some(fidl_avrcp::ShuffleMode::Off),
            None,
        );
        assert_eq!(settings.expect("Should work"), expected_settings);

        exec.set_fake_time(fasync::Time::from_nanos(555555555));
        let info = fidl_media::SessionInfoDelta {
            metadata: Some(create_metadata()),
            player_status: Some(create_player_status()),
            ..fidl_media::SessionInfoDelta::EMPTY
        };
        media_state.update_session_info(info);

        // 2. Updated state, all settings.
        let attribute_ids = vec![];
        let settings = media_state.session_info().get_player_application_settings(attribute_ids);
        let expected_settings = ValidPlayerApplicationSettings::new(
            None,
            Some(fidl_avrcp::RepeatStatusMode::Off),
            Some(fidl_avrcp::ShuffleMode::AllTrackShuffle),
            None,
        );
        assert_eq!(settings.expect("Should work"), expected_settings);

        // 3. Updated state, specific settings.
        let attribute_ids = vec![fidl_avrcp::PlayerApplicationSettingAttributeId::RepeatStatusMode];
        let settings = media_state.session_info().get_player_application_settings(attribute_ids);
        let expected_settings = ValidPlayerApplicationSettings::new(
            None,
            Some(fidl_avrcp::RepeatStatusMode::Off),
            None,
            None,
        );
        assert_eq!(settings.expect("Should work"), expected_settings);

        // 4. Updated state, unsupported settings.
        let unsupported_ids = vec![fidl_avrcp::PlayerApplicationSettingAttributeId::Equalizer];
        let settings = media_state.session_info().get_player_application_settings(unsupported_ids);
        assert!(settings.is_err());
    }

    #[fuchsia::test]
    /// Test that given the current view of the world, getting PlayStatus
    /// returns as expected after time passes.
    fn test_get_play_status() {
        let exec = fasync::TestExecutor::new_with_fake_time();
        exec.set_fake_time(fasync::Time::from_nanos(555555555));

        let (session_proxy, _) = create_proxy::<SessionControlMarker>().expect("Should work");
        let mut media_state = MediaState::new(session_proxy);

        // Add the media session status.
        exec.set_fake_time(fasync::Time::from_nanos(555555555));
        let info = fidl_media::SessionInfoDelta {
            metadata: Some(create_metadata()),
            player_status: Some(create_player_status()),
            ..fidl_media::SessionInfoDelta::EMPTY
        };
        media_state.update_session_info(info);

        let expected_play_status =
            ValidPlayStatus::new(Some(123), Some(55), Some(fidl_avrcp::PlaybackStatus::Playing));

        assert_eq!(media_state.session_info().get_play_status(), expected_play_status);

        // Fast forward time by a little bit.
        exec.set_fake_time(fasync::Time::after(fasync::Duration::from_seconds(7)));

        let expected_play_status =
            ValidPlayStatus::new(Some(123), Some(7055), Some(fidl_avrcp::PlaybackStatus::Playing));

        assert_eq!(media_state.session_info().get_play_status(), expected_play_status);

        // On an update from media, updates the play status ahain.
        let player_status = fidl_media::PlayerStatus {
            player_state: Some(fidl_media::PlayerState::Paused),
            duration: Some(123456789),
            timeline_function: Some(fidl_fuchsia_media::TimelineFunction {
                subject_time: 0,
                reference_time: fasync::Time::now().into_nanos(),
                subject_delta: 0,
                reference_delta: 1,
            }),
            ..fidl_media::PlayerStatus::EMPTY
        };

        let info = fidl_media::SessionInfoDelta {
            metadata: Some(create_metadata()),
            player_status: Some(player_status),
            ..fidl_media::SessionInfoDelta::EMPTY
        };
        media_state.update_session_info(info);

        let expected_play_status =
            ValidPlayStatus::new(Some(123), Some(0), Some(fidl_avrcp::PlaybackStatus::Paused));

        assert_eq!(media_state.session_info().get_play_status(), expected_play_status);

        // After a couple seocnds, it should still be paused.
        exec.set_fake_time(fasync::Time::after(fasync::Duration::from_seconds(7)));

        assert_eq!(media_state.session_info().get_play_status(), expected_play_status);
    }

    #[fuchsia::test]
    /// Test that retrieving a notification value correctly gets the current state.
    /// 1) Query with an unsupported `event_id`.
    /// 2) Query with a supported Event ID, with default state.
    /// 3) Query with all supported Event IDs.
    fn test_get_notification_value() {
        let exec = fasync::TestExecutor::new_with_fake_time();
        exec.set_fake_time(fasync::Time::from_nanos(555555555));
        let (session_proxy, _) =
            create_proxy::<SessionControlMarker>().expect("Couldn't create fidl proxy.");
        let mut media_state = MediaState::new(session_proxy);

        // 1. Unsupported ID.
        let unsupported_id = fidl_avrcp::NotificationEvent::UidsChanged;
        let res = media_state.session_info().get_notification_value(&unsupported_id);
        assert!(res.is_err());

        // 2. Supported ID, `media_state` contains default values.
        let res = media_state
            .session_info()
            .get_notification_value(&fidl_avrcp::NotificationEvent::PlaybackStatusChanged);
        let res = res.expect("Result should be returned");
        assert_eq!(res.status, Some(fidl_avrcp::PlaybackStatus::Stopped));

        // 3. All supported notification events.
        exec.set_fake_time(fasync::Time::from_nanos(555555555));
        let info = fidl_media::SessionInfoDelta {
            metadata: Some(create_metadata()),
            player_status: Some(create_player_status()),
            ..fidl_media::SessionInfoDelta::EMPTY
        };
        media_state.update_session_info(info);
        media_state.update_battery_status(fidl_avrcp::BatteryStatus::Critical);

        let expected_play_status = fidl_avrcp::PlaybackStatus::Playing;
        let expected_pas = fidl_avrcp::PlayerApplicationSettings {
            repeat_status_mode: Some(fidl_avrcp::RepeatStatusMode::Off),
            shuffle_mode: Some(fidl_avrcp::ShuffleMode::AllTrackShuffle),
            ..fidl_avrcp::PlayerApplicationSettings::EMPTY
        };
        let requested_events = vec![
            fidl_avrcp::NotificationEvent::PlayerApplicationSettingChanged,
            fidl_avrcp::NotificationEvent::PlaybackStatusChanged,
            fidl_avrcp::NotificationEvent::TrackChanged,
            fidl_avrcp::NotificationEvent::BattStatusChanged,
        ];
        let expected_values = vec![
            fidl_avrcp::Notification {
                application_settings: Some(expected_pas),
                ..fidl_avrcp::Notification::EMPTY
            },
            fidl_avrcp::Notification {
                status: Some(expected_play_status),
                ..fidl_avrcp::Notification::EMPTY
            },
            fidl_avrcp::Notification { track_id: Some(0), ..fidl_avrcp::Notification::EMPTY },
            fidl_avrcp::Notification {
                battery_status: Some(fidl_avrcp::BatteryStatus::Critical),
                ..fidl_avrcp::Notification::EMPTY
            },
        ];

        for (event_id, expected_v) in requested_events.iter().zip(expected_values.iter()) {
            let received: fidl_avrcp::Notification = media_state
                .session_info()
                .get_notification_value(event_id)
                .expect("The notification value should exist in the map")
                .into();
            assert_eq!(received, expected_v.clone());
        }
    }

    #[fuchsia::test]
    /// Tests sending an avc passthrough command results in expected behavior.
    /// Creates a fake Player and listens on the request stream.
    /// 1. Tests sending an unsupported AvcPanelCommand results in error.
    /// 2. Test sending a supported command is successful, including receiving the event
    /// on the PlayerRequestStream.
    async fn test_handle_avc_passthrough_command() {
        let (session_proxy, session_request_stream) =
            create_proxy_and_stream::<SessionControlMarker>().expect("Should work");
        let media_state = MediaState::new(session_proxy);

        // 1. Valid, but unsupported control request.
        let valid_unsupported = fidl_avrcp::AvcPanelCommand::Enter;
        let res = media_state.handle_avc_passthrough_command(valid_unsupported, false).await;
        assert!(res.is_err());

        // 2. Listen in on the player request stream and send a supported AvcPanelCommand::Play.
        let valid_command = fidl_avrcp::AvcPanelCommand::Play;
        let res0 = media_state.handle_avc_passthrough_command(valid_command, false).await;
        let res1 = wait_for_request(session_request_stream, |request| match request {
            SessionControlRequest::Play { .. } => true,
            _ => false,
        })
        .await;
        assert!(res0.is_ok());
        assert!(res1.is_ok());
    }

    #[fuchsia::test]
    /// Tests handling setting of PlayerApplicationSettings successfully propagates to the MediaSession.
    /// 1. Tests sending an unsupported application setting results in Error.
    /// 2. Tests sending an unsupported application setting among supported settings results
    /// in error; None of the settings should've been set.
    /// 3. Tests sending supported application settings results in success, with the set settings
    /// returned.
    async fn test_handle_set_player_application_settings_command() {
        let (session_proxy, session_request_stream) =
            create_proxy_and_stream::<SessionControlMarker>().expect("Should work");
        let media_state = MediaState::new(session_proxy);

        // 1. Equalizer is unsupported.
        let unsupported =
            ValidPlayerApplicationSettings::new(Some(fidl_avrcp::Equalizer::On), None, None, None);
        let res = media_state.handle_set_player_application_settings(unsupported).await;
        assert!(res.is_err());

        // 2. ScanMode is unsupported, but ShuffleMode is. Should return an err.
        let unsupported = ValidPlayerApplicationSettings::new(
            Some(fidl_avrcp::Equalizer::On),
            None,
            Some(fidl_avrcp::ShuffleMode::Off),
            None,
        );
        let res = media_state.handle_set_player_application_settings(unsupported).await;
        assert!(res.is_err());

        // 3. ShuffleMode, RepeatStatusMode supported.
        // However, since there is not a 1:1 mapping of AVRCP settings to Media,
        // the returned `set_settings` is different than the requested.
        let supported = ValidPlayerApplicationSettings::new(
            None,
            Some(fidl_avrcp::RepeatStatusMode::AllTrackRepeat),
            Some(fidl_avrcp::ShuffleMode::GroupShuffle),
            None,
        );
        let set_settings = media_state.handle_set_player_application_settings(supported).await;

        fn expect_fn(request: SessionControlRequest) -> bool {
            match request {
                SessionControlRequest::SetRepeatMode { .. } => true,
                SessionControlRequest::SetShuffleMode { .. } => true,
                _ => false,
            }
        }
        let res0 = wait_for_request(session_request_stream, &expect_fn).await;
        let expected = ValidPlayerApplicationSettings::new(
            None,
            Some(fidl_avrcp::RepeatStatusMode::GroupRepeat),
            Some(fidl_avrcp::ShuffleMode::AllTrackShuffle),
            None,
        );
        assert_eq!(set_settings.expect("Should be ok"), expected);
        assert!(res0.is_ok());
    }
}
