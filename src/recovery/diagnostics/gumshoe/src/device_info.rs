// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_hwinfo::{Architecture, ArchitectureUnknown, BoardInfo, ProductInfo};
use serde::{ser::SerializeStruct, Serialize, Serializer};
use std::collections::HashMap;

// In order to make BoardInfo (a type from another crate) Serializable,
// wrap it with a local type GumshoeBoardInfo that represents "only the
// fields Gumshoe is allowed to externalize."
struct GumshoeBoardInfo(BoardInfo);

impl Serialize for GumshoeBoardInfo {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        // Only include Security-approved board fields for Gumshoe here.
        let mut board_info = serializer.serialize_struct("GumshoeBoardInfo", 3)?;
        board_info.serialize_field("name", &self.0.name)?;
        board_info.serialize_field("revision", &self.0.revision)?;
        board_info.serialize_field(
            "cpu_architecture",
            &match self.0.cpu_architecture {
                Some(Architecture::X64) => Some("X64"),
                Some(Architecture::Arm64) => Some("Arm64"),
                Some(ArchitectureUnknown!()) => Some("Unknown"),
                None => None,
            },
        )?;
        board_info.end()
    }
}

#[derive(Serialize)]
pub struct DeviceInfoImpl {
    board_info: Option<GumshoeBoardInfo>,
    product_info: HashMap<String, Option<String>>,
}

/// Store of "stable" Device information, like product name and serial.
/// Used as a base for data passed to Handlebars template renderer.
impl DeviceInfoImpl {
    #[cfg(test)]
    /// Easy instancing of a DeviceInfoImpl for testing.
    pub fn stub() -> Self {
        Self { board_info: None, product_info: HashMap::new() }
    }

    fn build_product_map(product_info: Option<ProductInfo>) -> HashMap<String, Option<String>> {
        let mut product_map = HashMap::new();
        match product_info {
            Some(info) => {
                println!("Product Information provided.");
                product_map.insert("SKU".to_string(), info.sku);
                product_map.insert("LANGUAGE".to_string(), info.language);
                product_map.insert("NAME".to_string(), info.name);
                product_map.insert("MODEL".to_string(), info.model);
                product_map.insert("MANUFACTURER".to_string(), info.manufacturer);
                product_map.insert("BUILD_DATE".to_string(), info.build_date);
                product_map.insert("BUILD_NAME".to_string(), info.build_name);
                product_map.insert("COLORWAY".to_string(), info.colorway);
                product_map.insert("DISPLAY".to_string(), info.display);
                product_map.insert("MEMORY".to_string(), info.memory);
                product_map.insert("NAND_STORAGE".to_string(), info.nand_storage);
                product_map.insert("EMMC_STORAGE".to_string(), info.emmc_storage);
                product_map.insert("MICROPHONE".to_string(), info.microphone);
                product_map.insert("AUDIO_AMPLIFIER".to_string(), info.audio_amplifier);
            }
            None => {
                println!("Product Information not provided");
            }
        }
        product_map
    }

    pub fn new(board_info: Option<BoardInfo>, product_info: Option<ProductInfo>) -> Self {
        DeviceInfoImpl {
            board_info: board_info.map(|b| GumshoeBoardInfo(b)),
            product_info: Self::build_product_map(product_info),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    /// Verifies placeholder implementation of DeviceInfo sets name/serial.
    fn no_product_values_when_not_provided() {
        let device = DeviceInfoImpl::stub();
        assert_eq!(0, device.product_info.len());
    }

    #[test]
    fn product_values_present_when_provided() {
        let device = DeviceInfoImpl::new(Some(BoardInfo::EMPTY), Some(ProductInfo::EMPTY));
        // Gumshoe extracts 14 fields from ProductInfo.
        assert_eq!(14, device.product_info.len());
    }
}
