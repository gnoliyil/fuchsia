// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{anyhow, bail, Context as _, Result};
use argh::FromArgs;
use camino::{Utf8Path, Utf8PathBuf};
use gnaw_lib::CrateOutputMetadata;
use rayon::prelude::*;
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::File,
    io::{BufReader, Write},
    path::{Path, PathBuf},
    process::Command,
};
use walkdir::WalkDir;
use xml::reader::{EventReader, XmlEvent};

/// update OWNERS files for external Rust code
///
/// This tool relies on GN metadata produced from a maximal "kitchen sink" build. When run
/// outside the context of `fx update-3p-owners`, it also relies on being run after
/// `fx update-rustc-third-party`.
#[derive(FromArgs)]
struct Options {
    /// path to the JSON metadata produced by cargo-gnaw
    #[argh(option)]
    rust_metadata: Option<PathBuf>,

    /// path to the 3P integration manifest
    #[argh(option)]
    integration_manifest: Option<PathBuf>,

    /// path to the ownership overrides config file
    #[argh(option)]
    overrides: Utf8PathBuf,

    /// path to out/default (or the equivalent for the current build)
    #[argh(option)]
    out_dir: PathBuf,

    /// number of threads to allow, each thread runs 0-1 instances of GN at a time
    #[argh(option)]
    num_threads: Option<usize>,

    /// path to the prebuilt GN binary
    #[argh(option)]
    gn_bin: PathBuf,

    /// generate OWNERS only for the given projects path. Can contain multiple comma-separated
    /// paths.
    #[argh(option)]
    path: Option<String>,

    /// don't updated existing OWNERS files
    #[argh(switch)]
    skip_existing: bool,

    /// print the generated content without updating OWNERS files
    #[argh(switch)]
    dry_run: bool,
}

fn main() -> Result<()> {
    tracing_subscriber::fmt().compact().with_max_level(tracing::Level::INFO).init();
    let Options {
        rust_metadata,
        integration_manifest,
        overrides,
        out_dir,
        gn_bin,
        num_threads,
        path: projects_path,
        skip_existing,
        dry_run,
    } = argh::from_env();
    let update_strategy = match skip_existing {
        true => UpdateStrategy::OnlyMissing,
        false => UpdateStrategy::AllFiles,
    };

    if let Some(num_threads) = num_threads {
        rayon::ThreadPoolBuilder::new().num_threads(num_threads).build_global().unwrap();
    }

    OwnersDb::new(
        rust_metadata,
        integration_manifest,
        overrides,
        gn_bin,
        out_dir,
        update_strategy,
        projects_path,
        dry_run,
    )?
    .update_all_files()
}

struct ProjectMetadata {
    /// name of the project
    pub name: String,

    /// filesystem path to the project
    pub path: Utf8PathBuf,

    /// list of GN targets for depending on the project
    pub targets: Vec<String>,
}

#[derive(PartialEq)]
enum UpdateStrategy {
    /// update all the OWNERS files
    AllFiles,

    /// only add OWNERS files where missing, leaving the existing OWNERS files unchanged
    OnlyMissing,
}

struct OwnersDb {
    /// metadata about projects
    projects: Vec<ProjectMetadata>,

    /// cache of OWNERS file paths, indexed by target name. Holds the targets for which the
    /// corresponding OWNERS file path is known; cached to avoid probing the filesystem
    owners_path_cache: BTreeMap<String, Utf8PathBuf>,

    /// path to the JSON metadata produced by cargo-gnaw
    rust_metadata: Option<PathBuf>,

    /// explicit lists of OWNERS files to include instead of inferring, indexed by project name
    overrides: BTreeMap<String, Vec<Utf8PathBuf>>,

    /// if set, print update results instead of updating OWNERS file
    dry_run: bool,

    update_strategy: UpdateStrategy,
    gn_bin: PathBuf,
    out_dir: PathBuf,
}

