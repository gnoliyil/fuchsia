// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{anyhow, Context, Result};
use ffx_config::{self, ConfigLevel};
use fidl_fuchsia_developer_ffx_ext::{RepositorySpec, RepositoryTarget};
use percent_encoding::{percent_decode_str, percent_encode, AsciiSet, CONTROLS};
use serde_json::Value;
use std::collections::BTreeMap;

const CONFIG_KEY_REPOSITORIES: &str = "repository.repositories";
const CONFIG_KEY_REGISTRATIONS: &str = "repository.registrations";
const CONFIG_KEY_REGISTRATION_MODE: &str = "repository.registration-mode";
const CONFIG_KEY_DEFAULT_REPOSITORY: &str = "repository.default";
const CONFIG_KEY_SERVER_MODE: &str = "repository.server.mode";
const CONFIG_KEY_SERVER_ENABLED: &str = "repository.server.enabled";
const CONFIG_KEY_SERVER_LISTEN: &str = "repository.server.listen";
const CONFIG_KEY_LAST_USED_ADDRESS: &str = "repository.server.last_used_address";
const ESCAPE_SET: &AsciiSet = &CONTROLS.add(b'%').add(b'.');

// Try to figure out why the server is not running.
pub async fn determine_why_repository_server_is_not_running() -> anyhow::Error {
    macro_rules! check {
        ($e:expr) => {
            match $e {
                Ok(value) => value,
                Err(err) => {
                    return err;
                }
            }
        };
    }

    if !check!(get_repository_server_enabled().await) {
        return anyhow!(
            "Server is disabled. It can be started with:\n\
            $ ffx repository server start",
        );
    }

    match check!(repository_listen_addr().await) {
        Some(addr) => {
            return anyhow!(
                "ffx config detects repository.server.listen to be {} \
                Another process may be using that address. \
                Try shutting it down and restarting the \
                ffx daemon with:\n\
                $ ffx repository server start \n\
                Or alternatively specify at runtime \n\
                $ ffx repository server start --address <addr>",
                addr,
            );
        }
        None => {
            return anyhow!(
                "Server listening address is unspecified. You can fix this with:\n\
                $ ffx config set repository.server.listen '[::]:8083'\n\
                $ ffx repository server start\n\
                Or alternatively specify at runtime \n\
                $ ffx repository server start --address <port_number>",
            );
        }
    }
}

/// Return the repository server mode.
pub async fn repository_server_mode() -> Result<String> {
    if let Some(mode) = ffx_config::get(CONFIG_KEY_SERVER_MODE).await? {
        Ok(mode)
    } else {
        Ok(String::new())
    }
}

/// Return the repository registration mode.
pub async fn repository_registration_mode() -> Result<String> {
    if let Some(mode) = ffx_config::get(CONFIG_KEY_REGISTRATION_MODE).await? {
        Ok(mode)
    } else {
        // Default to FIDL
        Ok(String::from("fidl"))
    }
}

/// Return if the repository server is enabled.
pub async fn get_repository_server_enabled() -> Result<bool> {
    if let Some(enabled) = ffx_config::get(CONFIG_KEY_SERVER_ENABLED).await? {
        Ok(enabled)
    } else {
        Ok(false)
    }
}

/// Sets if the repository server is enabled.
pub async fn set_repository_server_enabled(enabled: bool) -> Result<()> {
    ffx_config::query(CONFIG_KEY_SERVER_ENABLED)
        .level(Some(ConfigLevel::User))
        .set(enabled.into())
        .await
}

/// Return if the last used repository address used.
pub async fn get_repository_server_last_address_used() -> Result<Option<std::net::SocketAddr>> {
    if let Some(address) =
        ffx_config::get::<Option<String>, _>(CONFIG_KEY_LAST_USED_ADDRESS).await?
    {
        if address.is_empty() {
            Ok(None)
        } else {
            Ok(Some(
                address
                    .parse::<std::net::SocketAddr>()
                    .with_context(|| format!("Parsing {}", CONFIG_KEY_LAST_USED_ADDRESS))?,
            ))
        }
    } else {
        Ok(None)
    }
}

