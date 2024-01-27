// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(missing_docs)]

//! `log_listener` listens to messages from `fuchsia.logger.Log` and prints them to stdout and/or
//! writes them to disk.

use anyhow::{format_err, Context as _, Error};
use chrono::TimeZone;
use fuchsia_async as fasync;
use fuchsia_syslog_listener as syslog_listener;
use fuchsia_syslog_listener::LogProcessor;
use fuchsia_zircon as zx;
use lazy_static::lazy_static;
use regex::{Captures, Regex};
use selectors::{self, VerboseError};
use std::collections::hash_map::DefaultHasher;
use std::collections::hash_set::HashSet;
use std::collections::HashMap;
use std::env;
use std::fs;
use std::hash::{Hash, Hasher};
use std::io::{self, Write};
use std::path::PathBuf;
use std::process;
use std::thread;
use std::time;

// Include the generated FIDL bindings for the `Logger` service.
use fidl_fuchsia_diagnostics::{Interest, LogInterestSelector, Severity, MAX_LOG_SELECTORS};
use fidl_fuchsia_logger::{
    LogFilterOptions, LogLevelFilter, LogMessage, MAX_TAGS, MAX_TAG_LEN_BYTES,
};

const DEFAULT_FILE_CAPACITY: u64 = 64000;

type Color = &'static str;
static ANSI_RESET: &str = "\x1B[1;0m";
static WHITE_ON_RED: &str = "\x1B[41;37m";
#[allow(dead_code)]
static BLACK: &str = "\x1B[30;1m";
static RED: &str = "\x1B[31;1m";
static GREEN: &str = "\x1B[32;1m";
static YELLOW: &str = "\x1B[33;1m";
static BLUE: &str = "\x1B[34;1m";
#[allow(dead_code)]
static MAGENTA: &str = "\x1B[35;1m";
#[allow(dead_code)]
static CYAN: &str = "\x1B[36;1m";

fn colorize_line(color: Color, line: String) -> String {
    [color, line.as_str(), ANSI_RESET].concat()
}

enum DecoratorMode {
    ColorByLogLevel,
    ColorByKeyword,
}

struct Decorator {
    mode: DecoratorMode,
    is_active: bool,
    log_levels: HashMap<i32, Color>,
    lines: HashMap<String, Color>,
    regex_str_color: String,
    re_color: Option<Regex>,
    regex_grp_color: Vec<RegexGroup>,
    color_components: bool,
}

struct RegexGroup {
    pub grp: String,
    pub color: Color,
}

impl Decorator {
    pub fn new(mode: DecoratorMode) -> Self {
        Decorator {
            mode,
            is_active: false,
            log_levels: HashMap::new(),
            lines: HashMap::new(),
            regex_str_color: String::default(),
            re_color: None,
            regex_grp_color: Vec::new(),
            color_components: false,
        }
    }

    fn fail_if_active(&self) {
        if self.is_active {
            panic!("already active, cannot modify");
        }
    }

    pub fn add_line(&mut self, keyword: String, color: Color) {
        self.fail_if_active();
        self.lines.insert(keyword, color);
    }

    pub fn add_word(&mut self, keyword_regex: String, color: Color) {
        self.fail_if_active();
        let grp_no = self.regex_grp_color.len();
        let grp = format!("g{}", grp_no);
        if self.regex_str_color.len() == 0 {
            self.regex_str_color.push_str(&format!(r#"(?i)(?P<{}>(?:{}))"#, grp, keyword_regex));
        } else {
            self.regex_str_color.push_str(&format!(r#"|(?P<{}>(?:{}))"#, grp, keyword_regex));
        }
        let regex_grp_color = RegexGroup { grp, color };
        self.regex_grp_color.push(regex_grp_color);
    }

    pub fn add_log_level(&mut self, level: LogLevelFilter, color: Color) {
        self.fail_if_active();
        self.log_levels.insert(level as i32, color);
    }

    pub fn init_regex(&mut self) {
        self.re_color = Some(Regex::new(self.regex_str_color.as_str()).unwrap_or_else(|e| {
            panic!("should create regex for {}: {:?}", self.regex_str_color, e)
        }));
    }

    pub fn activate(&mut self) {
        self.is_active = true;
        self.init_regex();
    }

    /// If line contains a keyword, color the entire line
    fn try_colorize_line(&self, line: String, keyword: &str, color: &Color) -> (String, bool) {
        if line.contains(keyword) {
            (colorize_line(color, line), true)
        } else {
            (line, false)
        }
    }

    fn colorize_words(&self, line: String, encompassing_color: Color) -> String {
        match &self.re_color {
            None => line,
            Some(regex) => regex
                .replace_all(line.as_str(), |caps: &Captures<'_>| {
                    for g in &self.regex_grp_color {
                        let grp_match = caps.name(&g.grp);
                        match grp_match {
                            None => continue,
                            Some(keyword) => {
                                let color = &g.color;
                                return [color, keyword.as_str(), ANSI_RESET, encompassing_color]
                                    .concat();
                            }
                        }
                    }
                    panic!("Code should not reach here. please file a bug");
                })
                .to_string(),
        }
    }

    pub fn decorate(&self, level: i32, mut line: String) -> String {
        // TODO(porce): Support styles such as bold, italic, blink
        if !self.is_active {
            return line;
        }

        match self.mode {
            DecoratorMode::ColorByLogLevel => {
                // Use log level to colorize entire line
                if let Some(color) = self.log_levels.get(&level) {
                    colorize_line(color, line)
                } else {
                    line
                }
            }
            DecoratorMode::ColorByKeyword => {
                // Find matching keywords and colorize message
                let mut encompassing_color = "";
                for (keyword, color) in &self.lines {
                    let ret = self.try_colorize_line(line, keyword, &color);
                    line = ret.0;
                    if ret.1 {
                        encompassing_color = color;
                        break;
                    }
                }
                if self.color_components {
                    lazy_static! {
                        static ref RE: Regex = Regex::new(r"(\[([^\[\]]*)\]){1,}\s").unwrap();
                    }
                    if let Some(caps) = RE.captures(&line) {
                        if let Some(name) = caps.get(2) {
                            let mut hasher = DefaultHasher::new();
                            name.as_str().hash(&mut hasher);
                            // Per
                            // https://en.wikipedia.org/wiki/ANSI_escape_code#8-bit,
                            // colors # 225 through # 100 appear "safe", i.e. not
                            // too bright and not too dark. This yields a range
                            // of 125 colors.
                            //
                            let index: usize = 100 + (hasher.finish() as usize % 125);
                            line.replace_range(
                                name.start()..name.end(),
                                &[
                                    // Colors here are of the form \x1B[38;5;0m - \x1B[38;5;255m
                                    "\x1B[38;5;",
                                    &index.to_string(),
                                    "m",
                                    //
                                    name.as_str(),
                                    ANSI_RESET,
                                    encompassing_color,
                                ]
                                .concat(),
                            );
                        }
                    }
                }
                self.colorize_words(line, &encompassing_color)
            }
        }
    }
}

#[derive(Debug, PartialEq)]
struct LogListenerOptions {
    filter: LogFilterOptions,
    local: LocalOptions,
    selectors: Vec<LogInterestSelector>,
}

impl Default for LogListenerOptions {
    fn default() -> LogListenerOptions {
        LogListenerOptions {
            filter: LogFilterOptions {
                filter_by_pid: false,
                pid: 0,
                min_severity: LogLevelFilter::Info,
                verbosity: 0,
                filter_by_tid: false,
                tid: 0,
                tags: vec![],
            },
            local: LocalOptions::default(),
            selectors: vec![],
        }
    }
}

#[derive(Debug, PartialEq, Clone)]
struct LocalOptions {
    file: Option<String>,
    file_capacity: u64,
    startup_sleep: u64,
    ignore_tags: HashSet<String>,
    clock: Clock,
    time_format: String,
    since_time: Option<i64>,
    is_pretty: bool,
    begin: Vec<String>,
    end: Vec<String>,
    only: Vec<String>,
    suppress: Vec<String>,
    dump_logs: bool,
    hide_metadata: bool,
    color_components: bool,
}

impl Default for LocalOptions {
    fn default() -> LocalOptions {
        LocalOptions {
            file: None,
            file_capacity: DEFAULT_FILE_CAPACITY,
            startup_sleep: 0,
            ignore_tags: HashSet::new(),
            clock: Clock::Monotonic,
            time_format: "%Y-%m-%d %H:%M:%S".to_string(),
            since_time: None,
            is_pretty: false,
            begin: vec![],
            end: vec![],
            only: vec![],
            suppress: vec![],
            dump_logs: false,
            hide_metadata: false,
            color_components: false,
        }
    }
}

impl LocalOptions {
    fn format_time(&self, timestamp: zx::sys::zx_time_t) -> String {
        match self.clock {
            Clock::Monotonic => {
                if self.hide_metadata {
                    format!("{}.{:03}", timestamp / 1000000000, (timestamp / 1000000) % 1000)
                } else {
                    format!("{:05}.{:06}", timestamp / 1000000000, (timestamp / 1000) % 1000000)
                }
            }
            Clock::UTC => self._monotonic_to_utc(timestamp).format(&self.time_format).to_string(),
            Clock::Local => chrono::Local
                .from_utc_datetime(&self._monotonic_to_utc(timestamp))
                .format(&self.time_format)
                .to_string(),
        }
    }

    fn _monotonic_to_utc(&self, timestamp: zx::sys::zx_time_t) -> chrono::NaiveDateTime {
        // Find UTC offset for Monotonic.
        // Must compute this every time since UTC time can be adjusted.
        // Note that when printing old messages from memory buffer then
        // this may offset them from UTC time as set when logged in
        // case of UTC time adjustments since.
        let monotonic_zero_as_utc = fuchsia_runtime::utc_time() - zx::Time::get_monotonic();
        let shifted_timestamp = monotonic_zero_as_utc.into_nanos() + timestamp;
        let seconds = (shifted_timestamp / 1000000000) as i64;
        let nanos = (shifted_timestamp % 1000000000) as u32;
        chrono::NaiveDateTime::from_timestamp(seconds, nanos)
    }
}

#[derive(Debug, PartialEq, Clone)]
enum Clock {
    Monotonic, // Corresponds to monotonic time reported by the kernel
    UTC,       // Corresponds to UTC as reported by the runtime
    Local,     // Localized wall time
}

struct MaxCapacityFile {
    file_path: PathBuf,
    file: fs::File,
    capacity: u64,
    curr_size: u64,
}

impl MaxCapacityFile {
    fn new<P: Into<PathBuf>>(file_path: P, capacity: u64) -> Result<MaxCapacityFile, Error> {
        let file_path = file_path.into();
        let file = fs::OpenOptions::new().append(true).create(true).open(&file_path)?;
        let curr_size = file.metadata()?.len();
        Ok(MaxCapacityFile { file, file_path, capacity, curr_size })
    }

    // rotate will move the current file to ${file_path}.log.old and create a new file at ${file_path}
    // to hold future messages.
    fn rotate(&mut self) -> io::Result<()> {
        let mut new_file_name = self
            .file_path
            .to_str()
            .ok_or(io::Error::new(io::ErrorKind::Other, "invalid file name"))?
            .to_string();
        new_file_name.push_str(".old");

        fs::rename(&self.file_path, PathBuf::from(new_file_name))?;
        self.file = fs::OpenOptions::new().append(true).create(true).open(&self.file_path)?;
        self.curr_size = 0;
        Ok(())
    }
}

impl Write for MaxCapacityFile {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if self.capacity == 0 {
            return Ok(buf.len());
        }
        if buf.len() as u64 > self.capacity / 2 {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "buffer size larger than file capacity",
            ));
        }
        if self.capacity != 0 && self.curr_size + (buf.len() as u64) > self.capacity / 2 {
            self.rotate()?;
        }
        self.curr_size += buf.len() as u64;
        self.file.write(buf)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.file.flush()
    }
}