impl OwnersDb {
    fn new(
        rust_metadata: Option<PathBuf>,
        integration_manifest: Option<PathBuf>,
        overrides: Utf8PathBuf,
        gn_bin: PathBuf,
        out_dir: PathBuf,
        update_strategy: UpdateStrategy,
        projects_path: Option<String>,
        dry_run: bool,
    ) -> Result<Self> {
        let rust_crates: Vec<CrateOutputMetadata> = rust_metadata
            .as_ref()
            .map(|metadata| {
                Ok::<_, anyhow::Error>(
                    serde_json::from_reader(
                        File::open(metadata)
                            .with_context(|| format!("opening {}", metadata.display()))?,
                    )
                    .with_context(|| format!("parsing {}", metadata.display()))?,
                )
            })
            .transpose()?
            .unwrap_or_default();

        // OWNERS path is currently only cached for rust projects.
        let mut owners_path_cache = rust_crates
            .iter()
            .map(|metadata| (metadata.canonical_target.clone(), metadata.path.clone()))
            .collect::<BTreeMap<_, _>>();
        let mut path_by_top_level_target = rust_crates
            .iter()
            .filter_map(|metadata| {
                metadata.shortcut_target.as_ref().map(|t| (t.clone(), metadata.path.clone()))
            })
            .collect::<BTreeMap<_, _>>();
        owners_path_cache.append(&mut path_by_top_level_target);

        let rust_projects: Vec<ProjectMetadata> = rust_crates
            .into_iter()
            .map(|metadata| ProjectMetadata {
                name: metadata.name,
                path: metadata.path,
                targets: toolchain_suffixed_targets(
                    &metadata.canonical_target,
                    metadata.shortcut_target.as_ref().map(String::as_str),
                ),
            })
            .collect();
        let integration_projects = integration_manifest
            .map(|manifest| {
                Ok::<_, anyhow::Error>(
                    parse_integration_manifest(&manifest)
                        .with_context(|| format!("parsing {}", manifest.display()))?,
                )
            })
            .transpose()?
            .unwrap_or_default();

        let path_projects = match projects_path {
            Some(path) => parse_path(path),
            None => vec![],
        };
        let projects = rust_projects
            .into_iter()
            .chain(integration_projects)
            .chain(path_projects)
            .collect::<Vec<_>>();

        let overrides: BTreeMap<String, Vec<Utf8PathBuf>> = toml::de::from_str(
            &std::fs::read_to_string(&overrides)
                .with_context(|| format!("reading {}", overrides))?,
        )
        .with_context(|| format!("parsing {}", overrides))?;

        Ok(Self {
            projects,
            owners_path_cache,
            rust_metadata,
            overrides,
            update_strategy,
            dry_run,
            gn_bin,
            out_dir,
        })
    }

    /// Update all OWNERS files for all projects.
    fn update_all_files(&self) -> Result<()> {
        tracing::info!("Updating OWNERS files...");
        self.projects
            .par_iter()
            .filter(|metadata| !metadata.path.starts_with("third_party/rust_crates/mirrors"))
            .map(|metadata| self.update_owners_file(metadata, &mut std::io::stdout()))
            .panic_fuse()
            .collect::<Result<()>>()?;
        tracing::info!("\nDone!");

        Ok(())
    }

    /// Update the OWNERS file for a single 3p project.
    fn update_owners_file<W: Write>(
        &self,
        metadata: &ProjectMetadata,
        output_buffer: &mut W,
    ) -> Result<()> {
        if self.update_strategy == UpdateStrategy::OnlyMissing
            && metadata.path.join("OWNERS").exists()
        {
            tracing::info!("\n{} has OWNERS file, skipping", metadata.path);
            return Ok(());
        }
        let file = self.compute_owners_file(metadata)?;
        let owners_path = metadata.path.join("OWNERS");
        if self.dry_run {
            tracing::info!("Dry-run: generated {} with content:\n", owners_path);
            output_buffer.write_all(file.to_string().as_bytes())?;
        } else {
            // We need to write every OWNERS file, even if it would be empty,
            // because the other OWNERS files may include the empty ones.
            std::fs::write(owners_path, file.to_string().as_bytes())?;
        }
        eprint!(".");

        Ok(())
    }

