// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_config::{api::query::SelectMode, ConfigLevel, ConfigQuery, EnvironmentContext};
use ffx_core::ffx_command;
use std::path::PathBuf;

#[ffx_command]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "config",
    description = "View and switch default and user configurations"
)]
pub struct ConfigCommand {
    #[argh(subcommand)]
    pub sub: SubCommand,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand)]
pub enum SubCommand {
    Env(EnvCommand),
    Get(GetCommand),
    Set(SetCommand),
    Remove(RemoveCommand),
    Add(AddCommand),
    Analytics(AnalyticsCommand),
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "set", description = "set config settings")]
pub struct SetCommand {
    #[argh(positional)]
    /// name of the property to set
    pub name: String,

    #[argh(positional, from_str_fn(parse_set_value))]
    /// value to associate with name
    pub value: serde_json::Value,

    #[argh(option, default = "ConfigLevel::User", short = 'l')]
    /// config level. Possible values are "user", "build", "global". Defaults to "user".
    pub level: ConfigLevel,

    #[argh(option, short = 'b')]
    /// an optional build directory to associate the build config provided - used for "build"
    /// configs. If not provided, it may attempt to autodiscover your active build directory.
    pub build_dir: Option<PathBuf>,
}

impl SetCommand {
    pub fn query<'a>(&'a self, ctx: &'a EnvironmentContext) -> ConfigQuery<'a> {
        ConfigQuery::new(
            Some(self.name.as_str()),
            Some(self.level),
            self.build_dir.as_deref().map(|dir| dir.into()),
            SelectMode::default(),
            Some(ctx),
        )
    }
}

#[derive(Debug, PartialEq, Copy, Clone)]
pub enum MappingMode {
    Raw,
    Substitute,
    File,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "get",
    description = "display config values",
    error_code(2, "No value found")
)]
pub struct GetCommand {
    #[argh(positional)]
    /// name of the config property
    pub name: Option<String>,

    #[argh(
        option,
        from_str_fn(parse_mapping_mode),
        default = "MappingMode::Substitute",
        short = 'p'
    )]
    /// how to process results. Possible values are "r/raw", "s/sub/substitute", or "f/file".
    /// Defaults to "substitute". Currently only supported if a name is given.
    pub process: MappingMode,

    #[argh(option, from_str_fn(parse_mode), default = "SelectMode::First", short = 's')]
    /// how to collect results. Possible values are "first" and "all".  Defaults to
    /// "first".  If the value is "first", the first value found in terms of priority is returned.
    /// If the value is "all", all values across all configuration levels are aggregrated and
    /// returned. Currently only supported if a name is given.
    pub select: SelectMode,

    #[argh(option, short = 'b')]
    /// an optional build directory to associate the build config provided - used for "build"
    /// configs. If not provided, it may attempt to autodiscover your active build directory.
    pub build_dir: Option<PathBuf>,
}

impl GetCommand {
    pub fn query<'a>(&'a self, ctx: &'a EnvironmentContext) -> ConfigQuery<'a> {
        ConfigQuery::new(
            self.name.as_deref(),
            None,
            self.build_dir.as_deref().map(|dir| dir.into()),
            self.select,
            Some(ctx),
        )
    }
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "remove",
    description = "remove config for a given level",
    note = "This will remove the entire value for the given name.  If the value is a subtree or \
       array, the entire subtree or array will be removed.  If you want to remove a specific value \
       from an array, consider editing the configuration file directly.  Configuration file \
       locations can be found by running `ffx config env get` command."
)]
pub struct RemoveCommand {
    #[argh(positional)]
    /// name of the config property
    pub name: String,

    #[argh(option, default = "ConfigLevel::User", short = 'l')]
    /// config level. Possible values are "user", "build", "global". Defaults to "user".
    pub level: ConfigLevel,

    #[argh(option, short = 'b')]
    /// an optional build directory to associate the build config provided - used for "build"
    /// configs. If not provided, it may attempt to autodiscover your active build directory.
    pub build_dir: Option<PathBuf>,
}

impl RemoveCommand {
    pub fn query<'a>(&'a self, ctx: &'a EnvironmentContext) -> ConfigQuery<'a> {
        ConfigQuery::new(
            Some(self.name.as_str()),
            Some(self.level),
            self.build_dir.as_deref().map(|dir| dir.into()),
            SelectMode::default(),
            Some(ctx),
        )
    }
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "add",
    description = "add config value the end of an array",
    note = "This will always add to the end of an array.  Adding to a subtree is not supported. \
        If the current value is not an array, it will convert the value to an array.  If you want \
        to insert a value in a different position, consider editing the configuration file \
        directly.  Configuration file locations can be found by running `ffx config env get` \
        command."
)]
pub struct AddCommand {
    #[argh(positional)]
    /// name of the property to set
    pub name: String,

