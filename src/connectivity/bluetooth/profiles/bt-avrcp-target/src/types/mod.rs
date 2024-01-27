// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    fidl_fuchsia_bluetooth_avrcp as fidl_avrcp, fuchsia_async as fasync,
    tracing::warn,
};

pub mod bounded_queue;

use crate::media::media_types::Notification;

/// An upper bound for the maximum number of responders that can be stored for an event ID.
/// This number was chosen because there can be at most 7 connected devices, and
/// each device can subscribe to the maximum number of notifications, which is
/// currently 7.
pub(crate) const MAX_NOTIFICATION_EVENT_QUEUE_SIZE: usize = 64;

/// The data stored for an outstanding notification.
#[derive(Debug)]
pub(crate) struct NotificationData {
    /// The event_id of the notification.
    event_id: fidl_avrcp::NotificationEvent,
    /// The current value of the notification when the client subscribed.
    current_value: Notification,
    /// The position change interval of the notification, for `TRACK_POS_CHANGED`.
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    pos_change_interval: u32,
    /// The time when we expect to reply automatically to the responder.
    expected_response_time: Option<fasync::Time>,
    /// The FIDL responder to send the reply when the notification value changes.
    responder: Option<fidl_avrcp::TargetHandlerWatchNotificationResponder>,
}

impl NotificationData {
    pub fn new(
        event_id: fidl_avrcp::NotificationEvent,
        current_value: Notification,
        pos_change_interval: u32,
        expected_response_time: Option<fasync::Time>,
        responder: fidl_avrcp::TargetHandlerWatchNotificationResponder,
    ) -> Self {
        Self {
            event_id,
            current_value,
            pos_change_interval,
            expected_response_time,
            responder: Some(responder),
        }
    }

    fn send(
        &mut self,
        value: Result<Notification, fidl_avrcp::TargetAvcError>,
    ) -> Result<(), fidl::Error> {
        if let Some(responder) = self.responder.take() {
            responder.send(value.map(Into::into).as_ref().map_err(|e| *e))
        } else {
            Err(fidl::Error::NotNullable)
        }
    }

    /// Send the `updated_val` over the responder.
    /// If the updated_value exists, and has changed, send over the responder.
    /// If the updated_value exists, but hasn't changed, return Self.
    /// If the updated_value is an error, send the error over the responder.
    pub fn update_responder(
        mut self,
        event_id: &fidl_avrcp::NotificationEvent,
        updated_val: Result<Notification, fidl_avrcp::TargetAvcError>,
    ) -> Result<Option<Self>, Error> {
        let response = if let Ok(val) = updated_val {
            // If an invalid `event_id` is provided, send RejectedInvalidParameter as per AVRCP 1.6.
            match self.notification_value_changed(event_id, &val) {
                Ok(true) => Ok(val.only_event(event_id)),
                Ok(false) => return Ok(Some(self)),
                Err(_) => Err(fidl_avrcp::TargetAvcError::RejectedInvalidParameter),
            }
        } else {
            updated_val
        };

        self.send(response).map(|_| None).map_err(|_| format_err!("Responder send error"))
    }

    /// Compares the initial value, `self.current_value` to the given new
    /// notification value, `current` for an event specified by `event_id`.
    /// Returns true if the value for `event_id` has changed.
    /// If an unsupported `event_id` is provided, an error will be returned.
    fn notification_value_changed(
        &self,
        event_id: &fidl_avrcp::NotificationEvent,
        new_value: &Notification,
    ) -> Result<bool, Error> {
        match event_id {
            fidl_avrcp::NotificationEvent::PlaybackStatusChanged => {
                Ok(self.current_value.status != new_value.status)
            }
            fidl_avrcp::NotificationEvent::PlayerApplicationSettingChanged => {
                Ok(self.current_value.application_settings != new_value.application_settings)
            }
            fidl_avrcp::NotificationEvent::TrackChanged => {
                // TODO(fxbug.dev/104358): be able to use track_id alone
                match (&new_value.media_info, &self.current_value.media_info) {
                    (None, None) => Ok(false),
                    (Some(_), None) | (None, Some(_)) => Ok(true),
                    (Some(new), Some(old)) => Ok(old.track_changed(new)),
                }
            }
            fidl_avrcp::NotificationEvent::TrackPosChanged => {
                let flag = self.current_value.pos != new_value.pos
                    || self.current_value.status != new_value.status
                    || self.current_value.media_info != new_value.media_info
                    || self.expected_response_time.map_or(false, |t| fasync::Time::now() >= t);
                Ok(flag)
            }
            fidl_avrcp::NotificationEvent::BattStatusChanged => {
                Ok(self.current_value.battery_status != new_value.battery_status)
            }
            fidl_avrcp::NotificationEvent::AddressedPlayerChanged => {
                Ok(self.current_value.player_id != new_value.player_id)
            }
            _ => {
                warn!(
                    "Received notification request for unsupported notification event_id {:?}",
                    event_id
                );
                Err(format_err!("Invalid event_id provided"))
            }
        }
    }
}

