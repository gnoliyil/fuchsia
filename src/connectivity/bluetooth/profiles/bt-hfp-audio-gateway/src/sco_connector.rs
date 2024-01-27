// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl::endpoints::Proxy;
use fidl_fuchsia_bluetooth_bredr as bredr;
use fuchsia_bluetooth::{profile::ValidScoConnectionParameters, types::PeerId};
use fuchsia_inspect_derive::Unit;
use futures::{Future, FutureExt, StreamExt};
use std::convert::TryInto;
use tracing::{debug, info};

use crate::error::ScoConnectError;
use crate::features::CodecId;

/// The components of an active SCO connection.
/// Dropping this struct will close the SCO connection.
#[derive(Debug)]
pub struct ScoConnection {
    /// The parameters that this connection was set up with.
    pub params: ValidScoConnectionParameters,
    /// Proxy for this connection.  Used to read/write to the connection.
    pub proxy: bredr::ScoConnectionProxy,
}

impl Unit for ScoConnection {
    type Data = <ValidScoConnectionParameters as Unit>::Data;
    fn inspect_create(&self, parent: &fuchsia_inspect::Node, name: impl AsRef<str>) -> Self::Data {
        self.params.inspect_create(parent, name)
    }

    fn inspect_update(&self, data: &mut Self::Data) {
        self.params.inspect_update(data)
    }
}

impl ScoConnection {
    pub fn on_closed(&self) -> impl Future<Output = ()> + 'static {
        self.proxy.on_closed().extend_lifetime().map(|_| ())
    }

    pub fn is_closed(&self) -> bool {
        self.proxy.is_closed()
    }

    #[cfg(test)]
    pub fn build(params: bredr::ScoConnectionParameters, proxy: bredr::ScoConnectionProxy) -> Self {
        ScoConnection { params: params.try_into().unwrap(), proxy }
    }
}

#[derive(Clone)]
pub struct ScoConnector {
    proxy: bredr::ProfileProxy,
    in_band_sco: bool,
}

fn common_sco_params() -> bredr::ScoConnectionParameters {
    bredr::ScoConnectionParameters {
        air_frame_size: Some(60), // Chosen to match legacy usage.
        // IO parameters are to fit 16-bit PSM Signed audio input expected from the audio chip.
        io_coding_format: Some(bredr::CodingFormat::LinearPcm),
        io_frame_size: Some(16),
        io_pcm_data_format: Some(fidl_fuchsia_hardware_audio::SampleFormat::PcmSigned),
        path: Some(bredr::DataPath::Offload),
        ..Default::default()
    }
}

/// If all eSCO parameters fail to setup a connection, these parameters are required to be
/// supported by all peers.  HFP 1.8 Section 5.7.1.
fn sco_params_fallback() -> bredr::ScoConnectionParameters {
    bredr::ScoConnectionParameters {
        parameter_set: Some(bredr::HfpParameterSet::D1),
        air_coding_format: Some(bredr::CodingFormat::Cvsd),
        // IO bandwidth to match an 8khz audio rate.
        io_bandwidth: Some(16000),
        ..common_sco_params()
    }
}

fn params_with_data_path(
    sco_params: bredr::ScoConnectionParameters,
    in_band_sco: bool,
) -> bredr::ScoConnectionParameters {
    bredr::ScoConnectionParameters {
        path: in_band_sco.then_some(bredr::DataPath::Host).or(Some(bredr::DataPath::Offload)),
        ..sco_params
    }
}

