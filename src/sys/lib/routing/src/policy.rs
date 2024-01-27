// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        capability_source::CapabilitySourceInterface,
        component_instance::ComponentInstanceInterface,
        config::{
            AllowlistEntry, AllowlistMatcher, CapabilityAllowlistKey, CapabilityAllowlistSource,
            DebugCapabilityKey, RuntimeConfig,
        },
    },
    fuchsia_zircon_status as zx,
    moniker::{AbsoluteMoniker, AbsoluteMonikerBase, ChildMonikerBase, ExtendedMoniker},
    std::sync::{Arc, Weak},
    thiserror::Error,
    tracing::{error, warn},
};

#[cfg(feature = "serde")]
use serde::{Deserialize, Serialize};

/// Errors returned by the PolicyChecker and the ScopedPolicyChecker.
#[cfg_attr(feature = "serde", derive(Deserialize, Serialize), serde(rename_all = "snake_case"))]
#[derive(Debug, Clone, Error, PartialEq)]
pub enum PolicyError {
    #[error("security policy was unavailable to check")]
    PolicyUnavailable,

    #[error("feature \"{policy}\" used by \"{moniker}\" is not supported by this instance of component manager")]
    Unsupported { policy: String, moniker: AbsoluteMoniker },

    #[error("security policy disallows \"{policy}\" job policy for \"{moniker}\"")]
    JobPolicyDisallowed { policy: String, moniker: AbsoluteMoniker },

    #[error("security policy disallows \"{policy}\" child policy for \"{moniker}\"")]
    ChildPolicyDisallowed { policy: String, moniker: AbsoluteMoniker },

    #[error("security policy was unable to extract the source from the routed capability")]
    InvalidCapabilitySource,

    #[error("security policy disallows \"{cap}\" from \"{source_moniker}\" being used at \"{target_moniker}\"")]
    CapabilityUseDisallowed {
        cap: String,
        source_moniker: ExtendedMoniker,
        target_moniker: AbsoluteMoniker,
    },

    #[error("debug security policy disallows \"{cap}\" from \"{source_moniker}\" being routed from environment \"{env_moniker}:{env_name}\" to \"{target_moniker}\"")]
    DebugCapabilityUseDisallowed {
        cap: String,
        source_moniker: ExtendedMoniker,
        env_moniker: AbsoluteMoniker,
        env_name: String,
        target_moniker: AbsoluteMoniker,
    },
}

impl PolicyError {
    fn unsupported(policy: impl Into<String>, moniker: &AbsoluteMoniker) -> Self {
        PolicyError::Unsupported { policy: policy.into(), moniker: moniker.clone() }
    }

    fn job_policy_disallowed(policy: impl Into<String>, moniker: &AbsoluteMoniker) -> Self {
        PolicyError::JobPolicyDisallowed { policy: policy.into(), moniker: moniker.clone() }
    }

    fn child_policy_disallowed(policy: impl Into<String>, moniker: &AbsoluteMoniker) -> Self {
        PolicyError::ChildPolicyDisallowed { policy: policy.into(), moniker: moniker.clone() }
    }

    fn capability_use_disallowed(
        cap: impl Into<String>,
        source_moniker: &ExtendedMoniker,
        target_moniker: &AbsoluteMoniker,
    ) -> Self {
        PolicyError::CapabilityUseDisallowed {
            cap: cap.into(),
            source_moniker: source_moniker.clone(),
            target_moniker: target_moniker.clone(),
        }
    }

    fn debug_capability_use_disallowed(
        cap: impl Into<String>,
        source_moniker: &ExtendedMoniker,
        env_moniker: &AbsoluteMoniker,
        env_name: impl Into<String>,
        target_moniker: &AbsoluteMoniker,
    ) -> Self {
        PolicyError::DebugCapabilityUseDisallowed {
            cap: cap.into(),
            source_moniker: source_moniker.clone(),
            env_moniker: env_moniker.clone(),
            env_name: env_name.into(),
            target_moniker: target_moniker.clone(),
        }
    }

