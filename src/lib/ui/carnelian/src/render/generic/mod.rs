// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{convert::TryFrom, error, fmt, fmt::Debug, hash::Hash, io::Read, ops::Add, u32};

use anyhow::Error;
use euclid::{
    default::{Point2D, Rect, Size2D, Transform2D, Vector2D},
    point2, size2,
};
use fidl::endpoints::ClientEnd;
use fidl_fuchsia_sysmem::BufferCollectionTokenMarker;
use fuchsia_framebuffer::PixelFormat;

use crate::{color::Color, drawing::DisplayRotation, Point, ViewAssistantContext};

pub mod forma;

pub use self::forma::Forma;

/// Either Spinel or Forma. Zero-sized.
pub trait Backend: Copy + Debug + Default + Eq + Hash + Ord + Sized + 'static {
    /// Buffer-backed image that can be used for rendering or storing pixel data.
    type Image: Copy + Debug + Eq + Hash + Ord;
    /// Backend's rendering context.
    type Context: Context<Self>;
    /// Vector path.
    type Path: Clone + Eq;
    /// Stateful path builder.
    type PathBuilder: PathBuilder<Self>;
    /// Compact rasterized form of any number of paths.
    type Raster: Raster;
    /// Stateful raster builder.
    type RasterBuilder: RasterBuilder<Self>;
    /// Composition of stylized rasters.
    type Composition: Composition<Self>;

    /// Creates a new rendering context
    fn new_context(
        token: ClientEnd<BufferCollectionTokenMarker>,
        size: Size2D<u32>,
        display_rotation: DisplayRotation,
    ) -> Self::Context;
}

/// Rectangular copy region.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CopyRegion {
    /// Top-left origin of source rectangle.
    pub src_offset: Point2D<u32>,
    /// Top-left origin of destination rectangle.
    pub dst_offset: Point2D<u32>,
    /// Size of both source and destination rectangles.
    pub extent: Size2D<u32>,
}

/// Rendering extensions.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RenderExt<B: Backend> {
    /// Clears render image before rendering.
    pub pre_clear: Option<PreClear>,
    /// Copies from source image to render image before rendering.
    pub pre_copy: Option<PreCopy<B>>,
    /// Copies from render image to destination image after rendering.
    pub post_copy: Option<PostCopy<B>>,
}

/// Pre-render image clear.
#[derive(Clone, Debug, PartialEq)]
pub struct PreClear {
    /// Clear color.
    pub color: Color,
}

/// Pre-render image copy.
#[derive(Clone, Debug, PartialEq)]
pub struct PreCopy<B: Backend> {
    /// Source image to copy from.
    pub image: B::Image,
    /// Copy region properties.
    pub copy_region: CopyRegion,
}

/// Post-render image copy.
#[derive(Clone, Debug, PartialEq)]
pub struct PostCopy<B: Backend> {
    /// Destination image to copy to. Must be different from render image.
    pub image: B::Image,
    /// Copy region properties.
    pub copy_region: CopyRegion,
}

/// Rendering context and API start point.
pub trait Context<B: Backend> {
    /// Returns the context's pixel format.
    fn pixel_format(&self) -> PixelFormat;
    /// Optionally returns a `PathBuilder`. May return `None` of old builder is still alive.
    fn path_builder(&self) -> Option<B::PathBuilder>;
    /// Optionally returns a `RasterBuilder`. May return `None` of old builder is still alive.
    fn raster_builder(&self) -> Option<B::RasterBuilder>;
    /// Creates a new image with `size`.
    fn new_image(&mut self, size: Size2D<u32>) -> B::Image;
    /// Creates a new image from PNG `reader`.
    fn new_image_from_png<R: Read>(
        &mut self,
        reader: &mut png::Reader<R>,
    ) -> Result<B::Image, Error>;
    /// Returns the image at `image_index`.
    fn get_image(&mut self, image_index: u32) -> B::Image;
    /// Returns the `context`'s current image.
    fn get_current_image(&mut self, context: &ViewAssistantContext) -> B::Image;
    /// Renders the composition with an optional clip to the image.
    fn render(
        &mut self,
        composition: &mut B::Composition,
        clip: Option<Rect<u32>>,
        image: B::Image,
        ext: &RenderExt<B>,
    ) {
        self.render_with_clip(
            composition,
            clip.unwrap_or_else(|| {
                Rect::new(point2(u32::MIN, u32::MIN), size2(u32::MAX, u32::MAX))
            }),
            image,
            ext,
        );
    }
    /// Renders the composition with a clip to the image.
    fn render_with_clip(
        &mut self,
        composition: &mut B::Composition,
        clip: Rect<u32>,
        image: B::Image,
        ext: &RenderExt<B>,
    );
}

