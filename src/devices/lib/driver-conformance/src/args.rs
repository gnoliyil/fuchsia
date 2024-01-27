// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{parser, results},
    anyhow::{anyhow, Result},
    argh::FromArgs,
    regex::Regex,
    std::path::{Path, PathBuf},
    std::{fmt, str::FromStr},
    url::Url,
};

/// Custom type to store a list of test URLs.
#[derive(Default, Clone, PartialEq, Debug)]
pub struct TestList(pub Vec<String>);

impl FromStr for TestList {
    type Err = anyhow::Error;

    fn from_str(value: &str) -> Result<Self> {
        let params: Vec<_> = value.split(",").map(|test| String::from(test)).collect();
        let re = Regex::new(r"fuchsia-pkg://.+/.+#meta/.+").unwrap();
        for param in params.iter() {
            if !re.is_match(&param.as_str()) {
                return Err(anyhow::anyhow!(
                    "'{}' does not appear to be a fuchsia-pkg URL.",
                    param
                ));
            }
        }
        Ok(Self { 0: params })
    }
}

impl fmt::Display for TestList {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for test in self.0.iter() {
            writeln!(f, "{}", test)?;
        }
        Ok(())
    }
}

/// Custom type to store a URL-safe version number.
#[derive(Default, Clone, PartialEq, Debug)]
pub struct Version(pub String);

impl FromStr for Version {
    type Err = anyhow::Error;

    fn from_str(value: &str) -> Result<Self> {
        // Verify the version string is URL-safe.
        let value = value.trim();
        if value.is_empty() {
            return Err(anyhow::anyhow!("Version string cannot be empty."));
        }
        let placeholder_url = format!("http://some.domain/{}", value.to_string());
        if let Ok(val) = Url::parse(&placeholder_url) {
            // If the parsed URL has changed, then the string had to be escaped (it was not URL-safe).
            if val.to_string() == placeholder_url {
                return Ok(Self { 0: value.to_string() });
            }
        }
        Err(anyhow::anyhow!(
            "Please limit your version number to consist of alphanumeric, '-', '.', '_', and '~'."
        ))
    }
}

/// Custom type to allow enforcing valid directory path.
#[derive(Default, Clone, PartialEq, Debug)]
pub struct OutputDirectory(pub PathBuf);

impl FromStr for OutputDirectory {
    type Err = anyhow::Error;

    fn from_str(value: &str) -> Result<Self> {
        let path = Path::new(value);
        if !path.is_dir() {
            return Err(anyhow!("Provided package output directory is not a valid directory. Verify the directory exists."));
        }
        Ok(Self { 0: path.to_path_buf() })
    }
}

/// Custom type to store a given list of device categories.
#[derive(Default, Clone, PartialEq, Debug)]
pub struct DeviceCategoryList(pub Vec<parser::DeviceCategory>);

impl FromStr for DeviceCategoryList {
    type Err = anyhow::Error;

    fn from_str(value: &str) -> Result<Self> {
        let mut ret: Vec<parser::DeviceCategory> = vec![];
        for item in value.split(",") {
            ret.push(parser::DeviceCategory::from_str(&item)?);
        }
        Ok(Self(ret))
    }
}

/// Download or run driver conformance tests.
#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "conformance",
    example = "To run all tests for a given driver:

$ ffx driver conformance test --driver fuchsia-boot:///#driver/my-driver.so

To run all tests for a given device:

$ ffx driver conformance test --device pci-00:05.0-fidl/my-device

To run all tests of a given type(s) for a given driver:

$ ffx driver conformance test --driver fuchsia-boot:///#driver/my-driver.so --types functional,performance

To run an arbitrary test(s) against a given driver:

$ ffx driver conformance test --driver fuchsia-boot:///#driver/my-driver.so --tests fuchsia-pkg://fuchsia.com/my-test#meta/my-test.cm"
)]
pub struct ConformanceCommand {
    #[argh(subcommand)]
    pub subcommand: ConformanceSubCommand,
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand)]
pub enum ConformanceSubCommand {
    Test(TestCommand),
}

/// Runs driver conformance tests.
#[derive(FromArgs, PartialEq, Debug, Default)]
#[argh(subcommand, name = "test")]
pub struct TestCommand {
    /// device topological path. e.g. pci-00:05.0-fidl/my-device
    #[argh(option)]
    pub device: Option<String>,

