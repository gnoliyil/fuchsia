// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Context, Result},
    fuchsia_hash::Hash,
    fuchsia_pkg::{PackageBuilder, PackageManifest, PackagePath},
    fuchsia_url::pkg_url::PinnedPkgUrl,
    serde_json::json,
    std::{collections::BTreeMap, fs::File, io::Write, path::Path, str::FromStr},
};

/// The path to the static package index file in the `base` package.
const STATIC_PACKAGE_INDEX: &str = "data/static_packages";

/// The path to the cache package index file in the `base` package.
const CACHE_PACKAGE_INDEX: &str = "data/cache_packages.json";

/// A builder that constructs base packages.
#[derive(Default)]
pub struct BasePackageBuilder {
    // Maps the blob destination -> source.
    contents: BTreeMap<String, String>,
    base_packages: PackageList,
    cache_packages: PackageUrlList,
}

impl BasePackageBuilder {
    /// Add all the blobs from `package` into the base package being built.
    pub fn add_files_from_package(&mut self, package: PackageManifest) {
        package.into_blobs().into_iter().filter(|b| b.path != "meta/").for_each(|b| {
            self.contents.insert(b.path, b.source_path);
        });
    }

    /// Add the `package` to the list of base packages, which is then added to
    /// base package as file `data/static_packages`.
    pub fn add_base_package(&mut self, package: PackageManifest) -> Result<()> {
        add_package_to(&mut self.base_packages, package)
    }

    /// Add the `package` to the list of cache packages, which is then added to
    /// base package as file `data/cache_packages`.
    pub fn add_cache_package(&mut self, package: PackageManifest) -> Result<()> {
        add_package_to(&mut self.cache_packages, package)
    }

    /// Build the base package and write the bytes to `out`.
    ///
    /// Intermediate files will be written to the directory specified by
    /// `gendir`.
    pub fn build(
        self,
        outdir: impl AsRef<Path>,
        gendir: impl AsRef<Path>,
        name: impl AsRef<str>,
        out: impl AsRef<Path>,
    ) -> Result<BasePackageBuildResults> {
        // Write all generated files in a subdir with the name of the package.
        let gendir = gendir.as_ref().join(name.as_ref());

        let Self { contents, base_packages, cache_packages } = self;

        // Capture the generated files
        let mut generated_files = BTreeMap::new();

        // Generate the base and cache package lists.
        let (dest, path) = write_index_file(&gendir, "base", STATIC_PACKAGE_INDEX, &base_packages)?;
        generated_files.insert(dest, path);

        let (dest, path) =
            write_index_file(&gendir, "cache", CACHE_PACKAGE_INDEX, &cache_packages)?;
        generated_files.insert(dest, path);

        // Construct the list of blobs in the base package that lives outside of the meta.far.
        let mut external_contents = contents;
        for (destination, source) in &generated_files {
            external_contents.insert(destination.clone(), source.clone());
        }

        // All base packages are named 'system-image' internally, for
        // consistency on the platform.
        let mut builder = PackageBuilder::new("system_image");
        // However, they can have different published names.  And the name here
        // is the name to publish it under (and to include in the generated
        // package manifest).
        builder.published_name(name);

        for (destination, source) in &external_contents {
            builder.add_file_as_blob(destination, source)?;
        }
        builder.manifest_path(outdir.as_ref().join("package_manifest.json"));
        builder.build(gendir, out)?;

        Ok(BasePackageBuildResults {
            contents: external_contents,
            base_packages,
            cache_packages,
            generated_files,
        })
    }
}

/// The results of building the `base` package.
///
/// These are based on the information that the builder is configured with, and
/// then augmented with the operations that the `BasePackageBuilder::build()`
/// fn performs, including an extra additions or removals.
///
/// This provides an audit trail of "what was created".
pub struct BasePackageBuildResults {
    // Maps the blob destination -> source.
    pub contents: BTreeMap<String, String>,
    pub base_packages: PackageList,
    pub cache_packages: PackageUrlList,

    /// The paths to the files generated by the builder.
    pub generated_files: BTreeMap<String, String>,
}

