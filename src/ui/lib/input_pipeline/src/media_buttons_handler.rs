// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::input_handler::UnhandledInputHandler,
    crate::{consumer_controls_binding, input_device},
    anyhow::{Context, Error},
    async_trait::async_trait,
    fidl::endpoints::Proxy,
    fidl_fuchsia_input_interaction_observation as interaction_observation,
    fidl_fuchsia_input_report as fidl_input_report, fidl_fuchsia_ui_input as fidl_ui_input,
    fidl_fuchsia_ui_policy as fidl_ui_policy, fuchsia_async as fasync, fuchsia_zircon as zx,
    fuchsia_zircon::AsHandleRef,
    futures::{channel::mpsc, StreamExt, TryStreamExt},
    std::cell::RefCell,
    std::collections::HashMap,
    std::rc::Rc,
};

/// A [`MediaButtonsHandler`] tracks MediaButtonListeners and sends media button events to them.
#[derive(Debug)]
pub struct MediaButtonsHandler {
    /// The mutable fields of this handler.
    inner: RefCell<MediaButtonsHandlerInner>,

    /// The FIDL proxy used to report media button activity to the activity
    /// service.
    aggregator_proxy: Option<interaction_observation::AggregatorProxy>,
}

#[derive(Debug)]
struct MediaButtonsHandlerInner {
    /// The media button listeners, key referenced by proxy channel's raw handle.
    pub listeners: HashMap<u32, fidl_ui_policy::MediaButtonsListenerProxy>,

    /// The last MediaButtonsEvent sent to all listeners.
    /// This is used to send new listeners the state of the media buttons.
    pub last_event: Option<fidl_ui_input::MediaButtonsEvent>,

    pub send_event_task_tracker: LocalTaskTracker,
}

#[async_trait(?Send)]
impl UnhandledInputHandler for MediaButtonsHandler {
    async fn handle_unhandled_input_event(
        self: Rc<Self>,
        unhandled_input_event: input_device::UnhandledInputEvent,
    ) -> Vec<input_device::InputEvent> {
        match unhandled_input_event {
            input_device::UnhandledInputEvent {
                device_event:
                    input_device::InputDeviceEvent::ConsumerControls(ref media_buttons_event),
                device_descriptor: input_device::InputDeviceDescriptor::ConsumerControls(_),
                event_time,
                trace_id: _,
            } => {
                let media_buttons_event = Self::create_media_buttons_event(media_buttons_event);

                // Send the event if the media buttons are supported.
                self.send_event_to_listeners(&media_buttons_event).await;

                // Store the sent event.
                self.inner.borrow_mut().last_event = Some(media_buttons_event);

                // Report the event to the Activity Service.
                if let Err(e) = self.report_media_buttons_activity(event_time).await {
                    tracing::error!("report_media_buttons_activity failed: {}", e);
                }

                // Consume the input event.
                vec![input_device::InputEvent::from(unhandled_input_event).into_handled()]
            }
            _ => vec![input_device::InputEvent::from(unhandled_input_event)],
        }
    }
}

impl MediaButtonsHandler {
    /// Creates a new [`MediaButtonsHandler`] that sends media button events to listeners.
    pub fn new() -> Rc<Self> {
        let aggregator_proxy = match fuchsia_component::client::connect_to_protocol::<
            interaction_observation::AggregatorMarker,
        >() {
            Ok(proxy) => Some(proxy),
            Err(e) => {
                tracing::error!("MedaButtonsHandler failed to connect to fuchsia.input.interaction.observation.Aggregator: {}", e);
                None
            }
        };

        Self::new_internal(aggregator_proxy)
    }

    fn new_internal(
        aggregator_proxy: Option<interaction_observation::AggregatorProxy>,
    ) -> Rc<Self> {
        let media_buttons_handler = Self {
            aggregator_proxy,
            inner: RefCell::new(MediaButtonsHandlerInner {
                listeners: HashMap::new(),
                last_event: None,
                send_event_task_tracker: LocalTaskTracker::new(),
            }),
        };
        Rc::new(media_buttons_handler)
    }

