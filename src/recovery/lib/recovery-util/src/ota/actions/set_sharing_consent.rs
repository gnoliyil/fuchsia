// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::ota::controller::SendEvent;
use crate::ota::state_machine::Event;
use fidl_fuchsia_settings::{PrivacyMarker, PrivacyProxy, PrivacySettings};
use fuchsia_async::Task;
use fuchsia_component::client::connect_to_protocol;

pub struct SetSharingConsentAction {}

impl SetSharingConsentAction {
    pub fn run(event_sender: Box<dyn SendEvent>, data_sharing_consent: bool) {
        let proxy = connect_to_protocol::<PrivacyMarker>().unwrap();
        Self::run_with_proxy(event_sender, data_sharing_consent, proxy)
    }

    fn run_with_proxy(
        mut event_sender: Box<dyn SendEvent>,
        data_sharing_consent: bool,
        proxy: PrivacyProxy,
    ) {
        let task = async move {
            let mut privacy_settings = PrivacySettings::EMPTY;
            privacy_settings.user_data_sharing_consent = Some(data_sharing_consent);
            #[cfg(feature = "debug_logging")]
            println!("Setting privacy to {}", data_sharing_consent);
            let res = proxy.set(privacy_settings).await;
            #[cfg(feature = "debug_logging")]
            println!("Privacy response is {:?}", res);
            match res {
                Ok(Err(error)) => {
                    // Errors come back inside an Ok!
                    event_sender.send(Event::Error(format!(
                        "Failed to set privacy permission: {:?}",
                        error
                    )));
                }
                Ok(Ok(())) => {
                    event_sender.send(Event::Privacy(data_sharing_consent));
                }
                Err(error) => {
                    // Something has gone horribly wrong in the service
                    eprintln!("Set privacy returned an error: {:?}", error);
                    event_sender.send(Event::Error(format!(
                        "Failed to set privacy permission: {:?}",
                        error
                    )));
                }
            };
        };
        Task::local(task).detach();
    }
}

#[cfg(test)]
mod test {
    use super::SetSharingConsentAction;
    use crate::ota::controller::{MockSendEvent, SendEvent};
    use crate::ota::state_machine::Event;
    use anyhow::Error;
    use fidl::endpoints::{ControlHandle, Responder};
    use fidl_fuchsia_settings::{
        Error as PrivacyError, PrivacyMarker, PrivacyProxy, PrivacyRequest,
    };
    use fuchsia_async as fasync;
    use futures::{future, TryStreamExt};
    use mockall::predicate::eq;

    // For future reference this test structure comes from
    // fxr/753732/4/src/recovery/lib/recovery-util/src/reboot.rs#49
    fn create_mock_privacy_server(will_succeed: Option<bool>) -> Result<PrivacyProxy, Error> {
        let (proxy, mut request_stream) =
            fidl::endpoints::create_proxy_and_stream::<PrivacyMarker>()?;
        fasync::Task::local(async move {
            while let Some(request) =
                request_stream.try_next().await.expect("failed to read mock request")
            {
                match request {
                    PrivacyRequest::Set { responder, settings: _ } => match will_succeed {
                        Some(will_succeed) => {
                            let mut response =
                                if will_succeed { Ok(()) } else { Err(PrivacyError::Failed) };
                            responder.send(&mut response).expect("Should not fail");
                        }
                        // We haven't been told to succeed or fail so cause a FIDL error
                        None => responder.control_handle().shutdown(),
                    },
                    _ => {}
                }
            }
        })
        .detach();
        Ok(proxy)
    }

    #[fuchsia::test]
    fn test_privacy_set_true() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let mut event_sender = MockSendEvent::new();
        event_sender
            .expect_send()
            .withf(move |event| {
                if let Event::Privacy(consent) = event {
                    // == true written for clarity!
                    *consent == true
                } else {
                    false
                }
            })
            .times(1)
            .return_const(());
        let event_sender: Box<dyn SendEvent> = Box::new(event_sender);
        let proxy = create_mock_privacy_server(Some(true)).unwrap();
        let consent = true;
        SetSharingConsentAction::run_with_proxy(event_sender, consent, proxy);
        let _ = exec.run_until_stalled(&mut future::pending::<()>());
    }

    #[fuchsia::test]
    fn test_privacy_set_false() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let mut event_sender = MockSendEvent::new();
        event_sender
            .expect_send()
            .withf(move |event| {
                if let Event::Privacy(consent) = event {
                    // == false written for clarity!
                    *consent == false
                } else {
                    false
                }
            })
            .times(1)
            .return_const(());
        let event_sender: Box<dyn SendEvent> = Box::new(event_sender);
        let proxy = create_mock_privacy_server(Some(true)).unwrap();
        let consent = false;
        SetSharingConsentAction::run_with_proxy(event_sender, consent, proxy);
        let _ = exec.run_until_stalled(&mut future::pending::<()>());
    }

    #[fuchsia::test]
    fn test_privacy_set_error() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let mut event_sender = MockSendEvent::new();
        event_sender
            .expect_send()
            .with(eq(Event::Error("Error".to_string())))
            .times(1)
            .return_const(());
        let event_sender: Box<dyn SendEvent> = Box::new(event_sender);
        let proxy = create_mock_privacy_server(Some(false)).unwrap();
        let consent = false;
        SetSharingConsentAction::run_with_proxy(event_sender, consent, proxy);
        let _ = exec.run_until_stalled(&mut future::pending::<()>());
    }

    #[fuchsia::test]
    fn test_privacy_fidl_error() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let mut event_sender = MockSendEvent::new();
        event_sender
            .expect_send()
            .with(eq(Event::Error("Error".to_string())))
            .times(1)
            .return_const(());
        let event_sender: Box<dyn SendEvent> = Box::new(event_sender);
        let proxy = create_mock_privacy_server(None).unwrap();
        let consent = false;
        SetSharingConsentAction::run_with_proxy(event_sender, consent, proxy);
        let _ = exec.run_until_stalled(&mut future::pending::<()>());
    }
}
