// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::Plugin,
    crate::{
        act::Action,
        metrics::{
            fetch::{FileDataFetcher, SelectorString},
            metric_value::MetricValue,
        },
    },
    itertools::Itertools,
    std::convert::TryFrom,
};

pub struct MemoryPlugin();

const SELECTOR: &'static str = "INSPECT:core/memory_monitor:root:current_digest";

impl Plugin for MemoryPlugin {
    fn name(&self) -> &'static str {
        "memory"
    }

    fn display_name(&self) -> &'static str {
        "Memory Summary"
    }

    fn run_structured(&self, inputs: &FileDataFetcher<'_>) -> Vec<Action> {
        let mut results = Vec::new();
        let val = match inputs
            .inspect
            .fetch(&SelectorString::try_from(SELECTOR.to_string()).expect("invalid selector"))
            .into_iter()
            .next()
        {
            Some(MetricValue::String(val)) => val,
            _ => {
                // Short circuit if value could not be found. This is not an error.
                return results;
            }
        };

        val.lines()
            .filter_map(|line| {
                let mut split = line.split(": ");
                let (name, value) = (split.next(), split.next());
                match (name, value) {
                    (Some(name), Some(value)) => {
                        if value.is_empty() || name == "Free" || name == "timestamp" {
                            return None;
                        }
                        let numeric = value.trim_matches(|c: char| !c.is_digit(10));
                        let (mult, parsed) = if value.ends_with("k") {
                            (1_000f64, numeric.parse::<f64>().ok())
                        } else if value.ends_with("M") {
                            (1_000_000f64, numeric.parse::<f64>().ok())
                        } else if value.ends_with("G") {
                            (1_000_000_000f64, numeric.parse::<f64>().ok())
                        } else {
                            (1f64, numeric.parse::<f64>().ok())
                        };

                        match parsed{
                            Some(parsed) => Some((name, value, mult*parsed)),
                            None => {
                                results.push(Action::new_synthetic_error(
                                    format!(
                                        "[DEBUG: BAD DATA] Could not parse '{}' as a valid size. Something is wrong with the output of memory_monitor.",
                                        value,
                                    ),
                                    "Tools>ffx>Profile>Memory".to_string(),
                                ));
                                None
                            }
                        }

                    }
                    _ => None,
                }
            })
            .sorted_by(|a, b| a.2.partial_cmp(&b.2).unwrap())
            .rev()
            .for_each(|entry| {
                results.push(Action::new_synthetic_string_gauge(entry.1.to_string(), None, Some(entry.0.to_string())));
            });

        results
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::metrics::fetch::InspectFetcher;
    use std::convert::TryInto;

    #[fuchsia::test]
    fn test_crashes() {
        let expected_gauges: Vec<String> =
            vec!["TestCmx: 2G", "Other: 7M", "Abcd: 10.3k", "Bbb: 9999"]
                .into_iter()
                .map(|s| s.to_string())
                .collect();
        let expected_errors: Vec<String> =
            vec!["[DEBUG: BAD DATA] Could not parse 'ABCD' as a valid size. Something is wrong with the output of memory_monitor."]
                .into_iter()
                .map(|s| s.to_string())
                .collect();
        let fetcher: InspectFetcher = r#"
[
  {
    "moniker": "core/memory_monitor",
    "payload": {
        "root": {
            "current_digest": "Abcd: 10.3k\nOther: 7M\nBbb: 9999\n\nTestCmx: 2G\ninvalid_line\ninvalid: \ninvalid_again: ABCD\n\nFree: 100M\ntimestamp: 10234\n\n"
        }
    }
  }
]
"#
        .try_into().expect("failed to parse inspect");

        let empty_diagnostics_vec = Vec::new();

        let mut inputs = FileDataFetcher::new(&empty_diagnostics_vec);
        inputs.inspect = &fetcher;
        let result = MemoryPlugin {}.run(&inputs);
        assert_eq!(result.gauges, expected_gauges);
        assert_eq!(result.errors, expected_errors);
    }
}
