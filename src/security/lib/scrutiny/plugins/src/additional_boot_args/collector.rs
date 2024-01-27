// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::additional_boot_args::collection::{
        AdditionalBootConfigCollection, AdditionalBootConfigContents, AdditionalBootConfigError,
        AdditionalBootConfigParseError,
    },
    anyhow::{Context, Result},
    scrutiny::model::{collector::DataCollector, model::DataModel},
    scrutiny_utils::{
        artifact::{ArtifactReader, FileArtifactReader},
        bootfs::BootfsReader,
        package::{open_update_package, read_content_blob},
        zbi::{ZbiReader, ZbiType},
    },
    std::{collections::HashMap, path::Path, path::PathBuf, str::from_utf8, sync::Arc},
};

/// The path to the unsigned fuchsia ZBI image relative to the update package root.
const FUCHSIA_ZBI_PATH: &str = "zbi";
/// The path to the signed fuchsia ZBI image relative to the update package root.
const FUCHSIA_ZBI_SIGNED_PATH: &str = "zbi.signed";
/// The path to the unsigned recovery ZBI image relative to the update package root.
const RECOVERY_ZBI_PATH: &str = "recovery";
/// The path to the signed recovery ZBI image relative to the update package root.
const RECOVERY_ZBI_SIGNED_PATH: &str = "recovery.signed";

// Load the additional boot configuration file by following update package -> zbi -> bootfs -> additional boot config
// file. The zbi is assumed to be stored at the file path "zbi.signed" or "zbi" in the update
// package, and `additional_boot_args_path` is a path in bootfs embedded in the ZBI.
//
// The purpose of loading the additional boot config is to parse bootstrapping information such as the system
// image merkle root used to bootstrap the software delivery stack.
//
// TODO(fxbug.dev/98030): This function should support the update package -> images package -> ...
// flow.
fn load_additional_boot_args<P1: AsRef<Path>, P2: AsRef<Path>>(
    update_package_path: P1,
    artifact_reader: &mut Box<dyn ArtifactReader>,
    additional_boot_args_path: P2,
    recovery: bool,
) -> Result<AdditionalBootConfigContents, AdditionalBootConfigError> {
    let additional_boot_args_path_ref = additional_boot_args_path.as_ref();
    let additional_boot_args_path_str =
        additional_boot_args_path_ref.to_str().ok_or_else(|| {
            AdditionalBootConfigError::FailedToParseAdditionalBootConfigPath {
                additional_boot_args_path: additional_boot_args_path_ref.to_path_buf(),
            }
        })?;
    let update_package_path_ref = update_package_path.as_ref();
    let mut far_reader =
        open_update_package(update_package_path_ref, artifact_reader).map_err(|err| {
            AdditionalBootConfigError::FailedToOpenUpdatePackage {
                update_package_path: update_package_path_ref.to_path_buf(),
                io_error: format!("{:?}", err),
            }
        })?;
    let (zbi_path, zbi_signed_path) = if recovery {
        (RECOVERY_ZBI_PATH, RECOVERY_ZBI_SIGNED_PATH)
    } else {
        (FUCHSIA_ZBI_PATH, FUCHSIA_ZBI_SIGNED_PATH)
    };
    let zbi_buffer = read_content_blob(&mut far_reader, artifact_reader, zbi_signed_path).or_else(
        |signed_err| {
            read_content_blob(&mut far_reader, artifact_reader, zbi_path).map_err(|err| {
                AdditionalBootConfigError::FailedToReadZbi {
                    update_package_path: update_package_path_ref.to_path_buf(),
                    io_error: format!("{:?}\n{:?}", signed_err, err),
                }
            })
        },
    )?;
    let mut reader = ZbiReader::new(zbi_buffer);
    let zbi_sections =
        reader.parse().map_err(|zbi_error| AdditionalBootConfigError::FailedToParseZbi {
            update_package_path: update_package_path_ref.to_path_buf(),
            zbi_error: zbi_error.to_string(),
        })?;

    for section in zbi_sections.iter() {
        if section.section_type == ZbiType::StorageBootfs {
            let mut bootfs_reader = BootfsReader::new(section.buffer.clone());
            let bootfs_data = bootfs_reader.parse().map_err(|bootfs_error| {
                AdditionalBootConfigError::FailedToParseBootfs {
                    update_package_path: update_package_path_ref.to_path_buf(),
                    bootfs_error: bootfs_error.to_string(),
                }
            })?;
            for (file, data) in bootfs_data.iter() {
                if file == additional_boot_args_path_str {
                    return Ok(parse_additional_boot_args_contents(from_utf8(&data).map_err(
                        |utf8_error| {
                            AdditionalBootConfigError::FailedToParseUtf8AdditionalBootConfig {
                                update_package_path: update_package_path_ref.to_path_buf(),
                                additional_boot_args_path: additional_boot_args_path_ref
                                    .to_path_buf(),
                                utf8_error: utf8_error.to_string(),
                            }
                        },
                    )?)
                    .map_err(|parse_error| {
                        AdditionalBootConfigError::FailedToParseAdditionalBootConfigFormat {
                            update_package_path: update_package_path_ref.to_path_buf(),
                            additional_boot_args_path: additional_boot_args_path_ref.to_path_buf(),
                            parse_error,
                        }
                    })?);
                }
            }
        }
    }
    Err(AdditionalBootConfigError::FailedToLocateAdditionalBootConfig {
        update_package_path: update_package_path_ref.to_path_buf(),
        additional_boot_args_path: additional_boot_args_path_ref.to_path_buf(),
    })
}