    fn compute_owners_file(&self, metadata: &ProjectMetadata) -> Result<OwnersFile> {
        let override_key = if metadata.path.starts_with("third_party/rust_crates") {
            let mut path = metadata.path.parent().expect("has parent").to_owned();
            path.push(&metadata.name);
            path.into_string()
        } else {
            metadata.path.to_owned().into_string()
        };
        if let Some(owners_overrides) = self.overrides.get(&override_key) {
            Ok(OwnersFile {
                path: metadata.path.join("OWNERS"),
                includes: owners_overrides.iter().map(Clone::clone).collect(),
                source: OwnersSource::Override,
            })
        } else {
            self.owners_files_from_reverse_deps(&metadata)
        }
    }

    /// Run `gn refs` for the project's GN target(s) and find the OWNERS files that correspond to its
    /// reverse deps.
    ///
    /// For Rust projects, cargo-gnaw metadata encodes version-unambiguous GN targets like
    /// `//third_party/rust_crates:foo-v1_0_0` but we discourage the use of those targets
    /// throughout the tree. To find dependencies from in-house code we need to also get reverse
    /// deps for the equivalent target without the version, e.g. `//third_party/rust_crates:foo`.
    fn owners_files_from_reverse_deps(&self, metadata: &ProjectMetadata) -> Result<OwnersFile> {
        let mut deps = metadata
            .targets
            .par_iter()
            .map(|target| self.reverse_deps(target))
            .collect::<Result<Vec<_>, _>>()?
            .into_iter()
            .flatten()
            .collect::<BTreeSet<String>>();

        // non-rust projects are sometimes referenced by file. If no deps were found, search for
        // references to any of the files in the project.
        if deps.is_empty() && !metadata.path.starts_with("third_party/rust_crates") {
            tracing::info!(
                "\n{} has no target references, searching for all file references",
                metadata.path
            );
            let targets: Vec<String> = WalkDir::new(&metadata.path)
                .into_iter()
                .filter_map(|entry| {
                    entry
                        .ok()
                        .filter(|e| e.path().is_file())
                        .map(|e| e.into_path().into_os_string().into_string().unwrap())
                })
                .collect();
            deps = targets
                .par_iter()
                .map(|target| self.reverse_deps(target))
                .collect::<Result<Vec<_>, _>>()?
                .into_iter()
                .flatten()
                .collect::<BTreeSet<String>>();
        }

        let mut includes = BTreeSet::new();
        for dep in &deps {
            let included = self.owners_file_for_gn_target(&*dep)?;
            if should_include(&included) {
                includes.insert(included);
            }
        }

        Ok(OwnersFile {
            path: metadata.path.join("OWNERS"),
            includes: includes
                .into_iter()
                .filter(|i| !metadata.path.starts_with(i.parent().unwrap()))
                .collect(),
            source: OwnersSource::ReverseDependencies { targets: metadata.targets.clone(), deps },
        })
    }

    /// Run `gn refs $OUT_DIR $GN_TARGET` and return a list of GN targets which depend on the
    /// target.
    fn reverse_deps(&self, target: &str) -> Result<BTreeSet<String>> {
        gn_reverse_deps(&self.gn_bin, &self.out_dir, target)
    }

    /// Given a GN target, find the most likely path for its corresponding OWNERS file.
    fn owners_file_for_gn_target(&self, target: &str) -> Result<Utf8PathBuf> {
        // none of the metadata we have emits toolchain suffices, so remove them. the target
        // toolchain is the default toolchain so we don't encounter an targets suffixed that way
        let target = if let Some(idx) = target.find(GN_TOOLCHAIN_SUFFIX_PREFIX) {
            target.split_at(idx).0
        } else {
            target
        };
        Ok(if target.starts_with(RUST_EXTERNAL_TARGET_PREFIX) && self.rust_metadata.is_some() {
            // if the target is for a 3p crate it might not have an owners file yet, so we don't
            // want to rely on probing the filesystem. instead we'll construct a path *a priori*
            if let Some(path) = self.owners_path_cache.get(target) {
                path.join("OWNERS")
            } else {
                bail!(
                    "{} not in {}",
                    target,
                    self.rust_metadata.as_ref().expect("metadata is set").display()
                );
            }
        } else {
            // the target is outside of the 3p directory, so we need to probe for the closest file
            let no_slashes =
                target.strip_prefix("//").expect("GN targets from refs should be absolute");
            // remove the target name after the colon
            let path_portion = no_slashes.rsplitn(2, ":").skip(1).next().unwrap();
            let mut target = Utf8Path::new(path_portion);
            while !target.join("OWNERS").exists() {
                target =
                    target.parent().expect("we will always find an OWNERS file in the source tree");
            }
            target.join("OWNERS")
        })
    }
}

