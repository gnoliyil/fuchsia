// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(fxbug.dev/51770): move everything except installer and policy into a shared crate,
// because these modules all come from //src/sys/pkg/bin/omaha-client.
mod http_request;
mod install_plan;
mod installer;
mod policy;
mod timer;
use {
    crate::omaha::{
        http_request::FuchsiaHyperHttpRequest, installer::IsolatedInstaller, timer::FuchsiaTimer,
    },
    anyhow::{Context, Error},
    futures::lock::Mutex,
    futures::prelude::*,
    omaha_client::{
        app_set::VecAppSet,
        common::App,
        configuration::{Config, Updater},
        cup_ecdsa::StandardCupv2Handler,
        http_request::HttpRequest,
        metrics::StubMetricsReporter,
        protocol::{request::OS, Cohort},
        state_machine::{update_check, StateMachineBuilder, StateMachineEvent, UpdateCheckError},
        storage::MemStorage,
        time::StandardTimeSource,
    },
    std::rc::Rc,
    tracing::error,
    version::Version,
};

/// Get a |Config| object to use when making requests to Omaha.
async fn get_omaha_config(version: &str, service_url: &str) -> Config {
    Config {
        updater: Updater { name: "Fuchsia".to_string(), version: Version::from([0, 0, 1, 0]) },

        os: OS {
            platform: "Fuchsia".to_string(),
            version: version.to_string(),
            service_pack: "".to_string(),
            arch: std::env::consts::ARCH.to_string(),
        },

        service_url: service_url.to_owned(),
        omaha_public_keys: None,
    }
}

/// Get the update URL to use from Omaha, and install the update.
pub async fn install_update(
    updater: crate::updater::Updater,
    appid: String,
    service_url: String,
    current_version: String,
    channel: String,
) -> Result<(), Error> {
    let version = match current_version.parse::<Version>() {
        Ok(version) => version,
        Err(e) => {
            error!("Unable to parse '{}' as Omaha version format: {:?}", current_version, e);
            Version::from([0])
        }
    };

    let cohort = Cohort { hint: Some(channel.clone()), name: Some(channel), ..Cohort::default() };
    let app_set =
        VecAppSet::new(vec![App::builder().id(appid).version(version).cohort(cohort).build()]);

    let config = get_omaha_config(&current_version, &service_url).await;
    install_update_with_http(updater, app_set, config, FuchsiaHyperHttpRequest::new())
        .await
        .context("installing update via http(s)")
}

#[derive(Debug, thiserror::Error)]
enum InstallUpdateError {
    #[error("expected exactly one UpdateCheckResult from Omaha, got {0:?}")]
    WrongNumberOfUpdateCheckResults(Vec<Result<update_check::Response, UpdateCheckError>>),
    #[error("expected exactly one app_response from Omaha, got {0:?}")]
    WrongNumberOfAppResponses(Vec<omaha_client::state_machine::update_check::AppResponse>),
    #[error("update check failed: {0}")]
    UpdateCheckFailed(UpdateCheckError),
    #[error("update check did not produce an update, took action {0:?}")]
    DidNotUpdate(update_check::Action),
}

