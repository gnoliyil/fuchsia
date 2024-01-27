// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        act::{validate_action, Actions, ActionsSchema, Severity},
        metrics::{
            fetch::{InspectFetcher, KeyValueFetcher, SelectorString, TextFetcher},
            Metric, Metrics, ValueSource,
        },
        validate::{validate, Trials, TrialsSchema},
        Action,
    },
    anyhow::{bail, format_err, Context, Error},
    num_derive::FromPrimitive,
    serde::{Deserialize, Deserializer},
    std::{collections::HashMap, convert::TryFrom},
};

// These numbers are used in the wasm-bindgen bridge so they are explicit and
// permanent. They don't need to be sequential. This enum must be consistent
// with the Source enum in //src/diagnostics/lib/triage/wasm/src/lib.rs.
#[derive(Debug, Clone, Copy, FromPrimitive, PartialEq)]
pub enum Source {
    Inspect = 0,
    Klog = 1,
    Syslog = 2,
    Bootlog = 3,
    Annotations = 4,
}

/// Schema for JSON triage configuration. This structure is parsed directly from the configuration
/// files using serde_json.
#[derive(Deserialize, Default, Debug)]
#[serde(deny_unknown_fields)]
pub struct ConfigFileSchema {
    /// Map of named Selectors. Each Selector selects a value from Diagnostic data.
    #[serde(rename = "select")]
    pub file_selectors: Option<HashMap<String, SelectorEntry>>,
    /// Map of named Evals. Each Eval calculates a value.
    #[serde(rename = "eval")]
    pub file_evals: Option<HashMap<String, String>>,
    /// Map of named Actions. Each Action uses a boolean value to trigger a warning.
    #[serde(rename = "act")]
    pub(crate) file_actions: Option<HashMap<String, ActionConfig>>,
    /// Map of named Tests. Each test applies sample data to lists of actions that should or
    /// should not trigger.
    #[serde(rename = "test")]
    pub file_tests: Option<TrialsSchema>,
}

/// A selector entry in the configuration file is either a single string
/// or a vector of string selectors. Either case is converted to a vector
/// with at least one element.
#[derive(Debug)]
pub struct SelectorEntry(Vec<String>);

impl<'de> Deserialize<'de> for SelectorEntry {
    fn deserialize<D>(d: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct SelectorVec(std::marker::PhantomData<Vec<String>>);

        impl<'de> serde::de::Visitor<'de> for SelectorVec {
            type Value = Vec<String>;

            fn expecting(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.write_str("either a single selector or an array of selectors")
            }

            fn visit_str<E>(self, value: &str) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                Ok(vec![value.to_string()])
            }

            fn visit_seq<A>(self, mut value: A) -> Result<Self::Value, A::Error>
            where
                A: serde::de::SeqAccess<'de>,
            {
                let mut out = vec![];
                while let Some(s) = value.next_element::<String>()? {
                    out.push(s);
                }
                if out.is_empty() {
                    use serde::de::Error;
                    Err(A::Error::invalid_length(0, &"expected at least one selector"))
                } else {
                    Ok(out)
                }
            }
        }

        Ok(SelectorEntry(d.deserialize_any(SelectorVec(std::marker::PhantomData))?))
    }
}

// TODO(fxb/111911): This struct should be removed once serde_json5 has DeserializeSeed support.
#[derive(Deserialize, Debug)]
#[serde(tag = "type")]
pub enum ActionConfig {
    Alert {
        trigger: String,
        print: String,
        file_bug: Option<String>,
        tag: Option<String>,
        severity: Severity,
    },
    Warning {
        trigger: String,
        print: String,
        file_bug: Option<String>,
        tag: Option<String>,
    },
    Gauge {
        value: String,
        format: Option<String>,
        tag: Option<String>,
    },
    Snapshot {
        trigger: String,
        repeat: String,
        signature: String,
    },
}

impl ConfigFileSchema {
    fn try_from_str_with_namespace(s: &str, namespace: &str) -> Result<Self, anyhow::Error> {
        let schema = serde_json5::from_str::<ConfigFileSchema>(&s)
            .map_err(|e| format_err!("Unable to deserialize config file {}", e))?;
        validate_config(&schema, namespace)?;
        Ok(schema)
    }
}

impl TryFrom<String> for ConfigFileSchema {
    type Error = anyhow::Error;

    fn try_from(s: String) -> Result<Self, Self::Error> {
        ConfigFileSchema::try_from_str_with_namespace(&s, /* namespace=*/ "")
    }
}