    #[argh(positional)]
    /// value to add to name
    pub value: String,

    #[argh(option, default = "ConfigLevel::User", short = 'l')]
    /// config level. Possible values are "user", "build", "global". Defaults to "user".
    pub level: ConfigLevel,

    #[argh(option, short = 'b')]
    /// an optional build directory to associate the build config provided - used for "build"
    /// configs. If not provided, it may attempt to autodiscover your active build directory.
    pub build_dir: Option<PathBuf>,
}

impl AddCommand {
    pub fn query<'a>(&'a self, ctx: &'a EnvironmentContext) -> ConfigQuery<'a> {
        ConfigQuery::new(
            Some(self.name.as_str()),
            Some(self.level),
            self.build_dir.as_deref().map(|dir| dir.into()),
            SelectMode::default(),
            Some(ctx),
        )
    }
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "env", description = "list environment settings")]
pub struct EnvCommand {
    #[argh(subcommand)]
    pub access: Option<EnvAccessCommand>,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand)]
pub enum EnvAccessCommand {
    Set(EnvSetCommand),
    Get(EnvGetCommand),
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "set", description = "set environment settings")]
pub struct EnvSetCommand {
    #[argh(positional)]
    /// path to the config file for the configuration level provided
    pub file: PathBuf,

    #[argh(option, default = "ConfigLevel::User", short = 'l')]
    /// config level. Possible values are "user", "build", "global". Defaults to "user".
    pub level: ConfigLevel,

    #[argh(option, short = 'b')]
    /// an optional build directory to associate the build config provided - used for "build"
    /// configs. If not provided, it may attempt to autodiscover your active build directory.
    pub build_dir: Option<PathBuf>,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "get", description = "list environment for a given level")]
pub struct EnvGetCommand {
    #[argh(positional)]
    /// config level. Possible values are "user", "build", "global".
    pub level: Option<ConfigLevel>,
}

fn parse_set_value(value: &str) -> Result<serde_json::Value, String> {
    serde_json::from_str(value).or_else(|_| Ok(serde_json::Value::String(value.to_string())))
}

fn parse_mapping_mode(value: &str) -> Result<MappingMode, String> {
    match value {
        "r" | "raw" => Ok(MappingMode::Raw),
        "s" | "sub" | "substitute" => Ok(MappingMode::Substitute),
        "f" | "file" => Ok(MappingMode::File),
        _ => Err(String::from(
            "Unrecognized value. Possible values are \"raw\", \"sub\", or \"file\".",
        )),
    }
}

fn parse_mode(value: &str) -> Result<SelectMode, String> {
    match value {
        "f" | "first" | "first_found" => Ok(SelectMode::First),
        "a" | "all" | "add" | "additive" => Ok(SelectMode::All),
        _ => Err(String::from(
            "Unrecognized value. Possible values are \"first_found\" or \"additive\".",
        )),
    }
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "analytics", description = "enable or disable analytics")]
pub struct AnalyticsCommand {
    #[argh(subcommand)]
    pub sub: AnalyticsControlCommand,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand)]
pub enum AnalyticsControlCommand {
    Enable(AnalyticsEnableCommand),
    Disable(AnalyticsDisableCommand),
    Show(AnalyticsShowCommand),
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "enable", description = "enable analytics")]
pub struct AnalyticsEnableCommand {}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "disable", description = "disable analytics")]
pub struct AnalyticsDisableCommand {}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "show", description = "show analytics")]
pub struct AnalyticsShowCommand {}

