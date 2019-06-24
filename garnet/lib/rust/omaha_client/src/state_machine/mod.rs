// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    clock,
    common::{App, CheckOptions, ProtocolState, UpdateCheckSchedule},
    configuration::Config,
    http_request::HttpRequest,
    installer::{Installer, Plan},
    policy::{CheckDecision, PolicyEngine, UpdateDecision},
    protocol::{
        self,
        request::{Event, EventErrorCode, EventResult, EventType, InstallSource},
        response::{parse_json_response, OmahaStatus, Response},
    },
    request_builder::{self, RequestBuilder, RequestParams},
};
use chrono::{DateTime, Utc};
use failure::Fail;
use futures::{compat::Stream01CompatExt, prelude::*};
use http::response::Parts;
use log::{error, info, warn};
use std::str::Utf8Error;
use std::time::SystemTime;

pub mod update_check;

mod timer;
pub use timer::Timer;

/// This is the core state machine for a client's update check.  It is instantiated and used to
/// perform a single update check process.
#[derive(Debug)]
pub struct StateMachine<PE, HR, IN, TM>
where
    PE: PolicyEngine,
    HR: HttpRequest,
    IN: Installer,
    TM: Timer,
{
    /// The immutable configuration of the client itself.
    client_config: Config,

    policy_engine: PE,

    http: HR,

    installer: IN,

    timer: TM,
}

