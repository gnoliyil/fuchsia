// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(non_upper_case_globals)]

use fidl_fuchsia_images2 as images2;
use thiserror::Error;
use {fidl_fuchsia_sysmem as sysmem, std::fmt, std::str};

/// Pixel format definitions that bridge different versions of sysmem / images2
/// formats Fuchsia uses for display and GPU drivers' internal image type
/// representation.
///
/// NOTE: The color-components are interpreted in memory in little-endian byte-order.
/// E.g., PixelFormat::Bgra32 as [u8; 4] would have components [blue, green, red, alpha].
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum PixelFormat {
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Invalid and
    /// fuchsia.images2.PixelFormat.Invalid.
    Invalid,
    /// RGB only, 8 bits per each of R/G/B/A sample
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.R8G8B8A8 and
    /// fuchsia.images2.PixelFormat.R8G8B8A8.
    R8G8B8A8,
    /// 32bpp BGRA, 1 plane.  RGB only, 8 bits per each of B/G/R/A sample.
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Bgra32 and
    /// fuchsia.images2.PixelFormat.Bgra32.
    Bgra32,
    /// YUV only, 8 bits per Y sample
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.I420 and
    /// fuchsia.images2.PixelFormat.I420.
    I420,
    /// YUV only, 8 bits per Y sample
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.M420 and
    /// fuchsia.images2.PixelFormat.M420.
    M420,
    /// YUV only, 8 bits per Y sample
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Nv12 and
    /// fuchsia.images2.PixelFormat.Nv12.
    Nv12,
    /// YUV only, 8 bits per Y sample
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Yuy2 and
    /// fuchsia.images2.PixelFormat.Yuy2.
    Yuy2,
    /// This value is reserved, and not currently used.
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Mjpeg and
    /// fuchsia.images2.PixelFormat.Mjpeg.
    Mjpeg,
    /// YUV only, 8 bits per Y sample
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Yv12 and
    /// fuchsia.images2.PixelFormat.Yv12.
    Yv12,
    /// 24bpp BGR, 1 plane. RGB only, 8 bits per each of B/G/R sample
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Bgr24 and
    /// fuchsia.images2.PixelFormat.Bgr24.
    Bgr24,
    /// 16bpp RGB, 1 plane. 5 bits (most significant) R, 6 bits G, 5 bits B
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Rgb565 and
    /// fuchsia.images2.PixelFormat.Rgb565.
    Rgb565,
    /// 8bpp RGB, 1 plane. 3 bits (most significant) R, 3 bits G, 2 bits B
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Rgb332 and
    /// fuchsia.images2.PixelFormat.Rgb332.
    Rgb332,
    /// 8bpp RGB, 1 plane. 2 bits (most significant) R, 2 bits G, 2 bits B
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.Rgb2220 and
    /// fuchsia.images2.PixelFormat.Rgb2220.
    Rgb2220,
    /// 8bpp, Luminance-only (red, green and blue have identical values.)
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.L8 and
    /// fuchsia.images2.PixelFormat.L8.
    L8,
    /// 8bpp, Red-only (Green and Blue are to be interpreted as 0).
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.R8 and
    /// fuchsia.images2.PixelFormat.R8.
    R8,
    /// 16bpp RG, 1 plane. 8 bits R, 8 bits G.
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.R8G8 and
    /// fuchsia.images2.PixelFormat.R8G8.
    R8G8,
    /// 32bpp RGBA, 1 plane. 2 bits A, 10 bits R/G/B.
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.A2R10G10B10 and
    /// fuchsia.images2.PixelFormat.A2R10G10B10.
    A2R10G10B10,
    /// 32bpp BGRA, 1 plane. 2 bits A, 10 bits R/G/B.
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.A2B10G10R10 and
    /// fuchsia.images2.PixelFormat.A2B10G10R10.
    A2B10G10R10,
    /// A client is explicitly indicating that the client does not care which
    /// pixel format is chosen / used.
    ///
    /// Equivalent to fuchsia.sysmem.PixelFormatType.DoNotCare and
    /// fuchsia.images2.PixelFormat.DoNotCare.
    DoNotCare,
}

impl Default for PixelFormat {
    fn default() -> PixelFormat {
        PixelFormat::Invalid
    }
}

