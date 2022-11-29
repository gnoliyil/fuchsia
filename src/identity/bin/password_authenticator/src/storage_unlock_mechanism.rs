// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        password_interaction::PasswordInteractionHandler, scrypt::EnrollScryptValidator, Config,
    },
    anyhow::anyhow,
    fidl_fuchsia_identity_authentication::{
        AttemptedEvent, Enrollment, Error as ApiError, InteractionProtocolServerEnd,
        StorageUnlockMechanismRequest, StorageUnlockMechanismRequestStream,
    },
    futures::TryStreamExt,
    identity_common::{EnrollmentData, PrekeyMaterial},
    tracing::log::error,
};

/// A struct to handle authentication and enrollment requests.
pub struct StorageUnlockMechanism {
    config: Config,
}

impl StorageUnlockMechanism {
    pub fn new(config: Config) -> Self {
        Self { config }
    }

    /// Serially process a stream of incoming StorageUnlockMechanism FIDL requests.
    pub async fn handle_requests_for_storage_unlock_mechanism(
        &self,
        mut stream: StorageUnlockMechanismRequestStream,
    ) {
        // TODO(fxb/109121): Ensure that a client closing the channel does not cause all clients to fail.
        while let Some(request) = stream.try_next().await.expect("read request") {
            self.handle_storage_unlock_mechanism_request(request).await.unwrap_or_else(|e| {
                error!("error handling StorageUnlockMechanism fidl request: {:#}", anyhow!(e));
            })
        }
    }

    /// Process a single StorageUnlockMechanism FIDL request and send a reply.
    async fn handle_storage_unlock_mechanism_request(
        &self,
        request: StorageUnlockMechanismRequest,
    ) -> Result<(), fidl::Error> {
        match request {
            StorageUnlockMechanismRequest::Authenticate { interaction, enrollments, responder } => {
                responder.send(&mut self.authenticate(interaction, enrollments))
            }
            StorageUnlockMechanismRequest::Enroll { interaction, responder } => {
                match self.enroll(interaction).await {
                    Ok((enrollment_data, prekey)) => {
                        responder.send(&mut Ok((enrollment_data.0, prekey.0)))
                    }
                    Err(e) => responder.send(&mut Err(e)),
                }
            }
        }
    }

    fn authenticate(
        &self,
        _interaction: InteractionProtocolServerEnd,
        _enrollments: Vec<Enrollment>,
    ) -> Result<AttemptedEvent, ApiError> {
        unimplemented!()
    }

    async fn enroll(
        &self,
        interaction: InteractionProtocolServerEnd,
    ) -> Result<(EnrollmentData, PrekeyMaterial), ApiError> {
        let validator = if self.config.allow_pinweaver {
            //TODO(fxb/114755): Remove error when implemented.
            return Err(ApiError::UnsupportedOperation);
        } else {
            EnrollScryptValidator {}
        };

        let server_end = if let InteractionProtocolServerEnd::Password(server_end) = interaction {
            server_end
        } else {
            return Err(ApiError::InvalidRequest);
        };

        let stream = server_end.into_stream().map_err(|_| ApiError::Resource)?;
        let (metadata, key) = PasswordInteractionHandler::new(validator)
            .await
            .handle_password_interaction_request_stream(stream)
            .await?;

        let enrollment_data =
            EnrollmentData(serde_json::to_vec(&metadata).map_err(|_| ApiError::InvalidDataFormat)?);
        Ok((enrollment_data, key))
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        fidl_fuchsia_identity_authentication::{PasswordInteractionMarker, TestInteractionMarker},
    };

    // By default, allow any implemented form of password and encryption.
    const DEFAULT_CONFIG: Config = Config { allow_scrypt: true, allow_pinweaver: true };
    // Define more restrictive configs to verify exclusions are implemented correctly.
    const SCRYPT_ONLY_CONFIG: Config = Config { allow_scrypt: true, allow_pinweaver: false };
    const PINWEAVER_ONLY_CONFIG: Config = Config { allow_scrypt: false, allow_pinweaver: true };

    #[fuchsia::test]
    #[should_panic(expected = "not implemented")]
    async fn test_authenticate_not_implemented() {
        let storage_unlock_mechanism = StorageUnlockMechanism::new(DEFAULT_CONFIG);
        let (_, interaction_server) =
            fidl::endpoints::create_endpoints::<TestInteractionMarker>().unwrap();

        let _ = storage_unlock_mechanism
            .authenticate(InteractionProtocolServerEnd::Test(interaction_server), vec![]);
    }

    #[fuchsia::test]
    async fn test_enroll_scrypt() {
        let storage_unlock_mechanism = StorageUnlockMechanism::new(SCRYPT_ONLY_CONFIG);
        let (client, interaction_server) =
            fidl::endpoints::create_endpoints::<PasswordInteractionMarker>().unwrap();

        let _ = client.into_proxy().unwrap().set_password("password").unwrap();

        let result = storage_unlock_mechanism
            .enroll(InteractionProtocolServerEnd::Password(interaction_server))
            .await;
        assert!(result.is_ok());
    }

    #[fuchsia::test]
    async fn test_enroll_pinweaver() {
        let storage_unlock_mechanism = StorageUnlockMechanism::new(PINWEAVER_ONLY_CONFIG);
        let (client, interaction_server) =
            fidl::endpoints::create_endpoints::<PasswordInteractionMarker>().unwrap();

        let _ = client.into_proxy().unwrap().set_password("password").unwrap();

        let result = storage_unlock_mechanism
            .enroll(InteractionProtocolServerEnd::Password(interaction_server))
            .await;
        assert_eq!(result, Err(ApiError::UnsupportedOperation));
    }

    #[fuchsia::test]
    async fn test_enroll_pinweaver_and_scrypt() {
        let storage_unlock_mechanism = StorageUnlockMechanism::new(DEFAULT_CONFIG);
        let (client, interaction_server) =
            fidl::endpoints::create_endpoints::<PasswordInteractionMarker>().unwrap();

        let _ = client.into_proxy().unwrap().set_password("password").unwrap();

        let result = storage_unlock_mechanism
            .enroll(InteractionProtocolServerEnd::Password(interaction_server))
            .await;
        // Choose the pinweaver implementation if both are available in config.
        // TODO(fxb/114755): Implement Pinweaver validation trait.
        assert_eq!(result, Err(ApiError::UnsupportedOperation));
    }

    #[fuchsia::test]
    async fn test_enroll_incorrect_ipse() {
        let storage_unlock_mechanism = StorageUnlockMechanism::new(SCRYPT_ONLY_CONFIG);
        let (client, interaction_server) =
            fidl::endpoints::create_endpoints::<TestInteractionMarker>().unwrap();

        let _ = client.into_proxy().unwrap().set_success().unwrap();

        let result = storage_unlock_mechanism
            .enroll(InteractionProtocolServerEnd::Test(interaction_server))
            .await;
        assert_eq!(result, Err(ApiError::InvalidRequest));
    }
}