    /// Convert this error into its approximate `zx::Status` equivalent.
    pub fn as_zx_status(&self) -> zx::Status {
        zx::Status::ACCESS_DENIED
    }
}

/// Evaluates security policy globally across the entire Model and all components.
/// This is used to enforce runtime capability routing restrictions across all
/// components to prevent high privilleged capabilities from being routed to
/// components outside of the list defined in the runtime configs security
/// policy.
#[derive(Clone, Debug, Default)]
pub struct GlobalPolicyChecker {
    /// The runtime configuration containing the security policy to apply.
    config: Arc<RuntimeConfig>,
}

impl GlobalPolicyChecker {
    /// Constructs a new PolicyChecker object configured by the
    /// RuntimeConfig::SecurityPolicy.
    pub fn new(config: Arc<RuntimeConfig>) -> Self {
        Self { config: config }
    }

    fn get_policy_key<'a, C>(
        capability_source: &'a CapabilitySourceInterface<C>,
    ) -> Result<CapabilityAllowlistKey, PolicyError>
    where
        C: ComponentInstanceInterface,
    {
        Ok(match &capability_source {
            CapabilitySourceInterface::Namespace { capability, .. } => CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentManager,
                source_name: capability
                    .source_name()
                    .ok_or(PolicyError::InvalidCapabilitySource)?
                    .clone(),
                source: CapabilityAllowlistSource::Self_,
                capability: capability.type_name(),
            },
            CapabilitySourceInterface::Component { capability, component }
            | CapabilitySourceInterface::FilteredService { capability, component, .. } => {
                CapabilityAllowlistKey {
                    source_moniker: ExtendedMoniker::ComponentInstance(
                        component.abs_moniker.clone(),
                    ),
                    source_name: capability
                        .source_name()
                        .ok_or(PolicyError::InvalidCapabilitySource)?
                        .clone(),
                    source: CapabilityAllowlistSource::Self_,
                    capability: capability.type_name(),
                }
            }
            CapabilitySourceInterface::Builtin { capability, .. } => CapabilityAllowlistKey {
                source_moniker: ExtendedMoniker::ComponentManager,
                source_name: capability.source_name().clone(),
                source: CapabilityAllowlistSource::Self_,
                capability: capability.type_name(),
            },
            CapabilitySourceInterface::Framework { capability, component } => {
                CapabilityAllowlistKey {
                    source_moniker: ExtendedMoniker::ComponentInstance(
                        component.abs_moniker.clone(),
                    ),
                    source_name: capability.source_name().clone(),
                    source: CapabilityAllowlistSource::Framework,
                    capability: capability.type_name(),
                }
            }
            CapabilitySourceInterface::Capability { source_capability, component } => {
                CapabilityAllowlistKey {
                    source_moniker: ExtendedMoniker::ComponentInstance(
                        component.abs_moniker.clone(),
                    ),
                    source_name: source_capability
                        .source_name()
                        .ok_or(PolicyError::InvalidCapabilitySource)?
                        .clone(),
                    source: CapabilityAllowlistSource::Capability,
                    capability: source_capability.type_name(),
                }
            }
            CapabilitySourceInterface::Collection { capability, component, .. }
            | CapabilitySourceInterface::Aggregate { capability, component, .. } => {
                CapabilityAllowlistKey {
                    source_moniker: ExtendedMoniker::ComponentInstance(
                        component.abs_moniker.clone(),
                    ),
                    source_name: capability.source_name().clone(),
                    source: CapabilityAllowlistSource::Self_,
                    capability: capability.type_name(),
                }
            }
        })
    }

    /// Returns Ok(()) if the provided capability source can be routed to the
    /// given target_moniker, else a descriptive PolicyError.
    pub fn can_route_capability<'a, C>(
        &self,
        capability_source: &'a CapabilitySourceInterface<C>,
        target_moniker: &'a AbsoluteMoniker,
    ) -> Result<(), PolicyError>
    where
        C: ComponentInstanceInterface,
    {
        let policy_key = Self::get_policy_key(capability_source).map_err(|e| {
            error!("Security policy could not generate a policy key for `{}`", capability_source);
            e
        })?;

        match self.config.security_policy.capability_policy.get(&policy_key) {
            Some(entries) => {
                let parts = target_moniker
                    .path()
                    .clone()
                    .into_iter()
                    .map(|c| AllowlistMatcher::Exact(c))
                    .collect();
                let entry = AllowlistEntry { matchers: parts };

                // Use the HashSet to find any exact matches quickly.
                if entries.contains(&entry) {
                    return Ok(());
                }

                // Otherwise linear search for any non-exact matches.
                if entries.iter().any(|entry| allowlist_entry_matches(entry, &target_moniker)) {
                    Ok(())
                } else {
                    warn!(
                        "Security policy prevented `{}` from `{}` being routed to `{}`.",
                        policy_key.source_name, policy_key.source_moniker, target_moniker
                    );
                    Err(PolicyError::capability_use_disallowed(
                        policy_key.source_name.str(),
                        &policy_key.source_moniker,
                        &target_moniker,
                    ))
                }
            }
            None => Ok(()),
        }
    }

    /// Returns Ok(()) if the provided debug capability source is allowed to be routed from given
    /// environment.
    pub fn can_route_debug_capability<'a, C>(
        &self,
        capability_source: &'a CapabilitySourceInterface<C>,
        env_moniker: &'a AbsoluteMoniker,
        env_name: &'a str,
        target_moniker: &'a AbsoluteMoniker,
    ) -> Result<(), PolicyError>
    where
        C: ComponentInstanceInterface,
    {
        let CapabilityAllowlistKey { source_moniker, source_name, source, capability } =
            Self::get_policy_key(capability_source).map_err(|e| {
                error!(
                    "Security policy could not generate a policy key for `{}`",
                    capability_source
                );
                e
            })?;
        let debug_key =
            DebugCapabilityKey { source_name, source, capability, env_name: env_name.to_string() };

        let route_allowed =
            match self.config.security_policy.debug_capability_policy.get(&debug_key) {
                None => false,
                Some(allowlist_set) => {
                    allowlist_set.iter().any(|entry| entry.matches(&source_moniker, env_moniker))
                }
            };
        if route_allowed {
            return Ok(());
        }

        warn!(
            "Debug security policy prevented `{}` from `{}` being routed to `{}`.",
            debug_key.source_name, source_moniker, target_moniker
        );
        Err(PolicyError::debug_capability_use_disallowed(
            debug_key.source_name.str(),
            &source_moniker,
            &env_moniker,
            env_name,
            &target_moniker,
        ))
    }

    /// Returns Ok(()) if `target_moniker` is allowed to have `on_terminate=REBOOT` set.
    pub fn reboot_on_terminate_allowed(
        &self,
        target_moniker: &AbsoluteMoniker,
    ) -> Result<(), PolicyError> {
        if !self.config.reboot_on_terminate_enabled {
            return Err(PolicyError::unsupported("reboot_on_terminate", &target_moniker));
        }
        if self
            .config
            .security_policy
            .child_policy
            .reboot_on_terminate
            .iter()
            .any(|entry| allowlist_entry_matches(entry, &target_moniker))
        {
            Ok(())
        } else {
            Err(PolicyError::child_policy_disallowed("reboot_on_terminate", &target_moniker))
        }
    }
}

