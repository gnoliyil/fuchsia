// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::light::light_hardware_configuration::DisableConditions;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::convert::TryFrom;

#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
pub struct LightInfo {
    pub light_groups: HashMap<String, LightGroup>,
}

/// Internal representation of a light group.
#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
pub struct LightGroup {
    pub name: String,
    pub enabled: bool,
    pub light_type: LightType,
    pub lights: Vec<LightState>,

    /// Each light in the underlying fuchsia.hardware.light API has a unique, fixed index. We need
    /// to remember the index of the lights in this light group in order to write values back.
    pub hardware_index: Vec<u32>,

    /// A list of conditions under which the "enabled" field of the light group should be false,
    /// which signals to clients the light's state is being overridden by external conditions, such
    /// as an LED dedicated to showing that a device's mic is muted that is off when the mic is not
    /// muted.
    ///
    /// Lights that are disabled can still have their value set, but the changes may not be
    /// noticeable to the user until the condition disabling/overriding ends.
    pub disable_conditions: Vec<DisableConditions>,
}

impl From<LightGroup> for fidl_fuchsia_settings::LightGroup {
    fn from(src: LightGroup) -> Self {
        fidl_fuchsia_settings::LightGroup {
            name: Some(src.name),
            enabled: Some(src.enabled),
            type_: Some(src.light_type.into()),
            lights: Some(src.lights.into_iter().map(LightState::into).collect()),
            ..fidl_fuchsia_settings::LightGroup::EMPTY
        }
    }
}

impl From<fidl_fuchsia_settings::LightGroup> for LightGroup {
    fn from(src: fidl_fuchsia_settings::LightGroup) -> Self {
        LightGroup {
            name: src.name.unwrap(),
            enabled: src.enabled.unwrap(),
            light_type: src.type_.unwrap().into(),
            lights: src.lights.unwrap().into_iter().map(LightState::from).collect(),
            // These are not used in storage, but will be filled out by the controller.
            hardware_index: vec![],
            disable_conditions: vec![],
        }
    }
}

#[derive(PartialEq, Eq, Debug, Copy, Clone, Serialize, Deserialize)]
pub enum LightType {
    Brightness,
    Rgb,
    Simple,
}

impl From<fidl_fuchsia_settings::LightType> for LightType {
    fn from(src: fidl_fuchsia_settings::LightType) -> Self {
        match src {
            fidl_fuchsia_settings::LightType::Brightness => LightType::Brightness,
            fidl_fuchsia_settings::LightType::Rgb => LightType::Rgb,
            fidl_fuchsia_settings::LightType::Simple => LightType::Simple,
        }
    }
}

impl From<LightType> for fidl_fuchsia_settings::LightType {
    fn from(src: LightType) -> Self {
        match src {
            LightType::Brightness => fidl_fuchsia_settings::LightType::Brightness,
            LightType::Rgb => fidl_fuchsia_settings::LightType::Rgb,
            LightType::Simple => fidl_fuchsia_settings::LightType::Simple,
        }
    }
}

/// Converts between a Capability and a LightType for convenience for tests.
impl From<fidl_fuchsia_hardware_light::Capability> for LightType {
    fn from(src: fidl_fuchsia_hardware_light::Capability) -> Self {
        match src {
            fidl_fuchsia_hardware_light::Capability::Brightness => LightType::Brightness,
            fidl_fuchsia_hardware_light::Capability::Rgb => LightType::Rgb,
            fidl_fuchsia_hardware_light::Capability::Simple => LightType::Simple,
        }
    }
}

/// Converts between a LightType and a Capability for convenience for tests.
impl From<LightType> for fidl_fuchsia_hardware_light::Capability {
    fn from(src: LightType) -> Self {
        match src {
            LightType::Brightness => fidl_fuchsia_hardware_light::Capability::Brightness,
            LightType::Rgb => fidl_fuchsia_hardware_light::Capability::Rgb,
            LightType::Simple => fidl_fuchsia_hardware_light::Capability::Simple,
        }
    }
}

#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
pub struct LightState {
    pub value: Option<LightValue>,
}

impl LightState {
    pub(crate) fn is_finite(&self) -> bool {
        (self.value).as_ref().map_or(true, |val| val.is_finite())
    }
}

impl From<fidl_fuchsia_settings::LightState> for LightState {
    fn from(src: fidl_fuchsia_settings::LightState) -> Self {
        LightState { value: src.value.map(LightValue::from) }
    }
}

impl From<LightState> for fidl_fuchsia_settings::LightState {
    fn from(src: LightState) -> Self {
        fidl_fuchsia_settings::LightState {
            value: src.value.map(fidl_fuchsia_settings::LightValue::from),
            ..fidl_fuchsia_settings::LightState::EMPTY
        }
    }
}

