// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::process::ExitStatus;

use fidl_fuchsia_developer_ffx::{
    DaemonError, OpenTargetError, TargetConnectionError, TunnelError,
};

/// Re-exported libraries for macros
#[doc(hidden)]
pub mod macro_deps {
    pub use anyhow;
}

/// The ffx main function expects a anyhow::Result from ffx plugins. If the Result is an Err it be
/// downcast to FfxError, and if successful this error is presented as a user-readable error. All
/// other error types are printed with full context and a BUG prefix, guiding the user to file bugs
/// to improve the error condition that they have experienced, with a goal to maximize actionable
/// errors over time.
// TODO(fxbug.dev/57592): consider extending this to allow custom types from plugins.
#[derive(thiserror::Error, Debug)]
pub enum FfxError {
    #[error("{}", .0)]
    Error(#[source] anyhow::Error, i32 /* Error status code */),

    #[error("{}", match .err {
        DaemonError::TargetCacheError => format!("Target {} could not be looked up in the cache due to an unspecified error. Retry your request, and if that fails run `ffx doctor`.", target_string(.target, .is_default_target)),
        DaemonError::TargetStateError => format!("Target {} is not in a state capable of the requested operation. Inspect `ffx target list` to determine if it is in the expected state.", target_string(.target, .is_default_target)),
        DaemonError::RcsConnectionError => format!("Target {} was not reachable. Run `ffx doctor` for diagnostic information.", target_string(.target, .is_default_target)),
        DaemonError::Timeout => format!("Timeout attempting to reach target {}", target_string(.target, .is_default_target)),
        DaemonError::TargetCacheEmpty => format!("No devices found."),
        DaemonError::TargetAmbiguous => format!("Target specification {} matched multiple targets. Use `ffx target list` to list known targets, and use a more specific matcher.", target_string(.target, .is_default_target)),
        DaemonError::TargetNotFound => format!("Target {} was not found.", target_string(.target, .is_default_target)),
        DaemonError::TargetInFastboot => format!("Target {} was found in Fastboot. Reboot or flash the target to continue.", target_string(.target, .is_default_target)),
        DaemonError::NonFastbootDevice => format!("Target {} was found, but is not in Fastboot, please boot the target into Fastboot to continue.", target_string(.target, .is_default_target)),
        DaemonError::ProtocolNotFound => "The requested ffx service was not found. Run `ffx doctor --restart-daemon`.".to_string(),
        DaemonError::ProtocolOpenError => "The requested ffx service failed to open. Run `ffx doctor --restart-daemon`.".to_string(),
        DaemonError::BadProtocolRegisterState => "The requested service could not be registered. Run `ffx doctor --restart-daemon`.".to_string(),
        DaemonError::TargetInZedboot => format!("Target {} was found in Zedboot. Reboot the target to continue.", target_string(.target, .is_default_target)),
    })]
    DaemonError { err: DaemonError, target: Option<String>, is_default_target: bool },

    #[error("{}", match .err {
        OpenTargetError::QueryAmbiguous => format!("Target specification {} matched multiple targets. Use `ffx target list` to list known targets, and use a more specific matcher.", target_string(.target, .is_default_target)),
        OpenTargetError::TargetNotFound => format!("Target specification {} was not found. Use `ffx target list` to list known targets, and use a different matcher.", target_string(.target, .is_default_target))
    })]
    OpenTargetError { err: OpenTargetError, target: Option<String>, is_default_target: bool },

    #[error("{}", match .err {
        TunnelError::CouldNotListen => "Could not establish a host-side TCP listen socket".to_string(),
        TunnelError::TargetConnectFailed => "Couldn not connect to target to establish a tunnel".to_string(),
    })]
    TunnelError { err: TunnelError, target: Option<String>, is_default_target: bool },

    #[error("{}", format!("No target with matcher {} was found.\n\n* Use `ffx target list` to verify the state of connected devices.\n* Use the SERIAL matcher with the --target (-t) parameter to explicity match a device.", target_string(.target, .is_default_target)))]
    FastbootError { target: Option<String>, is_default_target: bool },

    #[error("{}", match .err {
        TargetConnectionError::PermissionDenied => format!("Could not establish SSH connection to the target {}: Permission denied.", target_string(.target, .is_default_target)),
        TargetConnectionError::ConnectionRefused => format!("Could not establish SSH connection to the target {}: Connection refused.", target_string(.target, .is_default_target)),
        TargetConnectionError::UnknownNameOrService => format!("Could not establish SSH connection to the target {}: Unknown name or service.", target_string(.target, .is_default_target)),
        TargetConnectionError::Timeout => format!("Could not establish SSH connection to the target {}: Timed out awaiting connection.", target_string(.target, .is_default_target)),
        TargetConnectionError::KeyVerificationFailure => format!("Could not establish SSH connection to the target {}: Key verification failed.", target_string(.target, .is_default_target)),
        TargetConnectionError::NoRouteToHost => format!("Could not establish SSH connection to the target {}: No route to host.", target_string(.target, .is_default_target)),
        TargetConnectionError::NetworkUnreachable => format!("Could not establish SSH connection to the target {}: Network unreachable.", target_string(.target, .is_default_target)),
        TargetConnectionError::InvalidArgument => format!("Could not establish SSH connection to the target {}: Invalid argument. Please check the address of the target you are attempting to add.", target_string(.target, .is_default_target)),
        TargetConnectionError::UnknownError => format!("Could not establish SSH connection to the target {}. {}. Report the error to the FFX team at https://fxbug.dev/new/ffx+User+Bug", target_string(.target, .is_default_target), .logs.as_ref().map(|s| s.as_str()).unwrap_or("As-yet unknown error. Please refer to the logs at `ffx config get log.dir` and look for 'Unknown host-pipe error received'")),
        TargetConnectionError::FidlCommunicationError => format!("Connection was established to {}, but FIDL communication to the Remote Control Service failed. It may help to try running the command again. If this problem persists, please open a bug at https://fxbug.dev/new/ffx+Users+Bug", target_string(.target, .is_default_target)),
        TargetConnectionError::RcsConnectionError => format!("Connection was established to {}, but the Remote Control Service failed initiating a test connection. It may help to try running the command again. If this problem persists, please open a bug at https://fxbug.dev/new/ffx+Users+Bug", target_string(.target, .is_default_target)),
        TargetConnectionError::FailedToKnockService => format!("Connection was established to {}, but the Remote Control Service test connection was dropped prematurely. It may help to try running the command again. If this problem persists, please open a bug at https://fxbug.dev/new/ffx+Users+Bug", target_string(.target, .is_default_target)),
        TargetConnectionError::TargetIncompatible => format!("{}.", .logs.as_ref().map(|s| s.as_str()).unwrap_or(format!("ffx revision {:#X} is not compatible with the target. Unable to determine target ABI revision", version_history::LATEST_VERSION.abi_revision.0).as_str())),
    })]
    TargetConnectionError {
        err: TargetConnectionError,
        target: Option<String>,
        is_default_target: bool,
        logs: Option<String>,
    },
    #[error("Testing Error")]
    TestingError, // this is here to be used in tests for verifying errors are translated properly.
}