    /// driver libname. e.g. fuchsia-boot:///#driver/my-driver.so
    #[argh(option)]
    pub driver: Option<String>,

    /// path to FHCP metadata file.
    #[argh(option)]
    pub metadata_path: Option<PathBuf>,

    /// WIP. comma-separated list of test types. e.g. performance,functional
    #[argh(option)]
    pub types: Option<String>,

    /// comma-separated list of test categories. e.g. imaging::camera,usb
    #[argh(option)]
    pub categories: Option<DeviceCategoryList>,

    /// comma-separated list of test components. e.g. fuchsia-pkg://fuchsia.dev/sometest#meta/sometest.cm,fuchsia-pkg://fuchsia.dev/test2#meta/othertest.cm
    #[argh(option)]
    pub tests: Option<TestList>,

    /// WIP. local directory storing the resources required for offline testing.
    #[argh(option)]
    pub cache: Option<PathBuf>,

    /// path to the x64 driver binary package.
    #[argh(option)]
    pub x64_package: Option<PathBuf>,

    /// path to the ARM64 driver binary package.
    #[argh(option)]
    pub arm64_package: Option<PathBuf>,

    /// driver source URL. (ex. Gerrit link).
    #[argh(option)]
    pub driver_source: Option<String>,

    /// host type of the driver source. e.g. gerrit
    #[argh(option)]
    pub source_host_type: Option<results::SourceProvider>,

    /// name of the driver submission. Defaults to driver package URL.
    #[argh(option)]
    pub submission_name: Option<String>,

    /// version number of the driver. Must be a URL-safe format.
    #[argh(option)]
    pub version: Option<Version>,

    /// run only the automated tests.
    #[argh(switch)]
    pub automated_only: bool,

    /// run only the manual tests.
    #[argh(switch)]
    pub manual_only: bool,

    /// path to the license directory.
    #[argh(option)]
    pub licenses: Option<PathBuf>,

    // TODO(fxb/115097): Separate this logic into its own subcommand.
    /// generate submission package.
    ///
    /// To be used for uploading to the Fuchsia Hardware Portal.
    ///
    /// Defaults to current working directory. Use `--package-output-dir` to override.
    #[argh(switch)]
    pub generate_submission: bool,

    /// save submission package  `package.tar.gz` into the given directory.
    ///
    /// This will implicitly activate `--generate-submission`.
    #[argh(option)]
    pub package_output_dir: Option<OutputDirectory>,
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_test_list_from_str_ok() {
        let single_input = "fuchsia-pkg://fuchsia.com/fake-test#meta/fake-test.cm";
        let single = TestList::from_str(single_input).unwrap();
        assert_eq!(single.0.len(), 1);
        assert!(
            single.0.contains(&single_input.to_string()),
            "Did not find an item: {}",
            single_input
        );

        let multiple_input = vec![
            "fuchsia-pkg://fuchsia.com/fake-test#meta/fake-test.cm",
            "fuchsia-pkg://fuchsia.com/flake-test#meta/flake-test.cm",
            "fuchsia-pkg://fuchsia.com/bake-test#meta/bake-test.cm",
        ];
        let multiple_csv =
            format!("{},{},{}", multiple_input[0], multiple_input[1], multiple_input[2]);
        let multiple = TestList::from_str(multiple_csv.as_str()).unwrap();
        assert_eq!(multiple.0.len(), 3);
        for e in multiple_input.iter() {
            assert!(multiple.0.contains(&e.to_string()), "Did not find an item: {}", e.to_string());
        }
    }

    #[test]
    fn test_test_list_from_str_err() {
        let very_wrong = "abc";
        match TestList::from_str(very_wrong) {
            Ok(_) => assert!(false, "This call should not pass."),
            Err(e) => assert_eq!(e.to_string(), "'abc' does not appear to be a fuchsia-pkg URL."),
        }

        // Slight mismatch: meta -> mata
        let slightly_wrong = "fuchsia-pkg://some.domain/foobar#mata/foobar.cm";
        match TestList::from_str(slightly_wrong) {
            Ok(_) => assert!(false, "This call should not pass."),
            Err(e) => assert_eq!(e.to_string(), "'fuchsia-pkg://some.domain/foobar#mata/foobar.cm' does not appear to be a fuchsia-pkg URL."),
        }

        let multiple_csv = format!(
            "{},{},{}",
            "fuchsia-pkg://fuchsia.com/fake-test#meta/fake-test.cm", "def", "ghi"
        );
        match TestList::from_str(multiple_csv.as_str()) {
            Ok(_) => assert!(false, "This call should not pass."),
            Err(e) => assert_eq!(e.to_string(), "'def' does not appear to be a fuchsia-pkg URL."),
        }
    }

