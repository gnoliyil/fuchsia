// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::ota::controller::SendEvent;
use crate::ota::state_machine::Event;
use fidl_fuchsia_recovery::{FactoryResetMarker, FactoryResetProxy};
use fuchsia_async::{self as fasync};
use fuchsia_component::client::connect_to_protocol;

/// Asynchronously performs a factory reset.
/// It reboots almost instantly
pub struct FactoryResetAction {}

impl FactoryResetAction {
    pub fn run(event_sender: Box<dyn SendEvent>) {
        let proxy = connect_to_protocol::<FactoryResetMarker>().unwrap();
        Self::run_with_proxy(event_sender, proxy)
    }

    fn run_with_proxy(mut event_sender: Box<dyn SendEvent>, proxy: FactoryResetProxy) {
        let task = async move {
            println!("recovery: Executing factory reset command");
            let result = proxy.reset().await;
            if let Err(error) = result {
                event_sender.send(Event::Error(format!("Factory Reset failed: {:?}", error)));
            }
        };
        fasync::Task::local(task).detach();
    }
}

#[cfg(test)]
mod test {
    use super::FactoryResetAction;
    use crate::ota::controller::{MockSendEvent, SendEvent};
    use crate::ota::state_machine::Event;
    use anyhow::Error;
    use fidl_fuchsia_recovery::{FactoryResetMarker, FactoryResetProxy, FactoryResetRequest};
    use fuchsia_async::TimeoutExt;
    use fuchsia_async::{self as fasync};
    use fuchsia_zircon::sys::ZX_OK;
    use fuchsia_zircon::Duration;
    use futures::{channel::mpsc, StreamExt, TryStreamExt};
    use mockall::predicate::eq;

    const RESET_CALLED: i32 = 123456;

    // For future reference this test structure comes from
    // fxr/753732/4/src/recovery/lib/recovery-util/src/reboot.rs#49
    fn create_mock_factory_reset_server() -> Result<(FactoryResetProxy, mpsc::Receiver<i32>), Error>
    {
        let (mut sender, receiver) = mpsc::channel(1);
        let (proxy, mut request_stream) =
            fidl::endpoints::create_proxy_and_stream::<FactoryResetMarker>()?;

        fasync::Task::local(async move {
            while let Some(request) =
                request_stream.try_next().await.expect("failed to read mock request")
            {
                match request {
                    // This is the only possible value
                    FactoryResetRequest::Reset { responder } => {
                        sender.start_send(RESET_CALLED).unwrap();
                        responder.send(ZX_OK).ok();
                    }
                }
            }
        })
        .detach();
        Ok((proxy, receiver))
    }

    #[fuchsia::test]
    async fn test_reset_called() {
        let mut event_sender = MockSendEvent::new();
        event_sender.expect_send().with(eq(Event::Cancel)).times(0).return_const(());
        let event_sender: Box<dyn SendEvent> = Box::new(event_sender);

        let (proxy, mut receiver) = create_mock_factory_reset_server().unwrap();
        FactoryResetAction::run_with_proxy(event_sender, proxy);
        let status = receiver.next().on_timeout(Duration::from_seconds(5), || None).await.unwrap();
        assert_eq!(status, RESET_CALLED);
    }
}