// Pulls out the path and merkle from `package` and adds it to `packages` with a path to
// merkle mapping.
fn add_package_to(list: &mut impl WritablePackageList, package: PackageManifest) -> Result<()> {
    let package_path = package.package_path();
    let package_name = package_path.name().as_ref();
    if package_name == "system_image" || package_name == "update" {
        return Err(anyhow!("system_image and update packages are not allowed"));
    }

    let package_repository = package.repository().clone();
    let path = package_path.to_string();
    package
        .into_blobs()
        .into_iter()
        .find(|blob| blob.path == "meta/")
        .ok_or(anyhow!("Failed to add package {} to the list, unable to find meta blob", path))
        .and_then(|meta_blob| list.insert(package_repository, path, meta_blob.merkle))
}

/// Helper fn to handle the (repeated) process of writing a list of packages
/// out to the expected file, and returning a (destination, source) tuple
/// for inclusion in the base package's contents.
fn write_index_file(
    gendir: impl AsRef<Path>,
    name: &str,
    destination: impl AsRef<str>,
    package_list: &impl WritablePackageList,
) -> Result<(String, String)> {
    // TODO(fxbug.dev/76326) Decide on a consistent pattern for using gendir and
    //   how intermediate files should be named and where in gendir they should
    //   be placed.
    //
    // For a file of destination "data/foo.txt", and a gendir of "assembly/gendir",
    //   this creates "assembly/gendir/data/foo.txt".
    let path = gendir.as_ref().join(destination.as_ref());
    let path_str = path
        .to_str()
        .ok_or(anyhow!(format!("package index path is not valid UTF-8: {}", path.display())))?;

    // Create any parent dirs necessary.
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).context(format!(
            "Failed to create parent dir {} for {} in gendir",
            parent.display(),
            destination.as_ref()
        ))?;
    }

    let mut index_file = File::create(&path)
        .context(format!("Failed to create the {} packages index file: {}", name, path_str))?;

    package_list.write(&mut index_file).context(format!(
        "Failed to write the {} package index file: {}",
        name,
        path.display()
    ))?;

    Ok((destination.as_ref().to_string(), path_str.to_string()))
}

/// `WritablePackageList` represents a collection of packages that can be populated and
/// written into a file. This allows for gradual migration for packages config files (base, cache)
/// to JSON incrementally.
/// TODO(fxb/94601): refactor out once base_packages are migrated to JSON format.
trait WritablePackageList {
    fn insert(
        &mut self,
        repository: Option<impl AsRef<str>>,
        name: impl AsRef<str>,
        merkle: Hash,
    ) -> Result<()>;
    fn write(&self, out: &mut impl Write) -> Result<()>;
}

/// A list of mappings between package name and merkle, which can be written to
/// a file to be placed in the Base Package.
#[derive(Default, Debug)]
pub struct PackageList {
    // Map between package name and merkle.
    packages: BTreeMap<String, Hash>,
}

impl WritablePackageList for PackageList {
    /// Add a new package with `name` and `merkle`.
    fn insert(
        &mut self,
        _repository: Option<impl AsRef<str>>,
        name: impl AsRef<str>,
        merkle: Hash,
    ) -> Result<()> {
        self.packages.insert(name.as_ref().to_string(), merkle);
        Ok(())
    }

    /// Generate the file to be placed in the Base Package.
    fn write(&self, out: &mut impl Write) -> Result<()> {
        for (name, merkle) in self.packages.iter() {
            writeln!(out, "{}={}", name, merkle)?;
        }
        Ok(())
    }
}

/// A list of package URLs pinned to a hash, which can be written to a file.
#[derive(Default, Debug)]
pub struct PackageUrlList {
    packages: Vec<PinnedPkgUrl>,
}

impl WritablePackageList for PackageUrlList {
    /// Insert a new pinned to hash URL into the list.
    fn insert(
        &mut self,
        repository: Option<impl AsRef<str>>,
        name: impl AsRef<str>,
        merkle: Hash,
    ) -> Result<()> {
        let repository =
            repository.ok_or(anyhow!("Unable to create package url: empty repository field."))?;
        let path = PackagePath::from_str(name.as_ref())
            .map_err(|e| anyhow!("Failed to parse package path: {}", e))?;
        let url = PinnedPkgUrl::new_package(
            repository.as_ref().to_string(),
            format!("/{}", path),
            merkle,
        )
        .map_err(|e| anyhow!("Failed to create package url: {}", e))?;
        self.packages.push(url);
        Ok(())
    }

