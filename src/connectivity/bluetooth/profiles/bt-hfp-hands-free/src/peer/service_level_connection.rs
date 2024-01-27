// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use at::SerDe;
use at_commands as at;
use fuchsia_bluetooth::types::Channel;
use std::collections::hash_map::HashMap;
use tracing::warn;

use super::indicators::{AgIndicators, HfIndicators};
use super::procedure::{Procedure, ProcedureMarker};

use crate::config::HandsFreeFeatureSupport;
use crate::features::{AgFeatures, CallHoldAction, HfFeatures, CVSD};

pub struct SlcState {
    /// Collection of active procedures.
    pub procedures: HashMap<ProcedureMarker, Box<dyn Procedure>>,
    /// Collection of shared features and indicators between two
    /// devices.
    pub shared_state: SharedState,
}

#[derive(Clone, Default)]
pub struct SharedState {
    /// Featuers that the HF supports.
    pub hf_features: HfFeatures,
    /// Features that the AG supports.
    pub ag_features: AgFeatures,
    /// The current indicator status of the AG.
    pub ag_indicators: AgIndicators,
    /// The current indicator status of the HF
    pub hf_indicators: HfIndicators,
    /// Determines whether the SLCI procedure has completed and
    /// can proceed to do other procedures.
    pub initialized: bool,
    /// Determines whether the indicator status update function is enabled.
    pub indicators_update_enabled: bool,
    /// Features supported from the three-way calling or call waiting
    pub three_way_features: Vec<CallHoldAction>,
    /// The negotiated coded for this connection between the AG and HF.
    pub selected_codec: Option<u8>,
    /// The codec(s) supported by the HF.
    pub supported_codecs: Vec<u8>,
}

// TODO(fxbug.dev/104703): More fields for SLCI
impl SlcState {
    pub fn new(config: HandsFreeFeatureSupport) -> Self {
        let features = SharedState::new(config);
        Self { procedures: HashMap::new(), shared_state: features }
    }

    /// Identifies procedure from the response and inserts the identifying procedure marker into map.
    // TODO(fxr/127086) Procedure management
    #[allow(unused)]
    pub fn match_to_procedure(
        &mut self,
        initialized: bool,
        response: &at::Response,
    ) -> Result<ProcedureMarker, Error> {
        let procedure_id =
            ProcedureMarker::identify_procedure_from_response(initialized, &response);
        match procedure_id {
            Ok(id) => {
                let _ = self.procedures.entry(id).or_insert(id.initialize());
                Ok(id)
            }
            Err(e) => {
                warn!(?response, "Did not match to procedure");
                return Err(e);
            }
        }
    }
}

impl SharedState {
    pub fn new(config: HandsFreeFeatureSupport) -> Self {
        Self {
            hf_features: config.into(),
            ag_features: AgFeatures::default(),
            ag_indicators: AgIndicators::default(),
            hf_indicators: HfIndicators::default(),
            initialized: false,
            indicators_update_enabled: true,
            three_way_features: Vec::new(),
            selected_codec: None,
            supported_codecs: vec![CVSD],
        }
    }

    pub fn supports_codec_negotiation(&self) -> bool {
        self.ag_features.contains(AgFeatures::CODEC_NEGOTIATION)
            && self.hf_features.contains(HfFeatures::CODEC_NEGOTIATION)
    }

    pub fn supports_three_way_calling(&self) -> bool {
        self.ag_features.contains(AgFeatures::THREE_WAY_CALLING)
            && self.hf_features.contains(HfFeatures::THREE_WAY_CALLING)
    }

    pub fn supports_hf_indicators(&self) -> bool {
        self.ag_features.contains(AgFeatures::HF_INDICATORS)
            && self.hf_features.contains(HfFeatures::HF_INDICATORS)
    }

    #[cfg(test)]
    pub fn load_with_set_features(hf_features: HfFeatures, ag_features: AgFeatures) -> Self {
        Self { hf_features, ag_features, ..SharedState::default() }
    }
}

/// Serializes the AT commands and sends them through the provided RFCOMM channel
// TODO(127086) Move to AT connection and use in task.rs.
#[allow(unused)]
pub fn write_commands_to_channel(channel: &mut Channel, commands: &mut [at::Command]) {
    if commands.len() > 0 {
        let mut bytes = Vec::new();
        let _ = at::Command::serialize(&mut bytes, commands);
        if let Err(e) = channel.as_ref().write(&bytes) {
            warn!("Could not fully write serialized commands to channel: {:?}", e);
        };
    }
}
