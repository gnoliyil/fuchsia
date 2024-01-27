// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This library must remain platform-agnostic because it used by a host tool and within Fuchsia.

use {
    anyhow::{anyhow, Context, Result},
    fidl_fuchsia_component_internal as fcomponent_internal,
    moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
    serde::{Deserialize, Deserializer, Serialize, Serializer},
    std::collections::HashMap,
    std::convert::TryFrom,
    std::fs,
    std::str,
    thiserror::Error,
};

pub mod fidl_convert;

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq, Clone)]
pub struct InstanceIdEntry {
    pub instance_id: Option<String>,
    #[serde(
        default,
        deserialize_with = "str_to_abs_moniker",
        serialize_with = "abs_moniker_to_str"
    )]
    pub moniker: Option<AbsoluteMoniker>,
}

fn str_to_abs_moniker<'de, D>(deserializer: D) -> Result<Option<AbsoluteMoniker>, D::Error>
where
    D: Deserializer<'de>,
{
    let moniker: Option<String> = Option::deserialize(deserializer)?;
    match &moniker {
        Some(m) => Ok(Some(AbsoluteMoniker::parse_str(m).map_err(serde::de::Error::custom)?)),
        None => Ok(None),
    }
}

fn abs_moniker_to_str<S>(
    abs_moniker: &Option<AbsoluteMoniker>,
    serializer: S,
) -> Result<S::Ok, S::Error>
where
    S: Serializer,
{
    if let Some(abs_moniker) = abs_moniker {
        serializer.serialize_str(&abs_moniker.to_string())
    } else {
        serializer.serialize_none()
    }
}

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq, Clone)]
pub struct Index {
    pub instances: Vec<InstanceIdEntry>,
}

