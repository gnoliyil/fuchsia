// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    assert_matches::assert_matches,
    cm_moniker::InstancedAbsoluteMoniker,
    cm_rust::{
        CapabilityName, CapabilityTypeName, ProtocolDecl, StorageDecl, StorageDirectorySource,
    },
    fidl_fuchsia_component_decl as fdecl,
    moniker::{AbsoluteMoniker, ExtendedMoniker},
    routing::{
        capability_source::{CapabilitySourceInterface, ComponentCapability, InternalCapability},
        component_instance::ComponentInstanceInterface,
        config::{
            AllowlistEntry, AllowlistEntryBuilder, CapabilityAllowlistKey,
            CapabilityAllowlistSource, ChildPolicyAllowlists, DebugCapabilityAllowlistEntry,
            DebugCapabilityKey, JobPolicyAllowlists, RuntimeConfig, SecurityPolicy,
        },
        policy::GlobalPolicyChecker,
    },
    std::{
        collections::{HashMap, HashSet},
        iter::FromIterator,
        sync::{Arc, Weak},
    },
};

/// These GlobalPolicyChecker tests are run under multiple contexts, e.g. both on Fuchsia under
/// component_manager and on the build host under cm_fidl_analyzer. This macro helps ensure that all
/// tests are run in each context.
#[macro_export]
macro_rules! instantiate_global_policy_checker_tests {
    ($fixture_impl:path) => {
        // New GlobalPolicyCheckerTest tests must be added to this list to run.
        instantiate_global_policy_checker_tests! {
            $fixture_impl,
            global_policy_checker_can_route_capability_framework_cap,
            global_policy_checker_can_route_capability_namespace_cap,
            global_policy_checker_can_route_capability_component_cap,
            global_policy_checker_can_route_capability_capability_cap,
            global_policy_checker_can_route_debug_capability_capability_cap,
            global_policy_checker_can_route_debug_capability_with_realm_allowlist_entry,
            global_policy_checker_can_route_debug_capability_with_collection_allowlist_entry,
            global_policy_checker_can_route_capability_builtin_cap,
            global_policy_checker_can_route_capability_with_realm_allowlist_entry,
            global_policy_checker_can_route_capability_with_collection_allowlist_entry,
        }
    };
    ($fixture_impl:path, $test:ident, $($remaining:ident),+ $(,)?) => {
        instantiate_global_policy_checker_tests! { $fixture_impl, $test }
        instantiate_global_policy_checker_tests! { $fixture_impl, $($remaining),+ }
    };
    ($fixture_impl:path, $test:ident) => {
        #[test]
        fn $test() -> Result<(), Error> {
            <$fixture_impl as Default>::default().$test()
        }
    };
}

