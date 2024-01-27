// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{bail, Context, Result};
use assembly_tool::Tool;
use camino::{Utf8Path, Utf8PathBuf};
use std::collections::BTreeSet;

/// Builder for compiling a component out of cml shards.
pub struct ComponentBuilder {
    /// The name of the component.
    name: String,
    /// A list of component manifest shards to be merged
    /// into the final component manifest.
    manifest_shards: BTreeSet<Utf8PathBuf>,
}

impl ComponentBuilder {
    /// Construct a new ComponentBuilder that uses the cmc |tool|.
    pub fn new(name: impl Into<String>) -> Self {
        ComponentBuilder { name: name.into(), manifest_shards: BTreeSet::default() }
    }

    /// Add a CML shard or the primary CML file for this component.
    pub fn add_shard(&mut self, path: impl AsRef<Utf8Path>) -> Result<&mut Self> {
        let path = path.as_ref();
        let added = self.manifest_shards.insert(path.to_path_buf());

        if !added {
            bail!("Component shard path {} already added", path);
        }

        Ok(self)
    }

    /// Build the component.
    pub fn build(
        self,
        outdir: impl AsRef<Utf8Path>,
        cmc_tool: &dyn Tool,
        include_path: impl AsRef<Utf8Path>,
    ) -> Result<Utf8PathBuf> {
        // Write all generated files in a subdir with the name of the package.
        let outdir = outdir.as_ref().join(&self.name);
        let cmlfile = outdir.join(format!("{}.cml", &self.name));
        let mut args = vec!["merge".to_owned(), "--output".to_owned(), cmlfile.to_string()];

        args.extend(self.manifest_shards.iter().map(|shard| shard.to_string()));

        cmc_tool
            .run(&args)
            .with_context(|| format!("Failed to run cmc merge with shards {args:?}"))?;

        let cmfile = outdir.join(format!("{}.cm", &self.name));

        let args = vec![
            "compile".into(),
            "--includeroot".into(),
            include_path.as_ref().to_string(),
            "--includepath".into(),
            include_path.as_ref().to_string(),
            "-o".into(),
            cmfile.to_string(),
            cmlfile.to_string(),
        ];

        cmc_tool
            .run(&args)
            .with_context(|| format!("Failed to run cmc compile with args {args:?}"))?;

        Ok(cmfile)
    }
}

#[cfg(test)]
mod tests {
    use crate::ComponentBuilder;
    use assembly_tool::testing::FakeToolProvider;
    use assembly_tool::{ToolCommandLog, ToolProvider};
    use camino::Utf8Path;
    use serde_json::json;
    use tempfile::TempDir;

    #[test]
    fn add_shard_with_duplicates_returns_err() {
        let mut builder = ComponentBuilder::new("foo");
        builder.add_shard("foobar").unwrap();

        let result = builder.add_shard("foobar");

        assert!(result.is_err());
    }

    #[test]
    fn build_with_shards_compiles_component() {
        let tmpdir = TempDir::new().unwrap();
        let outdir = Utf8Path::from_path(tmpdir.path()).unwrap();
        let shard_path_1 = outdir.join("shard1.cml");
        let shard_path_2 = outdir.join("shard2.cml");
        let shard_path_3 = outdir.join("shard3.cml");
        let tools = FakeToolProvider::default();
        let mut builder = ComponentBuilder::new("test");
        builder.add_shard(&shard_path_1).unwrap();
        builder.add_shard(&shard_path_2).unwrap().add_shard(&shard_path_3).unwrap();
        let expected_commands: ToolCommandLog = serde_json::from_value(json!({
            "commands": [
                {
                    "tool": "./host_x64/cmc",
                    "args": [
                        "merge",
                        "--output",
                        outdir.join("test").join("test.cml").to_string(),
                        shard_path_1.to_string(),
                        shard_path_2.to_string(),
                        shard_path_3.to_string(),
                    ]
                },
                {
                    "tool": "./host_x64/cmc",
                    "args": [
                        "compile",
                        "--includeroot",
                        "include/path",
                        "--includepath",
                        "include/path",
                        "-o",
                        outdir.join("test").join("test.cm").to_string(),
                        outdir.join("test").join("test.cml").to_string(),
                    ]
                }
            ]
        }))
        .unwrap();

        let result = builder.build(outdir, tools.get_tool("cmc").unwrap().as_ref(), "include/path");

        assert!(result.is_ok());
        assert_eq!(&expected_commands, tools.log());
    }
}
