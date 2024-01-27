// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::Plugin,
    crate::{act::Action, metrics::fetch::FileDataFetcher},
    regex::Regex,
};

pub struct CrashesPlugin();

impl Plugin for CrashesPlugin {
    fn name(&self) -> &'static str {
        "crashes"
    }

    fn display_name(&self) -> &'static str {
        "Process Crashes"
    }

    fn run_structured(&self, inputs: &FileDataFetcher<'_>) -> Vec<Action> {
        let mut results = Vec::new();

        let re = Regex::new(r"\[(\d+)\.(\d+)\].*(?:CRASH:|fatal :)\s*([\w\-_\s\.]+)")
            .expect("regex compilation");
        for line in inputs.klog.lines.iter().chain(inputs.syslog.lines.iter()) {
            match re.captures(line) {
                Some(captures) => {
                    let s = captures.get(1).unwrap().as_str().parse::<i32>().unwrap_or_default();
                    let ms = captures.get(2).unwrap().as_str().parse::<i32>().unwrap_or_default();

                    let formatted_time =
                        format!("{}h{}m{}.{}s", s / 3600, s % 3600 / 60, s % 60, ms);

                    results.push(Action::new_synthetic_error(
                        format!(
                            "[ERROR]: {} crashed at {} [{}.{}]",
                            captures.get(3).unwrap().as_str(),
                            formatted_time,
                            s,
                            ms,
                        ),
                        "DeveloperExperience>Forensics>CrashReporting".to_string(),
                    ));
                }
                _ => {}
            };
        }
        results
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::metrics::fetch::TextFetcher;

    #[fuchsia::test]
    fn test_crashes() {
        let expected_errors: Vec<String> = vec![
            "[ERROR]: my_component.cmx crashed at 1h1m1.123s [3661.123]",
            "[ERROR]: my_component.cmx crashed at 1h2m2.345s [3722.345]",
        ]
        .into_iter()
        .map(|s| s.to_string())
        .collect::<Vec<_>>();
        let fetcher: TextFetcher = r#"
[3661.123] fatal : my_component.cmx[333]
[3722.345] CRASH: my_component.cmx[334]
"#
        .into();

        let empty_diagnostics_vec = Vec::new();

        let mut inputs = FileDataFetcher::new(&empty_diagnostics_vec);
        inputs.klog = &fetcher;
        assert_eq!(CrashesPlugin {}.run(&inputs).errors, expected_errors);

        let mut inputs = FileDataFetcher::new(&empty_diagnostics_vec);
        inputs.syslog = &fetcher;
        assert_eq!(CrashesPlugin {}.run(&inputs).errors, expected_errors);
    }
}