// Tests `GlobalPolicyChecker` for implementations of `ComponentInstanceInterface`.
pub trait GlobalPolicyCheckerTest<C>
where
    C: ComponentInstanceInterface,
{
    // Creates a `ComponentInstanceInterface` with the given `InstancedAbsoluteMoniker`.
    fn make_component(&self, instanced_moniker: InstancedAbsoluteMoniker) -> Arc<C>;

    // Tests `GlobalPolicyChecker::can_route_capability()` for framework capability sources.
    fn global_policy_checker_can_route_capability_framework_cap(&self) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_capability_policy(
            CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentInstance(AbsoluteMoniker::from(vec![
                    "foo", "bar",
                ])),
                source_name: CapabilityName::from("running"),
                source: CapabilityAllowlistSource::Framework,
                capability: CapabilityTypeName::Event,
            },
            vec![
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "foo", "bar",
                ])),
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "foo", "bar", "baz",
                ])),
            ],
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());
        let component = self.make_component(vec!["foo:0", "bar:0"].into());

        let event_capability = CapabilitySourceInterface::<C>::Framework {
            capability: InternalCapability::Event(CapabilityName::from("running")),
            component: component.as_weak(),
        };
        let valid_path_0 = AbsoluteMoniker::from(vec!["foo", "bar"]);
        let valid_path_1 = AbsoluteMoniker::from(vec!["foo", "bar", "baz"]);
        let invalid_path_0 = AbsoluteMoniker::from(vec!["foobar"]);
        let invalid_path_1 = AbsoluteMoniker::from(vec!["foo", "bar", "foobar"]);

        assert_matches!(
            global_policy_checker.can_route_capability(&event_capability, &valid_path_0),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&event_capability, &valid_path_1),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&event_capability, &invalid_path_0),
            Err(_)
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&event_capability, &invalid_path_1),
            Err(_)
        );
        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_capability()` for namespace capability sources.
    fn global_policy_checker_can_route_capability_namespace_cap(&self) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_capability_policy(
            CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentManager,
                source_name: CapabilityName::from("fuchsia.kernel.RootResource"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
            },
            vec![
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root",
                ])),
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root",
                    "bootstrap",
                ])),
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root", "core",
                ])),
            ],
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());

        let protocol_capability = CapabilitySourceInterface::<C>::Namespace {
            capability: ComponentCapability::Protocol(ProtocolDecl {
                name: "fuchsia.kernel.RootResource".into(),
                source_path: Some("/svc/fuchsia.kernel.RootResource".parse().unwrap()),
            }),
            top_instance: Weak::new(),
        };
        let valid_path_0 = AbsoluteMoniker::from(vec!["root"]);
        let valid_path_1 = AbsoluteMoniker::from(vec!["root", "bootstrap"]);
        let valid_path_2 = AbsoluteMoniker::from(vec!["root", "core"]);
        let invalid_path_0 = AbsoluteMoniker::from(vec!["foobar"]);
        let invalid_path_1 = AbsoluteMoniker::from(vec!["foo", "bar", "foobar"]);

        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &valid_path_0),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &valid_path_1),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &valid_path_2),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &invalid_path_0),
            Err(_)
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &invalid_path_1),
            Err(_)
        );
        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_capability()` for component capability sources.
    fn global_policy_checker_can_route_capability_component_cap(&self) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_capability_policy(
            CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentInstance(AbsoluteMoniker::from(vec![
                    "foo",
                ])),
                source_name: CapabilityName::from("fuchsia.foo.FooBar"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
            },
            vec![
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "foo",
                ])),
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root",
                    "bootstrap",
                ])),
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root", "core",
                ])),
            ],
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());
        let component = self.make_component(vec!["foo:0"].into());

        let protocol_capability = CapabilitySourceInterface::<C>::Component {
            capability: ComponentCapability::Protocol(ProtocolDecl {
                name: "fuchsia.foo.FooBar".into(),
                source_path: Some("/svc/fuchsia.foo.FooBar".parse().unwrap()),
            }),
            component: component.as_weak(),
        };
        let valid_path_0 = AbsoluteMoniker::from(vec!["root", "bootstrap"]);
        let valid_path_1 = AbsoluteMoniker::from(vec!["root", "core"]);
        let invalid_path_0 = AbsoluteMoniker::from(vec!["foobar"]);
        let invalid_path_1 = AbsoluteMoniker::from(vec!["foo", "bar", "foobar"]);

        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &valid_path_0),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &valid_path_1),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &invalid_path_0),
            Err(_)
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &invalid_path_1),
            Err(_)
        );
        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_capability()` for capability sources of type `Capability`.
    fn global_policy_checker_can_route_capability_capability_cap(&self) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_capability_policy(
            CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentInstance(AbsoluteMoniker::from(vec![
                    "foo",
                ])),
                source_name: CapabilityName::from("cache"),
                source: CapabilityAllowlistSource::Capability,
                capability: CapabilityTypeName::Storage,
            },
            vec![
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "foo",
                ])),
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root",
                    "bootstrap",
                ])),
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root", "core",
                ])),
            ],
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());
        let component = self.make_component(vec!["foo:0"].into());

        let protocol_capability = CapabilitySourceInterface::<C>::Capability {
            source_capability: ComponentCapability::Storage(StorageDecl {
                backing_dir: "/cache".into(),
                name: "cache".into(),
                source: StorageDirectorySource::Parent,
                subdir: None,
                storage_id: fdecl::StorageId::StaticInstanceIdOrMoniker,
            }),
            component: component.as_weak(),
        };
        let valid_path_0 = AbsoluteMoniker::from(vec!["root", "bootstrap"]);
        let valid_path_1 = AbsoluteMoniker::from(vec!["root", "core"]);
        let invalid_path_0 = AbsoluteMoniker::from(vec!["foobar"]);
        let invalid_path_1 = AbsoluteMoniker::from(vec!["foo", "bar", "foobar"]);

        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &valid_path_0),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &valid_path_1),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &invalid_path_0),
            Err(_)
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&protocol_capability, &invalid_path_1),
            Err(_)
        );
        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_debug_capability()` for capability sources of type `Capability`.
    fn global_policy_checker_can_route_debug_capability_capability_cap(&self) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_debug_capability_policy(
            DebugCapabilityKey {
                source_name: CapabilityName::from("debug_service1"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
                env_name: "foo_env".to_string(),
            },
            AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec!["foo"])),
            AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec!["foo"])),
        );
        config_builder.add_debug_capability_policy(
            DebugCapabilityKey {
                source_name: CapabilityName::from("debug_service1"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
                env_name: "bootstrap_env".to_string(),
            },
            AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec!["foo"])),
            AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                "root",
                "bootstrap",
            ])),
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());
        let component = self.make_component(vec!["foo:0"].into());

        let protocol_capability = CapabilitySourceInterface::<C>::Component {
            capability: ComponentCapability::Protocol(ProtocolDecl {
                name: "debug_service1".into(),
                source_path: Some("/svc/debug_service1".parse().unwrap()),
            }),
            component: component.as_weak(),
        };

        let valid_cases = vec![
            (AbsoluteMoniker::from(vec!["root", "bootstrap"]), "bootstrap_env".to_string()),
            (AbsoluteMoniker::from(vec!["foo"]), "foo_env".to_string()),
        ];

        let invalid_cases = vec![
            (AbsoluteMoniker::from(vec!["foobar"]), "foobar_env".to_string()),
            (AbsoluteMoniker::from(vec!["foo", "bar", "foobar"]), "foobar_env".to_string()),
            (AbsoluteMoniker::from(vec!["root", "bootstrap"]), "foo_env".to_string()),
            (AbsoluteMoniker::from(vec!["root", "baz"]), "foo_env".to_string()),
        ];

        let target_moniker = AbsoluteMoniker::from(vec!["target"]);

        for valid_case in valid_cases {
            assert_matches!(
                global_policy_checker.can_route_debug_capability(
                    &protocol_capability,
                    &valid_case.0,
                    &valid_case.1,
                    &target_moniker
                ),
                Ok(()),
                "{:?}",
                valid_case
            );
        }

        for invalid_case in invalid_cases {
            assert_matches!(
                global_policy_checker.can_route_debug_capability(
                    &protocol_capability,
                    &invalid_case.0,
                    &invalid_case.1,
                    &target_moniker
                ),
                Err(_),
                "{:?}",
                invalid_case
            );
        }

        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_debug_capability()` for capability sources of type
    // `Capability` with realm allowlist entries.
    fn global_policy_checker_can_route_debug_capability_with_realm_allowlist_entry(
        &self,
    ) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_debug_capability_policy(
            DebugCapabilityKey {
                source_name: CapabilityName::from("debug_service1"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
                env_name: "bar_env".to_string(),
            },
            AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec!["bar"])),
            AllowlistEntryBuilder::new()
                .exact_from_moniker(&AbsoluteMoniker::from(vec!["root", "bootstrap1"]))
                .any_descendant(),
        );
        config_builder.add_debug_capability_policy(
            DebugCapabilityKey {
                source_name: CapabilityName::from("debug_service1"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
                env_name: "foo_env".to_string(),
            },
            AllowlistEntryBuilder::new()
                .exact_from_moniker(&AbsoluteMoniker::from(vec!["foo"]))
                .any_descendant(),
            AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                "root",
                "bootstrap2",
            ])),
        );
        config_builder.add_debug_capability_policy(
            DebugCapabilityKey {
                source_name: CapabilityName::from("debug_service1"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
                env_name: "baz_env".to_string(),
            },
            AllowlistEntryBuilder::new()
                .exact_from_moniker(&AbsoluteMoniker::from(vec!["baz"]))
                .any_descendant(),
            AllowlistEntryBuilder::new()
                .exact_from_moniker(&AbsoluteMoniker::from(vec!["root", "bootstrap3"]))
                .any_descendant(),
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());

        // source, dest, env
        let valid_cases = vec![
            (vec!["bar:0"], vec!["root", "bootstrap1", "child"], "bar_env".to_string()),
            (
                vec!["bar:0"],
                vec!["root", "bootstrap1", "child", "grandchild"],
                "bar_env".to_string(),
            ),
            (vec!["foo:0", "child:0"], vec!["root", "bootstrap2"], "foo_env".to_string()),
            (
                vec!["foo:0", "child:0", "grandchild:0"],
                vec!["root", "bootstrap2"],
                "foo_env".to_string(),
            ),
            (vec!["baz:0", "child:0"], vec!["root", "bootstrap3", "child"], "baz_env".to_string()),
            (
                vec!["baz:0", "child:0", "granchild:0"],
                vec!["root", "bootstrap3", "child", "grandchild"],
                "baz_env".to_string(),
            ),
        ];

        let invalid_cases = vec![
            (vec!["bar:0"], vec!["root", "bootstrap"], "bar_env".to_string()),
            (vec!["bar:0"], vec!["root", "not_bootstrap"], "bar_env".to_string()),
            (vec!["foo:0"], vec!["root", "bootstrap2"], "foo_env".to_string()),
            (vec!["foo:0", "child:0"], vec!["root", "not_bootstrap"], "foo_env".to_string()),
            (vec!["baz:0"], vec!["root", "bootstrap3", "child"], "baz_env".to_string()),
            (vec!["baz:0", "child:0"], vec!["root", "bootstrap"], "baz_env".to_string()),
        ];

        let target_moniker = AbsoluteMoniker::from(vec!["target"]);

        for (source, dest, env) in valid_cases {
            let component = self.make_component(source.clone().into());
            let protocol_capability = CapabilitySourceInterface::<C>::Component {
                capability: ComponentCapability::Protocol(ProtocolDecl {
                    name: "debug_service1".into(),
                    source_path: Some("/svc/debug_service1".parse().unwrap()),
                }),
                component: component.as_weak(),
            };
            assert_matches!(
                global_policy_checker.can_route_debug_capability(
                    &protocol_capability,
                    &AbsoluteMoniker::from(dest.clone()),
                    &env,
                    &target_moniker
                ),
                Ok(()),
                "{:?}",
                (source, dest, env)
            );
        }

        for (source, dest, env) in invalid_cases {
            let component = self.make_component(source.clone().into());
            let protocol_capability = CapabilitySourceInterface::<C>::Component {
                capability: ComponentCapability::Protocol(ProtocolDecl {
                    name: "debug_service1".into(),
                    source_path: Some("/svc/debug_service1".parse().unwrap()),
                }),
                component: component.as_weak(),
            };
            assert_matches!(
                global_policy_checker.can_route_debug_capability(
                    &protocol_capability,
                    &AbsoluteMoniker::from(dest.clone()),
                    &env,
                    &target_moniker
                ),
                Err(_),
                "{:?}",
                (source, dest, env)
            );
        }

        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_debug_capability()` for capability sources of type
    // `Capability` with collection allowlist entries.
    fn global_policy_checker_can_route_debug_capability_with_collection_allowlist_entry(
        &self,
    ) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_debug_capability_policy(
            DebugCapabilityKey {
                source_name: CapabilityName::from("debug_service1"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
                env_name: "bar_env".to_string(),
            },
            AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec!["bar"])),
            AllowlistEntryBuilder::new()
                .exact_from_moniker(&AbsoluteMoniker::from(vec!["root", "bootstrap"]))
                .any_descendant_in_collection("coll1"),
        );
        config_builder.add_debug_capability_policy(
            DebugCapabilityKey {
                source_name: CapabilityName::from("debug_service1"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
                env_name: "foo_env".to_string(),
            },
            AllowlistEntryBuilder::new().exact("foo").any_descendant_in_collection("coll2"),
            AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                "root",
                "bootstrap2",
            ])),
        );
        config_builder.add_debug_capability_policy(
            DebugCapabilityKey {
                source_name: CapabilityName::from("debug_service1"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
                env_name: "baz_env".to_string(),
            },
            AllowlistEntryBuilder::new().exact("baz").any_descendant_in_collection("coll3"),
            AllowlistEntryBuilder::new()
                .exact("root")
                .exact("bootstrap3")
                .any_descendant_in_collection("coll4"),
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());

        // source, dest, env
        let valid_cases = vec![
            (vec!["bar:0"], vec!["root", "bootstrap", "coll1:instance1"], "bar_env".to_string()),
            (
                vec!["bar:0"],
                vec!["root", "bootstrap", "coll1:instance1", "child"],
                "bar_env".to_string(),
            ),
            (vec!["foo:0", "coll2:instance2:0"], vec!["root", "bootstrap2"], "foo_env".to_string()),
            (
                vec!["foo:0", "coll2:instance2:0", "child:0"],
                vec!["root", "bootstrap2"],
                "foo_env".to_string(),
            ),
            (
                vec!["baz:0", "coll3:instance3:0"],
                vec!["root", "bootstrap3", "coll4:instance4"],
                "baz_env".to_string(),
            ),
            (
                vec!["baz:0", "coll3:instance3:0", "child:0"],
                vec!["root", "bootstrap3", "coll4:instance4", "child"],
                "baz_env".to_string(),
            ),
        ];

        let invalid_cases = vec![
            (vec!["bar:0"], vec!["root", "bootstrap"], "bar_env".to_string()),
            (vec!["bar:0"], vec!["root", "not_bootstrap"], "bar_env".to_string()),
            (vec!["foo:0"], vec!["root", "bootstrap2"], "foo_env".to_string()),
            (vec!["foo:0", "child:0"], vec!["root", "not_bootstrap"], "foo_env".to_string()),
            (vec!["baz:0"], vec!["root", "bootstrap3", "child"], "baz_env".to_string()),
            (vec!["baz:0", "child:0"], vec!["root", "bootstrap"], "baz_env".to_string()),
        ];

        let target_moniker = AbsoluteMoniker::from(vec!["target"]);

        for (source, dest, env) in valid_cases {
            let component = self.make_component(source.clone().into());
            let protocol_capability = CapabilitySourceInterface::<C>::Component {
                capability: ComponentCapability::Protocol(ProtocolDecl {
                    name: "debug_service1".into(),
                    source_path: Some("/svc/debug_service1".parse().unwrap()),
                }),
                component: component.as_weak(),
            };
            assert_matches!(
                global_policy_checker.can_route_debug_capability(
                    &protocol_capability,
                    &AbsoluteMoniker::from(dest.clone()),
                    &env,
                    &target_moniker
                ),
                Ok(()),
                "{:?}",
                (source, dest, env)
            );
        }

        for (source, dest, env) in invalid_cases {
            let component = self.make_component(source.clone().into());
            let protocol_capability = CapabilitySourceInterface::<C>::Component {
                capability: ComponentCapability::Protocol(ProtocolDecl {
                    name: "debug_service1".into(),
                    source_path: Some("/svc/debug_service1".parse().unwrap()),
                }),
                component: component.as_weak(),
            };
            assert_matches!(
                global_policy_checker.can_route_debug_capability(
                    &protocol_capability,
                    &AbsoluteMoniker::from(dest.clone()),
                    &env,
                    &target_moniker
                ),
                Err(_),
                "{:?}",
                (source, dest, env)
            );
        }

        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_capability()` for builtin capabilities.
    fn global_policy_checker_can_route_capability_builtin_cap(&self) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_capability_policy(
            CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentManager,
                source_name: CapabilityName::from("hub"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Directory,
            },
            vec![
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root",
                ])),
                AllowlistEntryBuilder::build_exact_from_moniker(&AbsoluteMoniker::from(vec![
                    "root", "core",
                ])),
            ],
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());

        let dir_capability = CapabilitySourceInterface::<C>::Builtin {
            capability: InternalCapability::Directory(CapabilityName::from("hub")),
            top_instance: Weak::new(),
        };
        let valid_path_0 = AbsoluteMoniker::from(vec!["root"]);
        let valid_path_1 = AbsoluteMoniker::from(vec!["root", "core"]);
        let invalid_path_0 = AbsoluteMoniker::from(vec!["foobar"]);
        let invalid_path_1 = AbsoluteMoniker::from(vec!["foo", "bar", "foobar"]);

        assert_matches!(
            global_policy_checker.can_route_capability(&dir_capability, &valid_path_0),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&dir_capability, &valid_path_1),
            Ok(())
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&dir_capability, &invalid_path_0),
            Err(_)
        );
        assert_matches!(
            global_policy_checker.can_route_capability(&dir_capability, &invalid_path_1),
            Err(_)
        );
        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_capability()` for policy that includes non-exact
    // `AllowlistEntry::Realm` entries.
    fn global_policy_checker_can_route_capability_with_realm_allowlist_entry(
        &self,
    ) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_capability_policy(
            CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentManager,
                source_name: CapabilityName::from("fuchsia.kernel.RootResource"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
            },
            vec![
                AllowlistEntryBuilder::new().exact("tests").any_descendant(),
                AllowlistEntryBuilder::new().exact("core").exact("tests").any_descendant(),
            ],
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());
        let protocol_capability = CapabilitySourceInterface::<C>::Namespace {
            capability: ComponentCapability::Protocol(ProtocolDecl {
                name: "fuchsia.kernel.RootResource".into(),
                source_path: Some("/svc/fuchsia.kernel.RootResource".parse().unwrap()),
            }),
            top_instance: Weak::new(),
        };

        macro_rules! can_route {
            ($moniker:expr) => {
                global_policy_checker.can_route_capability(&protocol_capability, $moniker)
            };
        }

        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests", "test1"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests", "coll:test1"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests", "test1", "util"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests", "test2"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "tests", "test"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "tests", "coll:t"])).is_ok());

        assert!(can_route!(&AbsoluteMoniker::from(vec!["foo"])).is_err());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests"])).is_err());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "foo"])).is_err());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "tests"])).is_err());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "tests:test"])).is_err());
        Ok(())
    }

    // Tests `GlobalPolicyChecker::can_route_capability()` for policy that includes non-exact
    // `AllowlistEntry::Collection` entries.
    fn global_policy_checker_can_route_capability_with_collection_allowlist_entry(
        &self,
    ) -> Result<(), Error> {
        let mut config_builder = CapabilityAllowlistConfigBuilder::new();
        config_builder.add_capability_policy(
            CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentManager,
                source_name: CapabilityName::from("fuchsia.kernel.RootResource"),
                source: CapabilityAllowlistSource::Self_,
                capability: CapabilityTypeName::Protocol,
            },
            vec![
                AllowlistEntryBuilder::new().any_descendant_in_collection("tests"),
                AllowlistEntryBuilder::new().exact("core").any_descendant_in_collection("tests"),
            ],
        );
        let global_policy_checker = GlobalPolicyChecker::new(config_builder.build());
        let protocol_capability = CapabilitySourceInterface::<C>::Namespace {
            capability: ComponentCapability::Protocol(ProtocolDecl {
                name: "fuchsia.kernel.RootResource".into(),
                source_path: Some("/svc/fuchsia.kernel.RootResource".parse().unwrap()),
            }),
            top_instance: Weak::new(),
        };

        macro_rules! can_route {
            ($moniker:expr) => {
                global_policy_checker.can_route_capability(&protocol_capability, $moniker)
            };
        }

        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests:t1"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests:t2"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests:t1", "util"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "tests:t1"])).is_ok());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "tests:t2"])).is_ok());

        assert!(can_route!(&AbsoluteMoniker::from(vec!["foo"])).is_err());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["tests"])).is_err());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["coll:foo"])).is_err());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "foo"])).is_err());
        assert!(can_route!(&AbsoluteMoniker::from(vec!["core", "coll:tests"])).is_err());
        Ok(())
    }
}