/// Fully qualified GN targets have a toolchain suffix like `//foo:bar(//path/to/toolchain:target)`.
/// We need to remove these suffices from targets when looking them up in our JSON metadata because
/// cargo-gnaw doesn't emit toolchains in its metadata.
///
/// Fuchsia's toolchains are by convention all currently defined under `//build/toolchain`.
const GN_TOOLCHAIN_SUFFIX_PREFIX: &str = "(//build/toolchain";

/// Prefix found on all generated GN targets for 3p crates.
const RUST_EXTERNAL_TARGET_PREFIX: &str = "//third_party/rust_crates:";

fn toolchain_suffixed_targets(versioned: &str, top_level: Option<&str>) -> Vec<String> {
    let mut targets = vec![];
    add_all_toolchain_suffices(versioned, &mut targets);
    top_level.map(|t| add_all_toolchain_suffices(t, &mut targets));
    targets
}

fn add_all_toolchain_suffices(target: &str, targets: &mut Vec<String>) {
    // TODO(fxbug.dev/73485) support querying explicitly for both linux and mac
    // TODO(fxbug.dev/71352) support querying explicitly for both x64 and arm64
    #[cfg(target_arch = "x86_64")]
    const HOST_ARCH_SUFFIX: &str = "x64";
    #[cfg(target_arch = "aarch64")]
    const HOST_ARCH_SUFFIX: &str = "arm64";

    // without a suffix, default toolchain is target
    targets.push(target.to_string());
    // we can only query for linux on a linux host and for mac on a mac
    targets.push(format!("{}(//build/toolchain:host_{})", target, HOST_ARCH_SUFFIX));
    targets.push(format!("{}(//build/toolchain:unknown_wasm32)", target));
}

#[derive(Debug)]
enum OwnersSource {
    /// file is computed from reverse deps and they are listed here
    ReverseDependencies {
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        targets: Vec<String>,
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        deps: BTreeSet<String>,
    },
    /// file is computed from overrides in //third_party/owners.toml
    Override,
}

impl OwnersSource {
    fn is_computed(&self) -> bool {
        matches!(self, OwnersSource::ReverseDependencies { .. })
    }
}

#[derive(Debug)]
struct OwnersFile {
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    path: Utf8PathBuf,
    includes: BTreeSet<Utf8PathBuf>,
    source: OwnersSource,
}

const AUTOGENERATED_HEADER: &str = "# AUTOGENERATED FROM DEPENDENT BUILD TARGETS.";
const HEADER: &str = "\
# TO MAKE CHANGES HERE, UPDATE //third_party/owners.toml.
# DOCS: https://fuchsia.dev/reference/tools/fx/cmd/update-3p-owners
";

impl std::fmt::Display for OwnersFile {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.source.is_computed() {
            writeln!(f, "{}", AUTOGENERATED_HEADER)?;
        }

        writeln!(f, "{}", HEADER)?;
        for to_include in &self.includes {
            write!(f, "include /{}\n", to_include)?;
        }
        Ok(())
    }
}

fn gn_reverse_deps(gn_bin: &Path, out_dir: &Path, target: &str) -> Result<BTreeSet<String>> {
    let output = Command::new(gn_bin).arg("refs").arg(out_dir).arg(target).output()?;
    let stdout = String::from_utf8(output.stdout.clone())?;

    if !output.status.success() {
        if stdout.contains("The input matches no targets, configs, or files.") {
            // the target exists in the filesystem but isn't in the existing build graph
            return Ok(Default::default());
        }
        bail!("`gn refs {}` failed: {:?}", target, output);
    }

    let revdeps: BTreeSet<String> = if stdout.contains("Nothing references this.") {
        Default::default()
    } else {
        stdout.lines().map(ToString::to_string).collect()
    };

    Ok(revdeps)
}

