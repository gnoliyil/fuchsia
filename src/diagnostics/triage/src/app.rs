// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        config::{self, OutputFormat, ProgramStateHolder},
        Options,
    },
    anyhow::{bail, Context as _, Error},
    fuchsia_triage::{
        analyze, analyze_structured, analyze_verbose, ActionResultFormatter, ActionResults,
        DiagnosticData, ParseResult, TriageOutput,
    },
};

/// The entry point for the CLI app.
pub struct App {
    options: Options,
}

impl App {
    /// Creates a new App with the given options.
    pub fn new(options: Options) -> App {
        App { options }
    }

    /// Runs the App.
    ///
    /// This method consumes self and calls run_structured or run_unstructured
    /// depending on if structured output is in options. It then collects the results
    /// and writes the results to the dest. If an error occurs during the running of the app
    /// it will be returned as an Error.
    pub fn run(self, dest: &mut dyn std::io::Write) -> Result<bool, Error> {
        // TODO(fxbug.dev/50449): Use 'argh' crate.
        let ProgramStateHolder { parse_result, diagnostic_data, output_format } =
            config::initialize(self.options.clone())?;

        match output_format {
            OutputFormat::Structured => {
                let structured_run_result = self.run_structured(diagnostic_data, parse_result)?;
                structured_run_result.write_report(dest)?;
                Ok(structured_run_result.has_reportable_issues())
            }
            OutputFormat::Text | OutputFormat::VerboseText => {
                let run_result =
                    self.run_unstructured(diagnostic_data, parse_result, output_format)?;
                run_result.write_report(dest)?;
                Ok(run_result.has_problems())
            }
        }
    }

    fn run_unstructured(
        self,
        diagnostic_data: Vec<DiagnosticData>,
        parse_result: ParseResult,
        output_format: OutputFormat,
    ) -> Result<RunResult, Error> {
        let action_results = match output_format {
            OutputFormat::Text => analyze(&diagnostic_data, &parse_result)?,
            OutputFormat::VerboseText => analyze_verbose(&diagnostic_data, &parse_result)?,
            _ => unreachable!(),
        };

        Ok(RunResult::new(output_format, action_results))
    }

    fn run_structured(
        self,
        diagnostic_data: Vec<DiagnosticData>,
        parse_result: ParseResult,
    ) -> Result<StructuredRunResult, Error> {
        let triage_output = analyze_structured(&diagnostic_data, &parse_result)?;

        Ok(StructuredRunResult { triage_output })
    }
}

/// The result of calling App::run.
pub struct RunResult {
    output_format: OutputFormat,
    action_results: ActionResults,
}

impl RunResult {
    /// Creates a new RunResult struct. This method is intended to be used by the
    /// App:run method.
    fn new(output_format: OutputFormat, action_results: ActionResults) -> RunResult {
        RunResult { output_format, action_results }
    }

    /// Returns true if at least one ActionResults has a warning or errorF.
    fn has_problems(&self) -> bool {
        !(self.action_results.get_warnings().is_empty()
            && self.action_results.get_errors().is_empty())
    }

    /// Writes the contents of the run to the provided writer.
    ///
    /// This method can be used to output the results to a file or stdout.
    pub fn write_report(&self, dest: &mut dyn std::io::Write) -> Result<(), Error> {
        if self.output_format != OutputFormat::Text
            && self.output_format != OutputFormat::VerboseText
        {
            bail!("BUG: Incorrect output format requested");
        }

        let results_formatter = ActionResultFormatter::new(&self.action_results);
        let output = results_formatter.to_text();
        dest.write_fmt(format_args!("{}\n", output)).context("failed to write to destination")?;
        Ok(())
    }
}

/// The result of calling App::run_structured.
pub struct StructuredRunResult {
    triage_output: TriageOutput,
}

impl StructuredRunResult {
    /// Writes the contents of the run_structured to the provided writer.
    ///
    /// This method can be used to output the results to a file or stdout.
    pub fn write_report(&self, dest: &mut dyn std::io::Write) -> Result<(), Error> {
        let output = serde_json::to_string(&self.triage_output)?;
        dest.write_fmt(format_args!("{}\n", output)).context("failed to write to destination")?;
        Ok(())
    }

    /// Returns true if the result contains a reportable warning, error, or significant Problem
    pub fn has_reportable_issues(&self) -> bool {
        self.triage_output.has_reportable_issues()
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fuchsia_triage::{Action, ActionResults},
    };

    #[fuchsia::test]
    fn test_output_text_no_warnings() -> Result<(), Error> {
        let action_results = ActionResults::new();
        let run_result = RunResult::new(OutputFormat::Text, action_results);

        let mut dest = vec![];
        run_result.write_report(&mut dest)?;

        let output = String::from_utf8(dest)?;
        assert_eq!("No actions were triggered. All targets OK.\n", output);

        Ok(())
    }