pub(crate) fn allowlist_entry_matches(
    allowlist_entry: &AllowlistEntry,
    target_moniker: &AbsoluteMoniker,
) -> bool {
    let mut iter = target_moniker.path().iter();

    if allowlist_entry.matchers.is_empty() && !target_moniker.is_root() {
        // If there are no matchers in the allowlist, the moniker must be the root.
        // Anything else will not match.
        return false;
    }

    for matcher in &allowlist_entry.matchers {
        let cur_child = if let Some(target_child) = iter.next() {
            target_child
        } else {
            // We have more matchers, but the moniker has already ended.
            return false;
        };
        match matcher {
            AllowlistMatcher::Exact(child) => {
                if cur_child != child {
                    // The child does not exactly match.
                    return false;
                }
            }
            // Any child is acceptable. Continue with remaining matchers.
            AllowlistMatcher::AnyChild => continue,
            // Any descendant at this point is acceptable.
            AllowlistMatcher::AnyDescendant => return true,
            AllowlistMatcher::AnyDescendantInCollection(expected_collection) => {
                if let Some(collection) = cur_child.collection() {
                    if collection == expected_collection.as_str() {
                        // This child is in a collection and the name matches.
                        // Because we allow any descendant, return true immediately.
                        return true;
                    } else {
                        // This child is in a collection but the name does not match.
                        return false;
                    }
                } else {
                    // This child is not in a collection, so it does not match.
                    return false;
                }
            }
            AllowlistMatcher::AnyChildInCollection(expected_collection) => {
                if let Some(collection) = cur_child.collection() {
                    if collection != expected_collection.as_str() {
                        // This child is in a collection but the name does not match.
                        return false;
                    }
                } else {
                    // This child is not in a collection, so it does not match.
                    return false;
                }
            }
        }
    }

    if iter.next().is_some() {
        // We've gone through all the matchers, but there are still children
        // in the moniker. Descendant cases are already handled above, so this
        // must be a failure to match.
        false
    } else {
        true
    }
}

