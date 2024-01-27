// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

use crate::PublishOptions;
use fidl_fuchsia_diagnostics::{Interest, Severity};
use std::{collections::HashSet, fmt, marker::PhantomData, sync::Once};
use thiserror::Error;
use tracing::{level_filters::LevelFilter, Event, Level, Subscriber};
use tracing_log::NormalizeEvent;
use tracing_subscriber::{
    fmt::{
        format::{DefaultFields, Writer},
        time::{FormatTime, SystemTime},
        FmtContext, FormatEvent, FormatFields, FormattedFields, MakeWriter,
    },
    registry::LookupSpan,
    FmtSubscriber,
};

/// Tag derived from metadata.
///
/// Unlike tags, metatags are not represented as strings and instead must be resolved from event
/// metadata. This means that they may resolve to different text for different events.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum Metatag {
    /// The location of a span or event.
    ///
    /// The target is typically a module path, but this can be configured by a particular span or
    /// event when it is constructed.
    Target,
}

/// Options to configure a `Publisher`.
pub struct PublisherOptions<'t> {
    pub(crate) interest: Interest,
    pub(crate) metatags: HashSet<Metatag>,
    pub(crate) tags: &'t [&'t str],
    // Just for compatibility with the fuchsia struct which needs a lifetime.
    _lifetime: PhantomData<&'t ()>,
}

impl<'t> Default for PublisherOptions<'t> {
    fn default() -> Self {
        Self {
            interest: Interest::EMPTY,
            metatags: HashSet::new(),
            tags: &[],
            _lifetime: PhantomData,
        }
    }
}

/// Initializes logging. This should be called only once.
pub fn initialize(opts: PublishOptions<'_>) -> Result<(), PublishError> {
    static START: Once = Once::new();
    START.call_once(|| {
        let subscriber = create_subscriber(&opts, std::io::stderr).expect("create subscriber");
        tracing::subscriber::set_global_default(subscriber).expect("set global subscriber");
        if opts.ingest_log_events {
            crate::ingest_log_events().expect("ingest log events");
        }
        if opts.install_panic_hook {
            crate::install_panic_hook();
        }
    });
    Ok(())
}

type HostSubscriber<W> = FmtSubscriber<DefaultFields, HostFormatter, LevelFilter, W>;