/// This is the set of errors that can occur when making a request to Omaha.  This is an internal
/// collection of error types.
#[derive(Fail, Debug)]
pub enum OmahaRequestError {
    #[fail(display = "Unexpected JSON error constructing update check: {}", _0)]
    Json(#[cause] serde_json::Error),

    #[fail(display = "Error building update check HTTP request: {}", _0)]
    HttpBuilder(#[cause] http::Error),

    #[fail(display = "Hyper error performing update check: {}", _0)]
    Hyper(#[cause] hyper::Error),

    #[fail(display = "HTTP error performing update check: {}", _0)]
    HttpStatus(hyper::StatusCode),
}

impl From<request_builder::Error> for OmahaRequestError {
    fn from(err: request_builder::Error) -> Self {
        match err {
            request_builder::Error::Json(e) => OmahaRequestError::Json(e),
            request_builder::Error::Http(e) => OmahaRequestError::HttpBuilder(e),
        }
    }
}

impl From<hyper::Error> for OmahaRequestError {
    fn from(err: hyper::Error) -> Self {
        OmahaRequestError::Hyper(err)
    }
}

impl From<serde_json::Error> for OmahaRequestError {
    fn from(err: serde_json::Error) -> Self {
        OmahaRequestError::Json(err)
    }
}

impl From<http::Error> for OmahaRequestError {
    fn from(err: http::Error) -> Self {
        OmahaRequestError::HttpBuilder(err)
    }
}

impl From<http::StatusCode> for OmahaRequestError {
    fn from(sc: http::StatusCode) -> Self {
        OmahaRequestError::HttpStatus(sc)
    }
}

/// This is the set of errors that can occur when parsing the response body from Omaha.  This is an
/// internal collection of error types.
#[derive(Fail, Debug)]
#[allow(dead_code)]
pub enum ResponseParseError {
    #[fail(display = "Response was not valid UTF-8")]
    Utf8(#[cause] Utf8Error),

    #[fail(display = "Unexpected JSON error parsing update check response: {}", _0)]
    Json(#[cause] serde_json::Error),
}

#[derive(Fail, Debug)]
pub enum UpdateCheckError {
    #[fail(display = "Check not performed per policy: {:?}", _0)]
    Policy(CheckDecision),

    #[fail(display = "Error checking with Omaha: {:?}", _0)]
    OmahaRequest(OmahaRequestError),

    #[fail(display = "Error parsing Omaha response: {:?}", _0)]
    ResponseParser(ResponseParseError),

    #[fail(display = "Unable to create an install plan: {:?}", _0)]
    InstallPlan(failure::Error),
}

impl<PE, HR, IN, TM> StateMachine<PE, HR, IN, TM>
where
    PE: PolicyEngine,
    HR: HttpRequest,
    IN: Installer,
    TM: Timer,
{
    pub fn new(policy_engine: PE, http: HR, installer: IN, config: &Config, timer: TM) -> Self {
        StateMachine { client_config: config.clone(), policy_engine, http, installer, timer }
    }

    /// Load and initialize update check context from persistent storage.
    fn load_context(&mut self) -> update_check::Context {
        // TODO: Read last_update_time, server_dictated_poll_interval, etc from storage.
        update_check::Context {
            schedule: UpdateCheckSchedule {
                last_update_time: SystemTime::UNIX_EPOCH,
                next_update_time: clock::now(),
                next_update_window_start: clock::now(),
            },
            state: ProtocolState::default(),
        }
    }

    /// Start the StateMachine to do periodic update check in the background. The future this
    /// function returns never finishes!
    pub async fn start(&mut self, mut apps: Vec<App>) {
        let mut context = self.load_context();

        loop {
            context.schedule = await!(self.policy_engine.compute_next_update_time(
                &apps,
                &context.schedule,
                &context.state,
            ));
            // Wait if |next_update_time| is in the future.
            if let Ok(duration) = context.schedule.next_update_time.duration_since(clock::now()) {
                let date_time = DateTime::<Utc>::from(context.schedule.next_update_time);
                info!("Waiting until {} for the next update check...", date_time.to_rfc3339());

                await!(self.timer.wait(duration));
            }

            let options = CheckOptions { source: InstallSource::ScheduledTask };
            match await!(self.perform_update_check(options, &apps, context.clone())) {
                Ok(result) => {
                    info!("Update check result: {:?}", result);
                    // Update check succeeded, update |last_update_time|.
                    context.schedule.last_update_time = clock::now();

                    // Update the service dictated poll interval (which is an Option<>, so doesn't
                    // need to be tested for existence here).
                    context.state.server_dictated_poll_interval =
                        result.server_dictated_poll_interval;

                    // Increment |consecutive_failed_update_attempts| if any app failed to install,
                    // otherwise reset it to 0.
                    if result
                        .app_responses
                        .iter()
                        .any(|app| app.result == update_check::Action::InstallPlanExecutionError)
                    {
                        context.state.consecutive_failed_update_attempts += 1;
                    } else {
                        context.state.consecutive_failed_update_attempts = 0;
                    }

                    // Update check succeeded, reset |consecutive_failed_update_checks| to 0.
                    context.state.consecutive_failed_update_checks = 0;

                    // Update the cohort for each app from Omaha.
                    for app_response in result.app_responses {
                        for app in apps.iter_mut() {
                            if app.id == app_response.app_id {
                                app.cohort = app_response.cohort;
                                break;
                            }
                        }
                    }

                    // TODO: update consecutive_proxied_requests
                }
                Err(error) => {
                    error!("Update check failed: {:?}", error);
                    // Update check failed, increment |consecutive_failed_update_checks|.
                    context.state.consecutive_failed_update_checks += 1;

                    match error {
                        UpdateCheckError::ResponseParser(_) | UpdateCheckError::InstallPlan(_) => {
                            // We talked to Omaha, update |last_update_time|.
                            context.schedule.last_update_time = clock::now();
                        }
                        _ => {}
                    }
                }
            }
        }
    }

    /// This function constructs the chain of async futures needed to perform all of the async tasks
    /// that comprise an update check.
    pub async fn perform_update_check<'a>(
        &'a mut self,
        options: CheckOptions,
        apps: &'a [App],
        context: update_check::Context,
    ) -> Result<update_check::Response, UpdateCheckError> {
        info!("Checking to see if an update check is allowed at this time for {:?}", apps);
        let decision = await!(self.policy_engine.update_check_allowed(
            apps,
            &context.schedule,
            &context.state,
            &options,
        ));

        info!("The update check decision is: {:?}", decision);

        let request_params = match decision {
            // Positive results, will continue with the update check process
            CheckDecision::Ok(rp) | CheckDecision::OkUpdateDeferred(rp) => rp,

            // Negative results, exit early
            CheckDecision::TooSoon => {
                info!("Too soon for update check, ending");
                // TODO: Report status
                return Err(UpdateCheckError::Policy(decision));
            }
            CheckDecision::ThrottledByPolicy => {
                info!("Update check has been throttled by the Policy, ending");
                // TODO: Report status
                return Err(UpdateCheckError::Policy(decision));
            }
            CheckDecision::DeniedByPolicy => {
                info!("Update check has ben denied by the Policy");
                // TODO: Report status
                return Err(UpdateCheckError::Policy(decision));
            }
        };

        // Construct a request for the app(s).
        let mut request_builder = RequestBuilder::new(&self.client_config, &request_params);
        for app in apps {
            request_builder = request_builder.add_update_check(app);
        }

        let (_parts, data) = match await!(Self::do_omaha_request(&mut self.http, request_builder)) {
            Ok(res) => res,
            Err(OmahaRequestError::Json(e)) => {
                error!("Unable to construct request body! {:?}", e);
                // TODO:  Report progress status
                return Err(UpdateCheckError::OmahaRequest(e.into()));
            }
            Err(OmahaRequestError::HttpBuilder(e)) => {
                error!("Unable to construct HTTP request! {:?}", e);
                // TODO:  Report progress status
                return Err(UpdateCheckError::OmahaRequest(e.into()));
            }
            Err(OmahaRequestError::Hyper(e)) => {
                warn!("Unable to contact Omaha: {:?}", e);
                // TODO:  Report progress status
                // TODO:  Parse for proper retry behavior
                return Err(UpdateCheckError::OmahaRequest(e.into()));
            }
            Err(OmahaRequestError::HttpStatus(e)) => {
                warn!("Unable to contact Omaha: {:?}", e);
                // TODO:  Report progress status
                // TODO:  Parse for proper retry behavior
                return Err(UpdateCheckError::OmahaRequest(e.into()));
            }
        };

        let response = match Self::parse_omaha_response(&data) {
            Ok(res) => res,
            Err(err) => {
                warn!("Unable to parse Omaha response: {:?}", err);
                await!(self.report_error_event(
                    apps,
                    &request_params,
                    EventErrorCode::ParseResponse,
                ));
                // TODO: Report progress status (error)
                return Err(UpdateCheckError::ResponseParser(err));
            }
        };

        info!("result: {:?}", response);

        let statuses = Self::get_app_update_statuses(&response);
        for (app_id, status) in &statuses {
            // TODO:  Report or metric statuses other than 'no-update' and 'ok'
            info!("Omaha update check status: {} => {:?}", app_id, status);
        }

        let some_app_has_update = statuses.iter().any(|(_id, status)| **status == OmahaStatus::Ok);
        if !some_app_has_update {
            // A succesfull, no-update, check

            // TODO: Report progress status (done)
            Ok(Self::make_response(response, update_check::Action::NoUpdate))
        } else {
            info!(
                "At least one app has an update, proceeding to build and process an Install Plan"
            );

            let install_plan = match IN::InstallPlan::try_create_from(&response) {
                Ok(plan) => plan,
                Err(e) => {
                    error!("Unable to construct install plan! {}", e);
                    await!(self.report_error_event(
                        apps,
                        &request_params,
                        EventErrorCode::ConstructInstallPlan
                    ));
                    // TODO: Report progress status (Error)
                    return Err(UpdateCheckError::InstallPlan(e.into()));
                }
            };

            info!("Validating Install Plan with Policy");
            let install_plan_decision = await!(self.policy_engine.update_can_start(&install_plan));
            match install_plan_decision {
                UpdateDecision::Ok => {
                    info!("Proceeding with install plan.");
                }
                UpdateDecision::DeferredByPolicy => {
                    info!("Install plan was deferred by Policy.");
                    // Report "error" to Omaha (as this is an event that needs reporting as the
                    // install isn't starting immediately.
                    let event = Event {
                        event_type: EventType::UpdateComplete,
                        event_result: EventResult::UpdateDeferred,
                        ..Event::default()
                    };
                    await!(self.report_omaha_event(apps, &request_params, event));

                    // TODO: Report progress status (Deferred)
                    return Ok(Self::make_response(
                        response,
                        update_check::Action::DeferredByPolicy,
                    ));
                }
                UpdateDecision::DeniedByPolicy => {
                    warn!("Install plan was denied by Policy, see Policy logs for reasoning");
                    await!(self.report_error_event(
                        apps,
                        &request_params,
                        EventErrorCode::DeniedByPolicy
                    ));

                    // TODO: Report progress status (Error)
                    return Ok(Self::make_response(response, update_check::Action::DeniedByPolicy));
                }
            }

            // TODO: Report Status (Updating)
            await!(self.report_success_event(
                apps,
                &request_params,
                EventType::UpdateDownloadStarted
            ));

            let install_result = await!(self.installer.perform_install(&install_plan, None));
            if let Err(e) = install_result {
                warn!("Installation failed: {}", e);
                await!(self.report_error_event(
                    apps,
                    &request_params,
                    EventErrorCode::Installation
                ));
                return Ok(Self::make_response(
                    response,
                    update_check::Action::InstallPlanExecutionError,
                ));
            }

            // TODO: Report progress status (finishing)
            await!(self.report_success_event(
                apps,
                &request_params,
                EventType::UpdateDownloadFinished
            ));

            // TODO: Verify downloaded update if needed.

            await!(self.report_success_event(apps, &request_params, EventType::UpdateComplete));

            // TODO: Report progress status (update complete)
            Ok(Self::make_response(response, update_check::Action::Updated))
        }
    }

    /// Report an error event to Omaha.
    async fn report_error_event<'a>(
        &'a mut self,
        apps: &'a [App],
        request_params: &'a RequestParams,
        errorcode: EventErrorCode,
    ) {
        // TODO: Report ENCOUNTERED_ERROR status
        let event = Event {
            event_type: EventType::UpdateComplete,
            errorcode: Some(errorcode),
            ..Event::default()
        };
        await!(self.report_omaha_event(apps, &request_params, event));
    }

    /// Report a successful event to Omaha, for example download started, download finished, etc.
    async fn report_success_event<'a>(
        &'a mut self,
        apps: &'a [App],
        request_params: &'a RequestParams,
        event_type: EventType,
    ) {
        let event = Event { event_type, event_result: EventResult::Success, ..Event::default() };
        await!(self.report_omaha_event(apps, &request_params, event));
    }

    /// Report the given |event| to Omaha, errors occurred during reporting are logged but not
    /// acted on.
    async fn report_omaha_event<'a>(
        &'a mut self,
        apps: &'a [App],
        request_params: &'a RequestParams,
        event: Event,
    ) {
        let mut request_builder = RequestBuilder::new(&self.client_config, &request_params);
        for app in apps {
            request_builder = request_builder.add_event(app, &event);
        }
        if let Err(e) = await!(Self::do_omaha_request(&mut self.http, request_builder)) {
            warn!("Unable to report event to Omaha: {:?}", e);
        }
    }

    /// Make an http request to Omaha, and collect the response into an error or a blob of bytes
    /// that can be parsed.
    ///
    /// Given the http client and the request build, this makes the http request, and then coalesces
    /// the various errors into a single error type for easier error handling by the make process
    /// flow.
    ///
    /// This function also converts an HTTP error response into an Error, to divert those into the
    /// error handling paths instead of the Ok() path.
    async fn do_omaha_request<'a>(
        http: &'a mut HR,
        builder: RequestBuilder<'a>,
    ) -> Result<(Parts, Vec<u8>), OmahaRequestError> {
        let (parts, body) = await!(Self::make_request(http, builder.build()?))?;
        if !parts.status.is_success() {
            // Convert HTTP failure responses into Errors.
            Err(OmahaRequestError::HttpStatus(parts.status))
        } else {
            // Pass successful responses to the caller.
            info!("Omaha HTTP response: {}", parts.status);
            Ok((parts, body))
        }
    }