// Creates a RuntimeConfig based on the capability allowlist entries provided during
// construction.
struct CapabilityAllowlistConfigBuilder {
    capability_policy: HashMap<CapabilityAllowlistKey, HashSet<AllowlistEntry>>,
    debug_capability_policy: HashMap<DebugCapabilityKey, HashSet<DebugCapabilityAllowlistEntry>>,
}

impl CapabilityAllowlistConfigBuilder {
    pub fn new() -> Self {
        Self { capability_policy: HashMap::new(), debug_capability_policy: HashMap::new() }
    }

    /// Add a new entry to the configuration.
    pub fn add_capability_policy<'a>(
        &'a mut self,
        key: CapabilityAllowlistKey,
        value: Vec<AllowlistEntry>,
    ) -> &'a mut Self {
        let value_set = HashSet::from_iter(value.iter().cloned());
        self.capability_policy.insert(key, value_set);
        self
    }

    /// Add a new entry to the configuration.
    pub fn add_debug_capability_policy<'a>(
        &'a mut self,
        key: DebugCapabilityKey,
        source: AllowlistEntry,
        dest: AllowlistEntry,
    ) -> &'a mut Self {
        self.debug_capability_policy
            .entry(key)
            .or_default()
            .insert(DebugCapabilityAllowlistEntry::new(source, dest));
        self
    }

    /// Creates a configuration from the provided policies.
    pub fn build(&self) -> Arc<RuntimeConfig> {
        let config = Arc::new(RuntimeConfig {
            security_policy: SecurityPolicy {
                job_policy: JobPolicyAllowlists {
                    ambient_mark_vmo_exec: vec![],
                    main_process_critical: vec![],
                    create_raw_processes: vec![],
                },
                capability_policy: self.capability_policy.clone(),
                debug_capability_policy: self.debug_capability_policy.clone(),
                child_policy: ChildPolicyAllowlists { reboot_on_terminate: vec![] },
            },
            ..Default::default()
        });
        config
    }
}