/// Builds one closed path.
pub trait PathBuilder<B: Backend> {
    /// Move end-point to.
    fn move_to(&mut self, point: Point) -> &mut Self;
    /// Create line from end-point to point and update end-point.
    fn line_to(&mut self, point: Point) -> &mut Self;
    /// Create quadratic Bézier from end-point to `p2` with `p1` as control point.
    fn quad_to(&mut self, p1: Point, p2: Point) -> &mut Self;
    /// Create cubic Bézier from end-point to `p3` with `p1` and `p2` as control points.
    fn cubic_to(&mut self, p1: Point, p2: Point, p3: Point) -> &mut Self;
    /// Create rational quadratic Bézier from end-point to `p2` with `p1` as control point
    /// and `w` as its weight.
    fn rat_quad_to(&mut self, p1: Point, p2: Point, w: f32) -> &mut Self;
    /// Create rational cubic Bézier from end-point to `p3` with `p1` and `p2` as control
    /// points, and `w1` and `w2` their weights.
    fn rat_cubic_to(&mut self, p1: Point, p2: Point, p3: Point, w1: f32, w2: f32) -> &mut Self;
    /// Closes the path with a line if not yet closed and builds the path.
    ///
    /// Consumes the builder; another one can be requested from the `Context`.
    fn build(self) -> B::Path;
}

/// Raster.
pub trait Raster: Add<Output = Self> + Clone + Eq {
    /// Translate raster.
    fn translate(self, translation: Vector2D<i32>) -> Self;
}

/// Builds one Raster.
pub trait RasterBuilder<B: Backend> {
    /// Add a path to the raster with optional transform.
    fn add(&mut self, path: &B::Path, transform: Option<&Transform2D<f32>>) -> &mut Self {
        self.add_with_transform(path, transform.unwrap_or(&Transform2D::identity()))
    }
    /// Add a path to the raster with transform.
    fn add_with_transform(&mut self, path: &B::Path, transform: &Transform2D<f32>) -> &mut Self;
    /// Builds the raster.
    ///
    /// Consumes the builder; another one can be requested from the `Context`.
    fn build(self) -> B::Raster;
}

/// Raster fill rule that determines how self-intersecting parts of the path are filled. It uses the
/// winding number--the count of how many full counter-clockwise revolutions ('windings') the curve
/// makes around a point--to determine whether this point should be filled or not.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum FillRule {
    /// Points with non-zero winding number are filled.
    NonZero,
    /// Points with odd non-zero numbers are filled.
    EvenOdd,
}

#[derive(Clone, Copy, Debug)]
pub enum GradientType {
    Linear,
    Radial,
}

#[derive(Clone, Debug)]
pub struct Gradient {
    pub r#type: GradientType,
    pub start: Point,
    pub end: Point,
    pub stops: Vec<(Color, f32)>,
}

/// Raster fill type.
#[derive(Clone, Debug)]
pub enum Fill {
    /// Fills the raster with a uniform color.
    Solid(Color),
    /// Fills the raster with a gradient.
    Gradient(Gradient),
}

/// Raster blend mode. See https://www.w3.org/TR/compositing/#blending for more details.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BlendMode {
    /// Normal. Source is placed over the destination.
    Over,
    /// The source color is multiplied by the destination color and replaces the destination.
    Multiply,
    /// Multiplies the complements of the backdrop and source color values, then complements the
    /// result.
    Screen,
    /// Multiplies or screens the colors, depending on the backdrop color value.
    Overlay,
    /// Selects the darker of the backdrop and source colors.
    Darken,
    /// Selects the lighter of the backdrop and source colors.
    Lighten,
    /// Brightens the backdrop color to reflect the source color. Painting with black produces no
    /// changes.
    ColorDodge,
    /// Darkens the backdrop color to reflect the source color. Painting with white produces no
    /// change.
    ColorBurn,
    /// Multiplies or screens the colors, depending on the source color value. The effect is
    /// similar to shining a harsh spotlight on the backdrop.
    HardLight,
    /// Darkens or lightens the colors, depending on the source color value. The effect is similar
    /// to shining a diffused spotlight on the backdrop.
    SoftLight,
    /// Subtracts the darker of the two constituent colors from the lighter color.
    Difference,
    /// Produces an effect similar to that of the `Difference` mode but lower in contrast.
    /// Painting with white inverts the backdrop color; painting with black produces no change.
    Exclusion,
    /// Creates a color with the hue of the source color and the saturation and luminosity of the backdrop color.
    Hue,
    /// Creates a color with the saturation of the source color and the hue and luminosity of the backdrop color.
    /// Painting with this mode in an area of the backdrop that is a pure gray (no saturation) produces no change.
    Saturation,
    /// Creates a color with the hue and saturation of the source color and the luminosity of the backdrop color.
    /// This preserves the gray levels of the backdrop and is useful for coloring monochrome images or tinting color images.
    Color,
    /// Creates a color with the luminosity of the source color and the hue and saturation of the backdrop color.
    /// This produces an inverse effect to that of the `Color` mode.
    Luminosity,
}

/// Raster style.
#[derive(Clone, Debug)]
pub struct Style {
    /// Raster fill rule.
    pub fill_rule: FillRule,
    /// Raster fill type.
    pub fill: Fill,
    /// Raster blend mode.
    pub blend_mode: BlendMode,
}