fn should_include(owners_file: &Utf8Path) -> bool {
    let owners_file = owners_file.as_os_str().to_str().unwrap();
    // many of these repos aren't open
    !owners_file.starts_with("vendor") &&
    // we don't ever need to include the root OWNERS file
    owners_file != "OWNERS"
}

fn parse_integration_manifest(manifest_path: &PathBuf) -> Result<Vec<ProjectMetadata>> {
    let parser = EventReader::new(BufReader::new(File::open(&manifest_path)?));

    parser
        .into_iter()
        .filter(|e| {
            matches!(
                e,
                Ok(XmlEvent::StartElement { name, .. })
                    if name.local_name == String::from("project")
            )
        })
        .map(|e| match e {
            Ok(XmlEvent::StartElement { attributes, .. }) => {
                let name = &attributes
                    .iter()
                    .find(|&a| a.name.local_name == "name")
                    .ok_or(anyhow!("no name attribute"))?
                    .value;
                let path = &attributes
                    .iter()
                    .find(|&a| a.name.local_name == "path")
                    .ok_or(anyhow!("no path attribute"))?
                    .value
                    .trim_end_matches("/src");
                Ok(ProjectMetadata {
                    name: name.to_string(),
                    path: Utf8PathBuf::from(path),
                    // the project can be referred by any of its inner targets.
                    targets: vec![path.to_string() + "/*"],
                })
            }
            _ => bail!("unreachable"),
        })
        .collect()
}

fn parse_path(project_paths: String) -> Vec<ProjectMetadata> {
    let projects = project_paths
        .split_terminator(",")
        .map(|path| {
            let path = path.trim_end_matches('/');
            ProjectMetadata {
                name: path.to_string(),
                path: Utf8PathBuf::from(path.to_string()),
                targets: vec![path.to_owned() + ("/*")],
            }
        })
        .collect::<Vec<_>>();

    projects
}

#[cfg(test)]
mod tests {
    use super::*;
    use once_cell::sync::Lazy;
    use pretty_assertions::assert_eq;
    use serial_test::serial;
    use std::{path::PathBuf, process::Command};

    #[test]
    #[serial] // these tests mutate the current process' working directory
    fn parse_gn_reverse_deps() {
        let mut expected = BTreeSet::new();
        expected.insert("//:bar".to_string());
        assert_eq!(get_rev_deps("pass", "//foo"), expected);
    }

    #[test]
    #[serial] // these tests mutate the current process' working directory
    fn parse_gn_empty_reverse_deps() {
        assert_eq!(get_rev_deps("empty", "//foo"), Default::default());
    }

    #[test]
    #[serial] // these tests mutate the current process' working directory
    #[should_panic] // if the target is altogether missing, it should return an error
    fn parse_gn_target_isnt_in_build() {
        get_rev_deps("missing", "//foo");
    }

    #[test]
    fn parse_integration_manifest_projects() {
        let projects =
            parse_integration_manifest(&PATHS.test_base_dir.join("integration/manifest")).unwrap();
        assert_eq!(
            projects
                .into_iter()
                .map(|ProjectMetadata { name: _, path, targets: _ }| path
                    .as_os_str()
                    .to_str()
                    .unwrap()
                    .to_string())
                .collect::<Vec<String>>(),
            ["third_party/foo", "third_party/bar"]
        );
    }

    #[test]
    fn parse_path_projects() {
        let projects = parse_path("foo/,bar".to_string());
        assert_eq!(
            projects
                .into_iter()
                .map(|ProjectMetadata { name: _, path, targets: _ }| path
                    .as_os_str()
                    .to_str()
                    .unwrap()
                    .to_string())
                .collect::<Vec<String>>(),
            ["foo", "bar"]
        );
    }