fn help(name: &str) -> String {
    format!(
        r#"Usage: {name} [flags]
        Flags:
        --only <comma-separated-words>
            Filter-in lines containing at least one of the specified words.
            Ineffective if not set.

        --suppress <comma-separated-words>
            Filter-out lines containing any of the specified words

        --select <comma-separated-component-interests>
            Configure the minimum severity level for logs emitted by components on the
            target device matching the selector.
            Specify using the format <component-selector>#<log-level>, where 'log-level'
            must be one of FATAL|ERROR|WARN|INFO|DEBUG|TRACE.
            Multiple component selections are delimited by commas.
            Examples:
            --select foo.cmx#DEBUG,bar.cmx#TRACE

        --begin <comma-separated-words>
            Filter-in blocks starting with at least one of the specified words.
            The block ends with --end option if specified.
            Default is not to suppress.

        --end <comma-separated-words>
            Filter-out blocks starting with at least one of the specified words.
            The block end with --begin option if specified.
            Default is not to suppress.

        --tag <string>:
            Tag to filter on. Multiple tags can be specified by using multiple --tag flags.
            All the logs containing at least one of the passed tags would be printed.

        --ignore-tag <string>:
            Tag to ignore. Any logs containing at least one of the passed tags will not be
            printed.

        --pid <integer>:
            pid for the program to filter on.

        --pretty:
            Activate colorization. Note, suppression features does not need this option.
            TODO(porce): Use structopt and convert this to boolean.

        --color_components:
           With --pretty, colorize logged component names.

        --tid <integer>:
            tid for the program to filter on.

        --severity <TRACE|DEBUG|INFO|WARN|ERROR|FATAL>:
            Minimum severity to filter on.
            Defaults to INFO.

        --verbosity <integer>:
            Verbosity to filter on represented as a positive integer.
            If this is passed, it overrides default severity.
            Errors out if both this and --severity are passed.
            Defaults to 0 which means don't filter on verbosity.

        --file <string>:
            File to write logs to. If omitted, logs are written to stdout.

        --file_capacity <integer>:
            The maximum allowed amount of disk space to consume. Once the file being written to
            reaches half of the capacity, it is moved to FILE.old and a new log file is created.
            Defaults to {default_capacity}. Does nothing if --file is not specified. Setting this
            to 0 disables this functionality.

        --startup_sleep <integer>:
            Sleep for this number of milliseconds on program startup. Used to simulate early boot
            from upcoming boot phases implementation, will be removed in the future.

        --clock <Monotonic|UTC|Local>:
            Select clock to use for timestamps.
            Monotonic (default): monotonic time reported by the kernel.
            UTC: UTC time as reported by the runtime.
            Local: localized wall time.

        --time_format <format>:
            If --clock is not MONOTONIC, specify timestamp format.
            See chrono::format::strftime for format specifiers.
            Defaults to "%Y-%m-%d %H:%M:%S".

        --since_now:
            Ignore all logs from before this command is invoked.

        --dump_logs:
            Dump current logs in buffer and exit.

        --hide_metadata:
            Hides extraneous metadata (such as PID, TID) from log output.
            When paired with --pretty, lines are colorized by severity.

        --help | -h:
            Prints usage."#,
        name = name,
        default_capacity = DEFAULT_FILE_CAPACITY
    )
}

