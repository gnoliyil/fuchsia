// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{Error, FfxContext, MetricsSession, Result};
use anyhow::Context;
use argh::FromArgs;
use errors::ffx_error;
use ffx_config::{EnvironmentContext, FfxConfigBacked};
use ffx_daemon_proxy::Injection;
use ffx_target::TargetKind;
use ffx_writer::Format;
use hoist::Hoist;
use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    time::Duration,
};

pub use ffx_daemon_proxy::DaemonVersionCheck;

/// The environment variable name used for overriding the command name in help
/// output.
const FFX_WRAPPER_INVOKE: &'static str = "FFX_WRAPPER_INVOKE";

const FASTBOOT_INLINE_TARGET: &str = "ffx.fastboot.inline_target";

#[derive(Clone, Debug, PartialEq)]
/// The relevant argument and environment variables necessary to parse or
/// reconstruct an ffx command invocation.
pub struct FfxCommandLine {
    pub command: Vec<String>,
    pub ffx_args: Vec<String>,
    pub global: Ffx,
}

impl FfxCommandLine {
    /// Construct the command from the system environment ([`std::env::args`] and [`std::env::var`]), using
    /// the FFX_WRAPPER_INVOKE environment variable to obtain the `wrapper_name`, if present. See [`FfxCommand::new`]
    /// for more information.
    pub fn from_env() -> Result<Self> {
        let argv = Vec::from_iter(std::env::args());
        let wrapper_name = std::env::var(FFX_WRAPPER_INVOKE).ok();
        Self::new(wrapper_name.as_deref(), &argv)
    }

    /// Extract the command name from the given argument list, allowing for an overridden command name
    /// from a wrapper invocation so we provide useful information to the user. If the override has spaces, it will
    /// be split into multiple commands.
    pub fn new(wrapper_name: Option<&str>, argv: &[impl AsRef<str>]) -> Result<Self> {
        let mut args = argv.iter().map(AsRef::as_ref);
        let arg0 = args.next().context("No first argument in argument vector").bug()?;
        let args = Vec::from_iter(args);
        let command =
            wrapper_name.map_or_else(|| vec![Self::base_cmd(&arg0)], |s| s.split(" ").collect());
        let global =
            Ffx::from_args(&command, &args).map_err(|err| Error::from_early_exit(&command, err))?;
        // the ffx args are the ones not including those captured by the ffx struct's remain vec.
        let ffx_args_len = args.len() - global.subcommand.len();
        let ffx_args = args.into_iter().take(ffx_args_len).map(str::to_owned).collect();
        let command = command.into_iter().map(str::to_owned).collect();
        Ok(Self { command, ffx_args, global })
    }

    /// Creates a string of the ffx part of the command, but with user-supplied parameter values removed
    /// for analytics. This only contains the top-level flags before any subcommands have been
    /// entered.
    pub fn redact_ffx_cmd(&self) -> Vec<String> {
        Ffx::redact_arg_values(
            &Vec::from_iter(self.cmd_iter()),
            &Vec::from_iter(self.ffx_args_iter()),
        )
        .expect("Already parsed args should be redactable")
    }

    /// Redacts the full command line using type `C` to decide how to redact the subcommand arguments.
    ///
    /// May panic if you try to use the wrong type `C`, so you should only use this after you've
    /// successfully parsed the arguments. That's why this takes a ref to the command struct in
    /// `_cmd` argument even though it doesn't use it, to make sure you've parsed it first.
    pub fn redact_subcmd<C: FromArgs>(&self, _cmd: &C) -> Vec<String> {
        let mut args = self.redact_ffx_cmd();
        let tool_cmd = Vec::from_iter(self.subcmd_iter().take(1));
        let tool_args = Vec::from_iter(self.subcmd_iter().skip(1));
        args.append(
            &mut C::redact_arg_values(&tool_cmd, &tool_args)
                .expect("Already parsed command line should redact ok"),
        );
        args
    }