    /// Make an http request and collect the response body into a Vec of bytes.
    ///
    /// Specifically, this takes the body of the response and concatenates it into a single Vec of
    /// bytes so that any errors in receiving it can be captured immediately, instead of needing to
    /// handle them as part of parsing the response body.
    async fn make_request(
        http_client: &mut HR,
        request: http::Request<hyper::Body>,
    ) -> Result<(Parts, Vec<u8>), hyper::Error> {
        info!("Making http request to: {}", request.uri());
        let res = await!(http_client.request(request)).map_err(|err| {
            warn!("Unable to perform request: {}", err);
            err
        })?;

        let (parts, body) = res.into_parts();
        let data = await!(body.compat().try_concat())?;

        Ok((parts, data.to_vec()))
    }

    /// This method takes the response bytes from Omaha, and converts them into a protocol::Response
    /// struct, returning all of the various errors that can occur in that process as a consolidated
    /// error enum.
    fn parse_omaha_response(data: &[u8]) -> Result<Response, ResponseParseError> {
        parse_json_response(&data).map_err(ResponseParseError::Json)
    }

    /// Utility to extract pairs of app id => omaha status response, to make it easier to ask
    /// questions about the response.
    fn get_app_update_statuses(response: &Response) -> Vec<(&str, &OmahaStatus)> {
        response
            .apps
            .iter()
            .filter_map(|app| match &app.update_check {
                None => None,
                Some(u) => Some((app.id.as_str(), &u.status)),
            })
            .collect()
    }

