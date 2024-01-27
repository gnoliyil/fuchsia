// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{input_device::InputDevice, new_fake_device_info},
    anyhow::{Context as _, Error},
    fidl::endpoints,
    fidl_fuchsia_input::Key,
    fidl_fuchsia_input_injection::InputDeviceRegistryProxy,
    fidl_fuchsia_input_report::{
        Axis, ConsumerControlButton, ConsumerControlDescriptor, ConsumerControlInputDescriptor,
        ContactInputDescriptor, DeviceDescriptor, InputDeviceMarker, KeyboardDescriptor,
        KeyboardInputDescriptor, MouseDescriptor, MouseInputDescriptor, Range, TouchDescriptor,
        TouchInputDescriptor, TouchType, Unit, UnitType, TOUCH_MAX_CONTACTS,
    },
    fidl_fuchsia_ui_test_input::MouseButton,
    std::convert::TryFrom,
};

/// Implements the client side of the `fuchsia.input.injection.InputDeviceRegistry` protocol.
pub(crate) struct InputDeviceRegistry {
    proxy: InputDeviceRegistryProxy,
}

impl InputDeviceRegistry {
    pub fn new(proxy: InputDeviceRegistryProxy) -> Self {
        Self { proxy }
    }

    /// Registers a touchscreen device, with in injection coordinate space that spans [-1000, 1000]
    /// on both axes.
    /// # Returns
    /// A `input_device::InputDevice`, which can be used to send events to the
    /// `fuchsia.input.report.InputDevice` that has been registered with the
    /// `fuchsia.input.injection.InputDeviceRegistry` service.
    pub fn add_touchscreen_device(
        &mut self,
        min_x: i64,
        max_x: i64,
        min_y: i64,
        max_y: i64,
    ) -> Result<InputDevice, Error> {
        self.add_device(DeviceDescriptor {
            touch: Some(TouchDescriptor {
                input: Some(TouchInputDescriptor {
                    contacts: Some(
                        std::iter::repeat(ContactInputDescriptor {
                            position_x: Some(Axis {
                                range: Range { min: min_x, max: max_x },
                                unit: Unit { type_: UnitType::Other, exponent: 0 },
                            }),
                            position_y: Some(Axis {
                                range: Range { min: min_y, max: max_y },
                                unit: Unit { type_: UnitType::Other, exponent: 0 },
                            }),
                            contact_width: Some(Axis {
                                range: Range { min: min_x, max: max_x },
                                unit: Unit { type_: UnitType::Other, exponent: 0 },
                            }),
                            contact_height: Some(Axis {
                                range: Range { min: min_y, max: max_y },
                                unit: Unit { type_: UnitType::Other, exponent: 0 },
                            }),
                            ..ContactInputDescriptor::EMPTY
                        })
                        .take(
                            usize::try_from(TOUCH_MAX_CONTACTS)
                                .context("usize is impossibly small")?,
                        )
                        .collect(),
                    ),
                    max_contacts: Some(TOUCH_MAX_CONTACTS),
                    touch_type: Some(TouchType::Touchscreen),
                    buttons: Some(vec![]),
                    ..TouchInputDescriptor::EMPTY
                }),
                ..TouchDescriptor::EMPTY
            }),
            ..DeviceDescriptor::EMPTY
        })
    }

    /// Registers a media buttons device.
    /// # Returns
    /// A `input_device::InputDevice`, which can be used to send events to the
    /// `fuchsia.input.report.InputDevice` that has been registered with the
    /// `fuchsia.input.injection.InputDeviceRegistry` service.
    pub fn add_media_buttons_device(&mut self) -> Result<InputDevice, Error> {
        self.add_device(DeviceDescriptor {
            consumer_control: Some(ConsumerControlDescriptor {
                input: Some(ConsumerControlInputDescriptor {
                    buttons: Some(vec![
                        ConsumerControlButton::VolumeUp,
                        ConsumerControlButton::VolumeDown,
                        ConsumerControlButton::Pause,
                        ConsumerControlButton::FactoryReset,
                        ConsumerControlButton::MicMute,
                        ConsumerControlButton::Reboot,
                        ConsumerControlButton::CameraDisable,
                    ]),
                    ..ConsumerControlInputDescriptor::EMPTY
                }),
                ..ConsumerControlDescriptor::EMPTY
            }),
            ..DeviceDescriptor::EMPTY
        })
    }

