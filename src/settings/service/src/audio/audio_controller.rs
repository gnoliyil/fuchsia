// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::audio::types::{AudioInfo, AudioStream, AudioStreamType, SetAudioStream};
use crate::audio::{
    create_default_modified_counters, default_audio_info, ModifiedCounters, StreamVolumeControl,
};
use crate::base::SettingType;
use crate::handler::base::Request;
use crate::handler::setting_handler::persist::{
    controller as data_controller, ClientProxy, WriteResult,
};
use crate::handler::setting_handler::{
    controller, ControllerError, ControllerStateResult, Event, SettingHandlerResult, State,
};
use crate::{trace, trace_guard};
use async_trait::async_trait;
use fuchsia_async as fasync;
use fuchsia_trace as ftrace;
use futures::lock::Mutex;
use settings_storage::device_storage::{DeviceStorage, DeviceStorageCompatible};
use settings_storage::storage_factory::StorageAccess;
use std::collections::HashMap;
use std::sync::Arc;

fn get_streams_array_from_map(
    stream_map: &HashMap<AudioStreamType, StreamVolumeControl>,
) -> [AudioStream; 5] {
    let mut streams: [AudioStream; 5] = default_audio_info().streams;
    for stream in &mut streams {
        if let Some(volume_control) = stream_map.get(&stream.stream_type) {
            *stream = volume_control.stored_stream;
        }
    }

    streams
}

type VolumeControllerHandle = Arc<Mutex<VolumeController>>;

pub(crate) struct VolumeController {
    client: ClientProxy,
    audio_service_connected: bool,
    stream_volume_controls: HashMap<AudioStreamType, StreamVolumeControl>,
    modified_counters: ModifiedCounters,
}

enum UpdateFrom {
    AudioInfo(AudioInfo),
    NewStreams(Vec<SetAudioStream>),
}

impl VolumeController {
    async fn create(client: ClientProxy) -> VolumeControllerHandle {
        Arc::new(Mutex::new(Self {
            client,
            stream_volume_controls: HashMap::new(),
            audio_service_connected: false,
            modified_counters: create_default_modified_counters(),
        }))
    }

    /// Restores the necessary dependencies' state on boot.
    async fn restore(&mut self, id: ftrace::Id) -> ControllerStateResult {
        self.restore_volume_state(true, id).await
    }

    /// Extracts the audio state from persistent storage and restores it on
    /// the local state. Also pushes the changes to the audio core if
    /// `push_to_audio_core` is true.
    async fn restore_volume_state(
        &mut self,
        push_to_audio_core: bool,
        id: ftrace::Id,
    ) -> ControllerStateResult {
        let audio_info = self.client.read_setting::<AudioInfo>(id).await;
        // Ignore the notification triggered result.
        let _ = self
            .update_volume_streams(UpdateFrom::AudioInfo(audio_info), push_to_audio_core, id)
            .await?;
        Ok(())
    }

    async fn get_info(&self, id: ftrace::Id) -> Result<AudioInfo, ControllerError> {
        let mut audio_info = self.client.read_setting::<AudioInfo>(id).await;

        audio_info.modified_counters = Some(self.modified_counters.clone());
        Ok(audio_info)
    }

    async fn set_volume(
        &mut self,
        volume: Vec<SetAudioStream>,
        id: ftrace::Id,
    ) -> SettingHandlerResult {
        let guard = trace_guard!(id, "set volume updating counters");
        // Update counters for changed streams.
        for stream in &volume {
            // We don't care what the value of the counter is, just that it is different from the
            // previous value. We use wrapping_add to avoid eventual overflow of the counter.
            let _ = self.modified_counters.insert(
                stream.stream_type,
                self.modified_counters
                    .get(&stream.stream_type)
                    .map_or(0, |flag| flag.wrapping_add(1)),
            );
        }
        drop(guard);

        if !(self.update_volume_streams(UpdateFrom::NewStreams(volume), true, id).await?) {
            trace!(id, "set volume notifying");
            let info = self.get_info(id).await?.into();
            self.client.notify(Event::Changed(info)).await;
        }

        Ok(None)
    }