    /// Utility to take a set of protocol::response::Apps and then construct a response from the
    /// update check based on those app IDs.
    ///
    /// TODO: Change the Policy and Installer to return a set of results, one for each app ID, then
    ///       make this match that.
    fn make_response(
        response: protocol::response::Response,
        action: update_check::Action,
    ) -> update_check::Response {
        update_check::Response {
            app_responses: response
                .apps
                .iter()
                .map(|app| update_check::AppResponse {
                    app_id: app.id.clone(),
                    cohort: app.cohort.clone(),
                    user_counting: response.daystart.clone().into(),
                    result: action.clone(),
                })
                .collect(),
            server_dictated_poll_interval: None,
        }
    }
}

#[cfg(test)]
pub mod tests {
    use super::update_check::*;
    use super::*;
    use crate::{
        common::{App, CheckOptions, ProtocolState, UpdateCheckSchedule, UserCounting},
        configuration::test_support::config_generator,
        http_request::mock::MockHttpRequest,
        installer::stub::StubInstaller,
        policy::{MockPolicyEngine, StubPolicyEngine},
        protocol::Cohort,
    };
    use futures::executor::{block_on, LocalPool};
    use futures::task::LocalSpawnExt;
    use log::info;
    use pretty_assertions::assert_eq;
    use serde_json::json;
    use std::cell::RefCell;
    use std::rc::Rc;
    use std::time::{Duration, SystemTime};