    /// Handles the incoming DeviceListenerRegistryRequestStream.
    ///
    /// This method will end when the request stream is closed. If the stream closes with an
    /// error the error will be returned in the Result.
    ///
    /// # Parameters
    /// - `stream`: The stream of DeviceListenerRegistryRequestStream.
    pub async fn handle_device_listener_registry_request_stream(
        self: &Rc<Self>,
        mut stream: fidl_ui_policy::DeviceListenerRegistryRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) = stream
            .try_next()
            .await
            .context("Error handling device listener registry request stream")?
        {
            match request {
                fidl_ui_policy::DeviceListenerRegistryRequest::RegisterListener {
                    listener,
                    responder,
                } => {
                    if let Ok(proxy) = listener.into_proxy() {
                        // Add the listener to the registry.
                        self.inner
                            .borrow_mut()
                            .listeners
                            .insert(proxy.as_channel().raw_handle(), proxy.clone());
                        let proxy_clone = proxy.clone();

                        // Send the listener the last media button event.
                        if let Some(event) = &self.inner.borrow().last_event {
                            let event_to_send = event.clone();
                            let fut = async move {
                                match proxy_clone.on_event(&event_to_send).await {
                                    Ok(_) => {}
                                    Err(e) => {
                                        tracing::info!(
                                            "Failed to send media buttons event to listener {:?}",
                                            e
                                        )
                                    }
                                }
                            };
                            self.inner
                                .borrow()
                                .send_event_task_tracker
                                .track(fasync::Task::local(fut));
                        }
                    }
                    let _ = responder.send();
                }
                _ => {}
            }
        }

        Ok(())
    }

    /// Creates a fidl_ui_input::MediaButtonsEvent from a media_buttons::MediaButtonEvent.
    ///
    /// # Parameters
    /// -  `event`: The MediaButtonEvent to create a MediaButtonsEvent from.
    fn create_media_buttons_event(
        event: &consumer_controls_binding::ConsumerControlsEvent,
    ) -> fidl_ui_input::MediaButtonsEvent {
        let mut new_event = fidl_ui_input::MediaButtonsEvent {
            volume: Some(0),
            mic_mute: Some(false),
            pause: Some(false),
            camera_disable: Some(false),
            ..Default::default()
        };
        for button in &event.pressed_buttons {
            match button {
                fidl_input_report::ConsumerControlButton::VolumeUp => {
                    new_event.volume = Some(new_event.volume.unwrap().saturating_add(1));
                }
                fidl_input_report::ConsumerControlButton::VolumeDown => {
                    new_event.volume = Some(new_event.volume.unwrap().saturating_sub(1));
                }
                fidl_input_report::ConsumerControlButton::MicMute => {
                    new_event.mic_mute = Some(true);
                }
                fidl_input_report::ConsumerControlButton::Pause => {
                    new_event.pause = Some(true);
                }
                fidl_input_report::ConsumerControlButton::CameraDisable => {
                    new_event.camera_disable = Some(true);
                }
                _ => {}
            }
        }

        new_event
    }

    /// Sends media button events to media button listeners.
    ///
    /// # Parameters
    /// - `event`: The event to send to the listeners.
    async fn send_event_to_listeners(self: &Rc<Self>, event: &fidl_ui_input::MediaButtonsEvent) {
        let tracker = &self.inner.borrow().send_event_task_tracker;

        for (handle, listener) in &self.inner.borrow().listeners {
            let weak_handler = Rc::downgrade(&self);
            let listener_clone = listener.clone();
            let handle_clone = handle.clone();
            let event_to_send = event.clone();
            let fut = async move {
                match listener_clone.on_event(&event_to_send).await {
                    Ok(_) => {}
                    Err(e) => {
                        if let Some(handler) = weak_handler.upgrade() {
                            handler.inner.borrow_mut().listeners.remove(&handle_clone);
                            tracing::info!(
                                "Unregistering listener; unable to send MediaButtonsEvent: {:?}",
                                e
                            )
                        }
                    }
                }
            };

            tracker.track(fasync::Task::local(fut));
        }
    }

    /// Reports the given event_time to the activity service, if available.
    async fn report_media_buttons_activity(&self, event_time: zx::Time) -> Result<(), fidl::Error> {
        if let Some(proxy) = self.aggregator_proxy.clone() {
            return proxy.report_discrete_activity(event_time.into_nanos()).await;
        }

        Ok(())
    }
}

