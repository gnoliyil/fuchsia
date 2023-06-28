// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// NOTE: The doc comments on `IntetgrationTestCmd` and its fields appear as the helptext of
/// `fx testgen`. Please run that command to make sure the output looks correct before
/// submitting changes.
use crate::flags;

use {
    anyhow::{bail, Error},
    argh::FromArgs,
    chrono::Datelike,
    handlebars::{handlebars_helper, Handlebars},
    std::path::{Path, PathBuf},
    tempfile::TempDir,
    tracing::info,
    walkdir::{DirEntry, WalkDir},
};

/// Generates an integration test for a Fuchsia component.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "integration_test")]
pub(crate) struct IntegrationTestCmd {
    /// the absolute the path to the component-under-test's manifest.
    #[argh(option, short = 'm')]
    pub component_manifest: PathBuf,

    /// the root directory where code will be generated. This directory must not exist.
    #[argh(option, short = 'o')]
    pub test_root: PathBuf,
}

/// Generates an integration test for a Fuchsia component.
///
/// Code is generated by copying the template files in templates/integration_test.
/// Each template uses the [handlebars](https://docs.rs/handlebars/latest/handlebars/) syntax.
///
/// The full set of variables available to each template are derived from the user's input
/// and are as follows:
///
///   component_name
///       The name of the component, derived from the name of the input component manifest.
///       Example: /path/to/my-component.cml -> my-component
///
///   test_binary_name
///       The name of the generated test binary.
///
///   test_package_name
///       The name of the generated test Fuchsia package.
///       Example GN usage: `fuchsia_test_package("{{ test_package_name }}")`
///
///   fidl_rust_crate_name
///       The name of the Rust crate containing the test's generated RealmFactory FIDL bindings.
///       This is primarily useful for importing generated Rust fidl bindings in test code.
///       Example Rust usage: `use {{ fidl_rust_crate_name }} as ftest`
///
///   fidl_library_name
///       The name of the RealmFactory fidl library. This is primarily useful for defining and importing
///       the FIDL library in .fidl files.
///       Example FIDL usage: `library {{ fidl_library_name }};`
///
impl IntegrationTestCmd {
    pub async fn run(&self, _: &flags::Flags) -> Result<(), Error> {
        let test_root = self.test_root.clone();
        if test_root.exists() {
            bail!("{} already exists. Please choose a directory location that does not already exist.", test_root.display());
        }

        let component_manifest = self.component_manifest.clone();
        if !component_manifest.exists() {
            bail!("{} does not exist.", self.component_manifest.display());
        }

        info!("Generating an integration test at {}", test_root.display());

        // Initialize template variables and source code.
        let component_name = path_file_stem(&self.component_manifest);
        let mut template_vars = std::collections::BTreeMap::new();
        template_vars.insert("component_name", component_name.clone());
        template_vars.insert("test_binary_name", var_test_binary_name(&component_name));
        template_vars.insert("test_package_name", var_test_package_name(&component_name));
        // TODO(127973): Add back support for C++
        template_vars.insert("fidl_rust_crate_name", var_fidl_rust_crate_name(&component_name));
        template_vars.insert("fidl_library_name", var_fidl_library_name(&component_name));

        let templates = vec![
            ("tests/BUILD.gn", include_str!("../templates/integration_test/tests/BUILD.gn.hbrs")),
            ("tests/meta/test-root.cml", include_str!("../templates/integration_test/tests/meta/test-root.cml.hbrs")),
            ("tests/meta/test-suite.cml", include_str!("../templates/integration_test/tests/meta/test-suite.cml.hbrs")),
            ("tests/src/main.rs", include_str!("../templates/integration_test/tests/src/main.rs.hbrs")),
            ("testing/fidl/BUILD.gn", include_str!("../templates/integration_test/testing/fidl/BUILD.gn.hbrs")),
            ("testing/fidl/realm_factory.test.fidl", include_str!("../templates/integration_test/testing/fidl/realm_factory.test.fidl.hbrs")),
            ("testing/realm-factory/BUILD.gn", include_str!("../templates/integration_test/testing/realm-factory/BUILD.gn.hbrs")),
            ("testing/realm-factory/meta/default.cml", include_str!("../templates/integration_test/testing/realm-factory/meta/default.cml.hbrs")),
            ("testing/realm-factory/src/main.rs", include_str!("../templates/integration_test/testing/realm-factory/src/main.rs.hbrs")),
            ("testing/realm-factory/src/realm_factory_impl.rs", include_str!("../templates/integration_test/testing/realm-factory/src/realm_factory_impl.rs.hbrs")),
        ];

        // Generate source code in a staging directory.
        // This prevents making destructive changes when code gen fails.
        let tmp_dir = TempDir::new()?;
        let code_generator = CodeGenerator::new();
        for template in templates {
            let output_path = tmp_dir.path().join(template.0);
            let template_code = template.1.clone();
            let code = code_generator.generate(&template_code, &template_vars)?;
            file_write(output_path, &code)?;
        }

        // Copy the staged changes to the users's test root.
        dir_create_if_absent(&test_root)?;
        dir_copy(tmp_dir.path(), &test_root)?;
        Ok(())
    }
}