    /// Registers a keyboard device.
    /// # Returns
    /// An `input_device::InputDevice`, which can be used to send events to the
    /// `fuchsia.input.report.InputDevice` that has been registered with the
    /// `fuchsia.input.injection.InputDeviceRegistry` service.
    pub fn add_keyboard_device(&mut self) -> Result<InputDevice, Error> {
        // Generate a `Vec` of all known keys.
        // * Because there is no direct way to iterate over enum values, we iterate
        //   over the values corresponding to `Key::A` and `Key::MediaVolumeDecrement`.
        // * Some values in the range have no corresponding enum value. For example,
        //   the value 0x00070065 sits between `NonUsBackslash` (0x00070064), and
        //   `KeypadEquals` (0x00070067). Such primitives are removed by `filter_map()`.
        //
        // TODO(fxbug.dev/108531): Extend to include all values of the Key enum.
        let all_keys: Vec<Key> = (Key::A.into_primitive()
            ..=Key::MediaVolumeDecrement.into_primitive())
            .filter_map(Key::from_primitive)
            .collect();
        self.add_device(DeviceDescriptor {
            // Required for DeviceDescriptor.
            device_info: Some(new_fake_device_info()),
            keyboard: Some(KeyboardDescriptor {
                input: Some(KeyboardInputDescriptor {
                    keys3: Some(all_keys),
                    ..KeyboardInputDescriptor::EMPTY
                }),
                ..KeyboardDescriptor::EMPTY
            }),
            ..DeviceDescriptor::EMPTY
        })
    }

    pub fn add_mouse_device(&mut self) -> Result<InputDevice, Error> {
        self.add_device(DeviceDescriptor {
            // Required for DeviceDescriptor.
            device_info: Some(new_fake_device_info()),
            mouse: Some(MouseDescriptor {
                input: Some(MouseInputDescriptor {
                    movement_x: Some(Axis {
                        range: Range { min: -1000, max: 1000 },
                        unit: Unit { type_: UnitType::Other, exponent: 0 },
                    }),
                    movement_y: Some(Axis {
                        range: Range { min: -1000, max: 1000 },
                        unit: Unit { type_: UnitType::Other, exponent: 0 },
                    }),
                    // `scroll_v` and `scroll_h` are range of tick number on
                    // driver's report. [-100, 100] should be enough for
                    // testing.
                    scroll_v: Some(Axis {
                        range: Range { min: -100, max: 100 },
                        unit: Unit { type_: UnitType::Other, exponent: 0 },
                    }),
                    scroll_h: Some(Axis {
                        range: Range { min: -100, max: 100 },
                        unit: Unit { type_: UnitType::Other, exponent: 0 },
                    }),
                    // Match to the values of fuchsia.ui.test.input.MouseButton.
                    buttons: Some(
                        (MouseButton::First.into_primitive()..=MouseButton::Third.into_primitive())
                            .map(|b| {
                                b.try_into().expect("failed to convert mouse button to primitive")
                            })
                            .collect(),
                    ),
                    position_x: None,
                    position_y: None,
                    ..MouseInputDescriptor::EMPTY
                }),
                ..MouseDescriptor::EMPTY
            }),
            ..DeviceDescriptor::EMPTY
        })
    }

    /// Adds a device to the `InputDeviceRegistry` FIDL server connected to this
    /// `InputDeviceRegistry` struct.
    ///
    /// # Returns
    /// A `input_device::InputDevice`, which can be used to send events to the
    /// `fuchsia.input.report.InputDevice` that has been registered with the
    /// `fuchsia.input.injection.InputDeviceRegistry` service.
    fn add_device(&self, descriptor: DeviceDescriptor) -> Result<InputDevice, Error> {
        let (client_end, request_stream) = endpoints::create_request_stream::<InputDeviceMarker>()?;
        self.proxy.register(client_end)?;
        Ok(InputDevice::new(request_stream, descriptor))
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::input_device::InputDevice,
        anyhow::format_err,
        fidl_fuchsia_input_injection::{InputDeviceRegistryMarker, InputDeviceRegistryRequest},
        fuchsia_async as fasync,
        futures::{pin_mut, task::Poll, StreamExt},
        test_case::test_case,
    };