impl From<images2::PixelFormat> for PixelFormat {
    fn from(src: images2::PixelFormat) -> Self {
        match src {
            images2::PixelFormat::Invalid => Self::Invalid,
            images2::PixelFormat::R8G8B8A8 => Self::R8G8B8A8,
            images2::PixelFormat::Bgra32 => Self::Bgra32,
            images2::PixelFormat::I420 => Self::I420,
            images2::PixelFormat::M420 => Self::M420,
            images2::PixelFormat::Nv12 => Self::Nv12,
            images2::PixelFormat::Yuy2 => Self::Yuy2,
            images2::PixelFormat::Mjpeg => Self::Mjpeg,
            images2::PixelFormat::Yv12 => Self::Yv12,
            images2::PixelFormat::Bgr24 => Self::Bgr24,
            images2::PixelFormat::Rgb565 => Self::Rgb565,
            images2::PixelFormat::Rgb332 => Self::Rgb332,
            images2::PixelFormat::Rgb2220 => Self::Rgb2220,
            images2::PixelFormat::L8 => Self::L8,
            images2::PixelFormat::R8 => Self::R8,
            images2::PixelFormat::R8G8 => Self::R8G8,
            images2::PixelFormat::A2R10G10B10 => Self::A2R10G10B10,
            images2::PixelFormat::A2B10G10R10 => Self::A2B10G10R10,
            images2::PixelFormat::DoNotCare => Self::DoNotCare,
            _ => Self::Invalid,
        }
    }
}

impl From<&images2::PixelFormat> for PixelFormat {
    fn from(src: &images2::PixelFormat) -> Self {
        Self::from(*src)
    }
}

impl From<PixelFormat> for images2::PixelFormat {
    fn from(src: PixelFormat) -> Self {
        match src {
            PixelFormat::Invalid => Self::Invalid,
            PixelFormat::R8G8B8A8 => Self::R8G8B8A8,
            PixelFormat::Bgra32 => Self::Bgra32,
            PixelFormat::I420 => Self::I420,
            PixelFormat::M420 => Self::M420,
            PixelFormat::Nv12 => Self::Nv12,
            PixelFormat::Yuy2 => Self::Yuy2,
            PixelFormat::Mjpeg => Self::Mjpeg,
            PixelFormat::Yv12 => Self::Yv12,
            PixelFormat::Bgr24 => Self::Bgr24,
            PixelFormat::Rgb565 => Self::Rgb565,
            PixelFormat::Rgb332 => Self::Rgb332,
            PixelFormat::Rgb2220 => Self::Rgb2220,
            PixelFormat::L8 => Self::L8,
            PixelFormat::R8 => Self::R8,
            PixelFormat::R8G8 => Self::R8G8,
            PixelFormat::A2R10G10B10 => Self::A2R10G10B10,
            PixelFormat::A2B10G10R10 => Self::A2B10G10R10,
            PixelFormat::DoNotCare => Self::DoNotCare,
        }
    }
}

impl From<&PixelFormat> for images2::PixelFormat {
    fn from(src: &PixelFormat) -> Self {
        Self::from(*src)
    }
}

impl From<sysmem::PixelFormatType> for PixelFormat {
    fn from(src: sysmem::PixelFormatType) -> Self {
        match src {
            sysmem::PixelFormatType::Invalid => Self::Invalid,
            sysmem::PixelFormatType::R8G8B8A8 => Self::R8G8B8A8,
            sysmem::PixelFormatType::Bgra32 => Self::Bgra32,
            sysmem::PixelFormatType::I420 => Self::I420,
            sysmem::PixelFormatType::M420 => Self::M420,
            sysmem::PixelFormatType::Nv12 => Self::Nv12,
            sysmem::PixelFormatType::Yuy2 => Self::Yuy2,
            sysmem::PixelFormatType::Mjpeg => Self::Mjpeg,
            sysmem::PixelFormatType::Yv12 => Self::Yv12,
            sysmem::PixelFormatType::Bgr24 => Self::Bgr24,
            sysmem::PixelFormatType::Rgb565 => Self::Rgb565,
            sysmem::PixelFormatType::Rgb332 => Self::Rgb332,
            sysmem::PixelFormatType::Rgb2220 => Self::Rgb2220,
            sysmem::PixelFormatType::L8 => Self::L8,
            sysmem::PixelFormatType::R8 => Self::R8,
            sysmem::PixelFormatType::R8G8 => Self::R8G8,
            sysmem::PixelFormatType::A2R10G10B10 => Self::A2R10G10B10,
            sysmem::PixelFormatType::A2B10G10R10 => Self::A2B10G10R10,
            sysmem::PixelFormatType::DoNotCare => Self::DoNotCare,
        }
    }
}