    #[test]
    fn test_test_version_from_str_ok() {
        let test0 = "1.0";
        assert_eq!(
            Version::from_str(test0).unwrap().0,
            test0,
            "Version value was modified when saved."
        );
        let test1 = "1";
        assert_eq!(
            Version::from_str(test1).unwrap().0,
            test1,
            "Version value was modified when saved."
        );
        let test2 = "0.0.1b-29faf3";
        assert_eq!(
            Version::from_str(test2).unwrap().0,
            test2,
            "Version value was modified when saved."
        );
        let test3 = "1.0-rev3";
        assert_eq!(
            Version::from_str(test3).unwrap().0,
            test3,
            "Version value was modified when saved."
        );
        let test4 = "A";
        assert_eq!(
            Version::from_str(test4).unwrap().0,
            test4,
            "Version value was modified when saved."
        );
        let test5 = "1-2.3_4~5";
        assert_eq!(
            Version::from_str(test5).unwrap().0,
            test5,
            "Version value was modified when saved."
        );
    }

    #[test]
    fn test_test_version_from_str_err() {
        let has_curly = "1.0{b}";
        match Version::from_str(has_curly) {
            Ok(_) => assert!(false, "This call should not pass."),
            Err(e) => assert_eq!(e.to_string(), "Please limit your version number to consist of alphanumeric, '-', '.', '_', and '~'."),
        }

        let has_unicode = "1.0Ӂ";
        match Version::from_str(has_unicode) {
            Ok(_) => assert!(false, "This call should not pass."),
            Err(e) => assert_eq!(e.to_string(), "Please limit your version number to consist of alphanumeric, '-', '.', '_', and '~'."),
        }

        let has_space = "1.0 final";
        match Version::from_str(has_space) {
            Ok(_) => assert!(false, "This call should not pass."),
            Err(e) => assert_eq!(e.to_string(), "Please limit your version number to consist of alphanumeric, '-', '.', '_', and '~'."),
        }

        let empty = "";
        match Version::from_str(empty) {
            Ok(_) => assert!(false, "This call should not pass."),
            Err(e) => assert_eq!(e.to_string(), "Version string cannot be empty."),
        }

        let space = " ";
        match Version::from_str(space) {
            Ok(_) => assert!(false, "This call should not pass."),
            Err(e) => assert_eq!(e.to_string(), "Version string cannot be empty."),
        }
    }

    #[test]
    fn test_device_category_list_from_str_ok() {
        let single_input = "a::b";
        let single = DeviceCategoryList::from_str(single_input).unwrap();
        let expected_single = DeviceCategoryList {
            0: vec![parser::DeviceCategory {
                category: "a".to_string(),
                subcategory: "b".to_string(),
            }],
        };
        assert_eq!(single, expected_single);

        let multiple_input = vec!["a", "a::b", "c::d", "e"];
        let expected_multiple = DeviceCategoryList {
            0: vec![
                parser::DeviceCategory { category: "a".to_string(), subcategory: "".to_string() },
                parser::DeviceCategory { category: "a".to_string(), subcategory: "b".to_string() },
                parser::DeviceCategory { category: "c".to_string(), subcategory: "d".to_string() },
                parser::DeviceCategory { category: "e".to_string(), subcategory: "".to_string() },
            ],
        };
        let multiple_csv = format!(
            "{},{},{},{}",
            multiple_input[0], multiple_input[1], multiple_input[2], multiple_input[3]
        );
        let multiple = DeviceCategoryList::from_str(multiple_csv.as_str()).unwrap();
        assert_eq!(multiple, expected_multiple);
    }

    #[test]
    fn test_device_category_list_from_str_err() {
        let single = DeviceCategoryList::from_str(&"a::");
        assert!(single.is_err());

        let multiple = DeviceCategoryList::from_str(&"a::b,");
        assert!(multiple.is_err());
    }
}