/// Sets the repository server last used address.
pub async fn set_repository_server_last_address_used(socket_address: String) -> Result<()> {
    ffx_config::query(CONFIG_KEY_LAST_USED_ADDRESS)
        .level(Some(ConfigLevel::User))
        .set(socket_address.into())
        .await
}

/// Return the repository server address from ffx config.
pub async fn repository_listen_addr() -> Result<Option<std::net::SocketAddr>> {
    if let Some(address) = ffx_config::get::<Option<String>, _>(CONFIG_KEY_SERVER_LISTEN).await? {
        if address.is_empty() {
            Ok(None)
        } else {
            Ok(Some(
                address
                    .parse::<std::net::SocketAddr>()
                    .with_context(|| format!("Parsing {}", CONFIG_KEY_SERVER_LISTEN))?,
            ))
        }
    } else {
        Ok(None)
    }
}

fn repository_query(repo_name: &str) -> String {
    let repo_name = percent_encode(repo_name.as_bytes(), ESCAPE_SET);
    format!("{}.{}", CONFIG_KEY_REPOSITORIES, repo_name)
}

fn registration_query(repo_name: &str, target_identifier: &str) -> String {
    let repo_name = percent_encode(repo_name.as_bytes(), ESCAPE_SET);
    let target_identifier = percent_encode(target_identifier.as_bytes(), ESCAPE_SET);
    format!("{}.{}.{}", CONFIG_KEY_REGISTRATIONS, repo_name, target_identifier)
}

fn repository_registrations_query(repo_name: &str) -> String {
    let repo_name = percent_encode(repo_name.as_bytes(), ESCAPE_SET);
    format!("{}.{}", CONFIG_KEY_REGISTRATIONS, repo_name)
}

/// Return the default repository from the configuration if set.
pub async fn get_default_repository() -> Result<Option<String>> {
    let config_default: Option<String> = ffx_config::get(CONFIG_KEY_DEFAULT_REPOSITORY).await?;
    if config_default.is_some() {
        return Ok(config_default);
    } else {
        let repos = get_repositories().await;
        if repos.len() == 1 {
            return Ok(repos.keys().next().map(|s| s.clone()));
        } else {
            Ok(None)
        }
    }
}

/// Sets the default repository from the config.
pub async fn set_default_repository(repo_name: &str) -> Result<()> {
    ffx_config::query(CONFIG_KEY_DEFAULT_REPOSITORY)
        .level(Some(ConfigLevel::User))
        .set(repo_name.into())
        .await
}

/// Unsets the default repository from the config.
pub async fn unset_default_repository() -> Result<()> {
    ffx_config::query(CONFIG_KEY_DEFAULT_REPOSITORY).level(Some(ConfigLevel::User)).remove().await
}

/// Get repository spec from config.
pub async fn get_repository(repo_name: &str) -> Result<Option<RepositorySpec>> {
    if let Some(value) = ffx_config::get(&repository_query(repo_name)).await? {
        Ok(serde_json::from_value(value)?)
    } else {
        Ok(None)
    }
}

/// Read all the repositories from the config. This will log, but otherwise ignore invalid entries.
pub async fn get_repositories() -> BTreeMap<String, RepositorySpec> {
    let value = match ffx_config::get::<Option<Value>, _>(CONFIG_KEY_REPOSITORIES).await {
        Ok(Some(value)) => value,
        Ok(None) => {
            return BTreeMap::new();
        }
        Err(err) => {
            tracing::warn!("failed to load repositories: {:#?}", err);
            return BTreeMap::new();
        }
    };

    let entries = match value {
        Value::Object(entries) => entries,
        _ => {
            tracing::warn!("expected {} to be a map, not {}", CONFIG_KEY_REPOSITORIES, value);
            return BTreeMap::new();
        }
    };

    entries
        .into_iter()
        .filter_map(|(repo_name, entry)| {
            let repo_name = match percent_decode_str(&repo_name).decode_utf8() {
                Ok(repo_name) => repo_name.to_string(),
                Err(err) => {
                    tracing::warn!("failed to decode repo name {:?}: {:#?}", repo_name, err);
                    return None;
                }
            };

            // Parse the repository spec.
            match serde_json::from_value(entry) {
                Ok(repo_spec) => Some((repo_name, repo_spec)),
                Err(err) => {
                    tracing::warn!("failed to parse repository {:#?}: {:?}", repo_name, err);
                    None
                }
            }
        })
        .collect()
}

