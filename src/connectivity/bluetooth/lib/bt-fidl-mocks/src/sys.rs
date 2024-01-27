// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::expect::{expect_call, Status},
    anyhow::Error,
    fidl_fuchsia_bluetooth::PeerId,
    fidl_fuchsia_bluetooth_sys::{
        AccessMarker, AccessProxy, AccessRequest, AccessRequestStream, Error as AccessError,
        InputCapability, OutputCapability, PairingDelegateProxy, PairingMarker, PairingOptions,
        PairingProxy, PairingRequest, PairingRequestStream,
    },
    fuchsia_zircon::Duration,
};

/// Provides a simple mock implementation of `fuchsia.bluetooth.sys.Pairing`.
pub struct PairingMock {
    stream: PairingRequestStream,
    timeout: Duration,
}

impl PairingMock {
    pub fn new(timeout: Duration) -> Result<(PairingProxy, PairingMock), Error> {
        let (proxy, stream) = fidl::endpoints::create_proxy_and_stream::<PairingMarker>()?;
        Ok((proxy, PairingMock { stream, timeout }))
    }

    pub async fn expect_set_pairing_delegate(
        &mut self,
        expected_input_cap: InputCapability,
        expected_output_cap: OutputCapability,
    ) -> Result<PairingDelegateProxy, Error> {
        expect_call(&mut self.stream, self.timeout, move |req| match req {
            PairingRequest::SetPairingDelegate { input, output, delegate, control_handle: _ }
                if input == expected_input_cap && output == expected_output_cap =>
            {
                Ok(Status::Satisfied(delegate.into_proxy()?))
            }
            _ => Ok(Status::Pending),
        })
        .await
    }
}

/// Provides a simple mock implementation of `fuchsia.bluetooth.sys.Access`.
pub struct AccessMock {
    stream: AccessRequestStream,
    timeout: Duration,
}

impl AccessMock {
    pub fn new(timeout: Duration) -> Result<(AccessProxy, AccessMock), Error> {
        let (proxy, stream) = fidl::endpoints::create_proxy_and_stream::<AccessMarker>()?;
        Ok((proxy, AccessMock { stream, timeout }))
    }

    pub async fn expect_disconnect(
        &mut self,
        expected_peer_id: PeerId,
        result: Result<(), AccessError>,
    ) -> Result<(), Error> {
        expect_call(&mut self.stream, self.timeout, move |req| match req {
            AccessRequest::Disconnect { id, responder } if id == expected_peer_id => {
                responder.send(result)?;
                Ok(Status::Satisfied(()))
            }
            _ => Ok(Status::Pending),
        })
        .await
    }

    pub async fn expect_forget(
        &mut self,
        expected_peer_id: PeerId,
        result: Result<(), AccessError>,
    ) -> Result<(), Error> {
        expect_call(&mut self.stream, self.timeout, move |req| match req {
            AccessRequest::Forget { id, responder } if id == expected_peer_id => {
                responder.send(result)?;
                Ok(Status::Satisfied(()))
            }
            _ => Ok(Status::Pending),
        })
        .await
    }

    pub async fn expect_pair(
        &mut self,
        expected_peer_id: PeerId,
        expected_options: PairingOptions,
        result: Result<(), AccessError>,
    ) -> Result<(), Error> {
        expect_call(&mut self.stream, self.timeout, move |req| match req {
            AccessRequest::Pair { id, options, responder }
                if id == expected_peer_id && options == expected_options =>
            {
                responder.send(result)?;
                Ok(Status::Satisfied(()))
            }
            _ => Ok(Status::Pending),
        })
        .await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use {
        crate::timeout_duration, fidl_fuchsia_bluetooth_sys::PairingDelegateMarker, futures::join,
    };

    #[fuchsia_async::run_until_stalled(test)]
    async fn test_expect_disconnect() {
        let (proxy, mut mock) = AccessMock::new(timeout_duration()).expect("failed to create mock");
        let peer_id = PeerId { value: 1 };

        let disconnect = proxy.disconnect(&peer_id);
        let expect = mock.expect_disconnect(peer_id, Ok(()));

        let (disconnect_result, expect_result) = join!(disconnect, expect);
        let _ = disconnect_result.expect("disconnect request failed");
        let _ = expect_result.expect("expectation not satisfied");
    }

    #[fuchsia_async::run_until_stalled(test)]
    async fn test_expect_forget() {
        let (proxy, mut mock) = AccessMock::new(timeout_duration()).expect("failed to create mock");
        let peer_id = PeerId { value: 1 };

        let forget = proxy.forget(&peer_id);
        let expect = mock.expect_forget(peer_id, Ok(()));

        let (forget_result, expect_result) = join!(forget, expect);
        let _ = forget_result.expect("forget request failed");
        let _ = expect_result.expect("expectation not satisifed");
    }

    #[fuchsia_async::run_until_stalled(test)]
    async fn test_expect_set_pairing_delegate() {
        let (proxy, mut mock) =
            PairingMock::new(timeout_duration()).expect("failed to create mock");

        let input_cap = InputCapability::None;
        let output_cap = OutputCapability::Display;
        let (pairing_delegate_client, _pairing_delegate_server) =
            fidl::endpoints::create_endpoints::<PairingDelegateMarker>();

        let pair_set_result =
            proxy.set_pairing_delegate(input_cap, output_cap, pairing_delegate_client);
        let expect_result = mock.expect_set_pairing_delegate(input_cap, output_cap).await;

        let _ = pair_set_result.expect("set_pairing_delegate request failed");
        let _ = expect_result.expect("expectation not satisifed");
    }
}