#[derive(Debug, PartialEq)]
pub enum OrderError {
    ExceededLayerLimit(u32),
}

impl fmt::Display for OrderError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "exceeded layer limit ({})",
            match self {
                OrderError::ExceededLayerLimit(val) => val,
            }
        )
    }
}

const CARNELIAN_MAX_ORDER: usize = (1 << 18) - 1;

pub type Order = GenericOrder<CARNELIAN_MAX_ORDER>;

type GuaranteedOrderType = u16;
static_assertions::const_assert!((GuaranteedOrderType::MAX as u32) < (Order::MAX.as_u32()));

impl error::Error for OrderError {}

#[derive(Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct GenericOrder<const MAX: usize>(u32);

impl<const MAX: usize> GenericOrder<MAX> {
    pub const MAX: Self = Self(MAX as u32);

    pub const fn as_u32(&self) -> u32 {
        self.0
    }

    pub const fn new(order: u32) -> Result<Self, OrderError> {
        if order > Self::MAX.as_u32() {
            Err(OrderError::ExceededLayerLimit(Self::MAX.as_u32()))
        } else {
            Ok(Self(order))
        }
    }

    /// Create an order from a sixteen bit value that is guaranteed to
    /// below the maximum value for an order and thus doesn't need
    /// to be returned as a result and can be used in constant expressions.
    pub const fn from_u16(order: GuaranteedOrderType) -> Self {
        Self(order as u32)
    }
}

impl<const MAX: usize> TryFrom<u32> for GenericOrder<MAX> {
    type Error = OrderError;

    fn try_from(order: u32) -> Result<Self, OrderError> {
        Self::new(order)
    }
}

impl<const MAX: usize> TryFrom<usize> for GenericOrder<MAX> {
    type Error = OrderError;

    fn try_from(order: usize) -> Result<Self, OrderError> {
        u32::try_from(order)
            .map_err(|_| OrderError::ExceededLayerLimit(Self::MAX.as_u32()))
            .and_then(|x| Self::try_from(x))
    }
}

/// 2D layer containing a raster with a style.
#[derive(Clone, Debug)]
pub struct Layer<B: Backend> {
    /// Layer raster.
    pub raster: B::Raster,
    /// Layer clip.
    pub clip: Option<B::Raster>,
    /// Layer style.
    pub style: Style, // Will also contain txty when available.
}

/// A group of ordered layers.
pub trait Composition<B: Backend> {
    /// Creates a composition of ordered layers where the layers with lower index appear on top.
    fn new(background_color: Color) -> Self;
    /// Resets composition by removing all layers.
    fn clear(&mut self);
    /// Inserts layer into the composition.
    fn insert(&mut self, order: Order, layer: Layer<B>);
    /// Removes layer from the composition.
    fn remove(&mut self, order: Order);
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    use fuchsia_async as fasync;

    use crate::drawing::DisplayRotation;

    #[test]
    fn generic_compile_test() {
        fn _generic<B: Backend>(
            token: ClientEnd<BufferCollectionTokenMarker>,
            size: Size2D<u32>,
            display_rotation: DisplayRotation,
            view_context: &ViewAssistantContext,
        ) {
            let mut context = B::new_context(token, size, display_rotation);

            let mut path_builder = context.path_builder().unwrap();
            path_builder.move_to(point2(0.0, 0.0)).line_to(point2(1.0, 1.0));
            let path = path_builder.build();

            let mut raster_builder = context.raster_builder().unwrap();
            raster_builder.add(&path, None);
            let raster = raster_builder.build();

            let src_image = context.new_image(size2(100, 100));
            let dst_image = context.get_current_image(view_context);

            let mut composition: B::Composition = Composition::new(Color::new());
            composition.insert(
                Order::default(),
                Layer {
                    raster: raster.clone() + raster,
                    clip: None,
                    style: Style {
                        fill_rule: FillRule::NonZero,
                        fill: Fill::Solid(Color::white()),
                        blend_mode: BlendMode::Over,
                    },
                },
            );

            context.render(
                &mut composition,
                None,
                dst_image,
                &RenderExt {
                    pre_copy: Some(PreCopy {
                        image: src_image,
                        copy_region: CopyRegion {
                            src_offset: Point2D::zero(),
                            dst_offset: Point2D::zero(),
                            extent: size2(100, 100),
                        },
                    }),
                    ..Default::default()
                },
            );
        }
    }

    // Currently, render API tests are only run if a compatible Vulkan driver is found. This means
    // that these tests will be skipped on CI, but be able to run manually through the
    // `carnelian_tests` component.
    fn has_vk_instance() -> bool {
        false
    }

    pub(crate) fn run(f: impl Fn()) {
        if !has_vk_instance() {
            return;
        }

        let mut executor = fasync::LocalExecutor::new();
        let f = async { f() };
        executor.run_singlethreaded(f);
    }
}