// These flags don't require an additional parameter
const PARAMETERLESS_FLAGS: [&'static str; 5] =
    ["--pretty", "--since_now", "--dump_logs", "--hide_metadata", "--color_components"];

fn check_for_param(i: usize, args: &[String]) -> Result<bool, String> {
    let flag = &args[i];

    if PARAMETERLESS_FLAGS.contains(&flag.as_str()) {
        // This flag does not require a parameter
        if let Some(arg) = args.get(i + 1) {
            // There is another argument after this flag
            if arg == "yes" {
                // A parameterless flag may still have a `yes` param.
                // This is for compatibility reasons.
                return Ok(true);
            } else if arg.starts_with("-") {
                // The next argument is another flag.
                return Ok(false);
            } else {
                // The next argument appears to be a param
                return Err(format!("'{}' does not require a parameter", flag));
            }
        }
        // There is no next argument
        return Ok(false);
    }

    // This flag requires a parameter
    if let Some(arg) = args.get(i + 1) {
        // There is another argument after this flag
        if arg.starts_with("-") {
            // The next argument is another flag
            return Err(format!("'{}' is missing a parameter", flag));
        } else {
            // The next argument appears to be a param
            return Ok(true);
        }
    }

    // There is no next argument
    return Err(format!("'{}' is missing a parameter", flag));
}

fn parse_flags(args: &[String]) -> Result<LogListenerOptions, String> {
    let mut options = LogListenerOptions::default();
    let mut i = 0;
    let mut severity_passed = false;
    while i < args.len() {
        let argument = &args[i];
        let has_param = check_for_param(i, args)?;

        match argument.as_ref() {
            "--begin" => {
                options.local.begin.extend(args[i + 1].split(",").map(String::from));
            }
            "--end" => {
                options.local.end.extend(args[i + 1].split(",").map(String::from));
            }
            "--tag" => {
                let tag = &args[i + 1];
                if tag.len() > MAX_TAG_LEN_BYTES as usize {
                    return Err(format!(
                        "'{}' should not be more than {} characters",
                        tag, MAX_TAG_LEN_BYTES
                    ));
                }
                options.filter.tags.push(tag.to_owned());
                if options.filter.tags.len() > MAX_TAGS as usize {
                    return Err(format!("Max tags allowed: {}", MAX_TAGS));
                }
            }
            "--ignore-tag" => {
                let tag = &args[i + 1];
                if tag.len() > MAX_TAG_LEN_BYTES as usize {
                    return Err(format!(
                        "'{}' should not be more than {} characters",
                        tag, MAX_TAG_LEN_BYTES
                    ));
                }
                options.local.ignore_tags.insert(tag.to_owned());
            }
            "--only" => {
                options.local.only.extend(args[i + 1].split(",").map(String::from));
            }
            "--pretty" => {
                options.local.is_pretty = true;
            }
            "--since_now" => {
                options.local.since_time = Some(zx::Time::get_monotonic().into_nanos());
            }
            "--suppress" => {
                options.local.suppress.extend(args[i + 1].split(",").map(String::from));
            }
            "--severity" => {
                if options.filter.verbosity > 0 {
                    return Err(
                        "Invalid arguments: Cannot pass both severity and verbosity".to_string()
                    );
                }
                severity_passed = true;
                match args[i + 1].as_ref() {
                    "TRACE" => options.filter.min_severity = LogLevelFilter::Trace,
                    "DEBUG" => options.filter.min_severity = LogLevelFilter::Debug,
                    "INFO" => options.filter.min_severity = LogLevelFilter::Info,
                    "WARN" => options.filter.min_severity = LogLevelFilter::Warn,
                    "ERROR" => options.filter.min_severity = LogLevelFilter::Error,
                    "FATAL" => options.filter.min_severity = LogLevelFilter::Fatal,
                    a => return Err(format!("Invalid severity: {}", a)),
                }
            }
            "--verbosity" => {
                if let Ok(v) = args[i + 1].parse::<u8>() {
                    if severity_passed {
                        return Err("Invalid arguments: Cannot pass both severity and verbosity"
                            .to_string());
                    }
                    if v == 0 {
                        return Err(format!(
                            "Invalid verbosity: '{}', must be greater than 0.",
                            args[i + 1]
                        ));
                    }
                    options.filter.min_severity = LogLevelFilter::None;
                    options.filter.verbosity = v;
                } else {
                    return Err(format!("Invalid verbosity: '{}'", args[i + 1]));
                }
            }
            "--pid" => {
                options.filter.filter_by_pid = true;
                match args[i + 1].parse::<u64>() {
                    Ok(pid) => {
                        options.filter.pid = pid;
                    }
                    Err(_) => {
                        return Err(format!(
                            "Invalid pid: '{}', should be a positive integer.",
                            args[i + 1]
                        ));
                    }
                }
            }
            "--tid" => {
                options.filter.filter_by_tid = true;
                match args[i + 1].parse::<u64>() {
                    Ok(tid) => {
                        options.filter.tid = tid;
                    }
                    Err(_) => {
                        return Err(format!(
                            "Invalid tid: '{}', should be a positive integer.",
                            args[i + 1]
                        ));
                    }
                }
            }
            "--file" => {
                options.local.file = Some((&args[i + 1]).clone());
            }
            "--file_capacity" => match args[i + 1].parse::<u64>() {
                Ok(cap) => {
                    options.local.file_capacity = cap;
                }
                Err(_) => {
                    return Err(format!(
                        "Invalid file capacity: '{}', should be a positive integer.",
                        args[i + 1]
                    ));
                }
            },
            "--startup_sleep" => match args[i + 1].parse::<u64>() {
                Ok(n) => {
                    options.local.startup_sleep = n;
                }
                Err(_) => {
                    return Err(format!(
                        "Invalid startup_sleep: '{}', should be a positive integer.",
                        args[i + 1]
                    ));
                }
            },
            "--clock" => match args[i + 1].to_lowercase().as_ref() {
                "monotonic" => options.local.clock = Clock::Monotonic,
                "utc" => options.local.clock = Clock::UTC,
                "local" => options.local.clock = Clock::Local,
                a => return Err(format!("Invalid clock: {}", a)),
            },
            "--time_format" => {
                options.local.time_format = args[i + 1].clone();
            }
            "--dump_logs" => {
                options.local.dump_logs = true;
            }
            "--select" => {
                for selector in args[i + 1].split(',') {
                    let invalid_selector =
                        format!("Invalid component interest selector: '{}'.", selector);
                    // Parsed multiple comma delimited component#interest args.
                    let mut parts = selector.split('#');
                    // Split each arg into sub string vectors containing strings
                    // for component [0] and interest [1] respectively.
                    let component = parts.next().ok_or_else(|| invalid_selector.clone())?;
                    let interest = parts.next().ok_or_else(|| invalid_selector.clone())?;
                    if let Some(_extra) = parts.next() {
                        return Err(invalid_selector.clone());
                    }
                    let selector = match selectors::parse_component_selector::<VerboseError>(
                        &component.to_string(),
                    ) {
                        Ok(s) => s,
                        Err(e) => {
                            return Err(format!(
                                "Invalid selector: {:?}. Error: {}",
                                component.to_string(),
                                e
                            ))
                        }
                    };

                    let min_severity = match interest.to_uppercase().as_ref() {
                        "TRACE" => Some(Severity::Trace),
                        "DEBUG" => Some(Severity::Debug),
                        "INFO" => Some(Severity::Info),
                        "WARN" => Some(Severity::Warn),
                        "ERROR" => Some(Severity::Error),
                        "FATAL" => Some(Severity::Fatal),
                        "" => None,
                        _ => return Err(invalid_selector),
                    };
                    options.selectors.push(LogInterestSelector {
                        selector,
                        interest: Interest { min_severity, ..Interest::EMPTY },
                    });
                }

                if options.selectors.len() > MAX_LOG_SELECTORS.into() {
                    return Err(format!(
                        "Exceeded maximum number of --select args: {}.",
                        MAX_LOG_SELECTORS
                    ));
                }
            }
            "--hide_metadata" => {
                options.local.hide_metadata = true;
            }
            "--color_components" => {
                options.local.color_components = true;
            }
            a => {
                return Err(format!("Invalid option {}", a));
            }
        }

        if has_param {
            i = i + 2;
        } else {
            i = i + 1;
        }
    }
    if !severity_passed {
        for selector in &options.selectors {
            if let Some(severity) = selector.interest.min_severity {
                let mapped_severity = match severity {
                    Severity::Trace => LogLevelFilter::Trace,
                    Severity::Debug => LogLevelFilter::Debug,
                    Severity::Info => LogLevelFilter::Info,
                    Severity::Warn => LogLevelFilter::Warn,
                    Severity::Error => LogLevelFilter::Error,
                    Severity::Fatal => LogLevelFilter::Fatal,
                };
                if options.filter.min_severity > mapped_severity {
                    options.filter.min_severity = mapped_severity;
                }
            }
        }
    }
    return Ok(options);
}

struct Listener<W: Write + Send> {
    // stores pid, dropped_logs
    dropped_logs: HashMap<u64, u32>,
    local_options: LocalOptions,
    writer: W,
    decorator: Decorator,

    // Line suppression
    re_only: Option<Regex>,
    re_suppress: Option<Regex>,

    // Block suppression
    re_begin: Option<Regex>,
    re_end: Option<Regex>,

    is_block_suppressed: bool,
}

impl<W> Listener<W>
where
    W: Write + Send,
{
    pub fn new(writer: W, local_options: LocalOptions, decorator: Decorator) -> Self {
        let mut l = Listener {
            dropped_logs: HashMap::new(),
            local_options,
            writer,
            decorator,

            re_only: None,
            re_suppress: None,
            re_begin: None,
            re_end: None,
            is_block_suppressed: false,
        };

        l.init_regex();
        l
    }

    pub fn proc_regex(&self, keywords: &[String]) -> Option<Regex> {
        if keywords.len() == 0 {
            return None;
        }
        let s = fmt_regex_icase("k", keywords.join("|").as_str());
        match Regex::new(s.as_str()) {
            Err(e) => {
                println!("regex err: {} for keywords: {:?}", e, keywords);
                None
            }
            Ok(r) => Some(r),
        }
    }

    pub fn init_regex(&mut self) {
        if self.local_options.only.len() > 0 {
            self.re_only = self.proc_regex(&self.local_options.only);
        }

        if self.local_options.suppress.len() > 0 {
            self.re_suppress = self.proc_regex(&self.local_options.suppress);
        }

        if self.local_options.begin.len() > 0 {
            self.re_begin = self.proc_regex(&self.local_options.begin);
            self.is_block_suppressed = true;
        }

        if self.local_options.end.len() > 0 {
            self.re_end = self.proc_regex(&self.local_options.end);
        }
    }
}

impl<W> LogProcessor for Listener<W>
where
    W: Write + Send,
{
    fn log(&mut self, message: LogMessage) {
        if message.tags.iter().any(|tag| self.local_options.ignore_tags.contains(tag)) {
            return;
        }
        if let Some(time) = self.local_options.since_time {
            if time > message.time {
                return;
            }
        }
        let tags = message.tags.join(", ");

        let line = if self.local_options.hide_metadata {
            format!(
                "[{}][{}][{}] {}",
                self.local_options.format_time(message.time),
                tags,
                get_log_level_short(message.severity),
                message.msg
            )
        } else {
            format!(
                "[{}][{}][{}][{}] {}: {}",
                self.local_options.format_time(message.time),
                message.pid,
                message.tid,
                tags,
                get_log_level(message.severity),
                message.msg
            )
        };

        if self.re_only.is_some() && !self.re_only.as_ref().unwrap().is_match(line.as_str()) {
            return;
        }

        if self.re_suppress.is_some() && self.re_suppress.as_ref().unwrap().is_match(line.as_str())
        {
            return;
        }

        if self.is_block_suppressed {
            if self.re_begin.is_none() {
                return;
            }
            if !self.re_begin.as_ref().unwrap().is_match(line.as_str()) {
                return;
            }
            // begin condition is met. Activate.
            self.is_block_suppressed = false;
        }

        if !self.is_block_suppressed {
            if self.re_end.is_some() && self.re_end.as_ref().unwrap().is_match(line.as_str()) {
                // end condition is met. Deactivate.
                self.is_block_suppressed = true;
                return;
            }
        }

        if let Err(_) = writeln!(self.writer, "{}", self.decorator.decorate(message.severity, line))
        {
            process::exit(1);
        }

        if message.dropped_logs > 0
            && self
                .dropped_logs
                .get(&message.pid)
                .map(|d| d < &message.dropped_logs)
                .unwrap_or(true)
        {
            writeln!(
                self.writer,
                "[{}][{}][{}][{}] WARNING: Dropped logs count: {}",
                self.local_options.format_time(message.time),
                message.pid,
                message.tid,
                tags,
                message.dropped_logs
            )
            .expect("log_listener: not able to write dropped logs count");
            self.dropped_logs.insert(message.pid, message.dropped_logs);
        }
    }

    fn done(&mut self) {
        // no need to do anything, caller will return after making this call.
    }
}

fn get_log_level(level: i32) -> String {
    // note levels align with syslog logger.h definitions
    match level {
        l if (l == LogLevelFilter::Trace as i32) => "TRACE".to_string(),
        l if (l == LogLevelFilter::Debug as i32) => "DEBUG".to_string(),
        l if (l < LogLevelFilter::Info as i32 && l > LogLevelFilter::Debug as i32) => {
            format!("VLOG({})", (LogLevelFilter::Info as i32) - l)
        }
        l if (l == LogLevelFilter::Info as i32) => "INFO".to_string(),
        l if (l == LogLevelFilter::Warn as i32) => "WARNING".to_string(),
        l if (l == LogLevelFilter::Error as i32) => "ERROR".to_string(),
        l if (l == LogLevelFilter::Fatal as i32) => "FATAL".to_string(),
        l => format!("INVALID({})", l),
    }
}

fn get_log_level_short(level: i32) -> String {
    // note levels align with syslog logger.h definitions
    match level {
        l if (l == LogLevelFilter::Trace as i32) => "T".to_string(),
        l if (l == LogLevelFilter::Debug as i32) => "D".to_string(),
        l if (l < LogLevelFilter::Info as i32 && l > LogLevelFilter::Debug as i32) => {
            format!("VLOG({})", (LogLevelFilter::Info as i32) - l)
        }
        l if (l == LogLevelFilter::Info as i32) => "I".to_string(),
        l if (l == LogLevelFilter::Warn as i32) => "W".to_string(),
        l if (l == LogLevelFilter::Error as i32) => "E".to_string(),
        l if (l == LogLevelFilter::Fatal as i32) => "F".to_string(),
        l => format!("INVALID({})", l),
    }
}

fn new_listener(local_options: LocalOptions) -> Result<Listener<Box<dyn Write + Send>>, Error> {
    let writer: Box<dyn Write + Send> = match local_options.file {
        None => Box::new(io::stdout()),
        Some(ref name) => Box::new(MaxCapacityFile::new(name, local_options.file_capacity)?),
    };

    let mut d = if local_options.hide_metadata {
        let mut d = Decorator::new(DecoratorMode::ColorByLogLevel);
        d.add_log_level(LogLevelFilter::Trace, CYAN);
        d.add_log_level(LogLevelFilter::Debug, MAGENTA);
        d.add_log_level(LogLevelFilter::Warn, YELLOW);
        d.add_log_level(LogLevelFilter::Error, RED);
        d.add_log_level(LogLevelFilter::Fatal, WHITE_ON_RED);
        d
    } else {
        let mut d = Decorator::new(DecoratorMode::ColorByKeyword);
        d.add_line("welcome to Zircon".to_string(), WHITE_ON_RED);
        d.add_line("dm reboot".to_string(), WHITE_ON_RED);
        d.add_line(" bt#".to_string(), WHITE_ON_RED);
        d.add_line("OOM:".to_string(), WHITE_ON_RED);
        d.add_word("error|err".to_string(), RED);
        d.add_word("info".to_string(), YELLOW);
        d.add_word("unknown".to_string(), GREEN);
        d.add_word("warning|warn".to_string(), BLUE);
        d
    };

    if local_options.color_components {
        d.color_components = true;
    }
    if local_options.is_pretty {
        d.activate();
    }

    Ok(Listener::new(writer, local_options.clone(), d))
}

fn run_log_listener(options: Option<&mut LogListenerOptions>) -> Result<(), Error> {
    let mut executor = fasync::LocalExecutor::new().context("Error creating executor")?;
    let (filter_options, local_options, selectors) = options.map_or_else(
        || (None, LocalOptions::default(), None),
        |o| (Some(&mut o.filter), o.local.clone(), Some(&mut o.selectors)),
    );
    let dump_logs = local_options.dump_logs;
    let l = new_listener(local_options)?;
    let listener_fut = syslog_listener::run_log_listener(l, filter_options, dump_logs, selectors);
    executor.run_singlethreaded(listener_fut)?;
    Ok(())
}

fn fmt_regex_icase(capture_name: &str, search_key: &str) -> String {
    format!(r#"(?i)(?P<{}>(?:{}))"#, capture_name, search_key)
}

fn main() -> Result<(), Error> {
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 && (args[1] == "--help" || args[1] == "-h") {
        return Err(format_err!("{}\n", help(args[0].as_ref())));
    }
    let mut options =
        parse_flags(&args[1..]).map_err(|e| format_err!("{}\n{}\n", e, help(args[0].as_ref())))?;

    let sleep_time = time::Duration::from_millis(options.local.startup_sleep);
    thread::sleep(sleep_time);

    run_log_listener(Some(&mut options))
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::fs::File;
    use std::io::Read;
    use tempfile::TempDir;

    fn copy_log_message(msg: &LogMessage) -> LogMessage {
        LogMessage {
            pid: msg.pid,
            tid: msg.tid,
            severity: msg.severity,
            time: msg.time,
            msg: msg.msg.clone(),
            dropped_logs: msg.dropped_logs,
            tags: msg.tags.clone(),
        }
    }

    #[fuchsia::test]
    fn test_simple_log() {
        let mut local_options = LocalOptions::default();
        local_options.hide_metadata = true;
        let tmp_dir = TempDir::new().expect("log_listener: should have created tempdir");
        let file_path = tmp_dir.path().join("tmp_file");
        let tmp_file = File::create(&file_path).expect("log_listener: should have created file");
        let mut l =
            Listener::new(tmp_file, local_options, Decorator::new(DecoratorMode::ColorByLogLevel));

        // test log levels
        let mut message = LogMessage {
            pid: 123,
            tid: 321,
            severity: LogLevelFilter::Trace as i32,
            time: 76352234564,
            msg: "hello".to_string(),
            dropped_logs: 0,
            tags: vec!["a".to_string(), "b".to_string(), "c".to_string()],
        };
        // start with TRACE message
        l.log(copy_log_message(&message));
        // add messages with varying severity
        for level in vec![0x20, 0x30, 0x40, 0x50, 0x60] {
            message.severity = level;
            l.log(copy_log_message(&message));
        }
        let mut expected = "".to_string();
        for level in &["T", "D", "I", "W", "E", "F"] {
            expected.push_str(&format!("[76.352][a, b, c][{}] hello\n", level));
        }

        // Compare the log output with the expectation.
        let mut tmp_file = File::open(&file_path).expect("should have opened the file");
        let mut content = String::new();
        tmp_file.read_to_string(&mut content).expect("something went wrong reading the file");

        assert_eq!(content, expected);
    }

    #[fuchsia::test]
    fn test_log_fn() {
        let _executor = fasync::TestExecutor::new();
        let tmp_dir = TempDir::new().expect("log_listener: should have created tempdir");
        let file_path = tmp_dir.path().join("tmp_file");
        let tmp_file = File::create(&file_path).expect("log_listener: should have created file");

        let mut l = Listener::new(
            tmp_file,
            LocalOptions::default(),
            Decorator::new(DecoratorMode::ColorByKeyword),
        );

        // test log levels
        let mut message = LogMessage {
            pid: 123,
            tid: 321,
            severity: LogLevelFilter::Trace as i32,
            time: 76352234564,
            msg: "hello".to_string(),
            dropped_logs: 0,
            tags: vec![],
        };
        // start with TRACE message
        l.log(copy_log_message(&message));
        // add messages with varying severity
        for level in vec![0x20, 0x30, 0x40, 0x50, 0x60] {
            message.severity = level;
            l.log(copy_log_message(&message));
        }
        let mut expected = "".to_string();
        for level in &["TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"] {
            expected.push_str(&format!("[00076.352234][123][321][] {}: hello\n", level));
        }

        // test tags
        message.severity = LogLevelFilter::Info as i32;
        message.tags = vec!["tag1".to_string()];
        l.log(copy_log_message(&message));
        expected.push_str("[00076.352234][123][321][tag1] INFO: hello\n");

        message.tags.push("tag2".to_string());
        l.log(copy_log_message(&message));
        expected.push_str("[00076.352234][123][321][tag1, tag2] INFO: hello\n");

        // test Monotonic time
        message.time = 636253000631621;
        l.log(copy_log_message(&message));
        let s = "[636253.000631][123][321][tag1, tag2] INFO: hello\n";
        expected.push_str(s);

        // test dropped logs
        message.dropped_logs = 1;
        l.log(copy_log_message(&message));
        expected.push_str(s);
        expected.push_str("[636253.000631][123][321][tag1, tag2] WARNING: Dropped logs count: 1\n");
        l.log(copy_log_message(&message));
        // will not print log count again
        expected.push_str(s);

        // change pid and test
        message.pid = 1234;
        l.log(copy_log_message(&message));
        expected.push_str("[636253.000631][1234][321][tag1, tag2] INFO: hello\n");
        expected
            .push_str("[636253.000631][1234][321][tag1, tag2] WARNING: Dropped logs count: 1\n");

        // switch back pid and test
        message.pid = 123;
        l.log(copy_log_message(&message));
        expected.push_str(s);
        message.dropped_logs = 2;
        l.log(copy_log_message(&message));
        expected.push_str(s);
        expected.push_str("[636253.000631][123][321][tag1, tag2] WARNING: Dropped logs count: 2\n");

        // Compare the log output with the expectation.
        let mut tmp_file = File::open(&file_path).expect("should have opened the file");
        let mut content = String::new();
        tmp_file.read_to_string(&mut content).expect("something went wrong reading the file");

        assert_eq!(content, expected);
    }

    #[fuchsia::test]
    fn test_only_and_suppress() {
        let _executor = fasync::TestExecutor::new();
        let tmp_dir = TempDir::new().expect("should have created tempdir");
        let file_path = tmp_dir.path().join("tmp_file");
        let tmp_file = File::create(&file_path).expect("should have created file");
        let mut expected = "".to_string();

        let mut filter_options = LocalOptions::default();
        filter_options.suppress.push("noisy".to_string());
        filter_options.only.push("interesting".to_string());

        let mut l =
            Listener::new(tmp_file, filter_options, Decorator::new(DecoratorMode::ColorByKeyword));

        // Log message filter test
        let mut message2 = LogMessage {
            pid: 123,
            tid: 321,
            severity: LogLevelFilter::Info as i32,
            time: 76352234564,
            msg: "this is an interesting log".to_string(),
            dropped_logs: 0,
            tags: vec![],
        };

        l.log(copy_log_message(&message2));
        expected.push_str("[00076.352234][123][321][] INFO: this is an interesting log\n");

        message2.msg = "this is a noisy log".to_string();
        l.log(copy_log_message(&message2));
        // Above message is not expected to be logged.

        message2.msg = "this is a noisy but interesting log".to_string();
        l.log(copy_log_message(&message2));
        // Above message is not expected to be logged.

        // Compare the log output with the expectation.
        let mut tmp_file = File::open(&file_path).expect("should have opened the file");
        let mut content = String::new();
        tmp_file.read_to_string(&mut content).expect("something went wrong reading the file");

        assert_eq!(content, expected);
    }

    #[fuchsia::test]
    fn test_begin_end() {
        let _executor = fasync::TestExecutor::new();
        let tmp_dir = TempDir::new().expect("should have created tempdir");
        let file_path = tmp_dir.path().join("tmp_file");
        let tmp_file = File::create(&file_path).expect("should have created file");
        let mut expected = "".to_string();

        // Begin-End message filter test
        let mut filter_options = LocalOptions::default();
        filter_options.begin.push("second".to_string());
        filter_options.begin.push("fifth".to_string());
        filter_options.end.push("fourth".to_string());
        filter_options.end.push("sixth".to_string());

        let mut l =
            Listener::new(tmp_file, filter_options, Decorator::new(DecoratorMode::ColorByKeyword));
        let mut message3 = LogMessage {
            pid: 0,
            tid: 0,
            severity: LogLevelFilter::Info as i32,
            time: 0,
            msg: String::default(),
            dropped_logs: 0,
            tags: vec![],
        };

        message3.msg = "first".to_string();
        l.log(copy_log_message(&message3));
        message3.msg = "second".to_string();
        l.log(copy_log_message(&message3));
        message3.msg = "third".to_string();
        l.log(copy_log_message(&message3));
        message3.msg = "fourth".to_string();
        l.log(copy_log_message(&message3));
        message3.msg = "fifth".to_string();
        l.log(copy_log_message(&message3));
        message3.msg = "sixth".to_string();
        l.log(copy_log_message(&message3));
        message3.msg = "seventh".to_string();
        l.log(copy_log_message(&message3));

        expected.push_str("[00000.000000][0][0][] INFO: second\n");
        expected.push_str("[00000.000000][0][0][] INFO: third\n");
        expected.push_str("[00000.000000][0][0][] INFO: fifth\n");

        // Compare the log output with the expectation.
        let mut tmp_file = File::open(&file_path).expect("should have opened the file");
        let mut content = String::new();
        tmp_file.read_to_string(&mut content).expect("something went wrong reading the file");

        assert_eq!(content, expected);
    }

    #[fuchsia::test]
    fn test_since_now() {
        let _executor = fasync::TestExecutor::new();
        let tmp_dir = TempDir::new().expect("should have created tempdir");
        let file_path = tmp_dir.path().join("tmp_file");
        let tmp_file = File::create(&file_path).expect("should have created file");
        let mut expected = "".to_string();

        // since_now message filter test
        let mut filter_options = LocalOptions::default();

        // All log messages with a timestamp lower than `1000000000` (aka 1 second) should be
        // ignored
        filter_options.since_time = Some(1000000000);

        let mut l =
            Listener::new(tmp_file, filter_options, Decorator::new(DecoratorMode::ColorByKeyword));
        let mut msg = LogMessage {
            pid: 0,
            tid: 0,
            severity: LogLevelFilter::Info as i32,
            time: 0,
            msg: String::default(),
            dropped_logs: 0,
            tags: vec![],
        };

        // First message has time set to 0, this should be ignored
        msg.msg = "foobar".to_string();
        l.log(copy_log_message(&msg));

        // Second message has time set to 2000000000, this should be printed
        msg.msg = "foobar".to_string();
        msg.time = 2000000000;
        l.log(copy_log_message(&msg));

        expected.push_str("[00002.000000][0][0][] INFO: foobar\n");

        // Compare the log output with the expectation.
        let mut tmp_file = File::open(&file_path).expect("should have opened the file");
        let mut content = String::new();
        tmp_file.read_to_string(&mut content).expect("something went wrong reading the file");

        assert_eq!(content, expected);
    }

    #[fuchsia::test]
    fn test_max_capacity_file_write() {
        struct TestCase {
            file_cap: u64,
            file_1_initial_state: Vec<u8>,
            file_2_initial_state: Vec<u8>,
            write_to_perform: Vec<u8>,
            file_1_expected_state: Vec<u8>,
            file_2_expected_state: Vec<u8>,
        }

        let test_cases = vec![
            TestCase {
                file_cap: 10,
                file_1_initial_state: vec![],
                file_2_initial_state: vec![],
                write_to_perform: vec![],
                file_1_expected_state: vec![],
                file_2_expected_state: vec![],
            },
            TestCase {
                file_cap: 10,
                file_1_initial_state: vec![],
                file_2_initial_state: vec![],
                write_to_perform: vec![0],
                file_1_expected_state: vec![0],
                file_2_expected_state: vec![],
            },
            TestCase {
                file_cap: 10,
                file_1_initial_state: vec![0],
                file_2_initial_state: vec![0],
                write_to_perform: vec![],
                file_1_expected_state: vec![0],
                file_2_expected_state: vec![0],
            },
            TestCase {
                file_cap: 10,
                file_1_initial_state: vec![],
                file_2_initial_state: vec![],
                write_to_perform: vec![0, 1, 2, 3, 4],
                file_1_expected_state: vec![0, 1, 2, 3, 4],
                file_2_expected_state: vec![],
            },
            TestCase {
                file_cap: 10,
                file_1_initial_state: vec![0, 1, 2, 3, 4],
                file_2_initial_state: vec![],
                write_to_perform: vec![5],
                file_1_expected_state: vec![5],
                file_2_expected_state: vec![0, 1, 2, 3, 4],
            },
            TestCase {
                file_cap: 10,
                file_1_initial_state: vec![5, 6, 7, 8, 9],
                file_2_initial_state: vec![0, 1, 2, 3, 4],
                write_to_perform: vec![10, 11, 12, 13, 14],
                file_1_expected_state: vec![10, 11, 12, 13, 14],
                file_2_expected_state: vec![5, 6, 7, 8, 9],
            },
            TestCase {
                file_cap: 0,
                file_1_initial_state: vec![],
                file_2_initial_state: vec![],
                write_to_perform: vec![1, 2, 3, 4, 5],
                file_1_expected_state: vec![],
                file_2_expected_state: vec![],
            },
        ];

        for tc in test_cases {
            let tmp_dir = TempDir::new().unwrap();
            let tmp_file_path = tmp_dir.path().join("test.log");
            fs::OpenOptions::new()
                .append(true)
                .create(true)
                .open(&tmp_file_path)
                .unwrap()
                .write_all(&tc.file_1_initial_state)
                .unwrap();
            fs::OpenOptions::new()
                .append(true)
                .create(true)
                .open(&tmp_file_path.with_extension("log.old"))
                .unwrap()
                .write_all(&tc.file_2_initial_state)
                .unwrap();

            MaxCapacityFile::new(tmp_file_path.clone(), tc.file_cap)
                .unwrap()
                .write_all(&tc.write_to_perform)
                .unwrap();

            let mut file1 = fs::OpenOptions::new().read(true).open(&tmp_file_path).unwrap();
            let file_size = file1.metadata().unwrap().len();
            let mut buf = vec![0; file_size as usize];
            file1.read_exact(&mut buf).unwrap();
            assert_eq!(buf, tc.file_1_expected_state);

            let mut file2 = fs::OpenOptions::new()
                .read(true)
                .open(&tmp_file_path.with_extension("log.old"))
                .unwrap();
            let file_size = file2.metadata().unwrap().len();
            let mut buf = vec![0; file_size as usize];
            file2.read_exact(&mut buf).unwrap();
            assert_eq!(buf, tc.file_2_expected_state);
        }
    }

    #[fuchsia::test]
    fn test_format_monotonic_time() {
        let mut local_options = LocalOptions::default();
        let timestamp = 636253000631621;

        let formatted = local_options.format_time(timestamp); // Test default
        assert_eq!(formatted, "636253.000631");
        local_options.clock = Clock::Monotonic;
        let formatted = local_options.format_time(timestamp);
        assert_eq!(formatted, "636253.000631");
    }

    #[fuchsia::test]
    fn test_format_utc_time() {
        let mut local_options = LocalOptions::default();
        let timestamp = 636253000631621;
        local_options.clock = Clock::UTC;
        local_options.time_format = "%H:%M:%S %d/%m/%Y".to_string();

        let timestamp_utc_formatted = local_options.format_time(timestamp);
        let timestamp_utc_struct = chrono::NaiveDateTime::parse_from_str(
            &timestamp_utc_formatted,
            &local_options.time_format,
        )
        .unwrap();
        assert_eq!(
            timestamp_utc_struct.format(&local_options.time_format).to_string(),
            timestamp_utc_formatted
        );
        let zero_utc_formatted = local_options.format_time(0);
        assert_ne!(zero_utc_formatted, timestamp_utc_formatted);
    }

    mod parse_flags {
        use super::*;

        fn parse_flag_test_helper(args: &[String], options: Option<&LogListenerOptions>) {
            match parse_flags(args) {
                Ok(l) => match options {
                    None => {
                        panic!("parse_flags should have returned error, got: {:?}", l);
                    }
                    Some(options) => {
                        assert_eq!(&l, options);
                    }
                },
                Err(e) => {
                    if let Some(_) = options {
                        panic!("did not expect error: {}", e);
                    }
                }
            }
        }

        #[fuchsia::test]
        fn invalid_options() {
            let args = vec!["--tag".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn invalid_options2() {
            let args = vec!["--tag".to_string(), "--severity".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn invalid_flag() {
            let args = vec!["--tag".to_string(), "tag".to_string(), "--invalid".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn one_tag() {
            let args = vec!["--tag".to_string(), "tag".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.filter.tags.push("tag".to_string());
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn multiple_tags() {
            let args = vec![
                "--tag".to_string(),
                "tag".to_string(),
                "--tag".to_string(),
                "tag1".to_string(),
            ];
            let mut expected = LogListenerOptions::default();
            expected.filter.tags.push("tag".to_string());
            expected.filter.tags.push("tag1".to_string());
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn one_ignore_tag() {
            let args = vec!["--ignore-tag".to_string(), "tag".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.ignore_tags.insert("tag".to_string());
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn multiple_ignore_tags() {
            let args = vec![
                "--ignore-tag".to_string(),
                "tag".to_string(),
                "--ignore-tag".to_string(),
                "tag1".to_string(),
            ];
            let mut expected = LogListenerOptions::default();
            expected.local.ignore_tags.insert("tag".to_string());
            expected.local.ignore_tags.insert("tag1".to_string());
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn only() {
            let args = vec!["--only".to_string(), "x,yz,abc".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.only = vec!["x".to_string(), "yz".to_string(), "abc".to_string()];
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn only_error() {
            let args = vec!["--only".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn pid() {
            let args = vec!["--pid".to_string(), "123".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.filter.filter_by_pid = true;
            expected.filter.pid = 123;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn pid_error() {
            let args = vec!["--pid".to_string(), "123a".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn pretty() {
            let args = vec!["--pretty".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.is_pretty = true;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn pretty_yes() {
            let args = vec!["--pretty".to_string(), "yes".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.is_pretty = true;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn pretty_error() {
            let args = vec!["--pretty".to_string(), "123a".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn dump_logs() {
            let args = vec!["--dump_logs".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.dump_logs = true;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn dump_logs_error() {
            let args = vec!["--dump_logs".to_string(), "123a".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn pretty_and_hide_metadata() {
            let args = vec!["--hide_metadata".to_string(), "--pretty".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.hide_metadata = true;
            expected.local.is_pretty = true;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn verbosity_and_hide_metadata() {
            let args =
                vec!["--verbosity".to_string(), "1".to_string(), "--hide_metadata".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.filter.verbosity = 1;
            expected.filter.min_severity = LogLevelFilter::None;
            expected.local.hide_metadata = true;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn hide_metadata() {
            let args = vec!["--hide_metadata".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.hide_metadata = true;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn hide_metadata_error() {
            let args = vec!["--hide_metadata".to_string(), "123a".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn tid() {
            let args = vec!["--tid".to_string(), "123".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.filter.filter_by_tid = true;
            expected.filter.tid = 123;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn tid_error() {
            let args = vec!["--tid".to_string(), "123a".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn suppress() {
            let args = vec!["--suppress".to_string(), "x,yz,abc".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.suppress = vec!["x".to_string(), "yz".to_string(), "abc".to_string()];
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn suppress_error() {
            let args = vec!["--suppress".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn severity() {
            let mut expected = LogListenerOptions::default();
            expected.filter.min_severity = LogLevelFilter::Trace;
            for s in vec!["TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"] {
                let args = vec!["--severity".to_string(), s.to_string()];
                expected.filter.min_severity = match s {
                    "TRACE" => LogLevelFilter::Trace,
                    "DEBUG" => LogLevelFilter::Debug,
                    "INFO" => LogLevelFilter::Info,
                    "WARN" => LogLevelFilter::Warn,
                    "ERROR" => LogLevelFilter::Error,
                    "FATAL" => LogLevelFilter::Fatal,
                    _ => LogLevelFilter::None,
                };
                parse_flag_test_helper(&args, Some(&expected));
            }
        }

        #[fuchsia::test]
        fn severity_error() {
            let args = vec!["--severity".to_string(), "NONE".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn verbosity() {
            let args = vec!["--verbosity".to_string(), "1".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.filter.verbosity = 1;
            expected.filter.min_severity = LogLevelFilter::None;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn legacy_verbosity() {
            // handling legacy expectations
            let args = vec!["--verbosity".to_string(), "10".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.filter.verbosity = 10;
            expected.filter.min_severity = LogLevelFilter::None;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn severity_verbosity_together() {
            let args = vec![
                "--verbosity".to_string(),
                "1".to_string(),
                "--severity".to_string(),
                "DEBUG".to_string(),
            ];
            parse_flag_test_helper(&args, None);

            let args = vec![
                "--severity".to_string(),
                "DEBUG".to_string(),
                "--verbosity".to_string(),
                "1".to_string(),
            ];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn verbosity_error() {
            let mut args = vec!["--verbosity".to_string(), "-2".to_string()];
            parse_flag_test_helper(&args, None);

            args[1] = "str".to_string();
            parse_flag_test_helper(&args, None);

            args[1] = "DEBUG".to_string();
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn file_test() {
            let mut expected = LogListenerOptions::default();
            expected.local.file = Some("/data/test".to_string());
            let args = vec!["--file".to_string(), "/data/test".to_string()];
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn file_empty() {
            let args = Vec::new();
            let expected = LogListenerOptions::default();
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn clock() {
            let args = vec!["--clock".to_string(), "UTC".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.clock = Clock::UTC;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn clock_error() {
            let args = vec!["--clock".to_string(), "CLUCK!!".to_string()];
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn time_format() {
            let args = vec!["--time_format".to_string(), "%H:%M:%S".to_string()];
            let mut expected = LogListenerOptions::default();
            expected.local.time_format = "%H:%M:%S".to_string();
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn tag_edge_case() {
            let mut args = vec!["--tag".to_string()];
            let mut tag = "a".to_string();
            for _ in 1..MAX_TAG_LEN_BYTES {
                tag.push('a');
            }
            args.push(tag.clone());
            let mut expected = LogListenerOptions::default();
            expected.filter.tags.push(tag);
            parse_flag_test_helper(&args, Some(&expected));

            args[1] = "tag1".to_string();
            expected.filter.tags[0] = args[1].clone();
            for i in 1..MAX_TAGS {
                args.push("--tag".to_string());
                args.push(format!("tag{}", i));
                expected.filter.tags.push(format!("tag{}", i));
            }
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn tag_error() {
            let mut args = vec!["--tag".to_string()];
            let mut tag = "a".to_string();
            for _ in 0..MAX_TAG_LEN_BYTES {
                tag.push('a');
            }
            args.push(tag);
            parse_flag_test_helper(&args, None);

            args[1] = "tag1".to_string();
            for i in 0..MAX_TAGS + 5 {
                args.push("--tag".to_string());
                args.push(format!("tag{}", i));
            }
            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn select_single_loglevel() {
            let mut args = vec!["--select".to_string()];
            args.push("foo.cmx#DEBUG".to_string());

            let mut expected = LogListenerOptions::default();
            expected.selectors.push(LogInterestSelector {
                selector: selectors::parse_component_selector::<VerboseError>(
                    &"foo.cmx".to_string(),
                )
                .unwrap(),
                interest: Interest { min_severity: Some(Severity::Debug), ..Interest::EMPTY },
            });
            expected.filter.min_severity = LogLevelFilter::Debug;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn select_customer_is_always_right() {
            let mut args =
                vec!["--severity".to_string(), "INFO".to_string(), "--select".to_string()];
            args.push("foo.cmx#DEBUG".to_string());

            let mut expected = LogListenerOptions::default();
            expected.selectors.push(LogInterestSelector {
                selector: selectors::parse_component_selector::<VerboseError>(
                    &"foo.cmx".to_string(),
                )
                .unwrap(),
                interest: Interest { min_severity: Some(Severity::Debug), ..Interest::EMPTY },
            });
            expected.filter.min_severity = LogLevelFilter::Info;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn select_multiple_loglevel() {
            let mut args = vec!["--select".to_string()];
            args.push("foo.cmx#DEBUG,core/bar#WARN".to_string());

            let mut expected = LogListenerOptions::default();
            expected.selectors.push(LogInterestSelector {
                selector: selectors::parse_component_selector::<VerboseError>(
                    &"foo.cmx".to_string(),
                )
                .unwrap(),
                interest: Interest { min_severity: Some(Severity::Debug), ..Interest::EMPTY },
            });
            expected.selectors.push(LogInterestSelector {
                selector: selectors::parse_component_selector::<VerboseError>(
                    &"core/bar".to_string(),
                )
                .unwrap(),
                interest: Interest { min_severity: Some(Severity::Warn), ..Interest::EMPTY },
            });
            expected.filter.min_severity = LogLevelFilter::Debug;
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn select_loglevel_noseverity() {
            let mut args = vec!["--select".to_string()];
            args.push("foo.cmx#".to_string());

            let mut expected = LogListenerOptions::default();
            expected.selectors.push(LogInterestSelector {
                selector: selectors::parse_component_selector::<VerboseError>(
                    &"foo.cmx".to_string(),
                )
                .unwrap(),
                interest: Interest { min_severity: None, ..Interest::EMPTY },
            });
            parse_flag_test_helper(&args, Some(&expected));
        }

        #[fuchsia::test]
        fn select_loglevel_format_error() {
            let mut args = vec!["--select".to_string()];
            args.push("foo.cmx##DEBUG".to_string());

            parse_flag_test_helper(&args, None);
        }

        #[fuchsia::test]
        fn select_exceed_max_error() {
            let mut args = vec!["--select".to_string()];
            // Push `MAX_LOG_SELECTORS+1` selectors.
            args.push(
                (0..=MAX_LOG_SELECTORS)
                    .into_iter()
                    .map(|i| format!("{}.cmx#INFO", i))
                    .collect::<Vec<_>>()
                    .join(","),
            );
            parse_flag_test_helper(&args, None);
        }
    }

    #[fuchsia::test]
    fn test_complex_decorate() {
        let syslog = "
            [00051.028569][1051][1054][klog] INFO: bootsvc: Creating bootfs service...
            [00052.101073][5525][5550][klog] INFO: ath10k: ath10k: Probed chip QCA6174 ver: 2.1
            [00052.190295][4979][4996][klog] INFO: [ERROR:garnet/bin/appmgr/namespace_builder.cc(62)] Failed to migrate 'deprecated-global-persistent-storage' to new global data directory
            [00052.687460][5525][12795][klog] INFO: devhost: rpc:load-firmware failed: -25
            [00055.306545][526727887][0][netstack] INFO: netstack.go(363): NIC ethp001f6: DHCP acquired IP 192.168.42.193 for 24h0m0s
            [00229.964817][1170][1263][klog] INFO: bt#02: pc 0x644746c42725 sp 0x3279d688da00 (app:/boot/bin/sh,0x1b725)";

        let mut d = Decorator::new(DecoratorMode::ColorByKeyword);
        d.add_word("error".to_string(), RED);
        d.add_word("info".to_string(), YELLOW);
        d.activate();

        for line in syslog.split("\n") {
            println!("{}", d.decorate(0x30, line.to_string()));
        }
    }

    #[fuchsia::test]
    fn test_decorate_with_color_components() {
        let syslog = "
            [00051.028569][1051][1054][firstComponent] INFO: bootsvc: Creating bootfs service...
            [00052.101073][5525][5550][firstComponent] INFO: ath10k: ath10k: Probed chip QCA6174 ver: 2.1
            [00052.190295][4979][4996][second_component] INFO: [ERROR:garnet/bin/appmgr/namespace_builder.cc(62)] Failed to migrate 'deprecated-global-persistent-storage' to new global data directory
            [00052.687460][5525][12795][second_component] INFO: devhost: rpc:load-firmware failed: -25
            [00055.306545][526727887][0][third component] INFO: netstack.go(363): NIC ethp001f6: DHCP acquired IP 192.168.42.193 for 24h0m0s
            [00229.964817][1170][1263][third component] INFO: bt#02: pc 0x644746c42725 sp 0x3279d688da00 (app:/boot/bin/sh,0x1b725)
            [00051.028569][1051][1054][fourthComponent] INFO: bootsvc: Creating bootfs service...
            [00052.101073][5525][5550][fourthComponent] INFO: ath10k: ath10k: Probed chip QCA6174 ver: 2.1
            [00052.190295][4979][4996][fifth_component] INFO: [ERROR:garnet/bin/appmgr/namespace_builder.cc(62)] Failed to migrate 'deprecated-global-persistent-storage' to new global data directory
            [00052.687460][5525][12795][fifth_component] INFO: devhost: rpc:load-firmware failed: -25
            [00055.306545][526727887][0][sixth component] INFO: netstack.go(363): NIC ethp001f6: DHCP acquired IP 192.168.42.193 for 24h0m0s
            [00229.964817][1170][1263][sixth component] INFO: bt#02: pc 0x644746c42725 sp 0x3279d688da00 (app:/boot/bin/sh,0x1b725)";

        let mut d = Decorator::new(DecoratorMode::ColorByKeyword);
        d.add_word("error".to_string(), RED);
        d.add_word("info".to_string(), YELLOW);
        d.color_components = true;
        d.activate();

        for line in syslog.split("\n") {
            println!("{}", d.decorate(0x30, line.to_string()));
        }
    }
}