// pub in this crate for tests
pub(crate) fn parameter_sets_for_codec(
    codec_id: CodecId,
    in_band_sco: bool,
) -> Vec<bredr::ScoConnectionParameters> {
    use bredr::HfpParameterSet::*;
    match codec_id {
        CodecId::MSBC => {
            let (air_coding_format, io_bandwidth) = if in_band_sco {
                (Some(bredr::CodingFormat::Transparent), Some(16000))
            } else {
                // IO bandwidth to match an 16khz audio rate. (x2 for input + output)
                (Some(bredr::CodingFormat::Msbc), Some(32000))
            };
            let params_fn = |set| bredr::ScoConnectionParameters {
                parameter_set: Some(set),
                air_coding_format,
                io_bandwidth,
                ..params_with_data_path(common_sco_params(), in_band_sco)
            };
            // TODO(b/200305833): Disable MsbcT1 for now as it results in bad audio
            //vec![params_fn(T2), params_fn(T1)]
            vec![params_fn(T2)]
        }
        // CVSD parameter sets
        _ => {
            let (io_bandwidth, io_frame_size, io_coding_format) = if in_band_sco {
                (Some(16000), Some(8), Some(bredr::CodingFormat::Cvsd))
            } else {
                (Some(16000), Some(16), Some(bredr::CodingFormat::LinearPcm))
            };

            let params_fn = |set| bredr::ScoConnectionParameters {
                parameter_set: Some(set),
                io_bandwidth,
                io_frame_size,
                io_coding_format,
                ..params_with_data_path(sco_params_fallback(), in_band_sco)
            };
            vec![params_fn(S4), params_fn(S1), params_fn(D1)]
        }
    }
}

#[derive(Debug, Clone, PartialEq, Copy)]
enum ScoInitiatorRole {
    Initiate,
    Accept,
}

impl ScoConnector {
    pub fn build(proxy: bredr::ProfileProxy, in_band_sco: bool) -> Self {
        Self { proxy, in_band_sco }
    }

    async fn setup_sco_connection(
        proxy: bredr::ProfileProxy,
        peer_id: PeerId,
        role: ScoInitiatorRole,
        params: Vec<bredr::ScoConnectionParameters>,
    ) -> Result<ScoConnection, ScoConnectError> {
        let (client, mut requests) =
            fidl::endpoints::create_request_stream::<bredr::ScoConnectionReceiverMarker>()?;
        proxy.connect_sco(
            &peer_id.into(),
            /* initiate = */ role == ScoInitiatorRole::Initiate,
            &params,
            client,
        )?;
        match requests.next().await {
            Some(Ok(bredr::ScoConnectionReceiverRequest::Connected {
                connection,
                params,
                control_handle: _,
            })) => {
                let params = params.try_into().map_err(|_| ScoConnectError::ScoInvalidArguments)?;
                let proxy = connection.into_proxy().map_err(|_| ScoConnectError::ScoFailed)?;
                Ok(ScoConnection { params, proxy })
            }
            Some(Ok(bredr::ScoConnectionReceiverRequest::Error { error, .. })) => Err(error.into()),
            Some(Err(e)) => Err(e.into()),
            None => Err(ScoConnectError::ScoCanceled),
        }
    }

    fn parameters_for_codecs(&self, codecs: Vec<CodecId>) -> Vec<bredr::ScoConnectionParameters> {
        codecs
            .into_iter()
            .map(|id| parameter_sets_for_codec(id, self.in_band_sco))
            .flatten()
            .collect()
    }

    pub fn connect(
        &self,
        peer_id: PeerId,
        codecs: Vec<CodecId>,
    ) -> impl Future<Output = Result<ScoConnection, ScoConnectError>> + 'static {
        let params = self.parameters_for_codecs(codecs);
        info!("Initiating SCO connection for {}: {:?}", peer_id, &params);