    #[fuchsia::test]
    fn test_output_text_with_alerts() -> Result<(), Error> {
        let mut action_results = ActionResults::new();
        action_results.add_info("hmmm".to_string());
        action_results.add_warning("oops".to_string());
        action_results.add_error("fail".to_string());

        let readable_run_result = RunResult::new(OutputFormat::Text, action_results.clone());
        action_results.set_verbose(true);
        let verbose_run_result = RunResult::new(OutputFormat::VerboseText, action_results.clone());

        let mut readable_dest = vec![];
        readable_run_result.write_report(&mut readable_dest)?;
        let mut verbose_dest = vec![];
        verbose_run_result.write_report(&mut verbose_dest)?;

        let output = String::from_utf8(readable_dest)?;
        assert_eq!("Errors\n------\nfail\n\nWarnings\n--------\noops\n\n", output,);
        let output = String::from_utf8(verbose_dest)?;
        assert_eq!(
            "Errors\n------\nfail\n\nWarnings\n--------\noops\n\nInfo\n----\nhmmm\n\n",
            output,
        );

        Ok(())
    }

    #[fuchsia::test]
    fn test_output_text_with_gauges() -> Result<(), Error> {
        let mut action_results = ActionResults::new();
        action_results.add_gauge("gauge".to_string());
        let run_result = RunResult::new(OutputFormat::Text, action_results);

        let mut dest = vec![];
        run_result.write_report(&mut dest)?;

        let output = String::from_utf8(dest)?;
        assert_eq!(
            "Featured Values\n---------------\ngauge\n\n\
            No actions were triggered. All targets OK.\n",
            output
        );

        Ok(())
    }

    #[fuchsia::test]
    fn test_structured_output_no_warnings() -> Result<(), Error> {
        let triage_output = TriageOutput::new(Vec::new());
        let structured_run_result = StructuredRunResult { triage_output };

        let mut dest = vec![];
        structured_run_result.write_report(&mut dest)?;

        let output = String::from_utf8(dest)?;
        assert_eq!(
            "{\"actions\":{},\"metrics\":{},\"plugin_results\":{},\"triage_errors\":[]}\n",
            output
        );

        Ok(())
    }

    #[fuchsia::test]
    fn test_structured_output_with_warnings() -> Result<(), Error> {
        let mut triage_output = TriageOutput::new(vec!["file".to_string()]);
        triage_output.add_action(
            "file".to_string(),
            "warning_name".to_string(),
            Action::new_synthetic_warning("fail".to_string()),
        );
        let structured_run_result = StructuredRunResult { triage_output };

        let mut dest = vec![];
        structured_run_result.write_report(&mut dest)?;

        let output = String::from_utf8(dest)?;
        assert_eq!(
            "{\"actions\":{\"file\":{\"warning_name\":{\"type\":\"Alert\",\"trigger\":\
        {\"metric\":{\"Eval\":{\"raw_expression\":\"True()\",\"parsed_expression\":{\"Function\":\
        [\"True\",[]]}}},\"cached_value\":\
        {\"Bool\":true}},\"print\":\"fail\",\"file_bug\":null,\"tag\":null,\
         \"severity\":\"Warning\"}}},\"metrics\":\
        {\"file\":{}},\"plugin_results\":{},\"triage_errors\":[]}\n",
            output
        );

        Ok(())
    }

    #[fuchsia::test]
    fn test_structured_output_with_gauges() -> Result<(), Error> {
        let mut triage_output = TriageOutput::new(vec!["file".to_string()]);
        triage_output.add_action(
            "file".to_string(),
            "gauge_name".to_string(),
            Action::new_synthetic_string_gauge("gauge".to_string(), None, None),
        );
        let structured_run_result = StructuredRunResult { triage_output };

        let mut dest = vec![];
        structured_run_result.write_report(&mut dest)?;

        let output = String::from_utf8(dest)?;
        assert_eq!(
            "{\"actions\":{\"file\":{\"gauge_name\":{\"type\":\"Gauge\",\"value\":{\"metric\":\
            {\"Eval\":{\"raw_expression\":\"'gauge'\",\"parsed_expression\":{\"Value\":\
            {\"String\":\"gauge\"}}}},\"cached_value\":{\"String\":\"gauge\"}},\"format\":null,\
            \"tag\":null}}},\"metrics\":{\"file\":{}},\"plugin_results\":{},\"triage_errors\":[]}\n",
            output
        );

        Ok(())
    }
}