/// Evaluates security policy relative to a specific Component (based on that Component's
/// AbsoluteMoniker).
pub struct ScopedPolicyChecker {
    /// The runtime configuration containing the security policy to apply.
    config: Weak<RuntimeConfig>,

    /// The absolute moniker of the component that policy will be evaluated for.
    moniker: AbsoluteMoniker,
}

impl ScopedPolicyChecker {
    pub fn new(config: Weak<RuntimeConfig>, moniker: AbsoluteMoniker) -> Self {
        ScopedPolicyChecker { config, moniker }
    }

    pub fn get_scope(&self) -> &AbsoluteMoniker {
        &self.moniker
    }

    // This interface is super simple for now since there's only three allowlists. In the future
    // we'll probably want a different interface than an individual function per policy item.

    pub fn ambient_mark_vmo_exec_allowed(&self) -> Result<(), PolicyError> {
        let config = self.config.upgrade().ok_or(PolicyError::PolicyUnavailable)?;
        if config
            .security_policy
            .job_policy
            .ambient_mark_vmo_exec
            .iter()
            .any(|entry| allowlist_entry_matches(entry, &self.moniker))
        {
            Ok(())
        } else {
            Err(PolicyError::job_policy_disallowed("ambient_mark_vmo_exec", &self.moniker))
        }
    }

    pub fn main_process_critical_allowed(&self) -> Result<(), PolicyError> {
        let config = self.config.upgrade().ok_or(PolicyError::PolicyUnavailable)?;
        if config
            .security_policy
            .job_policy
            .main_process_critical
            .iter()
            .any(|entry| allowlist_entry_matches(entry, &self.moniker))
        {
            Ok(())
        } else {
            Err(PolicyError::job_policy_disallowed("main_process_critical", &self.moniker))
        }
    }