/// Maintains a collection of pending local [`Task`]s, allowing them to be dropped (and cancelled)
/// en masse.
#[derive(Debug)]
pub struct LocalTaskTracker {
    sender: mpsc::UnboundedSender<fasync::Task<()>>,
    _receiver_task: fasync::Task<()>,
}

impl LocalTaskTracker {
    pub fn new() -> Self {
        let (sender, receiver) = mpsc::unbounded();
        let receiver_task = fasync::Task::local(async move {
            // Drop the tasks as they are completed.
            receiver.for_each_concurrent(None, |task: fasync::Task<()>| task).await
        });

        Self { sender, _receiver_task: receiver_task }
    }

    /// Submits a new task to track.
    pub fn track(&self, task: fasync::Task<()>) {
        match self.sender.unbounded_send(task) {
            Ok(_) => {}
            // `Full` should never happen because this is unbounded.
            // `Disconnected` might happen if the `Service` was dropped. However, it's not clear how
            // to create such a race condition.
            Err(e) => tracing::error!("Unexpected {e:?} while pushing task"),
        };
    }
}

#[cfg(test)]
mod tests {
    use crate::input_handler::InputHandler;

    use {
        super::*,
        crate::testing_utilities,
        anyhow::Error,
        assert_matches::assert_matches,
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_input_report as fidl_input_report, fuchsia_async as fasync,
        futures::{channel::oneshot, StreamExt},
        pretty_assertions::assert_eq,
        std::{cell::RefCell, rc::Rc, task::Poll},
    };

    fn spawn_device_listener_registry_server(
        handler: Rc<MediaButtonsHandler>,
    ) -> fidl_ui_policy::DeviceListenerRegistryProxy {
        let (device_listener_proxy, device_listener_stream) =
            create_proxy_and_stream::<fidl_ui_policy::DeviceListenerRegistryMarker>()
                .expect("Failed to create DeviceListenerRegistry proxy and stream.");

        fasync::Task::local(async move {
            let _ = handler
                .handle_device_listener_registry_request_stream(device_listener_stream)
                .await;
        })
        .detach();

        device_listener_proxy
    }

    fn create_ui_input_media_buttons_event(
        volume: Option<i8>,
        mic_mute: Option<bool>,
        pause: Option<bool>,
        camera_disable: Option<bool>,
    ) -> fidl_ui_input::MediaButtonsEvent {
        fidl_ui_input::MediaButtonsEvent {
            volume,
            mic_mute,
            pause,
            camera_disable,
            ..Default::default()
        }
    }

    fn spawn_aggregator_request_server(
        expected_time: i64,
    ) -> Option<interaction_observation::AggregatorProxy> {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<interaction_observation::AggregatorMarker>()
                .expect("Failed to create interaction observation Aggregator proxy and stream.");

        fasync::Task::local(async move {
            if let Some(request) = stream.next().await {
                match request {
                    Ok(interaction_observation::AggregatorRequest::ReportDiscreteActivity {
                        event_time,
                        responder,
                    }) => {
                        assert_eq!(event_time, expected_time);
                        responder.send().expect("failed to respond");
                    }
                    other => panic!("expected aggregator report request, but got {:?}", other),
                };
            } else {
                panic!("AggregatorRequestStream failed.");
            }
        })
        .detach();

        Some(proxy)
    }