/// Writes the repository named `repo_name` to the configuration.
pub async fn set_repository(repo_name: &str, repo_spec: &RepositorySpec) -> Result<()> {
    let repo_spec = serde_json::to_value(repo_spec.clone())?;

    ffx_config::query(&repository_query(repo_name))
        .level(Some(ConfigLevel::User))
        .set(repo_spec)
        .await
}

/// Removes the repository named `repo_name` from the configuration.
pub async fn remove_repository(repo_name: &str) -> Result<()> {
    ffx_config::query(&repository_query(repo_name)).level(Some(ConfigLevel::User)).remove().await
}

/// Get the target registration from the config if exists.
pub async fn get_registration(
    repo_name: &str,
    target_identifier: &str,
) -> Result<Option<RepositoryTarget>> {
    if let Some(value) = ffx_config::get(&registration_query(repo_name, target_identifier)).await? {
        Ok(Some(serde_json::from_value(value)?))
    } else {
        Ok(None)
    }
}

pub async fn get_registrations() -> BTreeMap<String, BTreeMap<String, RepositoryTarget>> {
    let value = match ffx_config::get::<Option<Value>, _>(CONFIG_KEY_REGISTRATIONS).await {
        Ok(Some(value)) => value,
        Ok(None) => {
            return BTreeMap::new();
        }
        Err(err) => {
            tracing::warn!("failed to load registrations: {:#?}", err);
            return BTreeMap::new();
        }
    };

    let entries = match value {
        Value::Object(entries) => entries,
        _ => {
            tracing::warn!("expected {} to be a map, not {}", CONFIG_KEY_REGISTRATIONS, value);
            return BTreeMap::new();
        }
    };

    entries
        .into_iter()
        .filter_map(|(repo_name, targets)| {
            let repo_name = match percent_decode_str(&repo_name).decode_utf8() {
                Ok(repo_name) => repo_name.to_string(),
                Err(err) => {
                    tracing::warn!("unable to decode repo name {:?}: {:#?}", repo_name, err);
                    return None;
                }
            };

            let targets = parse_target_registrations(&repo_name, targets);
            Some((repo_name, targets))
        })
        .collect()
}

pub async fn get_repository_registrations(repo_name: &str) -> BTreeMap<String, RepositoryTarget> {
    let targets = match ffx_config::get(&repository_registrations_query(repo_name)).await {
        Ok(Some(targets)) => targets,
        Ok(None) => {
            return BTreeMap::new();
        }
        Err(err) => {
            tracing::warn!("failed to load repository registrations: {:?} {:#?}", repo_name, err);
            serde_json::Map::new().into()
        }
    };

    parse_target_registrations(repo_name, targets)
}

fn parse_target_registrations(
    repo_name: &str,
    targets: serde_json::Value,
) -> BTreeMap<String, RepositoryTarget> {
    let targets = match targets {
        Value::Object(targets) => targets,
        _ => {
            tracing::warn!("repository {:?} targets should be a map, not {:?}", repo_name, targets);
            return BTreeMap::new();
        }
    };

    targets
        .into_iter()
        .filter_map(|(target_nodename, target_info)| {
            let target_nodename = match percent_decode_str(&target_nodename).decode_utf8() {
                Ok(target_nodename) => target_nodename.to_string(),
                Err(err) => {
                    tracing::warn!(
                        "failed to decode target nodename: {:?}: {:#?}",
                        target_nodename,
                        err
                    );
                    return None;
                }
            };

            match serde_json::from_value(target_info) {
                Ok(target_info) => Some((target_nodename, target_info)),
                Err(err) => {
                    tracing::warn!("failed to parse registration {:?}: {:?}", target_nodename, err);
                    None
                }
            }
        })
        .collect()
}