fn validate_config(config: &ConfigFileSchema, namespace: &str) -> Result<(), Error> {
    if let Some(ref actions_config) = config.file_actions {
        for (action_name, action_config) in actions_config.iter() {
            validate_action(action_name, &action_config, namespace)?;
        }
    }
    Ok(())
}

pub enum DataFetcher {
    Inspect(InspectFetcher),
    Text(TextFetcher),
    KeyValue(KeyValueFetcher),
    None,
}

/// The path of the Diagnostic files and the data contained within them.
pub struct DiagnosticData {
    pub name: String,
    pub source: Source,
    pub data: DataFetcher,
}

impl DiagnosticData {
    pub fn new(name: String, source: Source, contents: String) -> Result<DiagnosticData, Error> {
        let data = match source {
            Source::Inspect => DataFetcher::Inspect(
                InspectFetcher::try_from(&*contents).context("Parsing inspect.json")?,
            ),
            Source::Syslog | Source::Klog | Source::Bootlog => {
                DataFetcher::Text(TextFetcher::try_from(&*contents).context("Parsing plain text")?)
            }
            Source::Annotations => DataFetcher::KeyValue(
                KeyValueFetcher::try_from(&*contents).context("Parsing annotations")?,
            ),
        };
        Ok(DiagnosticData { name, source, data })
    }

    pub fn new_empty(name: String, source: Source) -> DiagnosticData {
        DiagnosticData { name, source, data: DataFetcher::None }
    }
}

pub struct ParseResult {
    pub metrics: Metrics,
    pub(crate) actions: Actions,
    pub tests: Trials,
}

impl ParseResult {
    pub fn new(
        configs: &HashMap<String, String>,
        action_tag_directive: &ActionTagDirective,
    ) -> Result<ParseResult, Error> {
        let mut actions = HashMap::new();
        let mut metrics = HashMap::new();
        let mut tests = HashMap::new();

        for (namespace, file_data) in configs {
            let file_config =
                match ConfigFileSchema::try_from_str_with_namespace(file_data, namespace) {
                    Ok(c) => c,
                    Err(e) => bail!("Parsing file '{}': {}", namespace, e),
                };
            let ConfigFileSchema { file_actions, file_selectors, file_evals, file_tests } =
                file_config;
            // Other code assumes that each name will have an entry in all categories.
            let file_actions_config = file_actions.unwrap_or_else(|| HashMap::new());
            let file_actions = file_actions_config
                .into_iter()
                .map(|(k, action_config)| {
                    Action::from_config_with_namespace(action_config, namespace)
                        .map(|action| (k, action))
                })
                .collect::<Result<HashMap<_, _>, _>>()?;
            let file_selectors = file_selectors.unwrap_or_else(|| HashMap::new());
            let file_evals = file_evals.unwrap_or_else(|| HashMap::new());
            let file_tests = file_tests.unwrap_or_else(|| HashMap::new());
            let file_actions = filter_actions(file_actions, &action_tag_directive);
            let mut file_metrics = HashMap::new();
            for (key, value) in file_selectors.into_iter() {
                let mut selectors = vec![];
                for v in value.0 {
                    selectors.push(SelectorString::try_from(v)?);
                }
                file_metrics.insert(key, ValueSource::new(Metric::Selector(selectors)));
            }
            for (key, value) in file_evals.into_iter() {
                if file_metrics.contains_key(&key) {
                    bail!("Duplicate metric name {} in file {}", key, namespace);
                }
                file_metrics.insert(
                    key,
                    ValueSource::try_from_expression_with_namespace(&value, namespace)?,
                );
            }
            metrics.insert(namespace.clone(), file_metrics);
            actions.insert(namespace.clone(), file_actions);
            tests.insert(namespace.clone(), file_tests);
        }

        Ok(ParseResult { actions, metrics, tests })
    }

    pub fn all_selectors(&self) -> Vec<String> {
        let mut result = Vec::new();
        for (_, metric_set) in self.metrics.iter() {
            for (_, value_source) in metric_set.iter() {
                if let Metric::Selector(selectors) = &value_source.metric {
                    for selector in selectors {
                        result.push(selector.full_selector.to_owned());
                    }
                }
            }
        }
        result
    }

    pub fn validate(&self) -> Result<(), Error> {
        validate(self)
    }