#[cfg_attr(feature = "serde", derive(Deserialize, Serialize), serde(rename_all = "snake_case"))]
#[derive(Debug, Clone, Error, PartialEq)]
pub enum IndexError {
    #[error("invalid index")]
    ValidationError(#[from] ValidationError),
    #[error("could not convert FIDL index")]
    FidlConversionError(#[from] fidl_convert::FidlConversionError),
}

impl Index {
    // Construct an Index by merging index source files.
    //
    // - `index_file_paths` is a list of component ID index file paths to be validated and merged.
    // - `decoder` is a function which decodes (without schema-validating) an index file content into an Index.
    //
    // See `ValidationError` for possible errors.
    pub fn from_files_with_decoder(
        index_file_paths: &[String],
        decoder: impl Fn(&[u8]) -> anyhow::Result<Index>,
    ) -> anyhow::Result<Index> {
        let mut ctx = MergeContext::new();
        for input_file_path in index_file_paths {
            let contents = fs::read_to_string(&input_file_path)
                .with_context(|| anyhow!("Could not read index file {}", &input_file_path))?;
            let index = decoder(contents.as_str().as_bytes())
                .with_context(|| anyhow!("Could not parse index file {}", &input_file_path))?;
            ctx.merge(&input_file_path, &index)
                .with_context(|| anyhow!("Could not merge index file {}", &input_file_path))?;
        }
        Ok(ctx.output())
    }

    // Construct an Index from the given FIDL-schema'd index.
    //
    // The given fidl_index is validated.
    pub fn from_fidl(
        fidl_index: fcomponent_internal::ComponentIdIndex,
    ) -> Result<Index, IndexError> {
        let native_index = Index::try_from(fidl_index)?;
        let mut ctx = MergeContext::new();
        ctx.merge("", &native_index)?;
        Ok(ctx.output())
    }
}

impl Default for Index {
    fn default() -> Self {
        Index { instances: vec![] }
    }
}

#[cfg_attr(feature = "serde", derive(Deserialize, Serialize), serde(rename_all = "snake_case"))]
#[derive(Error, Debug, Clone, PartialEq)]
pub enum ValidationError {
    #[error("Instance ID '{}' must be unique but exists in following index files:\n {}\n {}", .instance_id, .source1, .source2)]
    DuplicateIds { instance_id: String, source1: String, source2: String },
    #[error("Some entries do not specify an instance ID.")]
    MissingInstanceIds { entries: Vec<InstanceIdEntry> },
    #[error("The following entry must contain a moniker: {:?}", .entry)]
    MissingMoniker { entry: InstanceIdEntry },
    #[error("The following entry's instance_id is invalid (must be 64 lower-cased hex chars): {:?}", .entry)]
    InvalidInstanceId { entry: InstanceIdEntry },
}

// MergeContext maintains a single merged index, along with some state for error checking, as indicies are merged together using MergeContext::merge().
//
// Usage:
// - Use MergeContext::new() to create a MergeContext.
// - Call MergeContext::merge() to merge an index. Can be called multiple times.
// - Call MergeContext::output() to access the merged index.
struct MergeContext {
    output_index: Index,
    // MergeConetext::merge() will accumulate the instance IDs which have been merged so far, along with the index source file which they came from.
    // This is used to validate that all instance IDs are unique and provide helpful error messages.
    // instance id -> path of file defining instance ID.
    accumulated_instance_ids: HashMap<String, String>,
}

impl MergeContext {
    fn new() -> MergeContext {
        MergeContext {
            output_index: Index { instances: vec![] },
            accumulated_instance_ids: HashMap::new(),
        }
    }

    // merge() merges `index` into the MergeContext.
    // This method can be called multiple times to merge multiple indicies.
    // The accumulated index can be accessed with output().
    fn merge(&mut self, source_index_path: &str, index: &Index) -> Result<(), ValidationError> {
        let mut missing_instance_ids = vec![];
        for entry in &index.instances {
            match entry.instance_id.as_ref() {
                None => {
                    // Instead of failing right away, continue processing the other entries.
                    missing_instance_ids.push(entry.clone());
                    continue;
                }
                Some(instance_id) => {
                    if !is_valid_instance_id(&instance_id) {
                        return Err(ValidationError::InvalidInstanceId { entry: entry.clone() });
                    }
                    if let Some(previous_source_path) = self
                        .accumulated_instance_ids
                        .insert(instance_id.clone(), source_index_path.to_string())
                    {
                        return Err(ValidationError::DuplicateIds {
                            instance_id: instance_id.clone(),
                            source1: previous_source_path.clone(),
                            source2: source_index_path.to_string(),
                        });
                    }
                }
            }
            if entry.moniker.is_none() {
                return Err(ValidationError::MissingMoniker { entry: entry.clone() });
            }
            self.output_index.instances.push(entry.clone());
        }
        if missing_instance_ids.len() > 0 {
            Err(ValidationError::MissingInstanceIds { entries: missing_instance_ids })
        } else {
            Ok(())
        }
    }

    // Access the accumulated index from calls to merge().
    fn output(self) -> Index {
        self.output_index
    }
}

// Generate a random instance ID.
pub fn gen_instance_id(rng: &mut impl rand::Rng) -> String {
    // generate random 256bits into a byte array
    let mut num: [u8; 256 / 8] = [0; 256 / 8];
    rng.fill_bytes(&mut num);
    // turn the byte array into a lower-cased hex string.
    num.iter().map(|byte| format!("{:02x}", byte)).collect::<Vec<String>>().join("")
}

fn is_valid_instance_id(id: &str) -> bool {
    // An instance ID is a lower-cased hex string of 256-bits.
    // 256 bits in base16 = 64 chars (1 char to represent 4 bits)
    id.len() == 64 && id.chars().all(|ch| (ch.is_numeric() || ch.is_lowercase()) && ch.is_digit(16))
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::Result;
    use proptest::prelude::*;
    use rand::SeedableRng as _;

    fn gen_index(num_instances: u32) -> Index {
        Index {
            instances: (0..num_instances)
                .map(|i| InstanceIdEntry {
                    instance_id: Some(gen_instance_id(&mut rand::thread_rng())),
                    moniker: Some(vec![i.to_string().as_str()].try_into().unwrap()),
                })
                .collect(),
        }
    }

    #[test]
    fn merge_empty_index() {
        let ctx = MergeContext::new();
        assert_eq!(ctx.output(), gen_index(0));
    }

    #[test]
    fn merge_single_index() -> Result<()> {
        let mut ctx = MergeContext::new();
        let index = gen_index(0);
        ctx.merge("/random/file/path", &index)?;
        assert_eq!(ctx.output(), index.clone());
        Ok(())
    }

    #[test]
    fn merge_duplicate_ids() -> Result<()> {
        let source1 = "/a/b/c";
        let source2 = "/d/e/f";

        let index1 = gen_index(1);
        let mut index2 = index1.clone();
        index2.instances[0].instance_id = index1.instances[0].instance_id.clone();

        let mut ctx = MergeContext::new();
        ctx.merge(source1, &index1)?;

        let err = ctx.merge(source2, &index2).unwrap_err();
        assert_eq!(
            err,
            ValidationError::DuplicateIds {
                instance_id: index1.instances[0].instance_id.as_ref().unwrap().clone(),
                source1: source1.to_string(),
                source2: source2.to_string()
            }
        );

        Ok(())
    }

    #[test]
    fn missing_instance_ids() -> Result<()> {
        let mut index = gen_index(4);
        index.instances[1].instance_id = None;
        index.instances[3].instance_id = None;

        let mut ctx = MergeContext::new();
        // this should be an error, since `index` has entries with a missing instance ID.
        let merge_result: Result<(), ValidationError> = ctx.merge("/a/b/c", &index);
        assert!(matches!(
            merge_result.as_ref(),
            Err(ValidationError::MissingInstanceIds { entries: _ })
        ));

        Ok(())
    }

    #[test]
    fn missing_moniker() {
        let mut index = gen_index(1);
        index.instances[0].moniker = None;

        let mut ctx = MergeContext::new();
        // this should be an error, since `index` has an entry without any monikers.
        let merge_result: Result<(), ValidationError> = ctx.merge("/a/b/c", &index);
        assert!(matches!(merge_result.as_ref(), Err(ValidationError::MissingMoniker { entry: _ })));
    }

    #[test]
    fn unique_gen_instance_id() {
        let seed = rand::thread_rng().next_u64();
        println!("using seed {}", seed);
        let mut rng = rand::rngs::StdRng::seed_from_u64(seed);
        let mut prev_id = gen_instance_id(&mut rng);
        for _i in 0..40 {
            let id = gen_instance_id(&mut rng);
            assert!(prev_id != id);
            prev_id = id;
        }
    }

    #[test]
    fn valid_gen_instance_id() {
        let seed = rand::thread_rng().next_u64();
        println!("using seed {}", seed);
        let mut rng = rand::rngs::StdRng::seed_from_u64(seed);
        for _i in 0..40 {
            assert!(is_valid_instance_id(&gen_instance_id(&mut rng)));
        }
    }

    proptest! {
        #[test]
        fn valid_instance_id(id in "[a-f0-9]{64}") {
            prop_assert_eq!(true, is_valid_instance_id(&id));
        }
    }

    #[test]
    fn invalid_instance_id() {
        // Invalid lengths
        assert!(!is_valid_instance_id("8c90d44863ff67586cf6961081feba4f760decab8bbbee376a3bfbc77"));
        assert!(!is_valid_instance_id("8c90d44863ff67586cf6961081feba4f760decab8bbbee376a"));
        assert!(!is_valid_instance_id("8c90d44863ff67586cf6961081"));
        // upper case chars are invalid
        assert!(!is_valid_instance_id(
            "8C90D44863FF67586CF6961081FEBA4F760DECAB8BBBEE376A3BFBC77B351280"
        ));
        // hex chars only
        assert!(!is_valid_instance_id(
            "8x90d44863ff67586cf6961081feba4f760decab8bbbee376a3bfbc77b351280"
        ));
        assert!(!is_valid_instance_id(
            "8;90d44863ff67586cf6961081feba4f760decab8bbbee376a3bfbc77b351280"
        ));
    }

    #[test]
    fn serialize_deserialize_valid_absolute_moniker() -> Result<()> {
        let mut expected_index = gen_index(3);
        expected_index.instances[0].moniker = Some(AbsoluteMoniker::parse_str("/a/b/c").unwrap());
        expected_index.instances[1].moniker =
            Some(AbsoluteMoniker::parse_str("/a/b:b/c/b:b").unwrap());
        expected_index.instances[2].moniker = Some(AbsoluteMoniker::parse_str("/").unwrap());

        let json_index = serde_json::to_string(&expected_index)?;
        let actual_index = serde_json::from_str(&json_index)?;
        assert_eq!(expected_index, actual_index);

        Ok(())
    }
}