fn create_subscriber<W>(opts: &PublishOptions<'_>, w: W) -> Result<HostSubscriber<W>, PublishError>
where
    W: for<'writer> MakeWriter<'writer> + 'static + Send + Sync,
{
    let level = match opts.publisher.interest.min_severity {
        Some(Severity::Trace) => Level::TRACE,
        Some(Severity::Debug) => Level::DEBUG,
        None | Some(Severity::Info) => Level::INFO,
        Some(Severity::Warn) => Level::WARN,
        Some(Severity::Error) | Some(Severity::Fatal) => Level::ERROR,
    };
    let builder = tracing_subscriber::fmt()
        .event_format(HostFormatter {
            tags: opts.publisher.tags.iter().map(|s| s.to_string()).collect(),
            display_module_path: opts.publisher.metatags.contains(&Metatag::Target),
        })
        .with_writer(w)
        .with_max_level(level);
    let subscriber = builder.finish();
    Ok(subscriber)
}

/// Errors arising while forwarding a diagnostics stream to the environment.
#[derive(Debug, Error)]
pub enum PublishError {
    /// Setting the default global [`tracing::Subscriber`] failed.
    #[error("failed to install forwarder as the global default")]
    SetGlobalDefault(#[from] tracing::subscriber::SetGlobalDefaultError),

    /// Installing a forwarder from [`log`] macros to [`tracing`] macros failed.
    #[error("failed to install a forwarder from `log` to `tracing`")]
    InitLogForward(#[from] tracing_log::log_tracer::SetLoggerError),
}

/// Implements a compact formatter which also knows about tags.
struct HostFormatter {
    tags: Vec<String>,
    display_module_path: bool,
}

// This implementation is based on the tracing-subscriber Format<Comnpact> implementation, but
// defaulting some fields and with support for tags.
// TODO: consider supporting colors for Levels, Bold for spans, dimmed for time, etc. like the
// default compact format with the ansi feature enabled.
impl<S, N> FormatEvent<S, N> for HostFormatter
where
    S: Subscriber + for<'a> LookupSpan<'a>,
    N: for<'a> FormatFields<'a> + 'static,
{
    fn format_event(
        &self,
        ctx: &FmtContext<'_, S, N>,
        mut writer: Writer<'_>,
        event: &Event<'_>,
    ) -> fmt::Result {
        let normalized_meta = event.normalized_metadata();
        let meta = normalized_meta.as_ref().unwrap_or_else(|| event.metadata());

        SystemTime::default().format_time(&mut writer)?;
        write!(writer, " {} ", meta.level())?;

        let mut seen = false;
        let span = event.parent().and_then(|id| ctx.span(id)).or_else(|| ctx.lookup_current());
        let scope = span.iter().flat_map(|span| span.scope().from_root());
        for span in scope {
            seen = true;
            write!(writer, "{}:", span.metadata().name())?;
        }
        if seen {
            writer.write_char(' ')?;
        }

        if !self.tags.is_empty() {
            write!(writer, "[{}] ", self.tags.join(", "))?;
        }

        if self.display_module_path {
            write!(writer, "{}: ", meta.target())?;
        }

        ctx.format_fields(writer.by_ref(), event)?;

        let scope = span.iter().flat_map(|span| span.scope());
        for span in scope {
            let exts = span.extensions();
            if let Some(fields) = exts.get::<FormattedFields<N>>() {
                if !fields.is_empty() {
                    write!(writer, " {}", fields.as_str())?;
                }
            }
        }
        writeln!(writer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use regex::Regex;
    use std::{
        io,
        sync::{Arc, Mutex},
    };
    use tracing::{error, info, warn};

    struct MockWriter(Arc<Mutex<Vec<u8>>>);

    impl io::Write for MockWriter {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            self.0.lock().unwrap().write(buf)
        }

        fn flush(&mut self) -> io::Result<()> {
            self.0.lock().unwrap().flush()
        }
    }

    #[test]
    fn minimal_logging() {
        let buffer = Arc::new(Mutex::new(Vec::new()));
        let buf = buffer.clone();
        let s = create_subscriber(&PublishOptions::default(), move || MockWriter(buf.clone()))
            .expect("initialize logs");
        let _s = tracing::subscriber::set_default(s);
        info!(key = 2, "this is a test");
        let buf = buffer.lock().unwrap();
        let re = Regex::new(".+INFO this is a test key=2\n$").unwrap();
        let result = std::str::from_utf8(buf.as_slice()).unwrap();
        assert!(re.is_match(result), "got: {:?}", result);
    }

    #[test]
    fn supports_tags() {
        let buffer = Arc::new(Mutex::new(Vec::new()));
        let buf = buffer.clone();
        let s =
            create_subscriber(&PublishOptions::default().tags(&["hello", "fuchsia"]), move || {
                MockWriter(buf.clone())
            })
            .expect("initialize logs");
        let _s = tracing::subscriber::set_default(s);
        info!(key = 2, "this is a test");
        let buf = buffer.lock().unwrap();
        let re = Regex::new(".+ INFO \\[hello, fuchsia\\] this is a test key=2\n$").unwrap();
        let result = std::str::from_utf8(buf.as_slice()).unwrap();
        assert!(re.is_match(result), "got: {:?}", result);
    }

    #[test]
    fn supports_module_path() {
        let buffer = Arc::new(Mutex::new(Vec::new()));
        let buf = buffer.clone();
        let s = create_subscriber(
            &PublishOptions::default().enable_metatag(Metatag::Target),
            move || MockWriter(buf.clone()),
        )
        .expect("initialize logs");
        let _s = tracing::subscriber::set_default(s);
        warn!(key = 2, "this is a test");
        let buf = buffer.lock().unwrap();
        let re =
            Regex::new(".+WARN diagnostics_log_lib_test::portable::tests: this is a test key=2\n$")
                .unwrap();
        let result = std::str::from_utf8(buf.as_slice()).unwrap();
        assert!(re.is_match(result), "got: {:?}", result);
    }

    #[test]
    fn full_logging() {
        let buffer = Arc::new(Mutex::new(Vec::new()));
        let buf = buffer.clone();
        let s = create_subscriber(
            &PublishOptions::default().enable_metatag(Metatag::Target).tags(&["hello", "fuchsia"]),
            move || MockWriter(buf.clone()),
        )
        .expect("initialize logs");
        let _s = tracing::subscriber::set_default(s);

        error!(key = 2, "this is a test");
        let buf = buffer.lock().unwrap();
        let re = Regex::new(concat!(
            ".+ERROR \\[hello, fuchsia\\] diagnostics_log_lib_test::portable::tests: ",
            "this is a test key=2\n$"
        ))
        .unwrap();
        let result = std::str::from_utf8(buf.as_slice()).unwrap();
        assert!(re.is_match(result), "got: {:?}", result);
    }
}