fn parse_additional_boot_args_contents(
    str_contents: &str,
) -> Result<AdditionalBootConfigContents, AdditionalBootConfigParseError> {
    let mut line_nos: HashMap<&str, (usize, &str)> = HashMap::new();
    let mut contents: AdditionalBootConfigContents = HashMap::new();
    let lines: Vec<&str> = str_contents.trim_matches(|ch| ch == '\n').split("\n").collect();
    for line_no in 0..lines.len() {
        let line_contents = lines[line_no];
        let kv: Vec<&str> = line_contents.split("=").collect();
        if kv.len() != 2 {
            return Err(AdditionalBootConfigParseError::FailedToParseKeyValue {
                line_no: line_no + 1,
                line_contents: line_contents.to_string(),
            });
        }
        if let Some((previous_line_no, previous_line_contents)) = line_nos.get(&kv[0]) {
            return Err(AdditionalBootConfigParseError::RepeatedKey {
                line_no: line_no + 1,
                line_contents: line_contents.to_string(),
                previous_line_no: previous_line_no + 1,
                previous_line_contents: previous_line_contents.to_string(),
            });
        }
        line_nos.insert(kv[0], (line_no + 1, line_contents));
        contents.insert(kv[0].to_string(), kv[1].trim().split("+").map(String::from).collect());
    }
    Ok(contents)
}

#[derive(Default)]
pub struct AdditionalBootConfigCollector;

