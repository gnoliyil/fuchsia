// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use serde::{Deserialize, Serialize};

pub type File = String;

pub type FidlLibraryName = String;

pub type CcLibraryName = String;

pub type BanjoLibraryName = String;

/// Shortcut for adding fmt::Display and FromStr to an enumeration.
///
/// fmt::Display is used to enable printing the values as they would be
/// serialized. Without it, the enum values can be printed in Debug mode, which
/// outputs the value as defined in the Rust object but ignores any serde
/// annotations that would be applied to the value during serialization.
///
/// FromStr is required by any enums that also derive argh::FromArgs. FromArgs
/// converts a command-line input, which is a String, into the enumeration
/// value to be loaded into the argh-backed structure. All ffx plugin code uses
/// this functionality, so any enums used in ffx plugin command args require
/// this.
///
/// Both are also useful for testing purposes, as they enable quick conversions
/// in the test code to and from string literals which match the json text that
/// would generate the same values.
///
/// Usage: define an enum, then add `display_impl!(TypeName);` alongside any
/// other impl blocks you may have for that type.
///
/// Note: the enumeration must also derive serde Serialize and Deserialize.
/// Also be aware that either of these implementations will fail if the
/// underlying Serialize/Deserialize functions generate errors. This should
/// only ever be a possibility with custom implementations, or if an
/// enumeration variant has an associated value that fails (such as a map with
/// non-string keys).
#[macro_export]
macro_rules! display_impl {
    ($enum:ty) => {
        impl std::fmt::Display for $enum {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                let trim: &[char] = &['"'];
                write!(f, "{}", serde_json::to_value(self).unwrap().to_string().trim_matches(trim))
            }
        }
        impl std::str::FromStr for $enum {
            type Err = anyhow::Error;
            fn from_str(text: &str) -> anyhow::Result<Self> {
                use anyhow::Context;
                serde_json::from_str(&format!("\"{}\"", text)).with_context(|| {
                    format!(
                        "could not parse '{}' as a valid {}. \
                        Please check the help text for allowed values and try again",
                        text,
                        std::any::type_name::<$enum>()
                    )
                })
            }
        }
    };
}

#[derive(Serialize, Default, Deserialize, Debug, Hash, Clone, PartialOrd, Ord, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum CpuArchitecture {
    Arm64,
    #[default]
    X64,
    Unsupported,
}

impl From<String> for CpuArchitecture {
    fn from(item: String) -> Self {
        CpuArchitecture::from(&item[..])
    }
}

impl From<&str> for CpuArchitecture {
    fn from(item: &str) -> Self {
        match item {
            // Values based on https://doc.rust-lang.org/std/env/consts/constant.ARCH.html.
            "aarch64" => Self::Arm64,
            "x86_64" => Self::X64,
            // Values from deserialization.
            "arm64" => Self::Arm64,
            "x64" => Self::X64,
            _ => Self::Unsupported,
        }
    }
}

display_impl!(CpuArchitecture);

#[derive(Serialize, Deserialize, Debug, Default, Hash, Clone, PartialOrd, Ord, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum PointingDevice {
    Mouse,
    #[default]
    None,
    Touch,
}

display_impl!(PointingDevice);

#[derive(Serialize, Default, Deserialize, Debug, Hash, Clone, PartialOrd, Ord, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum ScreenUnits {
    #[default]
    Pixels,
}

display_impl!(ScreenUnits);

#[derive(Serialize, Deserialize, Debug, Default, Hash, Clone, PartialOrd, Ord, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum DataUnits {
    #[default]
    Bytes,
    Kilobytes,
    Megabytes,
    Gigabytes,
    Terabytes,
}

display_impl!(DataUnits);

impl DataUnits {
    /// This function provides an alternative output to the full string
    /// returned by fmt::Display. The fmt::Display version is necessary for
    /// Serialization, while the abbreviated value is needed for certain
    /// command-line conversions. Note that this diverges from Fuchsia's
    /// convention of using MiB, KiB, etc. because these abbreviations are
    /// intended for legacy compatibility with the Qemu and FVM tool command
    /// lines.
    pub fn abbreviate(&self) -> &str {
        match self {
            DataUnits::Bytes => "",
            DataUnits::Kilobytes => "K",
            DataUnits::Megabytes => "M",
            DataUnits::Gigabytes => "G",
            DataUnits::Terabytes => "T",
        }
    }
}

#[derive(Serialize, Deserialize, Debug, Default, Hash, Clone, PartialOrd, Ord, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum AudioModel {
    AC97,
    AdLib,
    Cs4231A,
    ES1370,
    Gus,
    Hda,
    #[default]
    None,
    PcSpk,
    SB16,
}

display_impl!(AudioModel);

#[derive(Serialize, Deserialize, Debug, Hash, PartialEq, Eq, Clone, PartialOrd, Ord)]
#[serde(rename_all = "snake_case")]
pub enum ElementType {
    BanjoLibrary,
    CcPrebuiltLibrary,
    CcSourceLibrary,
    CompanionHostTool,
    Config,
    DartLibrary,
    Documentation,
    FfxTool,
    FidlLibrary,
    HostTool,
    License,
    LoadableModule,
    PhysicalDevice,
    ProductBundle,
    ProductBundleContainer,
    Sysroot,
    VirtualDevice,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct Envelope<D> {
    /// The value of the $id field of the schema constraining the envelope.
    pub schema_id: String,
    pub data: D,
}
