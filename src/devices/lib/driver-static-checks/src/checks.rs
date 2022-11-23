// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::utils;
use anyhow::Result;
use bind::interpreter::decode_bind_rules::DecodedRules;
use cm_rust::ComponentDecl;
use fidl_fuchsia_data as fdata;

#[derive(Debug, Eq, PartialEq)]
pub enum StaticCheckResult {
    Pass,
    Fail { reason: String },
}

pub type CheckFunction =
    fn(far_reader: &mut utils::FarReader, component: &ComponentDecl) -> Result<StaticCheckResult>;

/// Checks that a driver's compiled bind rules are present and valid.
pub fn check_bind_rules(
    far_reader: &mut utils::FarReader,
    component: &ComponentDecl,
) -> Result<StaticCheckResult> {
    if let Some(bind_path) = get_bindbc_path(component) {
        let contents = far_reader.read_file(&bind_path)?;
        if let Err(e) = DecodedRules::new(contents) {
            Ok(StaticCheckResult::Fail {
                reason: format!("Error while decoding bind rules: {}", e),
            })
        } else {
            Ok(StaticCheckResult::Pass)
        }
    } else {
        Ok(StaticCheckResult::Fail {
            reason: String::from(
                "'bind' key in component manifest's 'program' field is missing or is not a string.",
            ),
        })
    }
}

/// Returns the path to a driver's bindbc file from its component manifest.
fn get_bindbc_path(component: &ComponentDecl) -> Option<String> {
    if let Some(program) = &component.program {
        match utils::get_dictionary_value(&program.info, "bind") {
            Some(fdata::DictionaryValue::Str(path)) => Some(path.clone()),
            _ => None,
        }
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use bind::bytecode_constants;
    use cm_rust::ProgramDecl;
    use fuchsia_archive::Utf8Reader;
    use std::io::Cursor;

    const BIND_PATH: &'static str = "meta/bind/driver.bindbc";
    // Copied from src/devices/lib/bind/src/interpreter/decode_bind_rules.rs
    const BIND_HEADER: [u8; 8] = [0x42, 0x49, 0x4E, 0x44, 0x02, 0, 0, 0];

    #[test]
    fn test_check_bind_rules_valid() {
        let mut bytecode = BIND_HEADER.to_vec();
        bytecode.push(bytecode_constants::BYTECODE_DISABLE_DEBUG);
        bytecode.extend_from_slice(&bytecode_constants::SYMB_MAGIC_NUM.to_be_bytes());
        bytecode.extend_from_slice(&0_u32.to_le_bytes());
        bytecode.extend_from_slice(&bytecode_constants::INSTRUCTION_MAGIC_NUM.to_be_bytes());
        bytecode.extend_from_slice(&0_u32.to_le_bytes());

        let mut far_reader = make_test_far(bytecode);
        let component = make_test_component();
        assert_eq!(check_bind_rules(&mut far_reader, &component).unwrap(), StaticCheckResult::Pass);
    }

    #[test]
    fn test_check_bind_rules_invalid() {
        let mut far_reader = make_test_far(BIND_HEADER.to_vec());
        let component = make_test_component();

        assert!(std::matches!(
            check_bind_rules(&mut far_reader, &component).unwrap(),
            StaticCheckResult::Fail { .. }
        ));
    }

    #[test]
    fn test_get_bindbc_path() {
        let component = make_test_component();
        assert_eq!(get_bindbc_path(&component), Some(BIND_PATH.to_string()));
    }

    fn make_test_component() -> ComponentDecl {
        ComponentDecl {
            program: Some(ProgramDecl {
                runner: None,
                info: fdata::Dictionary {
                    entries: Some(vec![fdata::DictionaryEntry {
                        key: "bind".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str(BIND_PATH.to_string()))),
                    }]),
                    ..fdata::Dictionary::EMPTY
                },
            }),
            uses: vec![],
            exposes: vec![],
            offers: vec![],
            capabilities: vec![],
            children: vec![],
            collections: vec![],
            facets: None,
            environments: vec![],
            config: None,
        }
    }

    fn make_test_far(bind_rules: Vec<u8>) -> Utf8Reader<Cursor<Vec<u8>>> {
        let mut far_bytes = Vec::new();
        fuchsia_archive::write(
            &mut far_bytes,
            std::collections::BTreeMap::from_iter([(
                BIND_PATH,
                (
                    bind_rules.len().try_into().unwrap(),
                    Box::new(Cursor::new(bind_rules)) as Box<dyn std::io::Read>,
                ),
            )]),
        )
        .unwrap();

        fuchsia_archive::Utf8Reader::new(Cursor::new(far_bytes)).unwrap()
    }
}