async fn install_update_with_http<HR>(
    updater: crate::updater::Updater,
    app_set: VecAppSet,
    config: Config,
    http_request: HR,
) -> Result<(), InstallUpdateError>
where
    HR: HttpRequest,
{
    let storage = Rc::new(Mutex::new(MemStorage::new()));
    let installer = IsolatedInstaller { updater };
    let cup_handler = config.omaha_public_keys.as_ref().map(StandardCupv2Handler::new);
    let state_machine = StateMachineBuilder::new(
        policy::IsolatedPolicyEngine::new(StandardTimeSource),
        http_request,
        installer,
        FuchsiaTimer,
        StubMetricsReporter,
        storage,
        config,
        Rc::new(Mutex::new(app_set)),
        cup_handler,
    );

    let stream: Vec<StateMachineEvent> = state_machine.oneshot_check().await.collect().await;

    // TODO(fxbug.dev/117416): expose this data via the Monitor protocol, not just a single event.
    // Filter the state machine events down to just update check results,
    // which contain the final state of the check.
    let filtered_events: Vec<Result<update_check::Response, UpdateCheckError>> = stream
        .into_iter()
        .filter_map(|p| match p {
            StateMachineEvent::UpdateCheckResult(val) => Some(val),
            _ => None,
        })
        .collect();

    // Ensure we only got one update check result
    let [response]: [_; 1] =
        filtered_events.try_into().map_err(InstallUpdateError::WrongNumberOfUpdateCheckResults)?;
    let response = response.map_err(InstallUpdateError::UpdateCheckFailed)?;

    // Ensure that update check only contained one app response
    let [app_response]: [_; 1] =
        response.app_responses.try_into().map_err(InstallUpdateError::WrongNumberOfAppResponses)?;

    // Return the result of that single update check
    match app_response.result {
        update_check::Action::Updated => Ok(()),
        other_action => Err(InstallUpdateError::DidNotUpdate(other_action)),
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::updater::for_tests::{UpdaterBuilder, UpdaterForTest, UpdaterResult},
        anyhow::Context,
        fuchsia_pkg_testing::{make_current_epoch_json, PackageBuilder},
        fuchsia_zircon as zx,
        mock_paver::{hooks as mphooks, PaverEvent},
        omaha_client::http_request::mock::MockHttpRequest,
        serde_json::json,
    };

    const TEST_REPO_URL: &str = "fuchsia-pkg://example.com";

    const TEST_VERSION: &str = "20200101.0.0";
    const TEST_CHANNEL: &str = "test-channel";
    const TEST_APP_ID: &str = "qnzHyt4n";

    /// Use the Omaha state machine to perform an update.
    ///
    /// Arguments:
    /// * `updater`: UpdaterForTest environment to use with Omaha.
    /// * `app_set`: AppSet for use by Omaha.
    /// * `config`: Omaha client configuration.
    /// * `mock_responses`: In-order list of responses Omaha should get for each HTTP request it
    ///     makes.
    async fn run_omaha(
        updater: UpdaterForTest,
        app_set: VecAppSet,
        config: Config,
        mock_responses: Vec<serde_json::Value>,
    ) -> Result<UpdaterResult, Error> {
        let mut http = MockHttpRequest::empty();

        for response in mock_responses {
            let response = serde_json::to_vec(&response).unwrap();
            http.add_response(hyper::Response::new(response));
        }

        install_update_with_http(updater.updater, app_set, config, http)
            .await
            .context("Running omaha client")?;

        Ok(UpdaterResult {
            paver_events: updater.paver.take_events(),
            packages: updater.packages,
            resolver: updater.resolver,
            realm_instance: updater.realm_instance,
        })
    }

    fn get_test_app_set() -> VecAppSet {
        VecAppSet::new(vec![App::builder()
            .id(TEST_APP_ID.to_owned())
            .version([20200101, 0, 0, 0])
            .cohort(Cohort::new(TEST_CHANNEL))
            .build()])
    }

    fn get_test_config() -> Config {
        Config {
            updater: Updater { name: "Fuchsia".to_owned(), version: Version::from([0, 0, 1, 0]) },
            os: OS {
                platform: "Fuchsia".to_owned(),
                version: TEST_VERSION.to_owned(),
                service_pack: "".to_owned(),
                arch: std::env::consts::ARCH.to_owned(),
            },

            // Since we're using the mock http resolver, this doesn't matter.
            service_url: "http://example.com".to_owned(),
            omaha_public_keys: None,
        }
    }

    fn get_test_response(package_path: &str) -> Vec<serde_json::Value> {
        let update_response = json!({"response":{
            "server": "prod",
            "protocol": "3.0",
            "app": [{
                "appid": TEST_APP_ID,
                "status": "ok",
                "updatecheck": {
                    "status": "ok",
                    "urls": { "url": [{ "codebase": format!("{TEST_REPO_URL}/") }] },
                    "manifest": {
                        "version": "20200101.1.0.0",
                        "actions": {
                            "action": [
                                {
                                    "run": package_path,
                                    "event": "install"
                                },
                                {
                                    "event": "postinstall"
                                }
                            ]
                        },
                        "packages": {
                            "package": [
                                {
                                    "name": package_path,
                                    "fp": "2.20200101.1.0.0",
                                    "required": true,
                                }
                            ]
                        }
                    }
                }
            }],
        }});

        let event_response = json!({"response":{
            "server": "prod",
            "protocol": "3.0",
            "app": [{
                "appid": TEST_APP_ID,
                "status": "ok",
            }]
        }});

        let response =
            vec![update_response, event_response.clone(), event_response.clone(), event_response];
        response
    }

    /// Construct an UpdaterForTest for use in the Omaha tests.
    async fn build_updater() -> Result<UpdaterForTest, Error> {
        let data = "hello world!".as_bytes();
        let hook = |p: &PaverEvent| {
            if let PaverEvent::QueryActiveConfiguration = p {
                return zx::Status::NOT_SUPPORTED;
            }
            zx::Status::OK
        };
        let test_package = PackageBuilder::new("test_package")
            .add_resource_at("bin/hello", "this is a test".as_bytes())
            .add_resource_at("data/file", "this is a file".as_bytes())
            .add_resource_at("meta/test_package.cmx", "{}".as_bytes())
            .build()
            .await
            .context("Building test_package")?;
        let updater = UpdaterBuilder::new()
            .await
            .paver(|p| p.insert_hook(mphooks::return_error(hook)))
            .repo_url(TEST_REPO_URL)
            .add_package(test_package)
            .add_image("zbi.signed", data)
            .add_image("fuchsia.vbmeta", data)
            .add_image("recovery", data)
            .add_image("epoch.json", make_current_epoch_json().as_bytes())
            .add_image("recovery.vbmeta", data);
        let updater = updater.build().await;
        Ok(updater)
    }

    #[fuchsia::test]
    pub async fn test_omaha_update() -> Result<(), Error> {
        let updater = build_updater().await.context("Building updater")?;
        let package_path = format!("update?hash={}", updater.update_merkle_root);
        let app_set = get_test_app_set();
        let config = get_test_config();
        let response = get_test_response(&package_path);
        let result =
            run_omaha(updater, app_set, config, response).await.context("running omaha")?;

        result.verify_packages().await.expect("Packages are all there");
        Ok(())
    }

    #[fuchsia::test]
    pub async fn test_omaha_updater_reports_failure() -> Result<(), Error> {
        let app_set = get_test_app_set();
        let config = get_test_config();
        let updater = build_updater().await.context("Building updater")?;

        // No response, which means the update check should fail.
        let response = vec![];

        let result = run_omaha(updater, app_set, config, response).await;
        assert!(result.is_err());
        Ok(())
    }

    async fn build_updater_with_broken_paver() -> UpdaterForTest {
        let data = "hello world!".as_bytes();

        // Simulate the paver being completely broken, which means that installation should fail.
        let hook = |_p: &PaverEvent| zx::Status::INTERNAL;

        let updater = UpdaterBuilder::new()
            .await
            .paver(|p| p.insert_hook(mphooks::return_error(hook)))
            .repo_url(TEST_REPO_URL)
            .add_image("zbi.signed", data);

        updater.build().await
    }

    #[fuchsia::test]
    pub async fn test_installation_error_reports_failure() -> Result<(), Error> {
        let app_set = get_test_app_set();
        let config = get_test_config();
        let updater = build_updater_with_broken_paver().await;
        let package_path = format!("update?hash={}", updater.update_merkle_root);
        let response = get_test_response(&package_path);

        let result = run_omaha(updater, app_set, config, response).await;
        assert!(result.is_err());
        Ok(())
    }
}