    #[test]
    #[serial] // these tests mutate the current process' working directory
    fn override_owners() {
        let test_dir = setup_test_dir("owners");
        let test_dir_path = test_dir.path().to_path_buf();
        let out_dir = test_dir_path.join("out");
        let overrides = Utf8PathBuf::from_path_buf(PATHS.test_base_dir.join("owners/owners.toml"))
            .expect("path is valid");

        let rust_metadata = Some(PATHS.test_base_dir.join("owners/rust_metadata.json"));
        assert!(OwnersDb::new(
            rust_metadata,
            None,
            overrides,
            PATHS.gn_binary_path.clone(),
            out_dir,
            UpdateStrategy::AllFiles,
            None,
            false,
        )
        .expect("valid OwnersDb")
        .update_all_files()
        .is_ok());

        // check that an OWNERS file was created and that the content is correct.
        // The file should not contain the 'autogenerated' header because of the override.
        let content = std::fs::read_to_string(
            test_dir_path.join("third_party/rust_crates/vendor/foo/OWNERS"),
        )
        .expect("read succeeds");
        assert_eq!(
            content,
            format!(
                "{}
include /third_party/foo/OWNERS
",
                HEADER
            )
        );
    }

    #[test]
    #[serial] // these tests mutate the current process' working directory
    fn update_outdated_owners() {
        let test_dir = setup_test_dir("owners");
        let test_dir_path = test_dir.path().to_path_buf();
        let out_dir = test_dir_path.join("out");
        let overrides = Utf8PathBuf::from_path_buf(PATHS.test_base_dir.join("owners/owners.toml"))
            .expect("path is valid");
        let rust_metadata = Some(PATHS.test_base_dir.join("owners/rust_metadata.json"));

        // read the initial OWNERS file content
        let initial_content = std::fs::read_to_string(
            test_dir_path.join("third_party/rust_crates/vendor/bar/OWNERS"),
        )
        .expect("read succeeds");
        assert!(OwnersDb::new(
            rust_metadata,
            None,
            overrides,
            PATHS.gn_binary_path.clone(),
            out_dir,
            UpdateStrategy::AllFiles,
            None,
            false,
        )
        .expect("valid OwnersDb")
        .update_all_files()
        .is_ok());

        // check that the OWNERS file content was updated.
        let updated_content = std::fs::read_to_string(
            test_dir_path.join("third_party/rust_crates/vendor/bar/OWNERS"),
        )
        .expect("read succeeds");
        assert_ne!(initial_content, updated_content);
        assert_eq!(
            updated_content,
            format!(
                "{}\n{}
include /dep/OWNERS
",
                AUTOGENERATED_HEADER, HEADER
            )
        );
    }

    #[test]
    #[serial] // these tests mutate the current process' working directory
    fn generate_missing() {
        let test_dir = setup_test_dir("owners");
        let test_dir_path = test_dir.path().to_path_buf();
        let out_dir = test_dir_path.join("out");
        let overrides = Utf8PathBuf::from_path_buf(PATHS.test_base_dir.join("owners/owners.toml"))
            .expect("path is valid");
        let manifest = Some(PATHS.test_base_dir.join("owners/manifest"));
        let rust_metadata = Some(PATHS.test_base_dir.join("owners/rust_metadata.json"));

        // an OWNERS file should be generated for projects missing owners
        assert!(!test_dir_path.join("third_party/bar/OWNERS").exists());
        // the content should not be updated for projects that have OWNERS files
        let initial_content = std::fs::read_to_string(
            test_dir_path.join("third_party/rust_crates/vendor/bar/OWNERS"),
        )
        .expect("read succeeds");
        assert!(OwnersDb::new(
            rust_metadata,
            manifest,
            overrides,
            PATHS.gn_binary_path.clone(),
            out_dir,
            UpdateStrategy::OnlyMissing,
            None,
            false,
        )
        .expect("valid OwnersDb")
        .update_all_files()
        .is_ok());

        // check that the OWNERS file was generated
        let content = std::fs::read_to_string(test_dir_path.join("third_party/bar/OWNERS"))
            .expect("read succeeds");
        assert_eq!(
            content,
            format!(
                "{}\n{}
include /dep/OWNERS
",
                AUTOGENERATED_HEADER, HEADER
            )
        );
        // check that the OWNERS file was not updated
        assert_eq!(
            initial_content,
            std::fs::read_to_string(
                test_dir_path.join("third_party/rust_crates/vendor/bar/OWNERS"),
            )
            .expect("read succeeds")
        );
    }