#[cfg(test)]
mod tests {
    use super::*;
    const CMD_NAME: &'static [&'static str] = &["config"];

    #[test]
    fn test_env_get() {
        fn check(args: &[&str], expected_level: Option<ConfigLevel>) {
            assert_eq!(
                ConfigCommand::from_args(CMD_NAME, args),
                Ok(ConfigCommand {
                    sub: SubCommand::Env(EnvCommand {
                        access: Some(EnvAccessCommand::Get(EnvGetCommand {
                            level: expected_level,
                        })),
                    })
                })
            )
        }

        let levels = [
            ("build", Some(ConfigLevel::Build)),
            ("user", Some(ConfigLevel::User)),
            ("global", Some(ConfigLevel::Global)),
        ];

        for level_opt in levels.iter() {
            check(&["env", "get", &level_opt.0], level_opt.1);
        }
    }

    #[test]
    fn test_env_set() {
        fn check(args: &[&str], expected_level: ConfigLevel) {
            assert_eq!(
                ConfigCommand::from_args(CMD_NAME, args),
                Ok(ConfigCommand {
                    sub: SubCommand::Env(EnvCommand {
                        access: Some(EnvAccessCommand::Set(EnvSetCommand {
                            level: expected_level,
                            file: "/test/config.json".into(),
                            build_dir: Some("/test/".into()),
                        })),
                    })
                })
            )
        }

        let levels = [
            ("build", ConfigLevel::Build),
            ("user", ConfigLevel::User),
            ("global", ConfigLevel::Global),
        ];

        for level_opt in levels.iter() {
            check(
                &[
                    "env",
                    "set",
                    "/test/config.json",
                    "--level",
                    &level_opt.0,
                    "--build-dir",
                    "/test/",
                ],
                level_opt.1,
            );
        }
    }

    #[test]
    fn test_get() {
        fn check(args: &[&str], expected_key: &str, expected_build_dir: Option<PathBuf>) {
            assert_eq!(
                ConfigCommand::from_args(CMD_NAME, args),
                Ok(ConfigCommand {
                    sub: SubCommand::Get(GetCommand {
                        process: MappingMode::Substitute,
                        select: SelectMode::First,
                        name: Some(expected_key.to_string()),
                        build_dir: expected_build_dir,
                    })
                })
            )
        }

        let key = "test-key";
        let build_dir = "/test/";
        check(&["get", key], key, None);
        check(&["get", key, "--build-dir", build_dir], key, Some(build_dir.into()));
    }

    #[test]
    fn test_set() {
        fn check(
            args: &[&str],
            expected_level: ConfigLevel,
            expected_key: &str,
            expected_value: &serde_json::Value,
            expected_build_dir: Option<PathBuf>,
        ) {
            assert_eq!(
                ConfigCommand::from_args(CMD_NAME, args),
                Ok(ConfigCommand {
                    sub: SubCommand::Set(SetCommand {
                        level: expected_level,
                        name: expected_key.to_string(),
                        value: expected_value.clone(),
                        build_dir: expected_build_dir,
                    })
                })
            )
        }

        let key = "test-key";
        let value = "test-value";
        let value_json = serde_json::Value::String(value.to_string());
        let build_dir = "/test/";
        let levels = [
            ("build", ConfigLevel::Build),
            ("user", ConfigLevel::User),
            ("global", ConfigLevel::Global),
        ];

        for level_opt in levels.iter() {
            check(
                &["set", key, value, "--level", level_opt.0],
                level_opt.1,
                key,
                &value_json,
                None,
            );
            check(
                &["set", key, value, "--level", level_opt.0, "--build-dir", build_dir],
                level_opt.1,
                key,
                &value_json,
                Some(build_dir.into()),
            );
        }
    }

    #[test]
    fn test_set_json() {
        fn check(
            args: &[&str],
            expected_level: ConfigLevel,
            expected_key: &str,
            expected_value: &serde_json::Value,
            expected_build_dir: Option<PathBuf>,
        ) {
            assert_eq!(
                ConfigCommand::from_args(CMD_NAME, args),
                Ok(ConfigCommand {
                    sub: SubCommand::Set(SetCommand {
                        level: expected_level,
                        name: expected_key.to_string(),
                        value: expected_value.clone(),
                        build_dir: expected_build_dir,
                    })
                })
            )
        }

        let key = "test-key";
        let value = "{\"test\": \"test-value\"}";
        let value_json = serde_json::json!({"test": "test-value"});
        let build_dir = "/test/";
        let levels = [
            ("build", ConfigLevel::Build),
            ("user", ConfigLevel::User),
            ("global", ConfigLevel::Global),
        ];

        for level_opt in levels.iter() {
            check(
                &["set", key, value, "--level", level_opt.0],
                level_opt.1,
                key,
                &value_json,
                None,
            );
            check(
                &["set", key, value, "--level", level_opt.0, "--build-dir", build_dir],
                level_opt.1,
                key,
                &value_json,
                Some(build_dir.into()),
            );
        }
    }

    #[test]
    fn test_remove() {
        fn check(
            args: &[&str],
            expected_level: ConfigLevel,
            expected_key: &str,
            expected_build_dir: Option<PathBuf>,
        ) {
            assert_eq!(
                ConfigCommand::from_args(CMD_NAME, args),
                Ok(ConfigCommand {
                    sub: SubCommand::Remove(RemoveCommand {
                        level: expected_level,
                        name: expected_key.to_string(),
                        build_dir: expected_build_dir,
                    })
                })
            )
        }

        let key = "test-key";
        let build_dir = "/test/";
        let levels = [
            ("build", ConfigLevel::Build),
            ("user", ConfigLevel::User),
            ("global", ConfigLevel::Global),
        ];

        for level_opt in levels.iter() {
            check(&["remove", key, "--level", level_opt.0], level_opt.1, key, None);
            check(
                &["remove", key, "--level", level_opt.0, "--build-dir", build_dir],
                level_opt.1,
                key,
                Some(build_dir.into()),
            );
        }
    }
}