    pub fn reset_state(&self) {
        for (_, metric_set) in self.metrics.iter() {
            for (_, value_source) in metric_set.iter() {
                *value_source.cached_value.borrow_mut() = None;
            }
        }

        for (_, action_set) in self.actions.iter() {
            for (_, action) in action_set.iter() {
                match action {
                    Action::Alert(alert) => {
                        *alert.trigger.cached_value.borrow_mut() = None;
                    }
                    Action::Gauge(gauge) => {
                        *gauge.value.cached_value.borrow_mut() = None;
                    }
                    Action::Snapshot(snapshot) => {
                        *snapshot.trigger.cached_value.borrow_mut() = None;
                        *snapshot.repeat.cached_value.borrow_mut() = None;
                    }
                }
            }
        }
    }
}

/// A value which directs how to include Actions based on their tags.
pub enum ActionTagDirective {
    /// Include all of the Actions in the Config
    AllowAll,

    /// Only include the Actions which match the given tags
    Include(Vec<String>),

    /// Include all tags excluding the given tags
    Exclude(Vec<String>),
}

impl ActionTagDirective {
    /// Creates a new ActionTagDirective based on the following rules,
    ///
    /// - AllowAll iff tags is empty and exclude_tags is empty.
    /// - Include if tags is not empty and exclude_tags is empty.
    /// - Include if tags is not empty and exclude_tags is not empty, in this
    ///   situation the exclude_ags will be ignored since include implies excluding
    ///   all other tags.
    /// - Exclude iff tags is empty and exclude_tags is not empty.
    pub fn from_tags(tags: Vec<String>, exclude_tags: Vec<String>) -> ActionTagDirective {
        match (tags.is_empty(), exclude_tags.is_empty()) {
            // tags are not empty
            (false, _) => ActionTagDirective::Include(tags),
            // tags are empty, exclude_tags are not empty
            (true, false) => ActionTagDirective::Exclude(exclude_tags),
            _ => ActionTagDirective::AllowAll,
        }
    }
}