impl From<&sysmem::PixelFormatType> for PixelFormat {
    fn from(src: &sysmem::PixelFormatType) -> Self {
        Self::from(*src)
    }
}

impl From<PixelFormat> for sysmem::PixelFormatType {
    fn from(src: PixelFormat) -> Self {
        match src {
            PixelFormat::Invalid => Self::Invalid,
            PixelFormat::R8G8B8A8 => Self::R8G8B8A8,
            PixelFormat::Bgra32 => Self::Bgra32,
            PixelFormat::I420 => Self::I420,
            PixelFormat::M420 => Self::M420,
            PixelFormat::Nv12 => Self::Nv12,
            PixelFormat::Yuy2 => Self::Yuy2,
            PixelFormat::Mjpeg => Self::Mjpeg,
            PixelFormat::Yv12 => Self::Yv12,
            PixelFormat::Bgr24 => Self::Bgr24,
            PixelFormat::Rgb565 => Self::Rgb565,
            PixelFormat::Rgb332 => Self::Rgb332,
            PixelFormat::Rgb2220 => Self::Rgb2220,
            PixelFormat::L8 => Self::L8,
            PixelFormat::R8 => Self::R8,
            PixelFormat::R8G8 => Self::R8G8,
            PixelFormat::A2R10G10B10 => Self::A2R10G10B10,
            PixelFormat::A2B10G10R10 => Self::A2B10G10R10,
            PixelFormat::DoNotCare => Self::DoNotCare,
        }
    }
}

impl From<&PixelFormat> for sysmem::PixelFormatType {
    fn from(src: &PixelFormat) -> Self {
        Self::from(*src)
    }
}

impl fmt::Display for PixelFormat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let string: &str = match *self {
            PixelFormat::Invalid => "(Invalid)",
            PixelFormat::R8G8B8A8 => "RGBA 8888",
            PixelFormat::Bgra32 => "BGRA 8888",
            PixelFormat::I420 => "I420",
            PixelFormat::M420 => "M420",
            PixelFormat::Nv12 => "NV12",
            PixelFormat::Yuy2 => "YUY2",
            PixelFormat::Mjpeg => "MJPEG",
            PixelFormat::Yv12 => "YV12",
            PixelFormat::Bgr24 => "BGR 888",
            PixelFormat::Rgb565 => "RGB 565",
            PixelFormat::Rgb332 => "RGB 332",
            PixelFormat::Rgb2220 => "RGB 2220",
            PixelFormat::L8 => "Mono L8",
            PixelFormat::R8 => "Mono R8",
            PixelFormat::R8G8 => "R8G8",
            PixelFormat::A2R10G10B10 => "A2 R10 G10 B10",
            PixelFormat::A2B10G10R10 => "A2 B10 G10 R10",
            PixelFormat::DoNotCare => "(Do not care)",
        };
        write!(f, "{}", string)
    }
}

#[derive(Debug, PartialEq)]
pub enum ParsePixelFormatError {
    UnsupportedFormat(String),
}

impl fmt::Display for ParsePixelFormatError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ParsePixelFormatError::UnsupportedFormat(s) => write!(f, "Unsupported format {}", s),
        }
    }
}

impl str::FromStr for PixelFormat {
    type Err = ParsePixelFormatError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s.to_ascii_lowercase().as_str() {
            "invalid" => Ok(PixelFormat::Invalid),
            "rgba8888" | "r8g8b8a8" | "rgba32" => Ok(PixelFormat::R8G8B8A8),
            "bgra8888" | "b8g8r8a8" | "bgra32" => Ok(PixelFormat::Bgra32),
            "i420" => Ok(PixelFormat::I420),
            "m420" => Ok(PixelFormat::M420),
            "nv12" => Ok(PixelFormat::Nv12),
            "yuy2" => Ok(PixelFormat::Yuy2),
            "mjpeg" => Ok(PixelFormat::Mjpeg),
            "yv12" => Ok(PixelFormat::Yv12),
            "bgr888" | "bgr24" => Ok(PixelFormat::Bgr24),
            "rgb565" => Ok(PixelFormat::Rgb565),
            "rgb332" => Ok(PixelFormat::Rgb332),
            "rgb2220" => Ok(PixelFormat::Rgb2220),
            "monol8" => Ok(PixelFormat::L8),
            "monor8" => Ok(PixelFormat::R8),
            "r8g8" => Ok(PixelFormat::R8G8),
            "a2r10g10b10" => Ok(PixelFormat::A2R10G10B10),
            "a2b10g10r10" => Ok(PixelFormat::A2B10G10R10),
            "donotcare" => Ok(PixelFormat::DoNotCare),
            _ => Err(ParsePixelFormatError::UnsupportedFormat(s.to_string())),
        }
    }
}