    #[test]
    #[serial] // these tests mutate the current process' working directory
    fn dry_run() {
        let test_dir = setup_test_dir("owners");
        let test_dir_path = test_dir.path().to_path_buf();
        let out_dir = test_dir_path.join("out");
        let overrides = Utf8PathBuf::from_path_buf(PATHS.test_base_dir.join("owners/owners.toml"))
            .expect("path is valid");
        let manifest = Some(PATHS.test_base_dir.join("owners/manifest"));

        // check that the project doesn't have an OWNERS file
        assert!(!test_dir_path.join("third_party/bar/OWNERS").exists());

        let owners_db = OwnersDb::new(
            None,
            manifest,
            overrides,
            PATHS.gn_binary_path.clone(),
            out_dir,
            UpdateStrategy::AllFiles,
            None,
            true,
        )
        .expect("OwnersDb is valid");
        let unowned_metadata = owners_db
            .projects
            .iter()
            .find(|m| m.name.contains("bar"))
            .expect("should contain project metadata");
        let mut output_buffer = vec![];
        assert!(owners_db.update_owners_file(&unowned_metadata, &mut output_buffer).is_ok());

        // check that an OWNERS file was not created for the project
        assert!(!test_dir_path.join("third_party/bar/OWNERS").exists());

        // check that the dry-run output is correct
        let output = String::from_utf8(output_buffer).unwrap();
        assert_eq!(
            output,
            format!(
                "{}\n{}
include /dep/OWNERS
",
                AUTOGENERATED_HEADER, HEADER
            )
        );
    }

    #[test]
    #[serial] // these tests mutate the current process' working directory
    fn update_owners_for_path() {
        let test_dir = setup_test_dir("owners");
        let test_dir_path = test_dir.path().to_path_buf();
        let out_dir = test_dir_path.join("out");
        let overrides = Utf8PathBuf::from_path_buf(PATHS.test_base_dir.join("owners/owners.toml"))
            .expect("path is valid");

        assert!(OwnersDb::new(
            None,
            None,
            overrides,
            PATHS.gn_binary_path.clone(),
            out_dir,
            UpdateStrategy::AllFiles,
            Some("third_party/bar/".to_string()),
            false,
        )
        .expect("valid OwnersDb")
        .update_all_files()
        .is_ok());

        // check that the OWNERS file was generated and its content is correct
        let content = std::fs::read_to_string(test_dir_path.join("third_party/bar/OWNERS"))
            .expect("read succeeds");
        assert_eq!(
            content,
            format!(
                "{}\n{}
include /dep/OWNERS
",
                AUTOGENERATED_HEADER, HEADER
            )
        );
    }

    #[test]
    #[serial] // these tests mutate the current process' working directory
    fn search_for_file_refs() {
        let test_dir = setup_test_dir("owners");
        let test_dir_path = test_dir.path().to_path_buf();
        let out_dir = test_dir_path.join("out");
        let overrides = Utf8PathBuf::from_path_buf(PATHS.test_base_dir.join("owners/owners.toml"))
            .expect("path is valid");

        // the 'baz' project is depended on by file, not gn target.
        assert!(OwnersDb::new(
            None,
            None,
            overrides,
            PATHS.gn_binary_path.clone(),
            out_dir,
            UpdateStrategy::AllFiles,
            Some("third_party/baz/".to_string()),
            false,
        )
        .expect("valid OwnersDb")
        .update_all_files()
        .is_ok());

        // check that the OWNERS file was generated and its content is correct
        let content = std::fs::read_to_string(test_dir_path.join("third_party/baz/OWNERS"))
            .expect("read succeeds");
        assert_eq!(
            content,
            format!(
                "{}\n{}
include /dep/OWNERS
",
                AUTOGENERATED_HEADER, HEADER
            )
        );
    }