pub fn target_string(matcher: &Option<String>, is_default: &bool) -> String {
    let non_empty_matcher = matcher.as_ref().filter(|s| !s.is_empty());
    format!(
        "\"{}{}\"",
        non_empty_matcher.unwrap_or(&"unspecified".to_string()),
        if *is_default && !non_empty_matcher.is_none() {
            " (default)".to_string()
        } else {
            "".to_string()
        },
    )
}

// Utility macro for constructing a FfxError::Error with a simple error string.
#[macro_export]
macro_rules! ffx_error {
    ($error_message: expr) => {{
        $crate::FfxError::Error($crate::macro_deps::anyhow::anyhow!($error_message), 1)
    }};
    ($fmt:expr, $($arg:tt)*) => {
        $crate::ffx_error!(format!($fmt, $($arg)*));
    };
}

#[macro_export]
macro_rules! ffx_error_with_code {
    ($error_code:expr, $error_message:expr $(,)?) => {{
        $crate::FfxError::Error($crate::macro_deps::anyhow::anyhow!($error_message), $error_code)
    }};
    ($error_code:expr, $fmt:expr, $($arg:tt)*) => {
        $crate::ffx_error_with_code!($error_code, format!($fmt, $($arg)*));
    };
}

#[macro_export]
macro_rules! ffx_bail {
    ($msg:literal $(,)?) => {
        return Err($crate::ffx_error!($msg).into())
    };
    ($fmt:expr, $($arg:tt)*) => {
        return Err($crate::ffx_error!($fmt, $($arg)*).into());
    };
}

