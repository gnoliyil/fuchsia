// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::format_err,
    assert_matches::assert_matches,
    fidl_fuchsia_pkg_ext::RepositoryConfigBuilder,
    fidl_fuchsia_pkg_rewrite_ext::{Rule, RuleConfig},
    fuchsia_inspect::{
        assert_data_tree,
        reader::Property,
        testing::{AnyProperty, PropertyAssertion},
        tree_assertion,
    },
    fuchsia_pkg_testing::{serve::responder, PackageBuilder, RepositoryBuilder},
    futures::FutureExt as _,
    lib::MountsBuilder,
    lib::{TestEnvBuilder, EMPTY_REPO_PATH},
    std::sync::Arc,
};

#[fuchsia::test]
async fn initial_inspect_state() {
    let env = TestEnvBuilder::new().build().await;
    // Wait for inspect to be created
    env.wait_for_pkg_resolver_to_start().await;

    // Obtain inspect hierarchy
    let hierarchy = env.pkg_resolver_inspect_hierarchy().await;

    assert_data_tree!(
        hierarchy,
        root: {
            rewrite_manager: {
                dynamic_rules: {},
                dynamic_rules_path: format!("{:?}", Some(std::path::Path::new("rewrites.json"))),
                static_rules: {},
                generation: 0u64,
            },
            omaha_channel: {
                tuf_config_name: OptionDebugStringProperty::<String>::None,
                source: OptionDebugStringProperty::<String>::None,
            },
            repository_manager: {
                dynamic_configs_path: format!("{:?}", Some(std::path::Path::new("repositories.json"))),
                dynamic_configs: {},
                static_configs: {},
                persisted_repos_dir: "None".to_string(),
                repos: {},
                stats: {
                    mirrors: {}
                },
                tuf_metadata_timeout_seconds: 240u64,
            },
            resolver_service: {
                cache_fallbacks_due_to_not_found: 0u64,
                active_package_resolves: {}
            },
            blob_fetcher: {
                blob_header_timeout_seconds: 30u64,
                blob_body_timeout_seconds: 30u64,
                blob_download_resumption_attempts_limit: 50u64,
                blob_type: "Uncompressed",
                delivery_blob_fallback: false,
                queue: {},
            },
            structured_config: contains {},
        }
    );
    env.stop().await;
}

#[fuchsia::test]
async fn adding_repo_updates_inspect_state() {
    let env = TestEnvBuilder::new().build().await;
    let config = RepositoryConfigBuilder::new("fuchsia-pkg://example.com".parse().unwrap()).build();
    let () = env.proxies.repo_manager.add(config.clone().into()).await.unwrap().unwrap();

    // Obtain inspect service and convert into a node hierarchy.
    let hierarchy = env.pkg_resolver_inspect_hierarchy().await;

    assert_data_tree!(
        hierarchy,
        root: contains {
            repository_manager: contains {
                dynamic_configs: {
                    "example.com": {
                        root_keys: {},
                        mirrors: {},
                    },
                },
            },
        }
    );
    env.stop().await;
}

#[fuchsia::test]
async fn resolving_package_updates_inspect_state() {
    let env = TestEnvBuilder::new().build().await;

    let pkg = PackageBuilder::new("just_meta_far").build().await.expect("created pkg");
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );
    let served_repository = repo.server().start().unwrap();
    let repo_url = "fuchsia-pkg://example.com".parse().unwrap();
    let config = served_repository.make_repo_config(repo_url);
    let () = env.proxies.repo_manager.add(config.clone().into()).await.unwrap().unwrap();

    env.resolve_package("fuchsia-pkg://example.com/just_meta_far")
        .await
        .expect("package to resolve");

    assert_data_tree!(
        env.pkg_resolver_inspect_hierarchy().await,
        root: contains {
            repository_manager: contains {
                dynamic_configs: {
                    "example.com": {
                        root_keys: {
                          "0": format!("{:?}", config.root_keys()[0])
                        },
                        mirrors: {
                          "0": {
                            mirror_url: format!("{:?}", config.mirrors()[0].mirror_url()),
                            subscribe: format!("{:?}", config.mirrors()[0].subscribe()),
                            blob_mirror_url: format!("{:?}", config.mirrors()[0].blob_mirror_url())
                          }
                        },
                    }
                },
                repos: {
                    "example.com": {
                        merkles_successfully_resolved_count: 1u64,
                        last_merkle_successfully_resolved_time: OptionDebugStringProperty::Some(
                            AnyProperty
                        ),
                        "updating_tuf_client": {
                            update_check_success_count: 1u64,
                            update_check_failure_count: 0u64,
                            last_update_successfully_checked_time: OptionDebugStringProperty::Some(
                                AnyProperty
                            ),
                            updated_count: 1u64,
                            root_version: 1u64,
                            timestamp_version: 2u64,
                            snapshot_version: 2u64,
                            targets_version: 2u64,
                        }
                    },
                },
                stats: {
                    mirrors: {
                        format!("{}/blobs", served_repository.local_url()) => {
                            network_blips: 0u64,
                            network_rate_limits: 0u64,
                        },
                    },
                },
            },
        }
    );
    env.stop().await;
}