#[derive(Debug, Error, PartialEq)]
pub enum GetBytesPerPixelError {
    #[error("Unsupported format: {0}")]
    UnsupportedFormat(PixelFormat),
}

/// Get number of bytes per pixel for given `pixel_format`.
/// Currently only packed pixel formats are supported. Planar formats (e.g.
/// NV12) are not supported.
pub fn get_bytes_per_pixel(pixel_format: PixelFormat) -> Result<usize, GetBytesPerPixelError> {
    match pixel_format {
        PixelFormat::R8G8B8A8
        | PixelFormat::Bgra32
        | PixelFormat::A2R10G10B10
        | PixelFormat::A2B10G10R10 => Ok(4),
        PixelFormat::Bgr24 => Ok(3),
        PixelFormat::Rgb565 | PixelFormat::R8G8 => Ok(2),
        PixelFormat::Rgb332 | PixelFormat::Rgb2220 | PixelFormat::L8 | PixelFormat::R8 => Ok(1),
        _ => Err(GetBytesPerPixelError::UnsupportedFormat(pixel_format)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::str::FromStr;

    #[fuchsia::test]
    fn pixel_format_from_str_invalid() {
        assert_eq!(
            PixelFormat::from_str("bad"),
            Err(ParsePixelFormatError::UnsupportedFormat("bad".to_string()))
        );
    }

    #[fuchsia::test]
    fn pixel_format_from_str_valid() {
        assert_eq!(PixelFormat::from_str("invalid"), Ok(PixelFormat::Invalid));
        assert_eq!(PixelFormat::from_str("rgba8888"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("r8g8b8a8"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("rgba32"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("bgra8888"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("b8g8r8a8"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("bgra32"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("i420"), Ok(PixelFormat::I420));
        assert_eq!(PixelFormat::from_str("m420"), Ok(PixelFormat::M420));
        assert_eq!(PixelFormat::from_str("nv12"), Ok(PixelFormat::Nv12));
        assert_eq!(PixelFormat::from_str("yuy2"), Ok(PixelFormat::Yuy2));
        assert_eq!(PixelFormat::from_str("mjpeg"), Ok(PixelFormat::Mjpeg));
        assert_eq!(PixelFormat::from_str("yv12"), Ok(PixelFormat::Yv12));
        assert_eq!(PixelFormat::from_str("bgr888"), Ok(PixelFormat::Bgr24));
        assert_eq!(PixelFormat::from_str("bgr24"), Ok(PixelFormat::Bgr24));
        assert_eq!(PixelFormat::from_str("rgb565"), Ok(PixelFormat::Rgb565));
        assert_eq!(PixelFormat::from_str("rgb332"), Ok(PixelFormat::Rgb332));
        assert_eq!(PixelFormat::from_str("rgb2220"), Ok(PixelFormat::Rgb2220));
        assert_eq!(PixelFormat::from_str("monol8"), Ok(PixelFormat::L8));
        assert_eq!(PixelFormat::from_str("monor8"), Ok(PixelFormat::R8));
        assert_eq!(PixelFormat::from_str("r8g8"), Ok(PixelFormat::R8G8));
        assert_eq!(PixelFormat::from_str("a2r10g10b10"), Ok(PixelFormat::A2R10G10B10));
        assert_eq!(PixelFormat::from_str("a2b10g10r10"), Ok(PixelFormat::A2B10G10R10));
        assert_eq!(PixelFormat::from_str("donotcare"), Ok(PixelFormat::DoNotCare));

        assert_eq!(PixelFormat::from_str("INVALID"), Ok(PixelFormat::Invalid));
        assert_eq!(PixelFormat::from_str("RGBA8888"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("R8G8B8A8"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("RGBA32"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("BGRA8888"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("B8G8R8A8"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("BGRA32"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("I420"), Ok(PixelFormat::I420));
        assert_eq!(PixelFormat::from_str("M420"), Ok(PixelFormat::M420));
        assert_eq!(PixelFormat::from_str("NV12"), Ok(PixelFormat::Nv12));
        assert_eq!(PixelFormat::from_str("YUY2"), Ok(PixelFormat::Yuy2));
        assert_eq!(PixelFormat::from_str("MJPEG"), Ok(PixelFormat::Mjpeg));
        assert_eq!(PixelFormat::from_str("YV12"), Ok(PixelFormat::Yv12));
        assert_eq!(PixelFormat::from_str("BGR888"), Ok(PixelFormat::Bgr24));
        assert_eq!(PixelFormat::from_str("BGR24"), Ok(PixelFormat::Bgr24));
        assert_eq!(PixelFormat::from_str("RGB565"), Ok(PixelFormat::Rgb565));
        assert_eq!(PixelFormat::from_str("RGB332"), Ok(PixelFormat::Rgb332));
        assert_eq!(PixelFormat::from_str("RGB2220"), Ok(PixelFormat::Rgb2220));
        assert_eq!(PixelFormat::from_str("MONOL8"), Ok(PixelFormat::L8));
        assert_eq!(PixelFormat::from_str("MONOR8"), Ok(PixelFormat::R8));
        assert_eq!(PixelFormat::from_str("R8G8"), Ok(PixelFormat::R8G8));
        assert_eq!(PixelFormat::from_str("A2R10G10B10"), Ok(PixelFormat::A2R10G10B10));
        assert_eq!(PixelFormat::from_str("A2B10G10R10"), Ok(PixelFormat::A2B10G10R10));
        assert_eq!(PixelFormat::from_str("DONOTCARE"), Ok(PixelFormat::DoNotCare));

        assert_eq!(PixelFormat::from_str("Invalid"), Ok(PixelFormat::Invalid));
        assert_eq!(PixelFormat::from_str("Rgba8888"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("R8g8b8a8"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("Rgba32"), Ok(PixelFormat::R8G8B8A8));
        assert_eq!(PixelFormat::from_str("Bgra8888"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("B8g8r8a8"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("Bgra32"), Ok(PixelFormat::Bgra32));
        assert_eq!(PixelFormat::from_str("I420"), Ok(PixelFormat::I420));
        assert_eq!(PixelFormat::from_str("M420"), Ok(PixelFormat::M420));
        assert_eq!(PixelFormat::from_str("Nv12"), Ok(PixelFormat::Nv12));
        assert_eq!(PixelFormat::from_str("Yuy2"), Ok(PixelFormat::Yuy2));
        assert_eq!(PixelFormat::from_str("Mjpeg"), Ok(PixelFormat::Mjpeg));
        assert_eq!(PixelFormat::from_str("Yv12"), Ok(PixelFormat::Yv12));
        assert_eq!(PixelFormat::from_str("Bgr888"), Ok(PixelFormat::Bgr24));
        assert_eq!(PixelFormat::from_str("Bgr24"), Ok(PixelFormat::Bgr24));
        assert_eq!(PixelFormat::from_str("Rgb565"), Ok(PixelFormat::Rgb565));
        assert_eq!(PixelFormat::from_str("Rgb332"), Ok(PixelFormat::Rgb332));
        assert_eq!(PixelFormat::from_str("Rgb2220"), Ok(PixelFormat::Rgb2220));
        assert_eq!(PixelFormat::from_str("Monol8"), Ok(PixelFormat::L8));
        assert_eq!(PixelFormat::from_str("Monor8"), Ok(PixelFormat::R8));
        assert_eq!(PixelFormat::from_str("R8g8"), Ok(PixelFormat::R8G8));
        assert_eq!(PixelFormat::from_str("A2r10g10b10"), Ok(PixelFormat::A2R10G10B10));
        assert_eq!(PixelFormat::from_str("A2b10g10r10"), Ok(PixelFormat::A2B10G10R10));
        assert_eq!(PixelFormat::from_str("DoNotCare"), Ok(PixelFormat::DoNotCare));
    }

    #[fuchsia::test]
    fn get_bytes_per_pixel_valid() {
        assert_eq!(get_bytes_per_pixel(PixelFormat::R8G8B8A8), Ok(4));
        assert_eq!(get_bytes_per_pixel(PixelFormat::Bgra32), Ok(4));
        assert_eq!(get_bytes_per_pixel(PixelFormat::Bgr24), Ok(3));
        assert_eq!(get_bytes_per_pixel(PixelFormat::Rgb565), Ok(2));
        assert_eq!(get_bytes_per_pixel(PixelFormat::Rgb332), Ok(1));
        assert_eq!(get_bytes_per_pixel(PixelFormat::Rgb2220), Ok(1));
        assert_eq!(get_bytes_per_pixel(PixelFormat::L8), Ok(1));
        assert_eq!(get_bytes_per_pixel(PixelFormat::R8), Ok(1));
        assert_eq!(get_bytes_per_pixel(PixelFormat::R8G8), Ok(2));
        assert_eq!(get_bytes_per_pixel(PixelFormat::A2R10G10B10), Ok(4));
        assert_eq!(get_bytes_per_pixel(PixelFormat::A2B10G10R10), Ok(4));
    }

    #[fuchsia::test]
    fn get_bytes_per_pixel_invalid() {
        assert_eq!(
            get_bytes_per_pixel(PixelFormat::Invalid),
            Err(GetBytesPerPixelError::UnsupportedFormat(PixelFormat::Invalid))
        );
        assert_eq!(
            get_bytes_per_pixel(PixelFormat::I420),
            Err(GetBytesPerPixelError::UnsupportedFormat(PixelFormat::I420))
        );
        assert_eq!(
            get_bytes_per_pixel(PixelFormat::M420),
            Err(GetBytesPerPixelError::UnsupportedFormat(PixelFormat::M420))
        );
        assert_eq!(
            get_bytes_per_pixel(PixelFormat::Nv12),
            Err(GetBytesPerPixelError::UnsupportedFormat(PixelFormat::Nv12))
        );
        assert_eq!(
            get_bytes_per_pixel(PixelFormat::Yuy2),
            Err(GetBytesPerPixelError::UnsupportedFormat(PixelFormat::Yuy2))
        );
        assert_eq!(
            get_bytes_per_pixel(PixelFormat::Mjpeg),
            Err(GetBytesPerPixelError::UnsupportedFormat(PixelFormat::Mjpeg))
        );
        assert_eq!(
            get_bytes_per_pixel(PixelFormat::Yv12),
            Err(GetBytesPerPixelError::UnsupportedFormat(PixelFormat::Yv12))
        );
        assert_eq!(
            get_bytes_per_pixel(PixelFormat::DoNotCare),
            Err(GetBytesPerPixelError::UnsupportedFormat(PixelFormat::DoNotCare))
        );
    }

    #[fuchsia::test]
    fn sysmem2_pixel_format_conversion() {
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Invalid)),
            images2::PixelFormat::Invalid
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::R8G8B8A8)),
            images2::PixelFormat::R8G8B8A8
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Bgra32)),
            images2::PixelFormat::Bgra32
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::I420)),
            images2::PixelFormat::I420
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::M420)),
            images2::PixelFormat::M420
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Nv12)),
            images2::PixelFormat::Nv12
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Yuy2)),
            images2::PixelFormat::Yuy2
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Mjpeg)),
            images2::PixelFormat::Mjpeg
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Yv12)),
            images2::PixelFormat::Yv12
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Bgr24)),
            images2::PixelFormat::Bgr24
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Rgb565)),
            images2::PixelFormat::Rgb565
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Rgb332)),
            images2::PixelFormat::Rgb332
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::Rgb2220)),
            images2::PixelFormat::Rgb2220
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::L8)),
            images2::PixelFormat::L8
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::R8)),
            images2::PixelFormat::R8
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::R8G8)),
            images2::PixelFormat::R8G8
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::A2R10G10B10)),
            images2::PixelFormat::A2R10G10B10
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::A2B10G10R10)),
            images2::PixelFormat::A2B10G10R10
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(images2::PixelFormat::DoNotCare)),
            images2::PixelFormat::DoNotCare
        );
    }

    #[fuchsia::test]
    fn sysmem_pixel_format_conversion() {
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Invalid)),
            sysmem::PixelFormatType::Invalid
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::R8G8B8A8)),
            sysmem::PixelFormatType::R8G8B8A8
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Bgra32)),
            sysmem::PixelFormatType::Bgra32
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::I420)),
            sysmem::PixelFormatType::I420
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::M420)),
            sysmem::PixelFormatType::M420
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Nv12)),
            sysmem::PixelFormatType::Nv12
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Yuy2)),
            sysmem::PixelFormatType::Yuy2
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Mjpeg)),
            sysmem::PixelFormatType::Mjpeg
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Yv12)),
            sysmem::PixelFormatType::Yv12
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Bgr24)),
            sysmem::PixelFormatType::Bgr24
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Rgb565)),
            sysmem::PixelFormatType::Rgb565
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Rgb332)),
            sysmem::PixelFormatType::Rgb332
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::Rgb2220)),
            sysmem::PixelFormatType::Rgb2220
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::L8)),
            sysmem::PixelFormatType::L8
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::R8)),
            sysmem::PixelFormatType::R8
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::R8G8)),
            sysmem::PixelFormatType::R8G8
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::A2R10G10B10)),
            sysmem::PixelFormatType::A2R10G10B10
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::A2B10G10R10)),
            sysmem::PixelFormatType::A2B10G10R10
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(sysmem::PixelFormatType::DoNotCare)),
            sysmem::PixelFormatType::DoNotCare
        );
    }

    #[fuchsia::test]
    fn sysmem2_to_sysmem_pixel_format_conversion() {
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Invalid)),
            sysmem::PixelFormatType::Invalid
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::R8G8B8A8)),
            sysmem::PixelFormatType::R8G8B8A8
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Bgra32)),
            sysmem::PixelFormatType::Bgra32
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::I420)),
            sysmem::PixelFormatType::I420
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::M420)),
            sysmem::PixelFormatType::M420
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Nv12)),
            sysmem::PixelFormatType::Nv12
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Yuy2)),
            sysmem::PixelFormatType::Yuy2
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Mjpeg)),
            sysmem::PixelFormatType::Mjpeg
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Yv12)),
            sysmem::PixelFormatType::Yv12
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Bgr24)),
            sysmem::PixelFormatType::Bgr24
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Rgb565)),
            sysmem::PixelFormatType::Rgb565
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Rgb332)),
            sysmem::PixelFormatType::Rgb332
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::Rgb2220)),
            sysmem::PixelFormatType::Rgb2220
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::L8)),
            sysmem::PixelFormatType::L8
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::R8)),
            sysmem::PixelFormatType::R8
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::R8G8)),
            sysmem::PixelFormatType::R8G8
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::A2R10G10B10)),
            sysmem::PixelFormatType::A2R10G10B10
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::A2B10G10R10)),
            sysmem::PixelFormatType::A2B10G10R10
        );
        assert_eq!(
            sysmem::PixelFormatType::from(PixelFormat::from(images2::PixelFormat::DoNotCare)),
            sysmem::PixelFormatType::DoNotCare
        );
    }

    #[fuchsia::test]
    fn sysmem_to_sysmem2_pixel_format_conversion() {
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Invalid)),
            images2::PixelFormat::Invalid
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::R8G8B8A8)),
            images2::PixelFormat::R8G8B8A8
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Bgra32)),
            images2::PixelFormat::Bgra32
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::I420)),
            images2::PixelFormat::I420
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::M420)),
            images2::PixelFormat::M420
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Nv12)),
            images2::PixelFormat::Nv12
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Yuy2)),
            images2::PixelFormat::Yuy2
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Mjpeg)),
            images2::PixelFormat::Mjpeg
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Yv12)),
            images2::PixelFormat::Yv12
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Bgr24)),
            images2::PixelFormat::Bgr24
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Rgb565)),
            images2::PixelFormat::Rgb565
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Rgb332)),
            images2::PixelFormat::Rgb332
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::Rgb2220)),
            images2::PixelFormat::Rgb2220
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::L8)),
            images2::PixelFormat::L8
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::R8)),
            images2::PixelFormat::R8
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::R8G8)),
            images2::PixelFormat::R8G8
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::A2R10G10B10)),
            images2::PixelFormat::A2R10G10B10
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::A2B10G10R10)),
            images2::PixelFormat::A2B10G10R10
        );
        assert_eq!(
            images2::PixelFormat::from(PixelFormat::from(sysmem::PixelFormatType::DoNotCare)),
            images2::PixelFormat::DoNotCare
        );
    }
}