#[macro_export]
macro_rules! ffx_bail_with_code {
    ($code:literal, $msg:literal $(,)?) => {
        return Err($crate::ffx_error_with_code!($code, $msg).into())
    };
    ($code:expr, $fmt:expr, $($arg:tt)*) => {
        return Err($crate::ffx_error_with_code!($code, $fmt, $($arg)*).into());
    };
}

pub trait IntoExitCode {
    fn exit_code(&self) -> i32;
}

pub trait ResultExt: IntoExitCode {
    fn ffx_error<'a>(&'a self) -> Option<&'a FfxError>;
}

impl ResultExt for anyhow::Error {
    fn ffx_error<'a>(&'a self) -> Option<&'a FfxError> {
        self.downcast_ref()
    }
}

impl IntoExitCode for anyhow::Error {
    fn exit_code(&self) -> i32 {
        match self.downcast_ref() {
            Some(FfxError::Error(_, code)) => *code,
            _ => 1,
        }
    }
}

impl IntoExitCode for FfxError {
    fn exit_code(&self) -> i32 {
        match self {
            FfxError::Error(_, code) => *code,
            _ => 1,
        }
    }
}

// so that Result<(), E>::Ok is treated as exit code 0.
impl IntoExitCode for () {
    fn exit_code(&self) -> i32 {
        0
    }
}

impl IntoExitCode for ExitStatus {
    fn exit_code(&self) -> i32 {
        self.code().unwrap_or(0)
    }
}

impl<T, E> ResultExt for Result<T, E>
where
    T: IntoExitCode,
    E: ResultExt,
{
    fn ffx_error<'a>(&'a self) -> Option<&'a FfxError> {
        match self {
            Ok(_) => None,
            Err(ref err) => err.ffx_error(),
        }
    }
}

impl<T, E> IntoExitCode for Result<T, E>
where
    T: IntoExitCode,
    E: ResultExt,
{
    fn exit_code(&self) -> i32 {
        match self {
            Ok(code) => code.exit_code(),
            Err(err) => err.exit_code(),
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use anyhow::anyhow;
    use assert_matches::assert_matches;

    const FFX_STR: &str = "I am an ffx error";
    const ERR_STR: &str = "I am not an ffx error";

    #[test]
    fn test_ffx_result_extension() {
        let err = anyhow::Result::<()>::Err(anyhow!(ERR_STR));
        assert!(err.ffx_error().is_none());

        let err = anyhow::Result::<()>::Err(anyhow::Error::new(ffx_error!(FFX_STR)));
        assert_matches!(err.ffx_error(), Some(FfxError::Error(_, _)));
    }

    #[test]
    fn test_result_ext_exit_code_arbitrary_error() {
        let err = Result::<(), _>::Err(anyhow!(ERR_STR));
        assert_eq!(err.exit_code(), 1);
    }

    #[test]
    fn test_daemon_error_strings_containing_target_name() {
        fn assert_contains_target_name(err: DaemonError) {
            let name: Option<String> = Some("fuchsia-f00d".to_string());
            assert!(format!(
                "{}",
                FfxError::DaemonError { err, target: name.clone(), is_default_target: false }
            )
            .contains(name.as_ref().unwrap()));
        }
        assert_contains_target_name(DaemonError::TargetCacheError);
        assert_contains_target_name(DaemonError::TargetStateError);
        assert_contains_target_name(DaemonError::RcsConnectionError);
        assert_contains_target_name(DaemonError::Timeout);
        assert_contains_target_name(DaemonError::TargetAmbiguous);
        assert_contains_target_name(DaemonError::TargetNotFound);
        assert_contains_target_name(DaemonError::TargetInFastboot);
        assert_contains_target_name(DaemonError::NonFastbootDevice);
        assert_contains_target_name(DaemonError::TargetInZedboot);
    }

    #[test]
    fn test_target_string() {
        assert_eq!(target_string(&None, &false), "\"unspecified\"");
        assert_eq!(target_string(&Some("".to_string()), &false), "\"unspecified\"");
        assert_eq!(target_string(&Some("kittens".to_string()), &false), "\"kittens\"");
        assert_eq!(target_string(&Some("kittens".to_string()), &true), "\"kittens (default)\"");
    }
}