#[fuchsia::test]
async fn package_and_blob_queues() {
    // active_package_resolves exports the original and rewritten pkg urls, so we use a rewrite rule
    // and verify that the two exported urls are different.
    let env = TestEnvBuilder::new()
        .mounts(
            MountsBuilder::new()
                .dynamic_rewrite_rules(RuleConfig::Version1(vec![Rule::new(
                    "original.example.com",
                    "rewritten.example.com",
                    "/",
                    "/",
                )
                .unwrap()]))
                .enable_dynamic_config(lib::EnableDynamicConfig {
                    enable_dynamic_configuration: true,
                })
                .build(),
        )
        .build()
        .await;

    let pkg = PackageBuilder::new("just_meta_far").build().await.expect("created pkg");
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );

    let meta_far_blob_path = format!("/blobs/{}", pkg.meta_far_merkle_root());

    let flake_first_attempt = responder::ForPath::new(
        meta_far_blob_path.clone(),
        responder::OverrideNth::new(1, responder::StaticResponseCode::server_error()),
    );

    let (blocking_responder, unblocking_closure_receiver) = responder::BlockResponseBodyOnce::new();
    let block_second_attempt = responder::ForPath::new(
        meta_far_blob_path.clone(),
        responder::OverrideNth::new(2, blocking_responder),
    );

    let served_repository = repo
        .server()
        .response_overrider(flake_first_attempt)
        .response_overrider(block_second_attempt)
        .start()
        .unwrap();
    let () = env
        .proxies
        .repo_manager
        .add(
            served_repository
                .make_repo_config("fuchsia-pkg://rewritten.example.com".parse().unwrap())
                .into(),
        )
        .await
        .unwrap()
        .unwrap();

    let resolve_fut =
        env.resolve_package("fuchsia-pkg://original.example.com/just_meta_far").boxed_local();
    let unblocker = unblocking_closure_receiver.await.unwrap();

    let inspect_state_fut = env
        .wait_for_pkg_resolver_inspect_state(tree_assertion!(
            root: contains {
                blob_fetcher: contains {
                    queue: contains {
                        pkg.meta_far_merkle_root().to_string() => contains {
                            attempts: {
                                "2": contains {
                                    state: "read http body"
                                }
                            }
                        }
                    }
                }
            }
        ))
        .boxed_local();

    // The resolve should not fail, but if it does because of a bug somewhere, awaiting the
    // inspect future would probably hang, and we want the test to fail instead of hang so that
    // the test stdout is printed.
    let resolve_fut = match futures::future::select(resolve_fut, inspect_state_fut).await {
        futures::future::Either::Left((resolve_res, _)) => {
            panic!("the resolve should not have completed: {:?}", resolve_res);
        }
        futures::future::Either::Right(((), resolve_fut)) => resolve_fut,
    };

    assert_data_tree!(
        env.pkg_resolver_inspect_hierarchy().await,
        root: contains {
            blob_fetcher: contains {
                queue: {
                    pkg.meta_far_merkle_root().to_string() => {
                        fetch_ts: AnyProperty,
                        source: "http",
                        mirror: format!("{}{}", served_repository.local_url(), meta_far_blob_path),
                        attempts: {
                            "2": {
                                state: "read http body",
                                state_ts: AnyProperty,
                                expected_size_bytes: 12288u64,
                                bytes_written: 0u64,
                            }
                        }
                    }
                }
            },
            resolver_service: contains {
                active_package_resolves: {
                    "fuchsia-pkg://original.example.com/just_meta_far": {
                        resolve_ts: AnyProperty,
                        rewritten_url: "fuchsia-pkg://rewritten.example.com/just_meta_far",
                    }
                }
            }
        }
    );

    // After completing the resolve there should be no active blob fetches.
    unblocker();
    let _pkg = resolve_fut.await.unwrap();

    assert_data_tree!(
        env.pkg_resolver_inspect_hierarchy().await,
        root: contains {
            blob_fetcher: contains {
                queue: {}
            }
        }
    );

    env.stop().await;
}

