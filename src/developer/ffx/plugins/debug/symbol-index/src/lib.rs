// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use async_trait::async_trait;
use errors::ffx_bail;
use ffx_debug_symbol_index_args::*;
use fho::{FfxMain, FfxTool, SimpleWriter};
use std::path::Path;
use symbol_index::*;

#[derive(FfxTool)]
pub struct SymbolIndexTool {
    #[command]
    cmd: SymbolIndexCommand,
}

fho::embedded_plugin!(SymbolIndexTool);

#[async_trait(?Send)]
impl FfxMain for SymbolIndexTool {
    type Writer = SimpleWriter;

    async fn main(self, _writer: Self::Writer) -> fho::Result<()> {
        match self.cmd.sub_command {
            SymbolIndexSubCommand::List(cmd) => list(cmd, &global_symbol_index_path()?)?,
            SymbolIndexSubCommand::Add(cmd) => add(cmd, &global_symbol_index_path()?)?,
            SymbolIndexSubCommand::Remove(cmd) => remove(cmd, &global_symbol_index_path()?)?,
            SymbolIndexSubCommand::Clean(cmd) => clean(cmd, &global_symbol_index_path()?)?,
        }
        Ok(())
    }
}

fn list(cmd: ListCommand, global_symbol_index_path: &str) -> Result<()> {
    let index = if cmd.aggregated {
        SymbolIndex::load_aggregate(global_symbol_index_path)?
    } else {
        SymbolIndex::load(global_symbol_index_path)?
    };
    serde_json::to_writer_pretty(std::io::stdout(), &index)?;
    Ok(println!())
}

fn add(cmd: AddCommand, global_symbol_index_path: &str) -> Result<()> {
    let path = resolve_path_from_cwd(&cmd.path)?;
    let build_dir = cmd.build_dir.map(|p| resolve_path_from_cwd(&p).ok()).flatten();
    // Create a new one if the global symbol-index.json doesn't exist or is malformed.
    let mut index = SymbolIndex::load(global_symbol_index_path).unwrap_or(SymbolIndex::new());

    if path.ends_with(".json") {
        if index.includes.contains(&path) {
            return Ok(());
        }
        if build_dir.is_some() {
            ffx_bail!("--build-dir cannot be specified for json files");
        }
        index.includes.push(path);
    } else if path.ends_with("ids.txt") {
        if index.ids_txts.iter().any(|ids_txt| ids_txt.path == path) {
            return Ok(());
        }
        index.ids_txts.push(IdsTxt { path, build_dir });
    } else if Path::new(&path).is_dir() {
        if index.build_id_dirs.iter().any(|build_id_dir| build_id_dir.path == path) {
            return Ok(());
        }
        index.build_id_dirs.push(BuildIdDir { path, build_dir });
    } else {
        ffx_bail!("Unsupported format: {}", path);
    }

    index.save(global_symbol_index_path)
}

fn remove(cmd: RemoveCommand, global_symbol_index_path: &str) -> Result<()> {
    let path = resolve_path_from_cwd(&cmd.path)?;
    let mut index = SymbolIndex::load(global_symbol_index_path)?;
    index.includes.retain(|include| include != &path);
    index.ids_txts.retain(|ids_txt| ids_txt.path != path);
    index.build_id_dirs.retain(|build_id_dir| build_id_dir.path != path);
    index.save(global_symbol_index_path)
}

fn clean(_cmd: CleanCommand, global_symbol_index_path: &str) -> Result<()> {
    let mut index = SymbolIndex::load(global_symbol_index_path)?;
    index.includes.retain(|include| Path::new(include).exists());
    index.ids_txts.retain(|ids_txt| Path::new(&ids_txt.path).exists());
    index.build_id_dirs.retain(|build_id_dir| Path::new(&build_id_dir.path).exists());
    index.save(global_symbol_index_path)
}

/// Resovle a relative from current_dir. Do nothing if |relative| is actually absolute.
fn resolve_path_from_cwd(relative: &str) -> Result<String> {
    if Path::new(relative).is_absolute() {
        Ok(relative.to_owned())
    } else {
        Ok(resolve_path(&std::env::current_dir()?, relative))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::*;
    use tempfile::TempDir;

    #[test]
    fn test_list() {
        list(
            ListCommand { aggregated: false },
            "../../src/developer/ffx/lib/symbol-index/test_data/main.json",
        )
        .unwrap();
        list(
            ListCommand { aggregated: true },
            "../../src/developer/ffx/lib/symbol-index/test_data/main.json",
        )
        .unwrap();
    }

    #[test]
    fn test_add_remove_clean() {
        let tempdir = TempDir::new().unwrap();
        let tempdir_path = tempdir.path().to_str().unwrap();
        let build_id_dir = tempdir_path.to_owned() + "/.build-id";
        let ids_txt = tempdir_path.to_owned() + "/ids.txt";
        let package_json = tempdir_path.to_owned() + "/package.symbol-index.json";
        let nonexistent = tempdir_path.to_owned() + "/nonexistent.json";
        let build_dir = Some(tempdir_path.to_owned());
        let index_path = tempdir_path.to_owned() + "/symbol-index.json";

        create_dir(&build_id_dir).unwrap();
        File::create(&package_json).unwrap();
        File::create(&ids_txt).unwrap();

        // Test add.
        add(AddCommand { build_dir: None, path: ids_txt.clone() }, &index_path).unwrap();
        add(AddCommand { build_dir: build_dir.clone(), path: build_id_dir.clone() }, &index_path)
            .unwrap();
        // Duplicated adding should be a noop
        add(AddCommand { build_dir: None, path: build_id_dir.clone() }, &index_path).unwrap();
        // build_dir cannot be supplied for json files
        assert!(add(AddCommand { build_dir: build_dir, path: package_json.clone() }, &index_path)
            .is_err());
        add(AddCommand { build_dir: None, path: package_json.clone() }, &index_path).unwrap();
        // Duplicated adding should be a noop.
        add(AddCommand { build_dir: None, path: package_json }, &index_path).unwrap();
        // Adding a non-existent item is not an error.
        add(AddCommand { build_dir: None, path: nonexistent }, &index_path).unwrap();
        // Adding a relative path.
        add(AddCommand { build_dir: None, path: ".".to_owned() }, &index_path).unwrap();

        let symbol_index = SymbolIndex::load(&index_path).unwrap();
        assert_eq!(symbol_index.ids_txts.len(), 1);
        assert_eq!(symbol_index.build_id_dirs.len(), 2);
        assert!(symbol_index.build_id_dirs[0].build_dir.is_some());
        assert_eq!(symbol_index.includes.len(), 2);

        // Test remove.
        assert!(remove(RemoveCommand { path: ids_txt }, &index_path).is_ok());
        // Removing a relative path.
        assert!(remove(RemoveCommand { path: ".".to_owned() }, &index_path).is_ok());
        let symbol_index = SymbolIndex::load(&index_path).unwrap();
        assert_eq!(symbol_index.ids_txts.len(), 0);
        assert_eq!(symbol_index.build_id_dirs.len(), 1);

        // Test clean.
        remove_dir(build_id_dir).unwrap();
        assert!(clean(CleanCommand {}, &index_path).is_ok());
        let symbol_index = SymbolIndex::load(&index_path).unwrap();
        assert_eq!(symbol_index.build_id_dirs.len(), 0);
        assert_eq!(symbol_index.includes.len(), 1);
    }
}