/// Exfiltrates the actions in the ActionsSchema.
///
/// This method will enumerate the actions in the ActionsSchema and determine
/// which Actions are included based on the directive. Actions only contain a
/// single tag so an Include directive implies that all other tags should be
/// excluded and an Exclude directive implies that all other tags should be
/// included.
pub(crate) fn filter_actions(
    actions: ActionsSchema,
    action_directive: &ActionTagDirective,
) -> ActionsSchema {
    match action_directive {
        ActionTagDirective::AllowAll => actions,
        ActionTagDirective::Include(tags) => actions
            .into_iter()
            .filter(|(_, a)| match &a.get_tag() {
                Some(tag) => tags.contains(tag),
                None => false,
            })
            .collect(),
        ActionTagDirective::Exclude(tags) => actions
            .into_iter()
            .filter(|(_, a)| match &a.get_tag() {
                Some(tag) => !tags.contains(tag),
                None => true,
            })
            .collect(),
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::act::{Action, Alert, Severity},
        anyhow::Error,
        fidl_fuchsia_feedback::MAX_CRASH_SIGNATURE_LENGTH,
        maplit::hashmap,
        std::convert::TryFrom,
    };

    // initialize() will be tested in the integration test: "fx test triage_lib_test"
    // TODO(cphoenix) - set up dirs under test/ and test initialize() here.

    #[fuchsia::test]
    fn inspect_data_from_works() -> Result<(), Error> {
        assert!(InspectFetcher::try_from("foo").is_err(), "'foo' isn't valid JSON");
        assert!(InspectFetcher::try_from(r#"{"a":5}"#).is_err(), "Needed an array");
        assert!(InspectFetcher::try_from("[]").is_ok(), "A JSON array should have worked");
        Ok(())
    }

    #[fuchsia::test]
    fn action_tag_directive_from_tags_allow_all() {
        let result = ActionTagDirective::from_tags(vec![], vec![]);
        match result {
            ActionTagDirective::AllowAll => (),
            _ => panic!("failed to create correct ActionTagDirective"),
        }
    }

    #[fuchsia::test]
    fn action_tag_directive_from_tags_include() {
        let result =
            ActionTagDirective::from_tags(vec!["t1".to_string(), "t2".to_string()], vec![]);
        match result {
            ActionTagDirective::Include(tags) => {
                assert_eq!(tags, vec!["t1".to_string(), "t2".to_string()])
            }
            _ => panic!("failed to create correct ActionTagDirective"),
        }
    }

    #[fuchsia::test]
    fn action_tag_directive_from_tags_include_override_exclude() {
        let result = ActionTagDirective::from_tags(
            vec!["t1".to_string(), "t2".to_string()],
            vec!["t3".to_string()],
        );
        match result {
            ActionTagDirective::Include(tags) => {
                assert_eq!(tags, vec!["t1".to_string(), "t2".to_string()])
            }
            _ => panic!("failed to create correct ActionTagDirective"),
        }
    }

    #[fuchsia::test]
    fn action_tag_directive_from_tags_exclude() {
        let result =
            ActionTagDirective::from_tags(vec![], vec!["t1".to_string(), "t2".to_string()]);
        match result {
            ActionTagDirective::Exclude(tags) => {
                assert_eq!(tags, vec!["t1".to_string(), "t2".to_string()])
            }
            _ => panic!("failed to create correct ActionTagDirective"),
        }
    }

    // helper macro to create an ActionsSchema
    macro_rules! actions_schema {
        ( $($key:expr => $contents:expr, $tag:expr),+ ) => {
            {
                let mut m =  ActionsSchema::new();
                let trigger =  ValueSource::try_from_expression_with_default_namespace("a_trigger").unwrap();

                $(
                    let action = Action::Alert(Alert {
                        trigger: trigger.clone(),
                        print: $contents.to_string(),
                        tag: $tag,
                        file_bug: None,
                        severity: Severity::Warning,
                    });
                    m.insert($key.to_string(), action);
                )+
                m
            }
        }
    }

    // helper macro to create an ActionsSchema
    macro_rules! assert_has_action {
        ($result:expr, $key:expr, $contents:expr) => {
            match $result.get(&$key.to_string()) {
                Some(Action::Alert(a)) => {
                    assert_eq!(a.print, $contents.to_string());
                }
                _ => {
                    assert!(false);
                }
            }
        };
    }

    #[fuchsia::test]
    fn filter_actions_allow_all() {
        let result = filter_actions(
            actions_schema! {
                "no_tag" => "foo", None,
                "tagged" => "bar", Some("tag".to_string())
            },
            &ActionTagDirective::AllowAll,
        );
        assert_eq!(result.len(), 2);
    }

    #[fuchsia::test]
    fn filter_actions_include_one_tag() {
        let result = filter_actions(
            actions_schema! {
                "1" => "p1", Some("ignore".to_string()),
                "2" => "p2", Some("tag".to_string()),
                "3" => "p3", Some("tag".to_string())
            },
            &ActionTagDirective::Include(vec!["tag".to_string()]),
        );
        assert_eq!(result.len(), 2);
        assert_has_action!(result, "2", "p2");
        assert_has_action!(result, "3", "p3");
    }

    #[fuchsia::test]
    fn filter_actions_include_many_tags() {
        let result = filter_actions(
            actions_schema! {
                "1" => "p1", Some("ignore".to_string()),
                "2" => "p2", Some("tag1".to_string()),
                "3" => "p3", Some("tag2".to_string()),
                "4" => "p4", Some("tag2".to_string())
            },
            &ActionTagDirective::Include(vec!["tag1".to_string(), "tag2".to_string()]),
        );
        assert_eq!(result.len(), 3);
        assert_has_action!(result, "2", "p2");
        assert_has_action!(result, "3", "p3");
        assert_has_action!(result, "4", "p4");
    }

    #[fuchsia::test]
    fn filter_actions_exclude_one_tag() {
        let result = filter_actions(
            actions_schema! {
                "1" => "p1", Some("ignore".to_string()),
                "2" => "p2", Some("tag".to_string()),
                "3" => "p3", Some("tag".to_string())
            },
            &ActionTagDirective::Exclude(vec!["tag".to_string()]),
        );
        assert_eq!(result.len(), 1);
        assert_has_action!(result, "1", "p1");
    }

    #[fuchsia::test]
    fn filter_actions_exclude_many() {
        let result = filter_actions(
            actions_schema! {
                "1" => "p1", Some("ignore".to_string()),
                "2" => "p2", Some("tag1".to_string()),
                "3" => "p3", Some("tag2".to_string()),
                "4" => "p4", Some("tag2".to_string())
            },
            &ActionTagDirective::Exclude(vec!["tag1".to_string(), "tag2".to_string()]),
        );
        assert_eq!(result.len(), 1);
        assert_has_action!(result, "1", "p1");
    }

    #[fuchsia::test]
    fn filter_actions_include_does_not_include_empty_tag() {
        let result = filter_actions(
            actions_schema! {
                "1" => "p1", None,
                "2" => "p2", Some("tag".to_string())
            },
            &ActionTagDirective::Include(vec!["tag".to_string()]),
        );
        assert_eq!(result.len(), 1);
        assert_has_action!(result, "2", "p2");
    }

    #[fuchsia::test]
    fn filter_actions_exclude_does_include_empty_tag() {
        let result = filter_actions(
            actions_schema! {
                "1" => "p1", None,
                "2" => "p2", Some("tag".to_string())
            },
            &ActionTagDirective::Exclude(vec!["tag".to_string()]),
        );
        assert_eq!(result.len(), 1);
        assert_has_action!(result, "1", "p1");
    }

    #[fuchsia::test]
    fn select_section_parsing() {
        let config_result = ConfigFileSchema::try_from(
            r#"
        {
            select: {
                a: ["INSPECT:core:root:prop"],
                b: "INSPECT:core:root:prop",
                c: ["INSPECT:core:root:prop",
                    "INSPECT:core:root:prop2",
                    "INSPECT:core:root:prop3"],
            }
        }
        "#
            .to_string(),
        );
        assert_eq!(
            3,
            config_result.expect("parse json").file_selectors.expect("has selectors").len()
        );

        let config_result = ConfigFileSchema::try_from(
            r#"
        {
            select: {
                a: ["INSPECT:core:root:prop", 1],
            }
        }
        "#
            .to_string(),
        );
        assert!(format!("{}", config_result.expect_err("parsing should fail"))
            .contains("expected a string"));

        let config_result = ConfigFileSchema::try_from(
            r#"
        {
            select: {
                a: [],
            }
        }
        "#
            .to_string(),
        );
        assert!(format!("{}", config_result.expect_err("parsing should fail"))
            .contains("expected at least one selector"));
    }

    #[fuchsia::test]
    fn all_selectors_works() {
        macro_rules! s {
            ($s:expr) => {
                $s.to_string()
            };
        }
        let file_map = hashmap![
            s!("file1") => s!(r#"{ select: {selector1: "INSPECT:name:path:label"}}"#),
            s!("file2") =>
                s!(r#"
                    { select: {
                        selector1: "INSPECT:word:stuff:identifier",
                        selector2: ["INSPECT:a:b:c", "INSPECT:d:e:f"],
                      },
                      eval: {e: "2+2"} }"#),
        ];
        let parse = ParseResult::new(&file_map, &ActionTagDirective::AllowAll).unwrap();
        let selectors = parse.all_selectors();
        assert_eq!(selectors.len(), 4);
        assert!(selectors.contains(&s!("INSPECT:name:path:label")));
        assert!(selectors.contains(&s!("INSPECT:word:stuff:identifier")));
        assert!(selectors.contains(&s!("INSPECT:a:b:c")));
        assert!(selectors.contains(&s!("INSPECT:d:e:f")));
        // Internal logic test: Make sure we're not returning eval entries.
        assert!(!selectors.contains(&s!("2+2")));
    }

    #[fuchsia::test]
    fn too_long_signature_rejected() {
        macro_rules! s {
            ($s:expr) => {
                $s.to_string()
            };
        }
        // {:a<1$} means pad the interpolated arg with "a" to N chars, where N is from parameter 1.
        let signature_ok_config = format!(
            r#"{{
                act: {{ foo: {{
                    type: "Snapshot",
                    repeat: "1",
                    trigger: "1>0",
                    signature: "{:a<1$}",
                }} }}
            }} "#,
            "", // Empty string for "aaaa..." padding
            MAX_CRASH_SIGNATURE_LENGTH as usize
        );
        let signature_too_long_config = format!(
            r#"{{
                act: {{ foo: {{
                    type: "Snapshot",
                    repeat: "1",
                    trigger: "1>0",
                    signature: "{:a<1$}",
                }} }}
            }} "#,
            "", // Empty string for "aaaa..." padding
            MAX_CRASH_SIGNATURE_LENGTH as usize + 1
        );
        let file_map_ok = hashmap![s!("file") => signature_ok_config];
        let file_map_err = hashmap![s!("file") => signature_too_long_config];
        assert!(ParseResult::new(&file_map_ok, &ActionTagDirective::AllowAll).is_ok());
        assert!(ParseResult::new(&file_map_err, &ActionTagDirective::AllowAll).is_err());
    }
}