impl DataCollector for AdditionalBootConfigCollector {
    fn collect(&self, model: Arc<DataModel>) -> Result<()> {
        let model_config = model.config();
        let recovery = model_config.is_recovery();
        let update_package_path = model_config.update_package_path();
        let blobs_directory = model_config.blobs_directory();
        let additional_boot_args_path = model_config.additional_boot_args_path();

        // Initialize artifact reader; early exit on initialization failure.
        let mut artifact_reader: Box<dyn ArtifactReader> =
            Box::new(FileArtifactReader::new(&PathBuf::new(), &blobs_directory));

        // Execute query using deps-tracking artifact reader.
        let result = load_additional_boot_args(
            &update_package_path,
            &mut artifact_reader,
            &additional_boot_args_path,
            recovery,
        );

        // Store result in model.
        model
            .set(match result {
                Ok(additional_boot_args) => AdditionalBootConfigCollection {
                    additional_boot_args: Some(additional_boot_args),
                    deps: artifact_reader.get_deps(),
                    errors: vec![],
                },
                Err(err) => AdditionalBootConfigCollection {
                    additional_boot_args: None,
                    deps: artifact_reader.get_deps(),
                    errors: vec![err],
                },
            })
            .with_context(|| { format!(
                "Failed to collect data from additional boot config bootfs:{:?} in ZBI from update package at {:?}",
                additional_boot_args_path, update_package_path,
            )})?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use {super::parse_additional_boot_args_contents, maplit::hashmap};

    #[test]
    fn test_empty() {
        assert!(parse_additional_boot_args_contents("").is_err());
    }

    #[test]
    fn test_one() {
        assert_eq!(
            parse_additional_boot_args_contents("a=a").unwrap(),
            hashmap! {"a".to_string() => vec!["a".to_string()]}
        );
    }

    #[test]
    fn test_multiple_keys() {
        assert_eq!(
            parse_additional_boot_args_contents(
                "a=a
b=a"
            )
            .unwrap(),
            hashmap! {
                "a".to_string() => vec!["a".to_string()], "b".to_string() => vec!["a".to_string()]
            }
        );
    }

    #[test]
    fn test_duplicate_keys() {
        assert!(parse_additional_boot_args_contents(
            "a=a
a=a"
        )
        .is_err());
    }

    #[test]
    fn test_multiple_values() {
        assert_eq!(
            parse_additional_boot_args_contents("a=a+b+c+d").unwrap(),
            hashmap! {
                "a".to_string() => vec![
                    "a".to_string(), "b".to_string(), "c".to_string(), "d".to_string()
                ]
            }
        );
    }

    #[test]
    fn test_plus_containing_key() {
        assert_eq!(
            parse_additional_boot_args_contents("c++=u+a+f").unwrap(),
            hashmap! {"c++".to_string() => vec!["u".to_string(), "a".to_string(), "f".to_string()]}
        );
    }

    #[test]
    fn test_value_double_plus() {
        assert_eq!(
            parse_additional_boot_args_contents("a=a++a").unwrap(),
            hashmap! {"a".to_string() => vec!["a".to_string(), "".to_string(), "a".to_string()]}
        );
    }

    #[test]
    fn test_value_whitespace() {
        assert_eq!(
            parse_additional_boot_args_contents("a=a+ a +a").unwrap(),
            hashmap! {"a".to_string() => vec!["a".to_string(), " a ".to_string(), "a".to_string()]}
        );
    }

    #[test]
    fn test_too_many_eq() {
        assert!(parse_additional_boot_args_contents("a=b=c").is_err());
    }

    #[test]
    fn test_too_few_eq() {
        assert!(parse_additional_boot_args_contents("a").is_err());
    }

    #[test]
    fn test_leading_newlines() {
        assert_eq!(
            parse_additional_boot_args_contents(
                "

a=a"
            )
            .unwrap(),
            hashmap! {"a".to_string() => vec!["a".to_string()]}
        );
    }

    #[test]
    fn test_trailing_newlines() {
        assert_eq!(
            parse_additional_boot_args_contents(
                "a=a

"
            )
            .unwrap(),
            hashmap! {"a".to_string() => vec!["a".to_string()]}
        );
    }

    #[test]
    fn test_leading_trailing_newlines() {
        assert_eq!(
            parse_additional_boot_args_contents(
                "

a=a

"
            )
            .unwrap(),
            hashmap! {"a".to_string() => vec!["a".to_string()]}
        );
    }

    #[test]
    fn test_leading_whitespace() {
        assert_eq!(
            parse_additional_boot_args_contents(
                "

  a=a

"
            )
            .unwrap(),
            hashmap! {"  a".to_string() => vec!["a".to_string()]}
        );
    }

    #[test]
    fn test_unicode() {
        assert_eq!(
            parse_additional_boot_args_contents("🙂=🍞+≈+∔+幸せ").unwrap(),
            hashmap! {
                "🙂".to_string() => vec![
                    "🍞".to_string(), "≈".to_string(), "∔".to_string(), "幸せ".to_string()
                ]
            }
        );
    }
}