impl Drop for NotificationData {
    fn drop(&mut self) {
        // Try to send the current value back over the responder before dropping.
        // It is likely that the `NotificationData` does not contain an active responder, in
        // which case this is a no-op.
        let curr_value = self.current_value.clone().only_event(&self.event_id);
        let _ = self.send(Ok(curr_value));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use crate::media::media_types::MediaInfo;
    use crate::tests::generate_empty_watch_notification;

    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_bluetooth_avrcp::{self as fidl_avrcp, TargetHandlerMarker};

    #[fuchsia::test]
    /// Tests the comparison of `Notification` values works as intended.
    async fn test_notification_value_changed() -> Result<(), Error> {
        let (mut proxy, mut stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Couldn't create proxy and stream");

        let (result_fut, responder) =
            generate_empty_watch_notification(&mut proxy, &mut stream).await?;
        {
            let prev_value: Notification = fidl_avrcp::Notification {
                status: Some(fidl_avrcp::PlaybackStatus::Stopped),
                track_id: Some(999),
                pos: Some(12345),
                application_settings: Some(fidl_avrcp::PlayerApplicationSettings {
                    repeat_status_mode: Some(fidl_avrcp::RepeatStatusMode::GroupRepeat),
                    scan_mode: Some(fidl_avrcp::ScanMode::Off),
                    ..Default::default()
                }),
                battery_status: Some(fidl_avrcp::BatteryStatus::Normal),
                ..Default::default()
            }
            .into();
            let data = NotificationData::new(
                fidl_avrcp::NotificationEvent::TrackChanged,
                prev_value,
                0,
                None,
                responder,
            );

            let mut curr_value: Notification = fidl_avrcp::Notification {
                status: Some(fidl_avrcp::PlaybackStatus::Playing),
                track_id: Some(800),
                pos: Some(12345),
                application_settings: Some(fidl_avrcp::PlayerApplicationSettings {
                    repeat_status_mode: Some(fidl_avrcp::RepeatStatusMode::GroupRepeat),
                    scan_mode: Some(fidl_avrcp::ScanMode::Off),
                    ..Default::default()
                }),
                battery_status: Some(fidl_avrcp::BatteryStatus::Critical),
                ..Default::default()
            }
            .into();

            curr_value.media_info = Some(MediaInfo::default());

            let res1 = data.notification_value_changed(
                &fidl_avrcp::NotificationEvent::PlaybackStatusChanged,
                &curr_value,
            );
            assert_eq!(res1.unwrap(), true);

            let res2 = data.notification_value_changed(
                &fidl_avrcp::NotificationEvent::TrackChanged,
                &curr_value,
            );
            assert_eq!(res2.unwrap(), true);

            let res3 = data.notification_value_changed(
                &fidl_avrcp::NotificationEvent::PlayerApplicationSettingChanged,
                &curr_value,
            );
            assert_eq!(res3.unwrap(), false);

            let res4 = data.notification_value_changed(
                &fidl_avrcp::NotificationEvent::TrackPosChanged,
                &curr_value,
            );
            assert_eq!(res4.unwrap(), true);

            let res5 = data.notification_value_changed(
                &fidl_avrcp::NotificationEvent::BattStatusChanged,
                &curr_value,
            );
            assert_eq!(res5.unwrap(), true);

            let res6 = data.notification_value_changed(
                &fidl_avrcp::NotificationEvent::SystemStatusChanged,
                &curr_value,
            );
            assert!(res6.is_err());
        }

        assert!(result_fut.await.is_ok());
        Ok(())
    }

    #[fuchsia::test]
    /// Tests sending response with a changed value successfully sends over the responder.
    async fn test_update_responder_changed_value_success() -> Result<(), Error> {
        let (mut proxy, mut stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Couldn't create proxy and stream");

        let (result_fut, responder) =
            generate_empty_watch_notification(&mut proxy, &mut stream).await?;

        {
            // Create the data with responder.
            let curr_value = Notification::default();
            let data = NotificationData::new(
                fidl_avrcp::NotificationEvent::PlaybackStatusChanged,
                curr_value,
                0,
                None,
                responder,
            );

            // Send an update over the responder with a changed value. Should succeed.
            let changed_value = Notification {
                status: Some(fidl_avrcp::PlaybackStatus::Paused),
                ..Notification::default()
            };
            let result = data
                .update_responder(
                    &fidl_avrcp::NotificationEvent::PlaybackStatusChanged,
                    Ok(changed_value),
                )
                .expect("Updating the responder should succeed");
            assert!(result.is_none());
        }

        // The response should be the changed_value.
        let expected = Notification {
            status: Some(fidl_avrcp::PlaybackStatus::Paused),
            ..Notification::default()
        };
        assert_eq!(Ok(expected), result_fut.await.expect("FIDL call should work").map(Into::into));
        Ok(())
    }

    #[fuchsia::test]
    /// Tests sending response with an unchanged value does not send/consume the responder.
    /// Instead, it should return itself, with the unchanged responder.
    async fn test_update_responder_same_value_success() -> Result<(), Error> {
        let (mut proxy, mut stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Couldn't create proxy and stream");

        let (result_fut, responder) =
            generate_empty_watch_notification(&mut proxy, &mut stream).await?;

        {
            // Create the data with responder.
            let curr_value = Notification::default();
            let data = NotificationData::new(
                fidl_avrcp::NotificationEvent::PlaybackStatusChanged,
                curr_value,
                0,
                None,
                responder,
            );

            // Send an update over the responder with the same value.
            // Should return Some(Self).
            let same_value = Notification::default();
            let result = data
                .update_responder(
                    &fidl_avrcp::NotificationEvent::PlaybackStatusChanged,
                    Ok(same_value),
                )
                .expect("Should work");
            assert!(result.is_some());
        }

        // The response should be the original (unchanged) value.
        // This is because the reply over the responder is never sent and is instead
        // dropped at the end of the closure. Drop is impl'd for NotificationData, which
        // sends the current value.
        let expected = Notification::default();
        assert_eq!(Ok(expected), result_fut.await.expect("FIDL call should work").map(Into::into));
        Ok(())
    }

    #[fuchsia::test]
    /// Tests sending an error response sends & consumes the responder.
    async fn test_update_responder_with_error() -> Result<(), Error> {
        let (mut proxy, mut stream) = create_proxy_and_stream::<TargetHandlerMarker>()
            .expect("Couldn't create proxy and stream");

        let (result_fut, responder) =
            generate_empty_watch_notification(&mut proxy, &mut stream).await?;

        {
            // Create the data with responder.
            let curr_value = Notification::default();
            let data = NotificationData::new(
                fidl_avrcp::NotificationEvent::PlaybackStatusChanged,
                curr_value,
                0,
                None,
                responder,
            );

            // Send an update over the responder with an error value.
            // Should successfully send over the responder, and consume it.
            let result = data
                .update_responder(
                    &fidl_avrcp::NotificationEvent::PlaybackStatusChanged,
                    Err(fidl_avrcp::TargetAvcError::RejectedAddressedPlayerChanged),
                )
                .expect("Error should successfully send over responder");
            assert!(result.is_none());
        }

        // The response should be the error.
        let expected: Result<fidl_avrcp::Notification, String> =
            Err("RejectedAddressedPlayerChanged".to_string());
        assert_eq!(
            expected,
            result_fut.await.expect("FIDL call should work").map_err(|e| format!("{:?}", e))
        );
        Ok(())
    }
}