    /// This produces an error type that will print help appropriate help output
    /// for what the command line looks like, and do the appropriate metrics
    /// logic.
    ///
    /// Note that both the Ok() and Err() returns of this are Errors. The Ok
    /// result is the proper help output, while the other kind of error is an
    /// error on metrics submission.
    pub async fn no_handler_help<T: crate::ToolSuite>(
        &self,
        metrics: MetricsSession,
        suite: &T,
    ) -> Result<Error> {
        metrics.print_notice(&mut std::io::stderr()).await?;

        let subcmd_name = self.global.subcommand.first();
        let help_err = match subcmd_name {
            Some(name) => {
                let mut output =
                    format!("Unknown ffx tool `{name}`. Did you mean one of the following?\n\n");
                suite.print_command_list(&mut output).await.ok();
                let code = 1;
                Error::Help { command: self.command.clone(), output, code }
            }
            None => {
                let help_err = Ffx::from_args(&Vec::from_iter(self.cmd_iter()), &["help"])
                    .expect_err("argh should always return help from a help command");
                let mut output = help_err.output;
                let code = help_err.status.map_or(1, |_| 0);
                suite.print_command_list(&mut output).await.ok();
                Error::Help { command: self.command.clone(), output, code }
            }
        };
        // construct a 'sanitized' argument list that includes an indication of whether
        // it was just no arguments passed or an unknown subtool.
        let redacted: Vec<_> = self
            .redact_ffx_cmd()
            .into_iter()
            .chain(subcmd_name.map(|_| "<unknown-subtool>".to_owned()).into_iter())
            .collect();

        metrics.command_finished(help_err.exit_code() == 0, &redacted).await?;
        Ok(help_err)
    }

    /// Returns an iterator of the command part of the command line
    pub fn cmd_iter<'a>(&'a self) -> impl Iterator<Item = &'a str> {
        self.command.iter().map(|s| s.as_str())
    }

    /// Returns an iterator of the command part of the command line
    pub fn ffx_args_iter<'a>(&'a self) -> impl Iterator<Item = &'a str> {
        self.ffx_args.iter().map(|s| s.as_str())
    }

    /// Returns an iterator of the subcommand and its arguments
    pub fn subcmd_iter<'a>(&'a self) -> impl Iterator<Item = &'a str> {
        self.global.subcommand.iter().map(String::as_str)
    }

    /// Returns an iterator of the whole command line
    pub fn all_iter<'a>(&'a self) -> impl Iterator<Item = &'a str> {
        self.cmd_iter().chain(self.ffx_args_iter()).chain(self.subcmd_iter())
    }

    /// Extract the base cmd from a path
    fn base_cmd(path: &str) -> &str {
        std::path::Path::new(path).file_name().map(|s| s.to_str()).flatten().unwrap_or(path)
    }
}

#[derive(Clone, Default, FfxConfigBacked, FromArgs, Debug, PartialEq)]
/// Fuchsia's developer tool
pub struct Ffx {
    #[argh(option, short = 'c')]
    /// override configuration values (key=value or json)
    pub config: Vec<String>,

    #[argh(option, short = 'e')]
    /// override the path to the environment configuration file (file path)
    pub env: Option<String>,

    #[argh(option)]
    /// produce output for a machine in the specified format; available formats: "json",
    /// "json-pretty"
    pub machine: Option<Format>,

    #[argh(option)]
    /// create a stamp file at the given path containing the exit code
    pub stamp: Option<String>,

    #[argh(option, short = 't')]
    #[ffx_config_default("target.default")]
    /// apply operations across single or multiple targets
    pub target: Option<String>,

    #[argh(option, short = 'T')]
    #[ffx_config_default(key = "proxy.timeout_secs", default = "1.0")]
    /// override default proxy timeout
    pub timeout: Option<f64>,

    #[argh(option, short = 'l', long = "log-level")]
    #[ffx_config_default(key = "log.level", default = "Debug")]
    /// sets the log level for ffx output (default = Debug). Other possible values are Info, Error,
    /// Warn, and Trace. Can be persisted via log.level config setting.
    pub log_level: Option<String>,

    #[argh(option, long = "isolate-dir")]
    /// turn on isolation mode using the given directory to isolate all config and socket files into
    /// the specified directory. This overrides the FFX_ISOLATE_DIR env variable, which can also put
    /// ffx into this mode.
    pub isolate_dir: Option<PathBuf>,

    #[argh(switch, short = 'v', long = "verbose")]
    /// logs ffx output to stdio according to log level
    pub verbose: bool,

    #[argh(positional, greedy)]
    pub subcommand: Vec<String>,
}