pub async fn set_registration(target_nodename: &str, target_info: &RepositoryTarget) -> Result<()> {
    let json_target_info =
        serde_json::to_value(&target_info).context("serializing RepositorySpec")?;

    ffx_config::query(&registration_query(&target_info.repo_name, &target_nodename))
        .level(Some(ConfigLevel::User))
        .set(json_target_info)
        .await
}

pub async fn remove_registration(repo_name: &str, target_identifier: &str) -> Result<()> {
    ffx_config::query(&registration_query(repo_name, target_identifier))
        .level(Some(ConfigLevel::User))
        .remove()
        .await
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        maplit::btreemap,
        pretty_assertions::assert_eq,
        serde_json::json,
        std::{collections::BTreeSet, future::Future},
    };

    const CONFIG_KEY_ROOT: &str = "repository";

    // FIXME(http://fxbug.dev/80740): Unfortunately ffx_config is global, and so each of these tests
    // could step on each others ffx_config entries if run in parallel. To avoid this, we will:
    //
    // * use the `serial_test` crate to make sure each test runs sequentially
    // * clear out the config keys before we run each test to make sure state isn't leaked across
    //   tests.
    fn run_async_test<F: Future>(fut: F) -> F::Output {
        fuchsia_async::TestExecutor::new().run_singlethreaded(async move {
            let _env = ffx_config::test_init().await.unwrap();
            fut.await
        })
    }

    #[test]
    fn test_repository_query() {
        assert_eq!(repository_query(""), format!("{}.", CONFIG_KEY_REPOSITORIES));
        assert_eq!(repository_query("a-b"), format!("{}.a-b", CONFIG_KEY_REPOSITORIES));
        assert_eq!(repository_query("a.b"), format!("{}.a%2Eb", CONFIG_KEY_REPOSITORIES));
        assert_eq!(repository_query("a%b"), format!("{}.a%25b", CONFIG_KEY_REPOSITORIES));
    }

    #[test]
    fn test_registration_query() {
        assert_eq!(registration_query("", ""), format!("{}..", CONFIG_KEY_REGISTRATIONS));
        assert_eq!(
            registration_query("a-b", "c-d"),
            format!("{}.a-b.c-d", CONFIG_KEY_REGISTRATIONS)
        );
        assert_eq!(
            registration_query("a.b", "c.d"),
            format!("{}.a%2Eb.c%2Ed", CONFIG_KEY_REGISTRATIONS)
        );
        assert_eq!(
            registration_query("a%b", "c%d"),
            format!("{}.a%25b.c%25d", CONFIG_KEY_REGISTRATIONS)
        );
    }

    #[test]
    fn test_repository_registrations_query() {
        assert_eq!(repository_registrations_query(""), format!("{}.", CONFIG_KEY_REGISTRATIONS));
        assert_eq!(
            repository_registrations_query("a-b"),
            format!("{}.a-b", CONFIG_KEY_REGISTRATIONS)
        );
        assert_eq!(
            repository_registrations_query("a.b"),
            format!("{}.a%2Eb", CONFIG_KEY_REGISTRATIONS)
        );
        assert_eq!(
            repository_registrations_query("a%b"),
            format!("{}.a%25b", CONFIG_KEY_REGISTRATIONS)
        );
    }

    #[serial_test::serial]
    #[test]
    fn test_get_default_repository_with_only_one_configured() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_ROOT)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();

            // Initially there's no default.
            assert_eq!(get_default_repository().await.unwrap(), None);

            // Add the repository.
            let repository =
                RepositorySpec::Pm { path: "foo/bar/baz".into(), aliases: BTreeSet::new() };
            set_repository("repo", &repository).await.unwrap();

            // The single configured repo should be returned as the default
            assert_eq!(get_default_repository().await.unwrap(), Some(String::from("repo")));

            // Add a second repository.
            let repository =
                RepositorySpec::Pm { path: "foo/bar/baz2".into(), aliases: BTreeSet::new() };
            set_repository("repo2", &repository).await.unwrap();

            // The single configured repo should be returned as the default
            assert_eq!(get_default_repository().await.unwrap(), None);
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_set_unset_default_repository() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_ROOT)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();

            // Initially there's no default.
            assert_eq!(get_default_repository().await.unwrap(), None);

            // Setting the default should write to the config.
            set_default_repository("foo").await.unwrap();
            assert_eq!(
                ffx_config::get::<Value, _>(CONFIG_KEY_DEFAULT_REPOSITORY).await.unwrap(),
                json!("foo"),
            );
            assert_eq!(get_default_repository().await.unwrap(), Some("foo".into()));

            // We don't care if the repository has `.` in it.
            set_default_repository("foo.bar").await.unwrap();
            assert_eq!(
                ffx_config::get::<Value, _>(CONFIG_KEY_DEFAULT_REPOSITORY).await.unwrap(),
                json!("foo.bar"),
            );
            assert_eq!(get_default_repository().await.unwrap(), Some("foo.bar".into()));

            // Unset removes the default repository from the config.
            unset_default_repository().await.unwrap();
            assert_eq!(
                ffx_config::get::<Option<Value>, _>(CONFIG_KEY_DEFAULT_REPOSITORY).await.unwrap(),
                None,
            );
            assert_eq!(get_default_repository().await.unwrap(), None);

            // Unsetting the repo again returns an error.
            assert!(unset_default_repository().await.is_err());
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_set_remove_repository() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REPOSITORIES)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();

            // Initially the repositoy does not exist.
            assert_eq!(get_repository("repo").await.unwrap(), None);

            // Add the repository.
            let repository = RepositorySpec::Pm {
                path: "foo/bar/baz".into(),
                aliases: BTreeSet::from(["example.com".into(), "fuchsia.com".into()]),
            };
            set_repository("repo", &repository).await.unwrap();

            // Make sure we wrote to the config.
            assert_eq!(
                ffx_config::get::<Value, _>(CONFIG_KEY_REPOSITORIES).await.unwrap(),
                json!({
                    "repo": {
                        "type": "pm",
                        "path": "foo/bar/baz",
                        "aliases": ["example.com", "fuchsia.com"],
                    }
                }),
            );

            // Make sure we can get the repository.
            assert_eq!(get_repository("repo").await.unwrap(), Some(repository));

            // We can't get unknown repositories.
            assert_eq!(get_repository("unknown").await.unwrap(), None);

            // We can remove the repository.
            remove_repository("repo").await.unwrap();
            assert_eq!(
                ffx_config::get::<Option<Value>, _>(CONFIG_KEY_REPOSITORIES).await.unwrap(),
                None,
            );
            assert_eq!(get_repository("repo").await.unwrap(), None);
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_set_repository_encoding() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REPOSITORIES)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();

            set_repository(
                "repo.name",
                &RepositorySpec::Pm {
                    path: "foo/bar/baz".into(),
                    aliases: BTreeSet::from(["example.com".into(), "fuchsia.com".into()]),
                },
            )
            .await
            .unwrap();

            set_repository(
                "repo%name",
                &RepositorySpec::Pm { path: "foo/bar/baz".into(), aliases: BTreeSet::new() },
            )
            .await
            .unwrap();

            assert_eq!(
                ffx_config::get::<Value, _>(CONFIG_KEY_REPOSITORIES).await.unwrap(),
                json!({
                    "repo%2Ename": {
                        "type": "pm",
                        "path": "foo/bar/baz",
                        "aliases": ["example.com", "fuchsia.com"],
                    },
                    "repo%25name": {
                        "type": "pm",
                        "path": "foo/bar/baz",
                    }
                }),
            );
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_set_remove_registration() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();

            // Initially the registration does not exist.
            assert_eq!(get_registration("repo", "target").await.unwrap(), None);

            // Add the registration.
            let registration = RepositoryTarget {
                repo_name: "repo".into(),
                target_identifier: Some("target".into()),
                aliases: None,
                storage_type: None,
            };
            set_registration("target", &registration).await.unwrap();

            // Make sure it was written to the config.
            assert_eq!(
                ffx_config::get::<Value, _>(CONFIG_KEY_REGISTRATIONS).await.unwrap(),
                json!({
                    "repo": {
                        "target": {
                            "repo_name": "repo",
                            "target_identifier": "target",
                            "aliases": (),
                            "storage_type": (),
                        },
                    }
                }),
            );

            // Make sure we can get the registration.
            assert_eq!(get_registration("repo", "target").await.unwrap(), Some(registration));

            // We can't get unknown registrations.
            assert_eq!(get_registration("unkown", "target").await.unwrap(), None);
            assert_eq!(get_registration("repo", "unknown").await.unwrap(), None);

            // Remove the registration.
            remove_registration("repo", "target").await.unwrap();
            assert_eq!(get_registration("repo", "target").await.unwrap(), None);
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_set_registration_encoding() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();

            set_registration(
                "target.name",
                &RepositoryTarget {
                    repo_name: "repo.name".into(),
                    target_identifier: Some("target.name".into()),
                    aliases: None,
                    storage_type: None,
                },
            )
            .await
            .unwrap();

            set_registration(
                "target%name",
                &RepositoryTarget {
                    repo_name: "repo%name".into(),
                    target_identifier: Some("target%name".into()),
                    aliases: None,
                    storage_type: None,
                },
            )
            .await
            .unwrap();

            assert_eq!(
                ffx_config::get::<Value, _>(CONFIG_KEY_REGISTRATIONS).await.unwrap(),
                json!({
                    "repo%2Ename": {
                        "target%2Ename": {
                            "repo_name": "repo.name",
                            "target_identifier": "target.name",
                            "aliases": (),
                            "storage_type": (),
                        },
                    },
                    "repo%25name": {
                        "target%25name": {
                            "repo_name": "repo%name",
                            "target_identifier": "target%name",
                            "aliases": (),
                            "storage_type": (),
                        },
                    }
                }),
            );
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_repositories_empty() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REPOSITORIES)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();
            assert_eq!(get_repositories().await, btreemap! {});
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_repositories() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REPOSITORIES)
                .level(Some(ConfigLevel::User))
                .set(json!({
                    // Parse a normal repository.
                    "repo-name": {
                        "type": "pm",
                        "path": "foo/bar/baz",
                    },

                    // Parse encoded `repo.name`.
                    "repo%2Ename": {
                        "type": "pm",
                        "path": "foo/bar/baz",
                    },

                    // Parse encoded `repo%name`.
                    "repo%25name": {
                        "type": "pm",
                        "path": "foo/bar/baz",
                    },
                }))
                .await
                .unwrap();

            assert_eq!(
                get_repositories().await,
                btreemap! {
                    "repo-name".into() => RepositorySpec::Pm {
                        path: "foo/bar/baz".into(),
                        aliases: BTreeSet::new(),
                    },
                    "repo.name".into() => RepositorySpec::Pm {
                        path: "foo/bar/baz".into(),
                        aliases: BTreeSet::new(),
                    },
                    "repo%name".into() => RepositorySpec::Pm {
                        path: "foo/bar/baz".into(),
                        aliases: BTreeSet::new(),
                    },
                }
            );
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_repositories_ignores_invalid_entries() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REPOSITORIES)
                .level(Some(ConfigLevel::User))
                .set(json!({
                    // Ignores invalid repositories.
                    "invalid-entries": {},

                    // Ignores invalid encoded repository names.
                    "invalid%aaencoding": {
                        "type": "pm",
                        "path": "repo/bar/baz",
                    },
                }))
                .await
                .unwrap();

            assert_eq!(get_repositories().await, btreemap! {});
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_registrations_empty() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();
            assert_eq!(get_registrations().await, btreemap! {});
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_registrations() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({
                    "repo1": {
                        "target1": {
                            "repo_name": "repo1",
                            "target_identifier": "target1",
                            "storage_type": (),
                        },
                        "target2": {
                            "repo_name": "repo1",
                            "target_identifier": "target2",
                            "aliases": ["fuchsia.com"],
                            "storage_type": (),
                        },

                        // Ignores invalid targets.
                        "target3": {},
                    },

                    "repo2": {
                        "target1": {
                            "repo_name": "repo2",
                            "target_identifier": "target1",
                            "storage_type": (),
                        },
                        "target2": {
                            "repo_name": "repo2",
                            "target_identifier": "target2",
                            "storage_type": (),
                        },
                    },
                }))
                .await
                .unwrap();

            assert_eq!(
                get_registrations().await,
                btreemap! {
                    "repo1".into() => btreemap! {
                        "target1".into() => RepositoryTarget {
                            repo_name: "repo1".into(),
                            target_identifier: Some("target1".into()),
                            aliases: None,
                            storage_type: None,
                        },
                        "target2".into() => RepositoryTarget {
                            repo_name: "repo1".into(),
                            target_identifier: Some("target2".into()),
                            aliases: Some(BTreeSet::from(["fuchsia.com".into()])),
                            storage_type: None,
                        },
                    },
                    "repo2".into() => btreemap! {
                        "target1".into() => RepositoryTarget {
                            repo_name: "repo2".into(),
                            target_identifier: Some("target1".into()),
                            aliases: None,
                            storage_type: None,
                        },
                        "target2".into() => RepositoryTarget {
                            repo_name: "repo2".into(),
                            target_identifier: Some("target2".into()),
                            aliases: None,
                            storage_type: None,
                        },
                    },
                }
            );
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_registrations_encoding() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({
                    // Parse an encoded `repo.name`.
                    "repo%2Ename": {
                        // Parse an encoded `target.name`.
                        "target%2Ename": {
                            "repo_name": "repo.name",
                            "target_identifier": "target.name",
                            "aliases": (),
                            "storage_type": (),
                        },

                        // Parse an encoded `target%name`.
                        "target%25name": {
                            "repo_name": "repo.name",
                            "target_identifier": "target%name",
                            "aliases": (),
                            "storage_type": (),
                        },
                    },

                    // Parse encoded `foo%name`.
                    "repo%25name": {
                        "target-name": {
                            "repo_name": "repo%name",
                            "target_identifier": "target-name",
                            "aliases": (),
                            "storage_type": (),
                        },
                    },
                }))
                .await
                .unwrap();

            assert_eq!(
                get_registrations().await,
                btreemap! {
                    "repo.name".into() => btreemap! {
                        "target.name".into() => RepositoryTarget {
                            repo_name: "repo.name".into(),
                            target_identifier: Some("target.name".into()),
                            aliases: None,
                            storage_type: None,
                        },
                        "target%name".into() => RepositoryTarget {
                            repo_name: "repo.name".into(),
                            target_identifier: Some("target%name".into()),
                            aliases: None,
                            storage_type: None,
                        },
                    },
                    "repo%name".into() => btreemap! {
                        "target-name".into() => RepositoryTarget {
                            repo_name: "repo%name".into(),
                            target_identifier: Some("target-name".into()),
                            aliases: None,
                            storage_type: None,
                        },
                    },
                }
            );
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_registrations_invalid_entries() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({
                    // Ignores empty repositories.
                    "empty-entries": {},

                    "repo-name": {
                        // Ignores invalid targets.
                        "invalid-target": {},

                        // Ignores invalid encoded target.
                        "invalid%aaencoding": {
                            "repo_name": "repo-name",
                            "target_identifier": "target-name",
                            "aliases": [],
                            "storage_type": (),
                        },
                    },

                    // Ignores invalid encoded repository names.
                    "invalid%aarepo": {
                        "target-name": {
                            "repo_name": "repo-name",
                            "target_identifier": "target-name",
                            "aliases": [],
                            "storage_type": (),
                        },
                    },
                }))
                .await
                .unwrap();

            assert_eq!(
                get_registrations().await,
                btreemap! {
                    "repo-name".into() => btreemap! {},
                }
            );
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_repository_registrations_empty() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({}))
                .await
                .unwrap();
            assert_eq!(get_repository_registrations("repo").await, btreemap! {});
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_repository_registrations() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({
                    "repo": {
                        "target1": {
                            "repo_name": "repo1",
                            "target_identifier": "target1",
                            "aliases": ["fuchsia.com"],
                            "storage_type": (),
                        },
                        "target2": {
                            "repo_name": "repo1",
                            "target_identifier": "target2",
                            "aliases": (),
                            "storage_type": (),
                        },

                        // Ignores invalid targets.
                        "target3": {},
                    },
                }))
                .await
                .unwrap();

            assert_eq!(
                get_repository_registrations("repo").await,
                btreemap! {
                    "target1".into() => RepositoryTarget {
                        repo_name: "repo1".into(),
                        target_identifier: Some("target1".into()),
                        aliases: Some(BTreeSet::from(["fuchsia.com".into()])),
                        storage_type: None,
                    },
                    "target2".into() => RepositoryTarget {
                        repo_name: "repo1".into(),
                        target_identifier: Some("target2".into()),
                        aliases: None,
                        storage_type: None,
                    },
                },
            );

            // Getting an unknown repository gets nothing.
            assert_eq!(get_repository_registrations("unknown").await, btreemap! {});
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_repository_registrations_encoding() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({
                    // Parse an encoded `repo.name`.
                    "repo%2Ename": {
                        // Parse an encoded `target.name`.
                        "target%2Ename": {
                            "repo_name": "repo.name",
                            "target_identifier": "target.name",
                            "aliases": ["fuchsia.com"],
                            "storage_type": (),
                        },

                        // Parse an encoded `target%name`.
                        "target%25name": {
                            "repo_name": "repo.name",
                            "target_identifier": "target%name",
                            "aliases": (),
                            "storage_type": (),
                        },
                    },
                }))
                .await
                .unwrap();

            assert_eq!(
                get_repository_registrations("repo.name").await,
                btreemap! {
                    "target.name".into() => RepositoryTarget {
                        repo_name: "repo.name".into(),
                        target_identifier: Some("target.name".into()),
                        aliases: Some(BTreeSet::from(["fuchsia.com".into()])),
                        storage_type: None,
                    },
                    "target%name".into() => RepositoryTarget {
                        repo_name: "repo.name".into(),
                        target_identifier: Some("target%name".into()),
                        aliases: None,
                        storage_type: None,
                    },
                },
            );
        });
    }

    #[serial_test::serial]
    #[test]
    fn test_get_repository_registrations_invalid_entries() {
        run_async_test(async {
            ffx_config::query(CONFIG_KEY_REGISTRATIONS)
                .level(Some(ConfigLevel::User))
                .set(json!({
                    // Ignores empty repositories.
                    "empty-entries": {},

                    "repo-name": {
                        // Ignores invalid targets.
                        "invalid-target": {},

                        // Ignores invalid encoded target.
                        "invalid%aaencoding": {
                            "repo_name": "repo-name",
                            "target_identifier": "target-name",
                            "aliases": [],
                            "storage_type": (),
                        },
                    },

                    // Ignores invalid encoded repository names.
                    "invalid%aarepo": {
                        "target-name": {
                            "repo_name": "repo-name",
                            "target_identifier": "target-name",
                            "aliases": [],
                            "storage_type": (),
                        },
                    },
                }))
                .await
                .unwrap();

            assert_eq!(get_repository_registrations("empty-entries").await, btreemap! {});

            assert_eq!(get_repository_registrations("repo-name").await, btreemap! {});
        });
    }
}