    fn setup_test_dir(test_subdir: &str) -> tempfile::TempDir {
        let original_test_dir = PATHS.test_base_dir.join(test_subdir);
        let test_dir = tempfile::tempdir().unwrap();
        let test_dir_path = test_dir.path().to_path_buf();
        let out_dir = test_dir_path.join("out");

        copy_contents(&original_test_dir, &test_dir_path);
        copy_contents(&PATHS.test_base_dir.join("common"), &test_dir_path);

        // cd to test directory so the below command *and* those in `gn_reverse_deps()` share cwd
        std::env::set_current_dir(&test_dir_path).expect("setting current dir");

        // generate a gn out directory
        assert!(Command::new(&PATHS.gn_binary_path)
            .current_dir(test_dir_path)
            .arg("gen")
            .arg(&out_dir)
            .status()
            .expect("generating out directory")
            .success());
        test_dir
    }

    fn get_rev_deps(test_subdir: &str, target: &str) -> BTreeSet<String> {
        let test_dir = setup_test_dir(test_subdir);
        let out_dir = test_dir.path().to_path_buf().join("out");

        // parse the reverse deps
        gn_reverse_deps(&PATHS.gn_binary_path, &out_dir, target).expect("getting reverse deps")
    }

    fn copy_contents(original_test_dir: &Path, test_dir_path: &Path) {
        // copy the contents of original test dir to test_dir
        for entry in walkdir::WalkDir::new(&original_test_dir) {
            let entry = entry.expect("walking original test directory to copy files to /tmp");
            if !entry.file_type().is_file() {
                continue;
            }
            let to_copy = entry.path();
            let destination = test_dir_path.join(to_copy.strip_prefix(&original_test_dir).unwrap());
            std::fs::create_dir_all(destination.parent().unwrap())
                .expect("making parent of file to copy");
            std::fs::copy(to_copy, destination).expect("copying file");
        }
        tracing::info!("done copying files");
    }

    /// All the paths to runfiles and tools which are used in this test.
    ///
    /// All paths are absolute, and are resolved based on knowing that they are all
    /// beneath the directory in which this test binary is stored.  See the `BUILD.gn`
    /// file for this test target and the corresponding `host_test_data` targets.
    ///
    /// Note that it is not possible to refer to paths inside the source tree, because
    /// the source infra runners only have access to the output artifacts (i.e. contents
    /// of the "out" directory).
    #[derive(Debug)]
    struct Paths {
        /// `.../host_x64`
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        test_root_dir: PathBuf,

        /// `.../host_x64/test_data`, this is the root of the runfiles tree, a
        /// path //foo/bar will be copied at `.../host_x64/test_data/foo/bar` for
        /// this test.
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        test_data_dir: PathBuf,

        /// `.../host_x64/test_data/tools/auto_owners/tests`: this is the directory
        /// where GN golden files are placed. Corresponds to `//tools/auto_owners/tests`.
        test_base_dir: PathBuf,

        /// `.../host_x64/test_data/tools/auto_owners/runfiles`: this is the directory
        /// where the binary runfiles live.
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        runfiles_dir: PathBuf,

        /// `.../runfiles/gn`: the absolute path to the gn binary. gn is used for
        /// formatting.
        gn_binary_path: PathBuf,
    }

    /// Gets the hermetic test paths for the runfiles and tools used in this test.
    ///
    /// The hermetic test paths are computed based on the parent directory of this
    /// binary.
    static PATHS: Lazy<Paths> = Lazy::new(|| {
        let cwd = std::env::current_dir().unwrap();
        let first_arg = dbg!(std::env::args().next().unwrap());
        let test_binary_path = dbg!(cwd.join(first_arg));

        let test_root_dir = test_binary_path.parent().unwrap();

        let test_data_dir: PathBuf =
            [test_root_dir.to_str().unwrap(), "test_data"].iter().collect();

        let test_base_dir: PathBuf =
            [test_data_dir.to_str().unwrap(), "tools", "auto_owners", "tests"].iter().collect();

        let runfiles_dir: PathBuf =
            [test_root_dir.to_str().unwrap(), "test_data", "tools", "auto_owners", "runfiles"]
                .iter()
                .collect();

        let gn_binary_path: PathBuf = [runfiles_dir.to_str().unwrap(), "gn", "gn"].iter().collect();

        Paths {
            test_root_dir: test_root_dir.to_path_buf(),
            test_data_dir,
            test_base_dir,
            runfiles_dir,
            gn_binary_path,
        }
    });
}