    #[test_case(&|registry| InputDeviceRegistry::add_touchscreen_device(registry);
                "touchscreen_device")]
    #[test_case(&|registry| InputDeviceRegistry::add_media_buttons_device(registry);
                "media_buttons_device")]
    #[test_case(&|registry| InputDeviceRegistry::add_media_buttons_device(registry);
                "keyboard_device")]
    fn add_device_invokes_fidl_register_method_exactly_once(
        add_device_method: &dyn Fn(&mut super::InputDeviceRegistry) -> Result<InputDevice, Error>,
    ) -> Result<(), Error> {
        let mut executor = fasync::TestExecutor::new();
        let (proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceRegistryMarker>()
                .context("failed to create proxy and stream for InputDeviceRegistry")?;
        add_device_method(&mut InputDeviceRegistry { proxy }).context("adding device")?;

        let requests = match executor.run_until_stalled(&mut request_stream.collect::<Vec<_>>()) {
            Poll::Ready(reqs) => reqs,
            Poll::Pending => return Err(format_err!("request_stream did not terminate")),
        };
        assert_matches::assert_matches!(
            requests.as_slice(),
            [Ok(InputDeviceRegistryRequest::Register { .. })]
        );

        Ok(())
    }

    #[test_case(&|registry| InputDeviceRegistry::add_touchscreen_device(registry) =>
                matches Ok(DeviceDescriptor {
                    touch: Some(TouchDescriptor {
                        input: Some(TouchInputDescriptor { .. }),
                        ..
                    }),
                    .. });
                "touchscreen_device")]
    #[test_case(&|registry| InputDeviceRegistry::add_media_buttons_device(registry) =>
                matches Ok(DeviceDescriptor {
                    consumer_control: Some(ConsumerControlDescriptor {
                        input: Some(ConsumerControlInputDescriptor { .. }),
                        ..
                    }),
                    .. });
                "media_buttons_device")]
    #[test_case(&|registry| InputDeviceRegistry::add_keyboard_device(registry) =>
                matches Ok(DeviceDescriptor {
                    keyboard: Some(KeyboardDescriptor { .. }),
                    ..
                });
                "keyboard_device")]
    #[test_case(&|registry| InputDeviceRegistry::add_mouse_device(registry) =>
                matches Ok(DeviceDescriptor {
                    mouse: Some(MouseDescriptor { .. }),
                    ..
                });
                "mouse_device")]
    fn add_device_registers_correct_device_type(
        add_device_method: &dyn Fn(&mut super::InputDeviceRegistry) -> Result<InputDevice, Error>,
    ) -> Result<DeviceDescriptor, Error> {
        let mut executor = fasync::TestExecutor::new();
        // Create an `InputDeviceRegistry`, and add a device to it.
        let (registry_proxy, mut registry_request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceRegistryMarker>()
                .context("failed to create proxy and stream for InputDeviceRegistry")?;
        let mut input_device_registry = InputDeviceRegistry { proxy: registry_proxy };
        let input_device =
            add_device_method(&mut input_device_registry).context("adding input device")?;

        let test_fut = async {
            // `input_device_registry` should send a `Register` messgage to `registry_request_stream`.
            // Use `registry_request_stream` to grab the `ClientEnd` of the device added above,
            // and convert the `ClientEnd` into an `InputDeviceProxy`.
            let input_device_proxy = match registry_request_stream
                .next()
                .await
                .context("stream read should yield Some")?
                .context("fidl read")?
            {
                InputDeviceRegistryRequest::Register { device, .. } => device,
            }
            .into_proxy()
            .context("converting client_end to proxy")?;

            // Send a `GetDescriptor` request to `input_device`, and verify that the device
            // is the expected type.
            let input_device_get_descriptor_fut = input_device_proxy.get_descriptor();
            let input_device_server_fut = input_device.flush();
            std::mem::drop(input_device_proxy); // Terminate stream served by `input_device_server_fut`.

            let (_server_result, get_descriptor_result) =
                futures::future::join(input_device_server_fut, input_device_get_descriptor_fut)
                    .await;
            get_descriptor_result.map_err(anyhow::Error::from)
        };
        pin_mut!(test_fut);

        match executor.run_until_stalled(&mut test_fut) {
            Poll::Ready(r) => r,
            Poll::Pending => Err(format_err!("test did not complete")),
        }
    }
}