    pub fn create_raw_processes_allowed(&self) -> Result<(), PolicyError> {
        let config = self.config.upgrade().ok_or(PolicyError::PolicyUnavailable)?;
        if config
            .security_policy
            .job_policy
            .create_raw_processes
            .iter()
            .any(|entry| allowlist_entry_matches(entry, &self.moniker))
        {
            Ok(())
        } else {
            Err(PolicyError::job_policy_disallowed("create_raw_processes", &self.moniker))
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::config::{
            AllowlistEntryBuilder, ChildPolicyAllowlists, JobPolicyAllowlists, RuntimeConfig,
            SecurityPolicy,
        },
        assert_matches::assert_matches,
        moniker::{AbsoluteMoniker, AbsoluteMonikerBase, ChildMoniker},
        std::collections::HashMap,
    };

    #[test]
    fn allowlist_entry_checker() {
        let root = AbsoluteMoniker::root();
        let allowed = AbsoluteMoniker::from(vec!["foo", "bar"]);
        let disallowed_child_of_allowed = AbsoluteMoniker::from(vec!["foo", "bar", "baz"]);
        let disallowed = AbsoluteMoniker::from(vec!["baz", "fiz"]);
        let allowlist_exact = AllowlistEntryBuilder::new().exact_from_moniker(&allowed).build();
        assert!(allowlist_entry_matches(&allowlist_exact, &allowed));
        assert!(!allowlist_entry_matches(&allowlist_exact, &root));
        assert!(!allowlist_entry_matches(&allowlist_exact, &disallowed));
        assert!(!allowlist_entry_matches(&allowlist_exact, &disallowed_child_of_allowed));

        let allowed_realm_root = AbsoluteMoniker::from(vec!["qux"]);
        let allowed_child_of_realm = AbsoluteMoniker::from(vec!["qux", "quux"]);
        let allowed_nested_child_of_realm = AbsoluteMoniker::from(vec!["qux", "quux", "foo"]);
        let allowlist_realm =
            AllowlistEntryBuilder::new().exact_from_moniker(&allowed_realm_root).any_descendant();
        assert!(!allowlist_entry_matches(&allowlist_realm, &allowed_realm_root));
        assert!(allowlist_entry_matches(&allowlist_realm, &allowed_child_of_realm));
        assert!(allowlist_entry_matches(&allowlist_realm, &allowed_nested_child_of_realm));
        assert!(!allowlist_entry_matches(&allowlist_realm, &disallowed));
        assert!(!allowlist_entry_matches(&allowlist_realm, &root));

        let collection_holder = AbsoluteMoniker::from(vec!["corge"]);
        let collection_child = AbsoluteMoniker::from(vec!["corge", "collection:child"]);
        let collection_nested_child =
            AbsoluteMoniker::from(vec!["corge", "collection:child", "inner-child"]);
        let non_collection_child = AbsoluteMoniker::from(vec!["corge", "grault"]);
        let allowlist_collection = AllowlistEntryBuilder::new()
            .exact_from_moniker(&collection_holder)
            .any_descendant_in_collection("collection");
        assert!(!allowlist_entry_matches(&allowlist_collection, &collection_holder));
        assert!(allowlist_entry_matches(&allowlist_collection, &collection_child));
        assert!(allowlist_entry_matches(&allowlist_collection, &collection_nested_child));
        assert!(!allowlist_entry_matches(&allowlist_collection, &non_collection_child));
        assert!(!allowlist_entry_matches(&allowlist_collection, &disallowed));
        assert!(!allowlist_entry_matches(&allowlist_collection, &root));

        let collection_a = AbsoluteMoniker::from(vec!["foo", "bar:a", "baz", "qux"]);
        let collection_b = AbsoluteMoniker::from(vec!["foo", "bar:b", "baz", "qux"]);
        let parent_not_allowed = AbsoluteMoniker::from(vec!["foo", "bar:b", "baz"]);
        let collection_not_allowed = AbsoluteMoniker::from(vec!["foo", "bar:b", "baz"]);
        let different_collection_not_allowed =
            AbsoluteMoniker::from(vec!["foo", "test:b", "baz", "qux"]);
        let allowlist_exact_in_collection = AllowlistEntryBuilder::new()
            .exact("foo")
            .any_child_in_collection("bar")
            .exact("baz")
            .exact("qux")
            .build();
        assert!(allowlist_entry_matches(&allowlist_exact_in_collection, &collection_a));
        assert!(allowlist_entry_matches(&allowlist_exact_in_collection, &collection_b));
        assert!(!allowlist_entry_matches(&allowlist_exact_in_collection, &parent_not_allowed));
        assert!(!allowlist_entry_matches(&allowlist_exact_in_collection, &collection_not_allowed));
        assert!(!allowlist_entry_matches(
            &allowlist_exact_in_collection,
            &different_collection_not_allowed
        ));

        let any_child_allowlist = AllowlistEntryBuilder::new().exact("core").any_child().build();
        let allowed = AbsoluteMoniker::from(vec!["core", "abc"]);
        let disallowed_1 = AbsoluteMoniker::from(vec!["not_core", "abc"]);
        let disallowed_2 = AbsoluteMoniker::from(vec!["core", "abc", "def"]);
        assert!(allowlist_entry_matches(&any_child_allowlist, &allowed));
        assert!(!allowlist_entry_matches(&any_child_allowlist, &disallowed_1));
        assert!(!allowlist_entry_matches(&any_child_allowlist, &disallowed_2));

        let multiwildcard_allowlist = AllowlistEntryBuilder::new()
            .exact("core")
            .any_child()
            .any_child_in_collection("foo")
            .any_descendant();
        let allowed = AbsoluteMoniker::from(vec!["core", "abc", "foo:def", "ghi"]);
        let disallowed_1 = AbsoluteMoniker::from(vec!["not_core", "abc", "foo:def", "ghi"]);
        let disallowed_2 = AbsoluteMoniker::from(vec!["core", "abc", "not_foo:def", "ghi"]);
        let disallowed_3 = AbsoluteMoniker::from(vec!["core", "abc", "foo:def"]);
        assert!(allowlist_entry_matches(&multiwildcard_allowlist, &allowed));
        assert!(!allowlist_entry_matches(&multiwildcard_allowlist, &disallowed_1));
        assert!(!allowlist_entry_matches(&multiwildcard_allowlist, &disallowed_2));
        assert!(!allowlist_entry_matches(&multiwildcard_allowlist, &disallowed_3));
    }

    #[test]
    fn scoped_policy_checker_vmex() {
        macro_rules! assert_vmex_allowed_matches {
            ($config:expr, $moniker:expr, $expected:pat) => {
                let result = ScopedPolicyChecker::new($config.clone(), $moniker.clone())
                    .ambient_mark_vmo_exec_allowed();
                assert_matches!(result, $expected);
            };
        }
        macro_rules! assert_vmex_disallowed {
            ($config:expr, $moniker:expr) => {
                assert_vmex_allowed_matches!(
                    $config,
                    $moniker,
                    Err(PolicyError::JobPolicyDisallowed { .. })
                );
            };
        }
        let strong_config = Arc::new(RuntimeConfig::default());
        let config = Arc::downgrade(&strong_config);
        assert_vmex_disallowed!(config, AbsoluteMoniker::root());
        assert_vmex_disallowed!(config, AbsoluteMoniker::from(vec!["foo"]));

        let allowed1 = AbsoluteMoniker::from(vec!["foo", "bar"]);
        let allowed2 = AbsoluteMoniker::from(vec!["baz", "fiz"]);
        let strong_config = Arc::new(RuntimeConfig {
            security_policy: SecurityPolicy {
                job_policy: JobPolicyAllowlists {
                    ambient_mark_vmo_exec: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                    main_process_critical: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                    create_raw_processes: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                },
                capability_policy: HashMap::new(),
                debug_capability_policy: HashMap::new(),
                child_policy: ChildPolicyAllowlists {
                    reboot_on_terminate: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                },
            },
            ..Default::default()
        });
        let config = Arc::downgrade(&strong_config);
        assert_vmex_allowed_matches!(config, allowed1, Ok(()));
        assert_vmex_allowed_matches!(config, allowed2, Ok(()));
        assert_vmex_disallowed!(config, AbsoluteMoniker::root());
        assert_vmex_disallowed!(config, allowed1.parent().unwrap());
        assert_vmex_disallowed!(config, allowed1.child(ChildMoniker::from("baz")));

        drop(strong_config);
        assert_vmex_allowed_matches!(config, allowed1, Err(PolicyError::PolicyUnavailable));
        assert_vmex_allowed_matches!(config, allowed2, Err(PolicyError::PolicyUnavailable));
    }

    #[test]
    fn scoped_policy_checker_create_raw_processes() {
        macro_rules! assert_create_raw_processes_allowed_matches {
            ($config:expr, $moniker:expr, $expected:pat) => {
                let result = ScopedPolicyChecker::new($config.clone(), $moniker.clone())
                    .create_raw_processes_allowed();
                assert_matches!(result, $expected);
            };
        }
        macro_rules! assert_create_raw_processes_disallowed {
            ($config:expr, $moniker:expr) => {
                assert_create_raw_processes_allowed_matches!(
                    $config,
                    $moniker,
                    Err(PolicyError::JobPolicyDisallowed { .. })
                );
            };
        }
        let strong_config = Arc::new(RuntimeConfig::default());
        let config = Arc::downgrade(&strong_config);
        assert_create_raw_processes_disallowed!(config, AbsoluteMoniker::root());
        assert_create_raw_processes_disallowed!(config, AbsoluteMoniker::from(vec!["foo"]));

        let allowed1 = AbsoluteMoniker::from(vec!["foo", "bar"]);
        let allowed2 = AbsoluteMoniker::from(vec!["baz", "fiz"]);
        let strong_config = Arc::new(RuntimeConfig {
            security_policy: SecurityPolicy {
                job_policy: JobPolicyAllowlists {
                    ambient_mark_vmo_exec: vec![],
                    main_process_critical: vec![],
                    create_raw_processes: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                },
                capability_policy: HashMap::new(),
                debug_capability_policy: HashMap::new(),
                child_policy: ChildPolicyAllowlists { reboot_on_terminate: vec![] },
            },
            ..Default::default()
        });
        let config = Arc::downgrade(&strong_config);
        assert_create_raw_processes_allowed_matches!(config, allowed1, Ok(()));
        assert_create_raw_processes_allowed_matches!(config, allowed2, Ok(()));
        assert_create_raw_processes_disallowed!(config, AbsoluteMoniker::root());
        assert_create_raw_processes_disallowed!(config, allowed1.parent().unwrap());
        assert_create_raw_processes_disallowed!(config, allowed1.child(ChildMoniker::from("baz")));

        drop(strong_config);
        assert_create_raw_processes_allowed_matches!(
            config,
            allowed1,
            Err(PolicyError::PolicyUnavailable)
        );
        assert_create_raw_processes_allowed_matches!(
            config,
            allowed2,
            Err(PolicyError::PolicyUnavailable)
        );
    }

    #[test]
    fn scoped_policy_checker_main_process_critical_allowed() {
        macro_rules! assert_critical_allowed_matches {
            ($config:expr, $moniker:expr, $expected:pat) => {
                let result = ScopedPolicyChecker::new($config.clone(), $moniker.clone())
                    .main_process_critical_allowed();
                assert_matches!(result, $expected);
            };
        }
        macro_rules! assert_critical_disallowed {
            ($config:expr, $moniker:expr) => {
                assert_critical_allowed_matches!(
                    $config,
                    $moniker,
                    Err(PolicyError::JobPolicyDisallowed { .. })
                );
            };
        }
        let strong_config = Arc::new(RuntimeConfig::default());
        let config = Arc::downgrade(&strong_config);
        assert_critical_disallowed!(config, AbsoluteMoniker::root());
        assert_critical_disallowed!(config, AbsoluteMoniker::from(vec!["foo"]));

        let allowed1 = AbsoluteMoniker::from(vec!["foo", "bar"]);
        let allowed2 = AbsoluteMoniker::from(vec!["baz", "fiz"]);
        let strong_config = Arc::new(RuntimeConfig {
            security_policy: SecurityPolicy {
                job_policy: JobPolicyAllowlists {
                    ambient_mark_vmo_exec: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                    main_process_critical: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                    create_raw_processes: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                },
                capability_policy: HashMap::new(),
                debug_capability_policy: HashMap::new(),
                child_policy: ChildPolicyAllowlists { reboot_on_terminate: vec![] },
            },
            ..Default::default()
        });
        let config = Arc::downgrade(&strong_config);
        assert_critical_allowed_matches!(config, allowed1, Ok(()));
        assert_critical_allowed_matches!(config, allowed2, Ok(()));
        assert_critical_disallowed!(config, AbsoluteMoniker::root());
        assert_critical_disallowed!(config, allowed1.parent().unwrap());
        assert_critical_disallowed!(config, allowed1.child(ChildMoniker::from("baz")));

        drop(strong_config);
        assert_critical_allowed_matches!(config, allowed1, Err(PolicyError::PolicyUnavailable));
        assert_critical_allowed_matches!(config, allowed2, Err(PolicyError::PolicyUnavailable));
    }

    #[test]
    fn scoped_policy_checker_reboot_policy_allowed() {
        macro_rules! assert_reboot_allowed_matches {
            ($config:expr, $moniker:expr, $expected:pat) => {
                let result = GlobalPolicyChecker::new($config.clone())
                    .reboot_on_terminate_allowed(&$moniker);
                assert_matches!(result, $expected);
            };
        }
        macro_rules! assert_reboot_disallowed {
            ($config:expr, $moniker:expr) => {
                assert_reboot_allowed_matches!(
                    $config,
                    $moniker,
                    Err(PolicyError::ChildPolicyDisallowed { .. })
                );
            };
        }

        // Empty config and enabled.
        let config =
            Arc::new(RuntimeConfig { reboot_on_terminate_enabled: true, ..Default::default() });
        assert_reboot_disallowed!(config, AbsoluteMoniker::root());
        assert_reboot_disallowed!(config, AbsoluteMoniker::from(vec!["foo"]));

        // Nonempty config and enabled.
        let allowed1 = AbsoluteMoniker::from(vec!["foo", "bar"]);
        let allowed2 = AbsoluteMoniker::from(vec!["baz", "fiz"]);
        let config = Arc::new(RuntimeConfig {
            security_policy: SecurityPolicy {
                job_policy: JobPolicyAllowlists {
                    ambient_mark_vmo_exec: vec![],
                    main_process_critical: vec![],
                    create_raw_processes: vec![],
                },
                capability_policy: HashMap::new(),
                debug_capability_policy: HashMap::new(),
                child_policy: ChildPolicyAllowlists {
                    reboot_on_terminate: vec![
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed1),
                        AllowlistEntryBuilder::build_exact_from_moniker(&allowed2),
                    ],
                },
            },
            reboot_on_terminate_enabled: true,
            ..Default::default()
        });
        assert_reboot_allowed_matches!(config, allowed1, Ok(()));
        assert_reboot_allowed_matches!(config, allowed2, Ok(()));
        assert_reboot_disallowed!(config, AbsoluteMoniker::root());
        assert_reboot_disallowed!(config, allowed1.parent().unwrap());
        assert_reboot_disallowed!(config, allowed1.child(ChildMoniker::from("baz")));

        // Nonempty config and disabled.
        let config = Arc::new(RuntimeConfig {
            security_policy: SecurityPolicy {
                job_policy: JobPolicyAllowlists {
                    ambient_mark_vmo_exec: vec![],
                    main_process_critical: vec![],
                    create_raw_processes: vec![],
                },
                capability_policy: HashMap::new(),
                debug_capability_policy: HashMap::new(),
                child_policy: ChildPolicyAllowlists {
                    reboot_on_terminate: vec![AllowlistEntryBuilder::build_exact_from_moniker(
                        &allowed1,
                    )],
                },
            },
            ..Default::default()
        });
        assert_reboot_allowed_matches!(config, allowed1, Err(PolicyError::Unsupported { .. }));
        assert_reboot_allowed_matches!(
            config,
            AbsoluteMoniker::root(),
            Err(PolicyError::Unsupported { .. })
        );
    }
}