impl Ffx {
    pub fn load_context(&self) -> Result<EnvironmentContext, anyhow::Error> {
        // Configuration initialization must happen before ANY calls to the config (or the cache won't
        // properly have the runtime parameters.
        let overrides = self.runtime_config_overrides();
        let runtime_args = ffx_config::runtime::populate_runtime(&*self.config, overrides)?;
        let env_path = self.env.as_ref().map(PathBuf::from);

        // If we're given an isolation setting, use that. Otherwise do a normal detection of the environment.
        match (self, std::env::var_os("FFX_ISOLATE_DIR")) {
            (Ffx { isolate_dir: Some(path), .. }, _) => Ok(EnvironmentContext::isolated(
                path.to_path_buf(),
                HashMap::from_iter(std::env::vars()),
                runtime_args,
                env_path,
            )),
            (_, Some(path_str)) => Ok(EnvironmentContext::isolated(
                PathBuf::from(path_str),
                HashMap::from_iter(std::env::vars()),
                runtime_args,
                env_path,
            )),
            _ => EnvironmentContext::detect(runtime_args, &std::env::current_dir()?, env_path)
                .map_err(|e| ffx_error!(e).into()),
        }
    }

    pub async fn initialize_overnet(
        &self,
        hoist_cache_dir: &Path,
        router_interval: Option<Duration>,
        daemon_check: DaemonVersionCheck,
    ) -> Result<Injection> {
        // todo(fxb/108692) we should get this in the environment context instead and leave the global
        // hoist() unset for ffx but I'm leaving the last couple uses of it in place for the sake of
        // avoiding complicated merge conflicts with isolation. Once we're ready for that, this should be
        // `let Hoist = hoist::Hoist::new()...`
        let hoist = hoist::init_hoist_with(Hoist::with_cache_dir_maybe_router(
            hoist_cache_dir,
            router_interval,
        )?)
        .context("initializing hoist")?;

        let target = match self.target().await? {
            Some(t) => {
                if ffx_config::get(FASTBOOT_INLINE_TARGET).await.unwrap_or(false) {
                    Some(TargetKind::FastbootInline(t))
                } else {
                    Some(TargetKind::Normal(t))
                }
            }
            None => None,
        };

        Ok(Injection::new(daemon_check, hoist.clone(), self.machine, target))
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn cmd_only_last_component() {
        let args = ["test/things/ffx", "--verbose"].map(String::from);
        let cmd_line = FfxCommandLine::new(None, &args).expect("Command line should parse");
        assert_eq!(cmd_line.command, vec!["ffx"]);
        assert_eq!(cmd_line.ffx_args, vec!["--verbose"]);
    }

    #[test]
    fn cmd_override_invoke() {
        let args = ["test/things/ffx", "--verbose"].map(String::from);
        let cmd_line =
            FfxCommandLine::new(Some("tools/ffx"), &args).expect("Command line should parse");
        assert_eq!(cmd_line.command, vec!["tools/ffx"]);
        assert_eq!(cmd_line.ffx_args, vec!["--verbose"]);
    }

    #[test]
    fn cmd_override_multiple_terms_invoke() {
        let args = ["test/things/ffx", "--verbose"].map(String::from);
        let cmd_line =
            FfxCommandLine::new(Some("fx ffx"), &args).expect("Command line should parse");
        assert_eq!(cmd_line.command, vec!["fx", "ffx"]);
        assert_eq!(cmd_line.ffx_args, vec!["--verbose"]);
    }

    /// A subcommand
    #[derive(FromArgs, Default)]
    #[argh(subcommand, name = "subcommand")]
    #[allow(unused)]
    struct TestCmd {
        /// an argument
        #[argh(switch)]
        arg: bool,
        /// another argument
        #[argh(option)]
        stuff: String,
    }

    #[test]
    fn redact_ffx_args() {
        let args = ["ffx", "-v", "--env", "boom", "subcommand", "--arg"];
        let cmd_line = FfxCommandLine::new(None, &args).expect("Command line should parse");
        assert_eq!(cmd_line.command, vec!["ffx"]);
        assert_eq!(cmd_line.ffx_args, vec!["-v", "--env", "boom"]);
        assert_eq!(cmd_line.redact_ffx_cmd(), vec!["ffx", "--env", "-v"]);
    }

    #[test]
    fn redact_subcmd_args() {
        let args = ["ffx", "-v", "--env", "boom", "subcommand", "--arg", "--stuff", "wee"];
        let cmd_line = FfxCommandLine::new(None, &args).expect("Command line should parse");
        assert_eq!(cmd_line.global.subcommand, vec!["subcommand", "--arg", "--stuff", "wee"]);
        assert_eq!(
            cmd_line.redact_subcmd(&TestCmd::default()),
            vec!["ffx", "--env", "-v", "subcommand", "--arg", "--stuff"]
        );
    }
}
