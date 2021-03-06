// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::format_err;
use derivative::Derivative;
use fidl_fuchsia_hardware_audio::*;
use fuchsia_async as fasync;
use futures::StreamExt;
use std::sync::{Arc, Mutex};

use crate::driver::{ensure_dai_format_is_supported, ensure_pcm_format_is_supported};
use crate::DigitalAudioInterface;

/// The status of the current device.  Retrievable via `TestHandle::status`.
#[derive(Derivative, Clone, Debug)]
#[derivative(Default)]
pub enum TestStatus {
    #[derivative(Default)]
    Idle,
    Configured {
        dai_format: DaiFormat,
        pcm_format: PcmFormat,
    },
    Started {
        dai_format: DaiFormat,
        pcm_format: PcmFormat,
    },
}

impl TestStatus {
    fn start(&mut self) -> Result<(), ()> {
        if let Self::Configured { dai_format, pcm_format } = self {
            *self = Self::Started { dai_format: *dai_format, pcm_format: *pcm_format };
            Ok(())
        } else {
            Err(())
        }
    }

    fn stop(&mut self) {
        if let Self::Started { dai_format, pcm_format } = *self {
            *self = Self::Configured { dai_format, pcm_format };
        }
    }
}

#[derive(Clone)]
pub struct TestHandle(Arc<Mutex<TestStatus>>);

impl TestHandle {
    pub fn new() -> Self {
        Self(Arc::new(Mutex::new(TestStatus::default())))
    }

    pub fn status(&self) -> TestStatus {
        self.0.lock().unwrap().clone()
    }

    pub fn is_started(&self) -> bool {
        let lock = self.0.lock().unwrap();
        match *lock {
            TestStatus::Started { .. } => true,
            _ => false,
        }
    }

    fn set_configured(&self, dai_format: DaiFormat, pcm_format: PcmFormat) {
        let mut lock = self.0.lock().unwrap();
        *lock = TestStatus::Configured { dai_format, pcm_format };
    }

    fn start(&self) -> Result<(), ()> {
        self.0.lock().unwrap().start()
    }

    fn stop(&self) {
        self.0.lock().unwrap().stop()
    }
}

async fn handle_ring_buffer(mut requests: RingBufferRequestStream, handle: TestHandle) {
    while let Some(req) = requests.next().await {
        let req = req.expect("no error on ring buffer");
        match req {
            RingBufferRequest::Start { responder } => match handle.start() {
                Ok(()) => responder.send(0).expect("fidl response send ok"),
                Err(()) => {
                    log::warn!("Started when we couldn't expect it, shutting down");
                }
            },
            RingBufferRequest::Stop { responder } => {
                handle.stop();
                responder.send().expect("fidl response ok");
            }
            x => unimplemented!("RingBuffer Request not implemented: {:?}", x),
        };
    }
}

async fn test_handle_dai_requests(
    dai_formats: DaiSupportedFormats,
    pcm_formats: SupportedFormats,
    as_input: bool,
    mut requests: DaiRequestStream,
    handle: TestHandle,
) {
    use std::slice::from_ref;
    let properties = DaiProperties {
        is_input: Some(as_input),
        manufacturer: Some("Fuchsia".to_string()),
        ..DaiProperties::EMPTY
    };

    let mut _rb_task = None;
    while let Some(req) = requests.next().await {
        if let Err(e) = req {
            panic!("Had an error on the request stream for the DAI: {:?}", e);
        }
        match req.unwrap() {
            DaiRequest::GetProperties { responder } => {
                responder.send(properties.clone()).expect("properties response")
            }
            DaiRequest::GetDaiFormats { responder } => {
                responder.send(&mut Ok(vec![dai_formats.clone()])).expect("formats response")
            }
            DaiRequest::GetRingBufferFormats { responder } => {
                responder.send(&mut Ok(vec![pcm_formats.clone()])).expect("pcm response")
            }
            DaiRequest::CreateRingBuffer {
                dai_format, ring_buffer_format, ring_buffer, ..
            } => {
                let shutdown_bad_args =
                    |server: fidl::endpoints::ServerEnd<RingBufferMarker>, err: anyhow::Error| {
                        log::warn!("CreateRingBuffer: {:?}", err);
                        if let Err(e) =
                            server.close_with_epitaph(fuchsia_zircon::Status::INVALID_ARGS)
                        {
                            log::warn!("Couldn't send ring buffer epitaph: {:?}", e);
                        }
                    };
                if let Err(e) = ensure_dai_format_is_supported(from_ref(&dai_formats), &dai_format)
                {
                    shutdown_bad_args(ring_buffer, e);
                    continue;
                }
                let pcm_format = match ring_buffer_format.pcm_format {
                    Some(f) => f,
                    None => {
                        shutdown_bad_args(ring_buffer, format_err!("Only PCM format supported"));
                        continue;
                    }
                };
                if let Err(e) = ensure_pcm_format_is_supported(from_ref(&pcm_formats), &pcm_format)
                {
                    shutdown_bad_args(ring_buffer, e);
                    continue;
                }
                handle.set_configured(dai_format, pcm_format);
                let requests = ring_buffer.into_stream().expect("stream from serverend");
                _rb_task = Some(fasync::Task::spawn(handle_ring_buffer(requests, handle.clone())));
            }
            x => panic!("Got a reqest we haven't implemented: {:?}", x),
        };
    }
}

pub fn test_digital_audio_interface(as_input: bool) -> (DigitalAudioInterface, TestHandle) {
    let supported_dai_formats = DaiSupportedFormats {
        number_of_channels: vec![1],
        sample_formats: vec![DaiSampleFormat::PcmSigned],
        frame_formats: vec![DaiFrameFormat::FrameFormatStandard(DaiFrameFormatStandard::Tdm1)],
        frame_rates: vec![8000, 16000, 32000, 48000, 96000],
        bits_per_slot: vec![16],
        bits_per_sample: vec![16],
    };

    let supported_pcm_formats = SupportedFormats {
        pcm_supported_formats: Some(PcmSupportedFormats {
            number_of_channels: vec![1],
            sample_formats: vec![SampleFormat::PcmSigned],
            bytes_per_sample: vec![2],
            valid_bits_per_sample: vec![16],
            frame_rates: vec![8000, 16000, 32000, 48000, 96000],
        }),
        ..SupportedFormats::EMPTY
    };

    let (proxy, requests) =
        fidl::endpoints::create_proxy_and_stream::<DaiMarker>().expect("proxy creation");

    let handle = TestHandle::new();

    fasync::Task::spawn(test_handle_dai_requests(
        supported_dai_formats,
        supported_pcm_formats,
        as_input,
        requests,
        handle.clone(),
    ))
    .detach();

    (DigitalAudioInterface::from_proxy(proxy), handle)
}