    async fn do_update_check<'a, PE, HR, IN, TM>(
        state_machine: &'a mut StateMachine<PE, HR, IN, TM>,
        apps: &'a [App],
    ) -> Result<update_check::Response, UpdateCheckError>
    where
        PE: PolicyEngine,
        HR: HttpRequest,
        IN: Installer,
        TM: Timer,
    {
        let options = CheckOptions::default();

        let context = update_check::Context {
            schedule: UpdateCheckSchedule {
                last_update_time: clock::now() - std::time::Duration::new(500, 0),
                next_update_time: clock::now(),
                next_update_window_start: clock::now(),
            },
            state: ProtocolState::default(),
        };

        await!(state_machine.perform_update_check(options, &apps, context))
    }

    fn make_test_apps() -> Vec<App> {
        vec![App::new(
            "{00000000-0000-0000-0000-000000000001}",
            [1, 2, 3, 4],
            Cohort::new("stable-channel"),
        )]
    }

    // Assert that the last request made to |http| is equal to the request built by
    // |request_builder|.
    async fn assert_request<'a>(http: MockHttpRequest, request_builder: RequestBuilder<'a>) {
        let body = await!(request_builder.build().unwrap().into_body().compat().try_concat())
            .unwrap()
            .to_vec();
        // Compare string instead of Vec<u8> for easier debugging.
        let body_str = String::from_utf8_lossy(&body);
        await!(http.assert_body_str(&body_str));
    }

    #[test]
    fn run_simple_check_with_noupdate_result() {
        block_on(async {
            let config = config_generator();
            let response = json!({"response":{
              "server": "prod",
              "protocol": "3.0",
              "app": [{
                "appid": "{00000000-0000-0000-0000-000000000001}",
                "status": "ok",
                "updatecheck": {
                  "status": "noupdate"
                }
              }]
            }});
            let response = serde_json::to_vec(&response).unwrap();
            let http = MockHttpRequest::new(hyper::Response::new(response.into()));

            let mut state_machine = StateMachine::new(
                StubPolicyEngine,
                http,
                StubInstaller::default(),
                &config,
                timer::MockTimer::new(),
            );
            let apps = make_test_apps();
            await!(do_update_check(&mut state_machine, &apps)).unwrap();

            info!("update check complete!");
        });
    }

    #[test]
    fn test_cohort_returned_with_noupdate_result() {
        block_on(async {
            let config = config_generator();
            let response = json!({"response":{
              "server": "prod",
              "protocol": "3.0",
              "app": [{
                "appid": "{00000000-0000-0000-0000-000000000001}",
                "status": "ok",
                "cohort": "1",
                "cohortname": "stable-channel",
                "updatecheck": {
                  "status": "noupdate"
                }
              }]
            }});
            let response = serde_json::to_vec(&response).unwrap();
            let http = MockHttpRequest::new(hyper::Response::new(response.into()));

            let mut state_machine = StateMachine::new(
                StubPolicyEngine,
                http,
                StubInstaller::default(),
                &config,
                timer::MockTimer::new(),
            );
            let apps = make_test_apps();
            let response = await!(do_update_check(&mut state_machine, &apps)).unwrap();
            assert_eq!("{00000000-0000-0000-0000-000000000001}", response.app_responses[0].app_id);
            assert_eq!(Some("1".into()), response.app_responses[0].cohort.id);
            assert_eq!(Some("stable-channel".into()), response.app_responses[0].cohort.name);
            assert_eq!(None, response.app_responses[0].cohort.hint);
        });
    }

    #[test]
    fn test_report_parse_response_error() {
        block_on(async {
            let config = config_generator();
            let mut http = MockHttpRequest::new(hyper::Response::new("invalid response".into()));
            http.add_response(hyper::Response::new("".into()));

            let mut state_machine = StateMachine::new(
                StubPolicyEngine,
                http,
                StubInstaller::default(),
                &config,
                timer::MockTimer::new(),
            );
            let apps = make_test_apps();
            match await!(do_update_check(&mut state_machine, &apps)) {
                Err(UpdateCheckError::ResponseParser(_)) => {} // expected
                result @ _ => {
                    panic!("Unexpected result from do_update_check: {:?}", result);
                }
            }

            let request_params = RequestParams::default();
            let mut request_builder = RequestBuilder::new(&config, &request_params);
            let event = Event {
                event_type: EventType::UpdateComplete,
                errorcode: Some(EventErrorCode::ParseResponse),
                ..Event::default()
            };
            request_builder = request_builder.add_event(&apps[0], &event);
            await!(assert_request(state_machine.http, request_builder));
        });
    }

    #[test]
    fn test_report_construct_install_plan_error() {
        block_on(async {
            let config = config_generator();
            let response = json!({"response":{
              "server": "prod",
              "protocol": "4.0",
              "app": [{
                "appid": "{00000000-0000-0000-0000-000000000001}",
                "status": "ok",
                "updatecheck": {
                  "status": "ok"
                }
              }],
            }});
            let response = serde_json::to_vec(&response).unwrap();
            let mut http = MockHttpRequest::new(hyper::Response::new(response.into()));
            http.add_response(hyper::Response::new("".into()));

            let mut state_machine = StateMachine::new(
                StubPolicyEngine,
                http,
                StubInstaller::default(),
                &config,
                timer::MockTimer::new(),
            );
            let apps = make_test_apps();
            match await!(do_update_check(&mut state_machine, &apps)) {
                Err(UpdateCheckError::InstallPlan(_)) => {} // expected
                result @ _ => {
                    panic!("Unexpected result from do_update_check: {:?}", result);
                }
            }

            let request_params = RequestParams::default();
            let mut request_builder = RequestBuilder::new(&config, &request_params);
            let event = Event {
                event_type: EventType::UpdateComplete,
                errorcode: Some(EventErrorCode::ConstructInstallPlan),
                ..Event::default()
            };
            request_builder = request_builder.add_event(&apps[0], &event);
            await!(assert_request(state_machine.http, request_builder));
        });
    }

    #[test]
    fn test_report_installation_error() {
        block_on(async {
            let config = config_generator();
            let response = json!({"response":{
              "server": "prod",
              "protocol": "3.0",
              "app": [{
                "appid": "{00000000-0000-0000-0000-000000000001}",
                "status": "ok",
                "updatecheck": {
                  "status": "ok"
                }
              }],
            }});
            let response = serde_json::to_vec(&response).unwrap();
            let mut http = MockHttpRequest::new(hyper::Response::new(response.into()));
            // For reporting UpdateDownloadStarted
            http.add_response(hyper::Response::new("".into()));
            // For reporting installation error
            http.add_response(hyper::Response::new("".into()));

            let mut state_machine = StateMachine::new(
                StubPolicyEngine,
                http,
                StubInstaller { should_fail: true },
                &config,
                timer::MockTimer::new(),
            );
            let apps = make_test_apps();
            let response = await!(do_update_check(&mut state_machine, &apps)).unwrap();
            assert_eq!(Action::InstallPlanExecutionError, response.app_responses[0].result);

            let request_params = RequestParams::default();
            let mut request_builder = RequestBuilder::new(&config, &request_params);
            let event = Event {
                event_type: EventType::UpdateComplete,
                errorcode: Some(EventErrorCode::Installation),
                ..Event::default()
            };
            request_builder = request_builder.add_event(&apps[0], &event);
            await!(assert_request(state_machine.http, request_builder));
        });
    }

    #[test]
    fn test_report_deferred_by_policy() {
        block_on(async {
            let config = config_generator();
            let response = json!({"response":{
              "server": "prod",
              "protocol": "3.0",
              "app": [{
                "appid": "{00000000-0000-0000-0000-000000000001}",
                "status": "ok",
                "updatecheck": {
                  "status": "ok"
                }
              }],
            }});
            let response = serde_json::to_vec(&response).unwrap();
            let mut http = MockHttpRequest::new(hyper::Response::new(response.into()));
            http.add_response(hyper::Response::new("".into()));

            let policy_engine = MockPolicyEngine {
                update_decision: UpdateDecision::DeferredByPolicy,
                ..MockPolicyEngine::default()
            };
            let mut state_machine = StateMachine::new(
                policy_engine,
                http,
                StubInstaller::default(),
                &config,
                timer::MockTimer::new(),
            );
            let apps = make_test_apps();
            let response = await!(do_update_check(&mut state_machine, &apps)).unwrap();
            assert_eq!(Action::DeferredByPolicy, response.app_responses[0].result);

            let request_params = RequestParams::default();
            let mut request_builder = RequestBuilder::new(&config, &request_params);
            let event = Event {
                event_type: EventType::UpdateComplete,
                event_result: EventResult::UpdateDeferred,
                ..Event::default()
            };
            request_builder = request_builder.add_event(&apps[0], &event);
            await!(assert_request(state_machine.http, request_builder));
        });
    }

    #[test]
    fn test_report_denied_by_policy() {
        block_on(async {
            let config = config_generator();
            let response = json!({"response":{
              "server": "prod",
              "protocol": "3.0",
              "app": [{
                "appid": "{00000000-0000-0000-0000-000000000001}",
                "status": "ok",
                "updatecheck": {
                  "status": "ok"
                }
              }],
            }});
            let response = serde_json::to_vec(&response).unwrap();
            let mut http = MockHttpRequest::new(hyper::Response::new(response.into()));
            http.add_response(hyper::Response::new("".into()));
            let policy_engine = MockPolicyEngine {
                update_decision: UpdateDecision::DeniedByPolicy,
                ..MockPolicyEngine::default()
            };
            let mut state_machine = StateMachine::new(
                policy_engine,
                http,
                StubInstaller::default(),
                &config,
                timer::MockTimer::new(),
            );
            let apps = make_test_apps();
            let response = await!(do_update_check(&mut state_machine, &apps)).unwrap();
            assert_eq!(Action::DeniedByPolicy, response.app_responses[0].result);

            let request_params = RequestParams::default();
            let mut request_builder = RequestBuilder::new(&config, &request_params);
            let event = Event {
                event_type: EventType::UpdateComplete,
                errorcode: Some(EventErrorCode::DeniedByPolicy),
                ..Event::default()
            };
            request_builder = request_builder.add_event(&apps[0], &event);
            await!(assert_request(state_machine.http, request_builder));
        });
    }

    #[test]
    fn test_wait_timer() {
        let config = config_generator();
        let mut http = MockHttpRequest::new(hyper::Response::new("".into()));
        http.add_response(hyper::Response::new("".into()));
        let mut timer = timer::MockTimer::new();
        timer.expect(Duration::from_secs(111));
        let next_update_time = clock::now() + Duration::from_secs(111);
        let policy_engine = MockPolicyEngine {
            check_schedule: UpdateCheckSchedule {
                last_update_time: SystemTime::UNIX_EPOCH,
                next_update_time,
                next_update_window_start: next_update_time,
            },
            ..MockPolicyEngine::default()
        };

        let mut state_machine =
            StateMachine::new(policy_engine, http, StubInstaller::default(), &config, timer);

        let mut pool = LocalPool::new();
        pool.spawner()
            .spawn_local(async move {
                await!(state_machine.start(make_test_apps()));
            })
            .unwrap();
        pool.run_until_stalled();
    }

    #[test]
    fn test_update_cohort() {
        let config = config_generator();
        let response = json!({"response":{
            "server": "prod",
            "protocol": "3.0",
            "app": [{
              "appid": "{00000000-0000-0000-0000-000000000001}",
              "status": "ok",
              "cohort": "1",
              "cohortname": "stable-channel",
              "updatecheck": {
                "status": "noupdate"
              }
            }]
        }});
        let response = serde_json::to_vec(&response).unwrap();
        let mut http = MockHttpRequest::new(hyper::Response::new(response.clone().into()));
        http.add_response(hyper::Response::new(response.into()));
        let mut timer = timer::MockTimer::new();
        // Run the update check twice and then block.
        timer.expect(Duration::from_secs(111));
        timer.expect(Duration::from_secs(111));
        let next_update_time = clock::now() + Duration::from_secs(111);
        let policy_engine = MockPolicyEngine {
            check_schedule: UpdateCheckSchedule {
                last_update_time: SystemTime::UNIX_EPOCH,
                next_update_time,
                next_update_window_start: next_update_time,
            },
            ..MockPolicyEngine::default()
        };

        let state_machine =
            StateMachine::new(policy_engine, http, StubInstaller::default(), &config, timer);
        let state_machine = Rc::new(RefCell::new(state_machine));

        {
            let state_machine = state_machine.clone();
            let mut pool = LocalPool::new();
            pool.spawner()
                .spawn_local(async move {
                    let mut state_machine = state_machine.borrow_mut();
                    await!(state_machine.start(make_test_apps()));
                })
                .unwrap();
            pool.run_until_stalled();
        }

        let request_params = RequestParams::default();
        let mut request_builder = RequestBuilder::new(&config, &request_params);
        let mut apps = make_test_apps();
        apps[0].cohort.id = Some("1".to_string());
        apps[0].cohort.name = Some("stable-channel".to_string());
        request_builder = request_builder.add_update_check(&apps[0]);
        // Move |state_machine| out of Rc and RefCell.
        let state_machine = Rc::try_unwrap(state_machine).unwrap().into_inner();
        block_on(assert_request(state_machine.http, request_builder));
    }

    #[test]
    fn test_user_counting_returned() {
        block_on(async {
            let config = config_generator();
            let response = json!({"response":{
            "server": "prod",
            "protocol": "3.0",
            "daystart": {
              "elapsed_days": 1234567,
              "elapsed_seconds": 3645
            },
            "app": [{
              "appid": "{00000000-0000-0000-0000-000000000001}",
              "status": "ok",
              "cohort": "1",
              "cohortname": "stable-channel",
              "updatecheck": {
                "status": "noupdate"
                  }
              }]
            }});
            let response = serde_json::to_vec(&response).unwrap();
            let http = MockHttpRequest::new(hyper::Response::new(response.into()));

            let mut state_machine = StateMachine::new(
                StubPolicyEngine,
                http,
                StubInstaller::default(),
                &config,
                timer::MockTimer::new(),
            );
            let apps = make_test_apps();
            let response = await!(do_update_check(&mut state_machine, &apps)).unwrap();

            assert_eq!(
                UserCounting::ClientRegulatedByDate(Some(1234567)),
                response.app_responses[0].user_counting
            );
        });
    }
}
