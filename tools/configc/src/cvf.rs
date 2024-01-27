// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::common::load_manifest;
use anyhow::{Context as _, Error};
use argh::FromArgs;
use cm_rust::NativeIntoFidl;
use fidl::persist;
use std::{collections::BTreeMap, fs, io::Write, path::PathBuf};

#[derive(FromArgs, PartialEq, Debug)]
/// Generates a Configuration Value File (cvf) from a given manifest and JSON value file.
#[argh(subcommand, name = "cvf")]
pub struct GenerateValueFile {
    /// compiled manifest containing the config declaration
    #[argh(option)]
    cm: PathBuf,

    /// JSON5 file containing a single object with each config field as a top level key in an
    /// object.
    #[argh(option)]
    values: PathBuf,

    /// path to which to write configuration value file
    #[argh(option)]
    output: PathBuf,
}

impl GenerateValueFile {
    pub fn generate(self) -> Result<(), Error> {
        let component = load_manifest(&self.cm).context("loading component manifest")?;
        let config_decl = component
            .config
            .as_ref()
            .ok_or_else(|| anyhow::format_err!("missing config declaration in manifest"))?;

        // load & parse the json file containing value defs
        let values_raw = fs::read_to_string(self.values).context("reading values JSON")?;
        let values: BTreeMap<String, serde_json::Value> =
            serde_json5::from_str(&values_raw).context("parsing values JSON")?;

        // combine the manifest and provided values
        let values_data = config_value_file::populate_value_file(config_decl, values)
            .context("populating config values")?;
        let values_data = values_data.native_into_fidl();
        let encoded_output = persist(&values_data).context("encoding value file")?;

        // write result to value file output
        if let Some(parent) = self.output.parent() {
            // attempt to create all parent directories, ignore failures bc they might already exist
            std::fs::create_dir_all(parent).ok();
        }
        let mut out_file = fs::OpenOptions::new()
            .create(true)
            .truncate(true)
            .write(true)
            .open(self.output)
            .context("opening output file")?;
        out_file.write(&encoded_output).context("writing value file to output")?;

        Ok(())
    }
}