#[fuchsia::test]
async fn channel_in_vbmeta_appears_in_inspect_state() {
    let repo =
        Arc::new(RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH).build().await.unwrap());
    let served_repository = repo.server().start().unwrap();
    let repo_url = "fuchsia-pkg://test-repo".parse().unwrap();
    let config = served_repository.make_repo_config(repo_url);
    let env = TestEnvBuilder::new()
        .tuf_repo_config_boot_arg("test-repo".to_string())
        .create_ota_channel_rewrite_rule(true)
        .mounts(lib::MountsBuilder::new().static_repository(config.clone()).build())
        .build()
        .await;
    env.wait_for_pkg_resolver_to_start().await;

    let hierarchy = env.pkg_resolver_inspect_hierarchy().await;

    assert_data_tree!(
        hierarchy,
        root: contains {
            rewrite_manager: {
                dynamic_rules: {
                   "0": {
                        path_prefix_replacement: "/",
                        host_match: "fuchsia.com",
                        path_prefix_match: "/",
                        host_replacement: "test-repo",
                    }
                },
                dynamic_rules_path: "None",
                static_rules: {},
                generation: 0u64,
            },
            omaha_channel: {
                tuf_config_name: "test-repo",
                source: "Some(VbMeta)"
            },
        }
    );

    env.stop().await;
}

enum OptionDebugStringProperty<I> {
    Some(I),
    None,
}

impl<I> PropertyAssertion for OptionDebugStringProperty<I>
where
    I: PropertyAssertion,
{
    fn run(&self, actual: &Property) -> Result<(), anyhow::Error> {
        match actual {
            Property::String(name, value) => match self {
                OptionDebugStringProperty::Some(inner) => {
                    const PREFIX: &str = "Some(";
                    const SUFFIX: &str = ")";
                    if !value.starts_with(PREFIX) {
                        return Err(format_err!(
                            r#"expected property to be "Some(...", actual {:?}"#,
                            actual
                        ));
                    }
                    if !value.ends_with(SUFFIX) {
                        return Err(format_err!(
                            r#"expected property to be "...)", actual {:?}"#,
                            actual
                        ));
                    }
                    let inner_value = &value[PREFIX.len()..(value.len() - SUFFIX.len())];
                    inner.run(&Property::String(name.clone(), inner_value.to_owned()))
                }
                OptionDebugStringProperty::None => {
                    if value != "None" {
                        return Err(format_err!(
                            r#"expected property string to be "None", got {:?}"#,
                            actual
                        ));
                    }
                    Ok(())
                }
            },
            _wrong_type => Err(format_err!("expected string property, got {:?}", actual)),
        }
    }
}

#[fuchsia::test]
fn option_debug_string_property() {
    fn make_string_property(value: &str) -> Property {
        Property::String("name".to_owned(), value.to_owned())
    }

    fn dbg<D: std::fmt::Debug>(d: D) -> String {
        format!("{:?}", d)
    }

    // trivial ok
    assert_matches!(
        OptionDebugStringProperty::Some(AnyProperty).run(&make_string_property("Some()")),
        Ok(())
    );
    assert_matches!(
        OptionDebugStringProperty::<AnyProperty>::None.run(&make_string_property("None")),
        Ok(())
    );

    // trivial err
    assert_matches!(
        OptionDebugStringProperty::Some(AnyProperty).run(&make_string_property("None")),
        Err(_)
    );
    assert_matches!(
        OptionDebugStringProperty::Some(AnyProperty).run(&make_string_property("Some(foo")),
        Err(_)
    );
    assert_matches!(
        OptionDebugStringProperty::<AnyProperty>::None.run(&make_string_property("Some()")),
        Err(_)
    );

    // non-empty inner ok
    assert_matches!(
        OptionDebugStringProperty::Some(AnyProperty).run(&make_string_property("Some(value)")),
        Ok(())
    );
    assert_matches!(
        OptionDebugStringProperty::Some(dbg("string"))
            .run(&make_string_property("Some(\"string\")")),
        Ok(())
    );

    // non-empty inner err
    assert_matches!(
        OptionDebugStringProperty::Some(dbg("a")).run(&make_string_property("Some(\"b\")")),
        Err(_)
    );
}