        let proxy = self.proxy.clone();
        async move {
            for param in params {
                let result = Self::setup_sco_connection(
                    proxy.clone(),
                    peer_id,
                    ScoInitiatorRole::Initiate,
                    vec![param.clone()],
                )
                .await;
                match &result {
                    // Return early if there is a FIDL issue, or we succeeded.
                    Err(ScoConnectError::Fidl { .. }) | Ok(_) => return result,
                    // Otherwise continue to try the next params.
                    Err(e) => debug!(?peer_id, ?param, ?e, "Connection failed, trying next set.."),
                }
            }
            info!(?peer_id, "Exhausted SCO connection parameters");
            Err(ScoConnectError::ScoFailed)
        }
    }

    pub fn accept(
        &self,
        peer_id: PeerId,
        codecs: Vec<CodecId>,
    ) -> impl Future<Output = Result<ScoConnection, ScoConnectError>> + 'static {
        let params = self.parameters_for_codecs(codecs);
        info!("Accepting SCO connection for {}: {:?}.", peer_id, &params);

        let proxy = self.proxy.clone();
        Self::setup_sco_connection(proxy, peer_id, ScoInitiatorRole::Accept, params)
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    use crate::profile::test_server::setup_profile_and_test_server;
    use bredr::ScoConnectionMarker;
    use fidl_fuchsia_bluetooth_bredr::HfpParameterSet;

    #[track_caller]
    pub fn connection_for_codec(
        codec_id: CodecId,
        in_band: bool,
    ) -> (ScoConnection, bredr::ScoConnectionRequestStream) {
        let sco_params = parameter_sets_for_codec(codec_id, in_band).pop().unwrap();
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<ScoConnectionMarker>().unwrap();
        let connection = ScoConnection::build(sco_params, proxy);
        (connection, stream)
    }

    #[fuchsia::test]
    fn codec_parameters() {
        let _exec = fuchsia_async::TestExecutor::new();

        // Out-of-band SCO.
        let (_, profile_svc, _) = setup_profile_and_test_server();
        let sco = ScoConnector::build(profile_svc.clone(), false);
        let res = sco.parameters_for_codecs(vec![CodecId::MSBC, CodecId::CVSD]);
        assert_eq!(res.len(), 4);
        assert_eq!(
            res,
            vec![
                bredr::ScoConnectionParameters {
                    parameter_set: Some(HfpParameterSet::T2),
                    air_coding_format: Some(bredr::CodingFormat::Msbc),
                    io_bandwidth: Some(32000),
                    path: Some(bredr::DataPath::Offload),
                    ..common_sco_params()
                },
                bredr::ScoConnectionParameters {
                    parameter_set: Some(HfpParameterSet::S4),
                    path: Some(bredr::DataPath::Offload),
                    ..sco_params_fallback()
                },
                bredr::ScoConnectionParameters {
                    parameter_set: Some(HfpParameterSet::S1),
                    path: Some(bredr::DataPath::Offload),
                    ..sco_params_fallback()
                },
                bredr::ScoConnectionParameters {
                    path: Some(bredr::DataPath::Offload),
                    ..sco_params_fallback()
                },
            ]
        );

        // In-band SCO.
        let sco = ScoConnector::build(profile_svc.clone(), true);
        let res = sco.parameters_for_codecs(vec![CodecId::MSBC, CodecId::CVSD]);
        assert_eq!(res.len(), 4);
        assert_eq!(
            res,
            vec![
                bredr::ScoConnectionParameters {
                    parameter_set: Some(HfpParameterSet::T2),
                    air_coding_format: Some(bredr::CodingFormat::Transparent),
                    io_bandwidth: Some(16000),
                    path: Some(bredr::DataPath::Host),
                    ..common_sco_params()
                },
                bredr::ScoConnectionParameters {
                    parameter_set: Some(HfpParameterSet::S4),
                    io_coding_format: Some(bredr::CodingFormat::Cvsd),
                    io_frame_size: Some(8),
                    path: Some(bredr::DataPath::Host),
                    ..sco_params_fallback()
                },
                bredr::ScoConnectionParameters {
                    parameter_set: Some(HfpParameterSet::S1),
                    io_coding_format: Some(bredr::CodingFormat::Cvsd),
                    io_frame_size: Some(8),
                    path: Some(bredr::DataPath::Host),
                    ..sco_params_fallback()
                },
                bredr::ScoConnectionParameters {
                    io_coding_format: Some(bredr::CodingFormat::Cvsd),
                    io_frame_size: Some(8),
                    path: Some(bredr::DataPath::Host),
                    ..sco_params_fallback()
                },
            ]
        );
    }
}