#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
pub enum LightValue {
    Brightness(f64),
    Rgb(ColorRgb),
    Simple(bool),
}

impl LightValue {
    pub(crate) fn is_finite(&self) -> bool {
        match self {
            LightValue::Brightness(brightness) => brightness.is_finite(),
            LightValue::Rgb(color_rgb) => color_rgb.is_finite(),
            LightValue::Simple(_) => true,
        }
    }
}

impl From<fidl_fuchsia_settings::LightValue> for LightValue {
    fn from(src: fidl_fuchsia_settings::LightValue) -> Self {
        match src {
            fidl_fuchsia_settings::LightValue::On(on) => LightValue::Simple(on),
            fidl_fuchsia_settings::LightValue::Brightness(brightness) => {
                LightValue::Brightness(brightness)
            }
            fidl_fuchsia_settings::LightValue::Color(color) => LightValue::Rgb(color.into()),
        }
    }
}

impl From<fidl_fuchsia_hardware_light::Rgb> for LightValue {
    fn from(src: fidl_fuchsia_hardware_light::Rgb) -> Self {
        LightValue::Rgb(ColorRgb {
            red: src.red as f32,
            green: src.green as f32,
            blue: src.blue as f32,
        })
    }
}

impl From<LightValue> for fidl_fuchsia_settings::LightValue {
    fn from(src: LightValue) -> Self {
        match src {
            LightValue::Simple(on) => fidl_fuchsia_settings::LightValue::On(on),
            LightValue::Brightness(brightness) => {
                fidl_fuchsia_settings::LightValue::Brightness(brightness)
            }
            LightValue::Rgb(color) => fidl_fuchsia_settings::LightValue::Color(color.into()),
        }
    }
}

#[derive(PartialEq, Debug, Clone, Serialize, Deserialize)]
pub struct ColorRgb {
    pub red: f32,
    pub green: f32,
    pub blue: f32,
}

impl ColorRgb {
    pub(crate) fn is_finite(&self) -> bool {
        self.red.is_finite() && self.green.is_finite() && self.blue.is_finite()
    }
}

impl From<fidl_fuchsia_ui_types::ColorRgb> for ColorRgb {
    fn from(src: fidl_fuchsia_ui_types::ColorRgb) -> Self {
        ColorRgb { red: src.red, green: src.green, blue: src.blue }
    }
}

impl From<ColorRgb> for fidl_fuchsia_ui_types::ColorRgb {
    fn from(src: ColorRgb) -> Self {
        fidl_fuchsia_ui_types::ColorRgb { red: src.red, green: src.green, blue: src.blue }
    }
}

/// Converts between internal RGB representation and underlying fuchsia.hardware.light
/// representation.
impl TryFrom<ColorRgb> for fidl_fuchsia_hardware_light::Rgb {
    type Error = &'static str;
    fn try_from(src: ColorRgb) -> Result<Self, Self::Error> {
        if src.red > 1.0
            || src.green > 1.0
            || src.blue > 1.0
            || src.red < 0.0
            || src.green < 0.0
            || src.blue < 0.0
        {
            return Err("values must be between 0.0 and 1.0 inclusive");
        }

        Ok(fidl_fuchsia_hardware_light::Rgb {
            red: src.red as f64,
            green: src.green as f64,
            blue: src.blue as f64,
        })
    }
}

#[cfg(test)]
mod tests {
    use crate::light::types::ColorRgb;
    use std::convert::TryFrom;

    #[fuchsia::test]
    fn test_try_from_rgb() {
        assert!(fidl_fuchsia_hardware_light::Rgb::try_from(ColorRgb {
            red: -0.0,
            green: 0.1,
            blue: 1.0
        })
        .is_ok());

        assert!(fidl_fuchsia_hardware_light::Rgb::try_from(ColorRgb {
            red: 0.0 - f32::EPSILON,
            green: 0.1,
            blue: 0.2
        })
        .is_err());

        assert!(fidl_fuchsia_hardware_light::Rgb::try_from(ColorRgb {
            red: 0.3,
            green: 1.0 + f32::EPSILON,
            blue: 0.2
        })
        .is_err());

        assert_eq!(
            fidl_fuchsia_hardware_light::Rgb::try_from(ColorRgb {
                red: 0.0,
                green: 1.0,
                blue: 0.5
            }),
            Ok(fidl_fuchsia_hardware_light::Rgb { red: 0.0, green: 1.0, blue: 0.5 })
        );
    }
}