fn var_test_binary_name(component_name: &str) -> String {
    let binary_name = format!("{}_test", component_name);
    sanitize_binary_name(&binary_name)
}

fn var_test_package_name(component_name: &str) -> String {
    format!("{}-test", component_name)
}

fn var_fidl_library_name(component_name: &str) -> String {
    let fidl_library_name = format!("test.{}", component_name);
    sanitize_fidl_library_name(&fidl_library_name)
}

fn var_fidl_rust_crate_name(component_name: &str) -> String {
    let library_name = var_fidl_library_name(component_name);
    let crate_name = format!("fidl_{}", library_name);
    sanitize_rust_crate_name(&crate_name)
}

fn sanitize_binary_name(binary_name: &str) -> String {
    let binary_name = String::from(binary_name);
    let binary_name = binary_name.replace("-", "_");
    binary_name
}

fn sanitize_fidl_library_name(library_name: &str) -> String {
    let library_name = String::from(library_name);
    let library_name = library_name.replace("_", "");
    let library_name = library_name.replace("-", "");
    library_name
}

fn sanitize_rust_crate_name(crate_name: &str) -> String {
    let crate_name = crate_name.replace(".", "_");
    let crate_name = crate_name.replace("-", "_");
    crate_name
}

fn dir_create_if_absent<P: AsRef<Path>>(path: P) -> Result<(), Error> {
    let path = path.as_ref();
    if !path.exists() {
        info!("Creating directory {}", path.display());
        std::fs::create_dir_all(path)?;
    }
    Ok(())
}

fn dir_copy(src: impl AsRef<Path>, dst: impl AsRef<Path>) -> Result<(), Error> {
    let src = src.as_ref();
    let dst = dst.as_ref();
    let skip_root = true;
    for entry in walk_dir(src, skip_root) {
        let entry_src = entry.path();
        let entry_dst = dst.join(path_relative_to(src, entry_src));
        if entry.file_type().is_dir() {
            dir_copy(entry_src, entry_dst)?;
        } else {
            file_copy(entry_src, entry_dst)?;
        }
    }
    Ok(())
}

fn file_copy(src: impl AsRef<Path>, dst: impl AsRef<Path>) -> Result<(), Error> {
    let src = src.as_ref();
    let dst = dst.as_ref();
    dir_create_if_absent(dst.parent().unwrap())?;
    info!("Copying file {} to {}", src.display(), dst.display());
    std::fs::copy(src, dst)?;
    Ok(())
}

fn file_write<P: AsRef<Path>>(path: P, contents: &str) -> Result<(), Error> {
    let path = path.as_ref();
    dir_create_if_absent(path.parent().unwrap())?;
    info!("Writing file {}", path.display());
    std::fs::write(path, contents)?;
    Ok(())
}

fn path_file_stem<P: AsRef<Path>>(path: P) -> String {
    path.as_ref().file_stem().unwrap().to_str().unwrap().to_string()
}

fn path_relative_to(base: impl AsRef<Path>, path: impl AsRef<Path>) -> PathBuf {
    path.as_ref().strip_prefix(base).unwrap().to_owned()
}

// Walks the directory at `path` skipping '.' and '..'.
// If `skip_root` is true the returned iterator skips `path` and only iterates over its children.
fn walk_dir<P: AsRef<Path>>(path: P, skip_root: bool) -> impl Iterator<Item = DirEntry> {
    let mut walk = WalkDir::new(path);
    if skip_root {
        walk = walk.min_depth(1);
    }
    walk.into_iter().filter_entry(is_not_hidden).filter_map(|v| v.ok())
}

// Returns true if entry is a hidden filesystem entity.
fn is_not_hidden(entry: &DirEntry) -> bool {
    entry.file_name().to_str().map(|s| entry.depth() == 0 || !s.starts_with(".")).unwrap_or(false)
}

struct CodeGenerator {
    handlebars: handlebars::Handlebars<'static>,
}

impl CodeGenerator {
    fn new() -> Self {
        let mut code_generator = Self { handlebars: Handlebars::new() };
        code_generator.install_handlebars_helpers();
        code_generator
    }

    fn generate(
        &self,
        template_code: &str,
        template_vars: impl serde::Serialize,
    ) -> Result<String, Error> {
        let code = self.handlebars.render_template(template_code, &template_vars)?;
        Ok(code)
    }

    fn install_handlebars_helpers(&mut self) {
        // Returns the current year.
        handlebars_helper!(helper_year: |*args| {
            let _ = args;
            format!("{}", chrono::Utc::now().year())
        });

        self.handlebars.register_helper("year", Box::new(helper_year));
    }
}