    /// Makes a `Task` that waits for a `oneshot`'s value to be set, and then forwards that value to
    /// a reference-counted container that can be observed outside the task.
    fn make_signalable_task<T: Default + 'static>(
    ) -> (oneshot::Sender<T>, fasync::Task<()>, Rc<RefCell<T>>) {
        let (sender, receiver) = oneshot::channel();
        let task_completed = Rc::new(RefCell::new(<T as Default>::default()));
        let task_completed_ = task_completed.clone();
        let task = fasync::Task::local(async move {
            if let Ok(value) = receiver.await {
                *task_completed_.borrow_mut() = value;
            }
        });
        (sender, task, task_completed)
    }

    /// Tests that a media button listener can be registered and is sent the latest event upon
    /// registration.
    #[fasync::run_singlethreaded(test)]
    async fn register_media_buttons_listener() {
        // Set up DeviceListenerRegistry.
        let (aggregator_proxy, _) =
            fidl::endpoints::create_proxy_and_stream::<interaction_observation::AggregatorMarker>()
                .expect("Failed to create interaction observation Aggregator proxy and stream.");
        let media_buttons_handler = Rc::new(MediaButtonsHandler {
            aggregator_proxy: Some(aggregator_proxy),
            inner: RefCell::new(MediaButtonsHandlerInner {
                listeners: HashMap::new(),
                last_event: Some(create_ui_input_media_buttons_event(Some(1), None, None, None)),
                send_event_task_tracker: LocalTaskTracker::new(),
            }),
        });
        let device_listener_proxy =
            spawn_device_listener_registry_server(media_buttons_handler.clone());

        // Register a listener.
        let (listener, mut listener_stream) =
            fidl::endpoints::create_request_stream::<fidl_ui_policy::MediaButtonsListenerMarker>()
                .unwrap();
        let register_listener_fut = async {
            let res = device_listener_proxy.register_listener(listener).await;
            assert!(res.is_ok());
        };

        // Assert listener was registered and received last event.
        let expected_event = create_ui_input_media_buttons_event(Some(1), None, None, None);
        let assert_fut = async {
            match listener_stream.next().await {
                Some(Ok(fidl_ui_policy::MediaButtonsListenerRequest::OnEvent {
                    event,
                    responder,
                })) => {
                    assert_eq!(event, expected_event);
                    responder.send().expect("responder failed.");
                }
                _ => assert!(false),
            }
        };
        futures::join!(register_listener_fut, assert_fut);
        assert_eq!(media_buttons_handler.inner.borrow().listeners.len(), 1);
    }

    /// Tests that all supported buttons are sent.
    #[fasync::run_singlethreaded(test)]
    async fn listener_receives_all_buttons() {
        let event_time = zx::Time::get_monotonic();
        let aggregator_proxy = spawn_aggregator_request_server(event_time.into_nanos());
        let media_buttons_handler = MediaButtonsHandler::new_internal(aggregator_proxy);
        let device_listener_proxy =
            spawn_device_listener_registry_server(media_buttons_handler.clone());

        // Register a listener.
        let (listener, listener_stream) =
            fidl::endpoints::create_request_stream::<fidl_ui_policy::MediaButtonsListenerMarker>()
                .unwrap();
        let _ = device_listener_proxy.register_listener(listener).await;

        // Setup events and expectations.
        let descriptor = testing_utilities::consumer_controls_device_descriptor();
        let input_events = vec![testing_utilities::create_consumer_controls_event(
            vec![
                fidl_input_report::ConsumerControlButton::VolumeUp,
                fidl_input_report::ConsumerControlButton::VolumeDown,
                fidl_input_report::ConsumerControlButton::Pause,
                fidl_input_report::ConsumerControlButton::MicMute,
                fidl_input_report::ConsumerControlButton::CameraDisable,
            ],
            event_time,
            &descriptor,
        )];
        let expected_events =
            vec![create_ui_input_media_buttons_event(Some(0), Some(true), Some(true), Some(true))];

        // Assert registered listener receives event.
        use crate::input_handler::InputHandler as _; // Adapt UnhandledInputHandler to InputHandler
        assert_input_event_sequence_generates_media_buttons_events!(
            input_handler: media_buttons_handler,
            input_events: input_events,
            expected_events: expected_events,
            media_buttons_listener_request_stream: vec![listener_stream],
        );
    }

    /// Tests that multiple listeners are supported.
    #[fasync::run_singlethreaded(test)]
    async fn multiple_listeners_receive_event() {
        let event_time = zx::Time::get_monotonic();
        let aggregator_proxy = spawn_aggregator_request_server(event_time.into_nanos());
        let media_buttons_handler = MediaButtonsHandler::new_internal(aggregator_proxy);
        let device_listener_proxy =
            spawn_device_listener_registry_server(media_buttons_handler.clone());

        // Register two listeners.
        let (first_listener, first_listener_stream) =
            fidl::endpoints::create_request_stream::<fidl_ui_policy::MediaButtonsListenerMarker>()
                .unwrap();
        let (second_listener, second_listener_stream) =
            fidl::endpoints::create_request_stream::<fidl_ui_policy::MediaButtonsListenerMarker>()
                .unwrap();
        let _ = device_listener_proxy.register_listener(first_listener).await;
        let _ = device_listener_proxy.register_listener(second_listener).await;

        // Setup events and expectations.
        let descriptor = testing_utilities::consumer_controls_device_descriptor();
        let input_events = vec![testing_utilities::create_consumer_controls_event(
            vec![fidl_input_report::ConsumerControlButton::VolumeUp],
            event_time,
            &descriptor,
        )];
        let expected_events = vec![create_ui_input_media_buttons_event(
            Some(1),
            Some(false),
            Some(false),
            Some(false),
        )];

        // Assert registered listeners receives event.
        use crate::input_handler::InputHandler as _; // Adapt UnhandledInputHandler to InputHandler
        assert_input_event_sequence_generates_media_buttons_events!(
            input_handler: media_buttons_handler,
            input_events: input_events,
            expected_events: expected_events,
            media_buttons_listener_request_stream:
                vec![first_listener_stream, second_listener_stream],
        );
    }

    /// Tests that listener is unregistered if channel is closed and we try to send input event to listener
    #[fuchsia::test]
    fn unregister_listener_if_channel_closed() {
        let mut exec = fasync::TestExecutor::new();

        let event_time = zx::Time::get_monotonic();
        let aggregator_proxy = spawn_aggregator_request_server(event_time.into_nanos());
        let media_buttons_handler = MediaButtonsHandler::new_internal(aggregator_proxy);
        let media_buttons_handler_clone = media_buttons_handler.clone();

        let mut task = fasync::Task::local(async move {
            let device_listener_proxy =
                spawn_device_listener_registry_server(media_buttons_handler.clone());

            // Register three listeners.
            let (first_listener, mut first_listener_stream) =
                fidl::endpoints::create_request_stream::<fidl_ui_policy::MediaButtonsListenerMarker>()
                    .unwrap();
            let (second_listener, mut second_listener_stream) =
                fidl::endpoints::create_request_stream::<fidl_ui_policy::MediaButtonsListenerMarker>()
                    .unwrap();
            let (third_listener, third_listener_stream) = fidl::endpoints::create_request_stream::<
                fidl_ui_policy::MediaButtonsListenerMarker,
            >()
            .unwrap();
            let _ = device_listener_proxy.register_listener(first_listener).await;
            let _ = device_listener_proxy.register_listener(second_listener).await;
            let _ = device_listener_proxy.register_listener(third_listener).await;
            assert_eq!(media_buttons_handler.inner.borrow().listeners.len(), 3);

            // Generate input event to be handled by MediaButtonsHandler.
            let descriptor = testing_utilities::consumer_controls_device_descriptor();
            let input_event = testing_utilities::create_consumer_controls_event(
                vec![fidl_input_report::ConsumerControlButton::VolumeUp],
                event_time,
                &descriptor,
            );

            let expected_media_buttons_event =
                create_ui_input_media_buttons_event(Some(1), Some(false), Some(false), Some(false));

            // Drop third registered listener.
            std::mem::drop(third_listener_stream);

            let _ = media_buttons_handler.clone().handle_input_event(input_event).await;
            // First listener stalls, responder doesn't send response - subsequent listeners should still be able receive event.
            if let Some(request) = first_listener_stream.next().await {
                match request {
                    Ok(fidl_ui_policy::MediaButtonsListenerRequest::OnEvent {
                        event,
                        responder: _,
                    }) => {
                        pretty_assertions::assert_eq!(event, expected_media_buttons_event);

                        // No need to send response because we want to simulate reader getting stuck.
                    }
                    _ => assert!(false),
                }
            } else {
                assert!(false);
            }

            // Send response from responder on second listener stream
            if let Some(request) = second_listener_stream.next().await {
                match request {
                    Ok(fidl_ui_policy::MediaButtonsListenerRequest::OnEvent {
                        event,
                        responder,
                    }) => {
                        pretty_assertions::assert_eq!(event, expected_media_buttons_event);
                        let _ = responder.send();
                    }
                    _ => assert!(false),
                }
            } else {
                assert!(false);
            }
        });

        // Must manually run tasks with executor to ensure all tasks in LocalTaskTracker complete/stall before we call final assertion.
        let _ = exec.run_until_stalled(&mut task);

        // Should only be two listeners still registered in 'inner' after we unregister the listener with closed channel.
        let _ = exec.run_singlethreaded(async {
            assert_eq!(media_buttons_handler_clone.inner.borrow().listeners.len(), 2);
        });
    }

    /// Tests that handle_input_event returns even if reader gets stuck while sending event to listener
    #[fasync::run_singlethreaded(test)]
    async fn stuck_reader_wont_block_input_pipeline() {
        let event_time = zx::Time::get_monotonic();
        let aggregator_proxy = spawn_aggregator_request_server(event_time.into_nanos());
        let media_buttons_handler = MediaButtonsHandler::new_internal(aggregator_proxy);
        let device_listener_proxy =
            spawn_device_listener_registry_server(media_buttons_handler.clone());

        let (first_listener, mut first_listener_stream) =
            fidl::endpoints::create_request_stream::<fidl_ui_policy::MediaButtonsListenerMarker>()
                .unwrap();
        let (second_listener, mut second_listener_stream) =
            fidl::endpoints::create_request_stream::<fidl_ui_policy::MediaButtonsListenerMarker>()
                .unwrap();
        let _ = device_listener_proxy.register_listener(first_listener).await;
        let _ = device_listener_proxy.register_listener(second_listener).await;

        // Setup events and expectations.
        let descriptor = testing_utilities::consumer_controls_device_descriptor();
        let first_unhandled_input_event = input_device::UnhandledInputEvent {
            device_event: input_device::InputDeviceEvent::ConsumerControls(
                consumer_controls_binding::ConsumerControlsEvent::new(vec![
                    fidl_input_report::ConsumerControlButton::VolumeUp,
                ]),
            ),
            device_descriptor: descriptor.clone(),
            event_time,
            trace_id: None,
        };
        let first_expected_media_buttons_event =
            create_ui_input_media_buttons_event(Some(1), Some(false), Some(false), Some(false));

        assert_matches!(
            media_buttons_handler
                .clone()
                .handle_unhandled_input_event(first_unhandled_input_event)
                .await
                .as_slice(),
            [input_device::InputEvent { handled: input_device::Handled::Yes, .. }]
        );

        let mut save_responder = None;

        // Ensure handle_input_event attempts to send event to first listener.
        if let Some(request) = first_listener_stream.next().await {
            match request {
                Ok(fidl_ui_policy::MediaButtonsListenerRequest::OnEvent { event, responder }) => {
                    pretty_assertions::assert_eq!(event, first_expected_media_buttons_event);

                    // No need to send response because we want to simulate reader getting stuck.

                    // Save responder to send response later
                    save_responder = Some(responder);
                }
                _ => assert!(false),
            }
        } else {
            assert!(false)
        }

        // Ensure handle_input_event still sends event to second listener when reader for first listener is stuck.
        if let Some(request) = second_listener_stream.next().await {
            match request {
                Ok(fidl_ui_policy::MediaButtonsListenerRequest::OnEvent { event, responder }) => {
                    pretty_assertions::assert_eq!(event, first_expected_media_buttons_event);
                    let _ = responder.send();
                }
                _ => assert!(false),
            }
        } else {
            assert!(false)
        }

        // Setup second event to handle
        let second_unhandled_input_event = input_device::UnhandledInputEvent {
            device_event: input_device::InputDeviceEvent::ConsumerControls(
                consumer_controls_binding::ConsumerControlsEvent::new(vec![
                    fidl_input_report::ConsumerControlButton::MicMute,
                ]),
            ),
            device_descriptor: descriptor.clone(),
            event_time,
            trace_id: None,
        };
        let second_expected_media_buttons_event =
            create_ui_input_media_buttons_event(Some(0), Some(true), Some(false), Some(false));

        // Ensure we can handle a subsequent event if listener stalls on first event.
        assert_matches!(
            media_buttons_handler
                .clone()
                .handle_unhandled_input_event(second_unhandled_input_event)
                .await
                .as_slice(),
            [input_device::InputEvent { handled: input_device::Handled::Yes, .. }]
        );

        // Ensure events are still sent to listeners if a listener stalls on a previous event.
        if let Some(request) = second_listener_stream.next().await {
            match request {
                Ok(fidl_ui_policy::MediaButtonsListenerRequest::OnEvent { event, responder }) => {
                    pretty_assertions::assert_eq!(event, second_expected_media_buttons_event);
                    let _ = responder.send();
                }
                _ => assert!(false),
            }
        } else {
            assert!(false)
        }

        match save_responder {
            Some(save_responder) => {
                // Simulate delayed response to first listener for first event
                let _ = save_responder.send();
                // First listener should now receive second event after delayed response for first event
                if let Some(request) = first_listener_stream.next().await {
                    match request {
                        Ok(fidl_ui_policy::MediaButtonsListenerRequest::OnEvent {
                            event,
                            responder: _,
                        }) => {
                            pretty_assertions::assert_eq!(
                                event,
                                second_expected_media_buttons_event
                            );

                            // No need to send response
                        }
                        _ => assert!(false),
                    }
                } else {
                    assert!(false)
                }
            }
            None => {
                assert!(false)
            }
        }
    }

    // Test for LocalTaskTracker
    #[fuchsia::test]
    fn local_task_tracker_test() -> Result<(), Error> {
        let mut exec = fasync::TestExecutor::new();

        let (mut sender_1, task_1, completed_1) = make_signalable_task::<bool>();
        let (sender_2, task_2, completed_2) = make_signalable_task::<bool>();

        let mut tracker = LocalTaskTracker::new();

        tracker.track(task_1);
        tracker.track(task_2);

        assert_matches!(exec.run_until_stalled(&mut tracker._receiver_task), Poll::Pending);
        assert_eq!(Rc::strong_count(&completed_1), 2);
        assert_eq!(Rc::strong_count(&completed_2), 2);
        assert!(!sender_1.is_canceled());
        assert!(!sender_2.is_canceled());

        assert!(sender_2.send(true).is_ok());
        assert_matches!(exec.run_until_stalled(&mut tracker._receiver_task), Poll::Pending);

        assert_eq!(Rc::strong_count(&completed_1), 2);
        assert_eq!(Rc::strong_count(&completed_2), 1);
        assert_eq!(*completed_1.borrow(), false);
        assert_eq!(*completed_2.borrow(), true);
        assert!(!sender_1.is_canceled());

        drop(tracker);
        let mut sender_1_cancellation = sender_1.cancellation();
        assert_matches!(exec.run_until_stalled(&mut sender_1_cancellation), Poll::Ready(()));
        assert_eq!(Rc::strong_count(&completed_1), 1);
        assert!(sender_1.is_canceled());

        Ok(())
    }
}
