// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
mod tests;

use crate::protocol::Cohort;
use serde::Deserialize;
use serde_json::{Map, Value};

/// An Omaha protocol response.
///
/// This holds the data for a response from the Omaha service.
///
/// See https://github.com/google/omaha/blob/HEAD/doc/ServerProtocolV3.md#response
#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct Response {
    /// The current Omaha protocol version (which this is meant to be used with, is 3.0.  This
    /// should always be set to "3.0".
    ///
    /// This is the 'protocol' attribute of the response object.
    #[serde(rename = "protocol")]
    pub protocol_version: String,

    /// A string identifying the server or server family for diagnostic purposes.
    pub server: Option<String>,

    /// The server time at the time the request was received.
    pub daystart: Option<DayStart>,

    /// The applications to update.
    ///
    /// These are the 'app' children objects of the request object.
    #[serde(rename = "app")]
    pub apps: Vec<App>,
}

#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct DayStart {
    /// The number of calendar days that have elapsed since January 1st, 2007 in the server's
    /// locale, at the time the request was received.
    pub elapsed_days: Option<u32>,
    /// The number of seconds since the most recent midnight of the server's locale, at the time
    /// the request was received.
    pub elapsed_seconds: Option<u32>,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct App {
    #[serde(rename = "appid")]
    pub id: String,

    /// The state of the product on the server.
    pub status: OmahaStatus,

    /// This holds the following fields of the app object:
    ///   cohort
    ///   cohorthint
    ///   cohortname
    #[serde(flatten)]
    pub cohort: Cohort,

    /// Optional ping, used for user counting.
    pub ping: Option<Ping>,

    /// Information about the update.
    #[serde(rename = "updatecheck")]
    pub update_check: Option<UpdateCheck>,

    /// Any number of event status.
    #[serde(rename = "event")]
    pub events: Option<Vec<Event>>,

    /// Optional attributes Omaha sends.
    #[serde(flatten)]
    pub extra_attributes: Map<String, Value>,
}

impl App {
    pub fn get_manifest_version(&self) -> Option<String> {
        self.update_check.as_ref().and_then(|update_check| {
            update_check.manifest.as_ref().map(|manifest| manifest.version.clone())
        })
    }
}

#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq)]
#[serde(field_identifier, rename_all = "lowercase")]
pub enum OmahaStatus {
    #[default]
    Ok,
    /// The product is recognized, but due to policy restrictions the server must refuse to give a
    /// meaningful response.
    Restricted,
    /// No update is available for this client at this time.
    NoUpdate,
    Error(String),
}

#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct Ping {
    /// Should be "ok".
    status: OmahaStatus,
}

#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct Event {
    /// Should be "ok".
    pub status: OmahaStatus,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct UpdateCheck {
    /// Whether there's an update available.
    pub status: OmahaStatus,
    /// More information about the status.
    pub info: Option<String>,

    /// The base URL of all the packages in this app.
    pub urls: Option<URLs>,

    /// The manifest about the update.
    pub manifest: Option<Manifest>,

    /// Possibly contains whether urgent_update is specified or realm_id.
    #[serde(flatten)]
    pub extra_attributes: Map<String, Value>,
}

impl UpdateCheck {
    pub fn ok(urls: impl IntoIterator<Item = impl Into<String>>) -> Self {
        UpdateCheck {
            urls: Some(URLs::new(urls.into_iter().map(Into::into).collect())),
            ..UpdateCheck::default()
        }
    }

    pub fn no_update() -> Self {
        UpdateCheck { status: OmahaStatus::NoUpdate, ..UpdateCheck::default() }
    }

    /// Returns an iterator of all url codebases in this `updatecheck`.
    pub fn get_all_url_codebases(&self) -> impl Iterator<Item = &str> {
        self.urls.iter().flat_map(|urls| &urls.url).map(|url| url.codebase.as_str())
    }

    /// Returns an iterator of all packages in this `updatecheck`.
    pub fn get_all_packages(&self) -> impl Iterator<Item = &Package> {
        self.manifest.iter().flat_map(|m| &m.packages.package)
    }

    /// Returns an iterator of all full urls in this `updatecheck`.
    pub fn get_all_full_urls(&self) -> impl Iterator<Item = String> + '_ {
        self.get_all_url_codebases().flat_map(move |codebase| {
            self.get_all_packages().map(move |package| format!("{}{}", codebase, package.name))
        })
    }
}

/// Wrapper for a list of URL.
#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct URLs {
    pub url: Vec<URL>,
}

impl URLs {
    pub fn new(urls: Vec<String>) -> Self {
        URLs { url: urls.into_iter().map(|url| URL { codebase: url }).collect() }
    }
}

#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct URL {
    // The base URL of all the packages in this app.
    pub codebase: String,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct Manifest {
    pub version: String,

    pub actions: Actions,
    pub packages: Packages,
}

/// Wrapper for a list of Action.
#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct Actions {
    pub action: Vec<Action>,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct Action {
    /// The name of the event.
    pub event: Option<String>,

    /// The command to run.
    pub run: Option<String>,

    #[serde(flatten)]
    pub extra_attributes: Map<String, Value>,
}

/// Wrapper for a list of Package.
#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct Packages {
    pub package: Vec<Package>,
}

impl Packages {
    pub fn new(package: Vec<Package>) -> Self {
        Self { package }
    }
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct Package {
    /// Package name, append to the URL base to form a full URL.
    pub name: String,
    pub required: bool,
    pub size: Option<u64>,
    /// SHA1 of the package file encoded in base64.
    pub hash: Option<String>,
    /// SHA256 of the package file encoded in hex string.
    pub hash_sha256: Option<String>,

    /// The fingerprint of the package.
    #[serde(rename = "fp")]
    pub fingerprint: String,

    #[serde(flatten)]
    pub extra_attributes: Map<String, Value>,
}

impl Package {
    pub fn with_name(name: impl Into<String>) -> Self {
        Self { name: name.into(), ..Self::default() }
    }
}

/// Parse a slice of bytes into a Response object (stripping out the ResponseWrapper in the process)
pub fn parse_json_response(json: &[u8]) -> serde_json::Result<Response> {
    #[derive(Deserialize)]
    struct ResponseWrapper {
        response: Response,
    }

    let wrapper: ResponseWrapper = parse_safe_json(json)?;
    Ok(wrapper.response)
}

/// The returned JSON may use a strategy to mitigate against XSSI attacks by pre-pending the
/// following string to the actual, valid, JSON:
///
/// ")]}'\n"
///
/// This function detects this case and has serde parse the valid json instead.
fn parse_safe_json<'a, T>(raw: &'a [u8]) -> serde_json::Result<T>
where
    T: Deserialize<'a>,
{
    let safety_prefix = b")]}'\n";
    // if the raw data starts with the safety prefix, adjust the slice to parse to be after the
    // safety prefix.
    if raw.starts_with(safety_prefix) {
        serde_json::from_slice(&raw[safety_prefix.len()..])
    } else {
        serde_json::from_slice(raw)
    }
}