    /// Generate the file to be placed in the Base Package.
    fn write(&self, writer: &mut impl Write) -> Result<()> {
        let contents = json!({
            "version": "1",
            "content": &self.packages,
        });
        serde_json::to_writer(writer, &contents).map_err(|e| anyhow!("Error writing JSON: {}", e))
    }
}

#[cfg(test)]
impl PartialEq<PackageList> for Vec<(String, Hash)> {
    fn eq(&self, other: &PackageList) -> bool {
        if self.len() == other.packages.len() {
            for item in self {
                match other.packages.get(&item.0) {
                    Some(hash) => {
                        if hash != &item.1 {
                            return false;
                        }
                    }
                    None => {
                        return false;
                    }
                }
            }
            return true;
        }
        return false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fuchsia_archive::Reader;
    use serde_json::json;
    use std::path::Path;
    use tempfile::{NamedTempFile, TempDir};

    #[test]
    fn package_list() {
        let mut out: Vec<u8> = Vec::new();
        let mut packages = PackageList::default();
        packages.insert(Some("testrepository.com"), "package0", Hash::from([0u8; 32])).unwrap();
        packages.insert(Some("testrepository.com"), "package1", Hash::from([17u8; 32])).unwrap();
        packages.write(&mut out).unwrap();
        assert_eq!(
            b"package0=0000000000000000000000000000000000000000000000000000000000000000\n\
                    package1=1111111111111111111111111111111111111111111111111111111111111111\n",
            &*out
        );
    }

    #[test]
    fn package_url_list() {
        let mut out: Vec<u8> = Vec::new();
        let mut packages = PackageUrlList::default();
        packages.insert(Some("testrepository.com"), "package0/0", Hash::from([0u8; 32])).unwrap();
        packages.insert(Some("testrepository.com"), "package1/0", Hash::from([17u8; 32])).unwrap();
        packages.write(&mut out).unwrap();
        assert_eq!(
            br#"{"content":["fuchsia-pkg://testrepository.com/package0/0?hash=0000000000000000000000000000000000000000000000000000000000000000","fuchsia-pkg://testrepository.com/package1/0?hash=1111111111111111111111111111111111111111111111111111111111111111"],"version":"1"}"#,
            &*out
        );
    }

    #[test]
    fn test_add_package_to() {
        let system_image = generate_test_manifest("system_image", "0", None);
        let update = generate_test_manifest("update", "0", None);
        let valid = generate_test_manifest("valid", "0", None);
        let mut packages = PackageList::default();
        assert!(add_package_to(&mut packages, system_image).is_err());
        assert!(add_package_to(&mut packages, update).is_err());
        assert!(add_package_to(&mut packages, valid).is_ok());
    }

    #[test]
    fn test_add_package_to_url_list() {
        let system_image = generate_test_manifest("system_image", "0", None);
        let update = generate_test_manifest("update", "0", None);
        let valid = generate_test_manifest("valid", "0", None);
        let mut packages = PackageUrlList::default();
        assert!(add_package_to(&mut packages, system_image).is_err());
        assert!(add_package_to(&mut packages, update).is_err());
        assert!(add_package_to(&mut packages, valid).is_ok());
    }

    #[test]
    fn build_with_unsupported_packages() {
        let mut builder = BasePackageBuilder::default();
        assert!(builder
            .add_base_package(generate_test_manifest("system_image", "0", None))
            .is_err());
        assert!(builder.add_base_package(generate_test_manifest("update", "0", None)).is_err());
    }

    #[test]
    fn build() {
        let outdir = TempDir::new().unwrap();
        let far_path = outdir.path().join("base.far");

        // Build the base package with an extra file, a base package, and a cache package.
        let mut builder = BasePackageBuilder::default();
        let test_file = NamedTempFile::new().unwrap();
        builder.add_files_from_package(generate_test_manifest(
            "package",
            "0",
            Some(test_file.path()),
        ));
        builder.add_base_package(generate_test_manifest("base_package", "0", None)).unwrap();
        builder.add_cache_package(generate_test_manifest("cache_package", "0", None)).unwrap();

        let gendir = TempDir::new().unwrap();
        let build_results =
            builder.build(&outdir.path(), &gendir.path(), "system_image", &far_path).unwrap();

        // The following asserts lead up to the final one, catching earlier failure points where it
        // can be more obvious as to why the test is failing, as the hashes themselves are opaque.

        // Verify the package list intermediate structures.
        assert_eq!(
            vec![("base_package/0".to_string(), Hash::from([0u8; 32]))],
            build_results.base_packages
        );
        let url: PinnedPkgUrl = "fuchsia-pkg://testrepository.com/cache_package/0\
             ?hash=0000000000000000000000000000000000000000000000000000000000000000"
            .parse()
            .unwrap();
        assert_eq!(vec![url], build_results.cache_packages.packages);

        // Inspect the generated files to verify their contents.
        let gen_static_index = build_results.generated_files.get("data/static_packages").unwrap();
        assert_eq!(
            "base_package/0=0000000000000000000000000000000000000000000000000000000000000000\n",
            std::fs::read_to_string(gen_static_index).unwrap()
        );

        let gen_cache_index =
            build_results.generated_files.get("data/cache_packages.json").unwrap();
        let cache_packages_json = r#"{"content":["fuchsia-pkg://testrepository.com/cache_package/0?hash=0000000000000000000000000000000000000000000000000000000000000000"],"version":"1"}"#;

        assert_eq!(cache_packages_json, std::fs::read_to_string(gen_cache_index).unwrap());

        // Validate that the generated files are in the contents.
        for (generated_file, _) in &build_results.generated_files {
            assert!(
                build_results.contents.contains_key(generated_file),
                "Unable to find generated file in base package contents: {}",
                generated_file
            );
        }

        // Read the output and ensure it contains the right files (and their hashes)
        let mut far_reader = Reader::new(File::open(&far_path).unwrap()).unwrap();
        let package = far_reader.read_file("meta/package").unwrap();
        assert_eq!(br#"{"name":"system_image","version":"0"}"#, &*package);
        let contents = far_reader.read_file("meta/contents").unwrap();
        let contents = std::str::from_utf8(&contents).unwrap();
        let expected_contents = "\
            data/cache_packages.json=49d59d7e9567de7ce2d5fc8632ea544965402426a8fa66456fbd68dccca36b4c\n\
            data/file.txt=15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b\n\
            data/static_packages=2d86ccb37d003499bdc7bdd933428f4b83d9ed224d1b64ad90dc257d22cff460\n\
        "
        .to_string();
        assert_eq!(expected_contents, contents);
    }

    // Generates a package manifest to be used for testing. The `name` is used in the blob file
    // names to make each manifest somewhat unique. If supplied, `file_path` will be used as the
    // non-meta-far blob source path, which allows the tests to use a real file.
    fn generate_test_manifest(
        name: &str,
        version: &str,
        file_path: Option<&Path>,
    ) -> PackageManifest {
        let meta_source = format!("path/to/{}/meta.far", name);
        let file_source = match file_path {
            Some(path) => path.to_string_lossy().into_owned(),
            _ => format!("path/to/{}/file.txt", name),
        };
        serde_json::from_value::<PackageManifest>(json!(
            {
                "version": "1",
                "repository": "testrepository.com",
                "package": {
                    "name": name,
                    "version": version,
                },
                "blobs": [
                    {
                        "source_path": meta_source,
                        "path": "meta/",
                        "merkle":
                            "0000000000000000000000000000000000000000000000000000000000000000",
                        "size": 1
                    },

                    {
                        "source_path": file_source,
                        "path": "data/file.txt",
                        "merkle":
                            "1111111111111111111111111111111111111111111111111111111111111111",
                        "size": 1
                    },
                ]
            }
        ))
        .expect("valid json")
    }
}