    /// Updates the state with the given streams' volume levels.
    ///
    /// If `push_to_audio_core` is true, pushes the changes to the audio core.
    /// If not, just sets it on the local stored state. Should be called with
    /// true on first restore and on volume changes, and false otherwise.
    /// Returns whether the change triggered a notification.
    async fn update_volume_streams(
        &mut self,
        update_from: UpdateFrom,
        push_to_audio_core: bool,
        id: ftrace::Id,
    ) -> Result<bool, ControllerError> {
        let mut new_vec = vec![];
        trace!(id, "update volume streams");
        let calculating_guard = trace_guard!(id, "check and bind");
        let (stored_value, new_streams) = match &update_from {
            UpdateFrom::AudioInfo(audio_info) => (None, audio_info.streams.iter()),
            UpdateFrom::NewStreams(streams) => {
                trace!(id, "reading setting");
                let stored_value = self.client.read_setting::<AudioInfo>(id).await;
                for set_stream in streams.iter() {
                    let stored_stream = stored_value
                        .streams
                        .iter()
                        .find(|stream| stream.stream_type == set_stream.stream_type)
                        .ok_or_else(|| {
                            ControllerError::InvalidArgument(
                                SettingType::Audio,
                                "stream".into(),
                                format!("{set_stream:?}").into(),
                            )
                        })?;
                    new_vec.push(AudioStream {
                        stream_type: stored_stream.stream_type,
                        source: set_stream.source,
                        user_volume_level: set_stream
                            .user_volume_level
                            .unwrap_or(stored_stream.user_volume_level),
                        user_volume_muted: set_stream
                            .user_volume_muted
                            .unwrap_or(stored_stream.user_volume_muted),
                    });
                }
                (Some(stored_value), new_vec.iter())
            }
        };

        if push_to_audio_core {
            let guard = trace_guard!(id, "push to core");
            self.check_and_bind_volume_controls(id, default_audio_info().streams.iter()).await?;
            drop(guard);

            trace!(id, "setting core");
            for stream in new_streams {
                if let Some(volume_control) =
                    self.stream_volume_controls.get_mut(&stream.stream_type)
                {
                    volume_control.set_volume(id, *stream).await?;
                }
            }
        } else {
            trace!(id, "without push to core");
            self.check_and_bind_volume_controls(id, new_streams).await?;
        }
        drop(calculating_guard);

        if let Some(mut stored_value) = stored_value {
            let guard = trace_guard!(id, "updating streams and counters");
            stored_value.streams = get_streams_array_from_map(&self.stream_volume_controls);
            stored_value.modified_counters = Some(self.modified_counters.clone());
            drop(guard);

            let guard = trace_guard!(id, "writing setting");
            let write_result = self.client.write_setting(stored_value.into(), id).await;
            drop(guard);
            Ok(write_result.notified())
        } else {
            Ok(false)
        }
    }

    /// Populates the local state with the given `streams` and binds it to the audio core service.
    async fn check_and_bind_volume_controls(
        &mut self,
        id: ftrace::Id,
        streams: impl Iterator<Item = &AudioStream>,
    ) -> ControllerStateResult {
        trace!(id, "check and bind fn");
        if self.audio_service_connected {
            return Ok(());
        }

        let guard = trace_guard!(id, "connecting to service");
        let service_result = self
            .client
            .get_service_context()
            .connect::<fidl_fuchsia_media::AudioCoreMarker>()
            .await;

        let audio_service = service_result.map_err(|e| {
            ControllerError::ExternalFailure(
                SettingType::Audio,
                "fuchsia.media.audio".into(),
                "connect for audio_core".into(),
                format!("{e:?}").into(),
            )
        })?;

        // The stream_volume_controls are generated in two steps instead of
        // one so that if one of the bindings fails during the first loop,
        // none of the streams are modified.
        drop(guard);
        let mut stream_tuples = Vec::new();
        for stream in streams {
            trace!(id, "create stream volume control");
            let client = self.client.clone();

            // Generate a tuple with stream type and StreamVolumeControl.
            stream_tuples.push((
                stream.stream_type,
                StreamVolumeControl::create(
                    id,
                    &audio_service,
                    *stream,
                    Some(Arc::new(move || {
                        // When the StreamVolumeControl exits early, inform the
                        // proxy we have exited. The proxy will then cleanup this
                        // AudioController.
                        let client = client.clone();
                        fasync::Task::spawn(async move {
                            trace!(id, "stream exit");
                            client
                                .notify(Event::Exited(Err(ControllerError::UnexpectedError(
                                    "stream_volume_control exit".into(),
                                ))))
                                .await;
                        })
                        .detach();
                    })),
                    None,
                )
                .await?,
            ));
        }

        stream_tuples.into_iter().for_each(|(stream_type, stream_volume_control)| {
            // Ignore the previous value, if any.
            let _ = self.stream_volume_controls.insert(stream_type, stream_volume_control);
        });
        self.audio_service_connected = true;

        Ok(())
    }
}

pub(crate) struct AudioController {
    volume: VolumeControllerHandle,
}

impl StorageAccess for AudioController {
    type Storage = DeviceStorage;
    const STORAGE_KEYS: &'static [&'static str] = &[AudioInfo::KEY];
}

#[async_trait]
impl data_controller::Create for AudioController {
    async fn create(client: ClientProxy) -> Result<Self, ControllerError> {
        Ok(AudioController { volume: VolumeController::create(client).await })
    }
}

#[async_trait]
impl controller::Handle for AudioController {
    async fn handle(&self, request: Request) -> Option<SettingHandlerResult> {
        match request {
            Request::Restore => Some({
                let id = ftrace::Id::new();
                trace!(id, "controller restore");
                self.volume.lock().await.restore(id).await.map(|_| None)
            }),
            Request::SetVolume(volume, id) => {
                trace!(id, "controller set");
                // Validate volume contains valid volume level numbers.
                for audio_stream in &volume {
                    if !audio_stream.has_valid_volume_level() {
                        return Some(Err(ControllerError::InvalidArgument(
                            SettingType::Audio,
                            "stream".into(),
                            format!("{audio_stream:?}").into(),
                        )));
                    }
                }
                Some(self.volume.lock().await.set_volume(volume, id).await)
            }
            Request::Get => {
                let id = ftrace::Id::new();
                Some(self.volume.lock().await.get_info(id).await.map(|info| Some(info.into())))
            }
            _ => None,
        }
    }

    async fn change_state(&mut self, state: State) -> Option<ControllerStateResult> {
        match state {
            State::Startup => {
                // Restore the volume state locally but do not push to the audio core.
                Some({
                    let id = ftrace::Id::new();
                    trace!(id, "controller startup");
                    self.volume.lock().await.restore_volume_state(false, id).await
                })
            }
            _ => None,
        }
    }
}
