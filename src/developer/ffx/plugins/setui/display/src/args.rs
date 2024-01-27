// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;
use fidl_fuchsia_settings::{DisplaySettings, LowLightMode, Theme};

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq, Clone)]
#[argh(subcommand, name = "display")]
/// get or set display settings
pub struct Display {
    #[argh(subcommand)]
    pub subcommand: SubCommandEnum,
}

#[derive(FromArgs, PartialEq, Debug, Clone)]
#[argh(subcommand)]
pub enum SubCommandEnum {
    /// sets display settings
    Set(SetArgs),
    /// gets the current display settings
    Get(GetArgs),
    /// gets the current display settings and watches for changes
    Watch(WatchArgs),
}

#[derive(FromArgs, Debug, PartialEq, Clone)]
#[argh(subcommand, name = "set", description = "Sets display settings.")]
pub struct SetArgs {
    /// the brightness value specified as a float in the range [0, 1]
    #[argh(option, short = 'b')]
    pub brightness: Option<f32>,

    /// the brightness values used to control auto brightness as a float in the range [0, 1]
    #[argh(option, short = 'o')]
    pub auto_brightness_level: Option<f32>,

    /// when set to 'true', enables auto brightness
    #[argh(option, short = 'a')]
    pub auto_brightness: Option<bool>,

    /// which low light mode setting to enable. Valid options are enable, disable, and
    /// disable_immediately
    #[argh(option, short = 'm', from_str_fn(str_to_low_light_mode))]
    pub low_light_mode: Option<LowLightMode>,

    /// which theme to set for the device. Valid options are default, dark, light, darkauto, and
    /// lightauto
    #[argh(option, short = 't', from_str_fn(str_to_theme))]
    pub theme: Option<Theme>,

    /// when set to 'true' the screen is enabled
    #[argh(option, short = 's')]
    pub screen_enabled: Option<bool>,
}

#[derive(FromArgs, Debug, PartialEq, Clone)]
#[argh(subcommand, name = "get", description = "Get the current display settings.")]
pub struct GetArgs {
    /// choose which display settings field value to return, valid options are auto and brightness
    #[argh(option, short = 'f', from_str_fn(str_to_field))]
    pub field: Option<Field>,
}

#[derive(Debug, PartialEq, Clone)]
pub enum Field {
    Brightness,
    Auto,
}

fn str_to_field(src: &str) -> Result<Field, String> {
    match src {
        "brightness" | "b" => Ok(Field::Brightness),
        "auto" | "a" => Ok(Field::Auto),
        _ => Err(String::from("Couldn't parse display settings field")),
    }
}

#[derive(FromArgs, Debug, PartialEq, Clone)]
#[argh(
    subcommand,
    name = "watch",
    description = "Get the current display settings and watch for changes."
)]
pub struct WatchArgs {}

fn str_to_low_light_mode(src: &str) -> Result<fidl_fuchsia_settings::LowLightMode, String> {
    match src {
        "enable" | "e" => Ok(fidl_fuchsia_settings::LowLightMode::Enable),
        "disable" | "d" => Ok(fidl_fuchsia_settings::LowLightMode::Disable),
        "disable_immediately" | "i" => Ok(fidl_fuchsia_settings::LowLightMode::DisableImmediately),
        _ => Err(String::from("Couldn't parse low light mode")),
    }
}

fn str_to_theme(src: &str) -> Result<fidl_fuchsia_settings::Theme, String> {
    match src {
        "default" => Ok(Theme {
            theme_type: Some(fidl_fuchsia_settings::ThemeType::Default),
            ..Default::default()
        }),
        "dark" => Ok(Theme {
            theme_type: Some(fidl_fuchsia_settings::ThemeType::Dark),
            ..Default::default()
        }),
        "light" => Ok(Theme {
            theme_type: Some(fidl_fuchsia_settings::ThemeType::Light),
            ..Default::default()
        }),
        "darkauto" => Ok(Theme {
            theme_type: Some(fidl_fuchsia_settings::ThemeType::Dark),
            theme_mode: Some(fidl_fuchsia_settings::ThemeMode::AUTO),
            ..Default::default()
        }),
        "lightauto" => Ok(Theme {
            theme_type: Some(fidl_fuchsia_settings::ThemeType::Light),
            theme_mode: Some(fidl_fuchsia_settings::ThemeMode::AUTO),
            ..Default::default()
        }),
        _ => Err(String::from("Couldn't parse theme.")),
    }
}

impl From<SetArgs> for DisplaySettings {
    fn from(src: SetArgs) -> DisplaySettings {
        DisplaySettings {
            auto_brightness: src.auto_brightness,
            brightness_value: src.brightness,
            low_light_mode: src.low_light_mode,
            screen_enabled: src.screen_enabled,
            theme: src.theme,
            adjusted_auto_brightness: src.auto_brightness_level,
            ..Default::default()
        }
    }
}

impl From<DisplaySettings> for SetArgs {
    fn from(src: DisplaySettings) -> SetArgs {
        SetArgs {
            auto_brightness: src.auto_brightness,
            brightness: src.brightness_value,
            low_light_mode: src.low_light_mode,
            screen_enabled: src.screen_enabled,
            theme: src.theme,
            auto_brightness_level: src.adjusted_auto_brightness,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const CMD_NAME: &'static [&'static str] = &["display"];

    #[test]
    fn test_display_set_cmd() {
        // Test input arguments are generated to according struct.
        let turned_on_auto = "true";
        let low_light_mode = "disable";
        let theme = "darkauto";
        let args = &["set", "-a", turned_on_auto, "-m", low_light_mode, "-t", theme];
        assert_eq!(
            Display::from_args(CMD_NAME, args),
            Ok(Display {
                subcommand: SubCommandEnum::Set(SetArgs {
                    brightness: None,
                    auto_brightness_level: None,
                    auto_brightness: Some(true),
                    low_light_mode: Some(str_to_low_light_mode(low_light_mode).unwrap()),
                    theme: Some(str_to_theme(theme).unwrap()),
                    screen_enabled: None,
                })
            })
        )
    }

    #[test]
    fn test_display_get_cmd() {
        // Test input arguments are generated to according struct.
        let args = &["get"];
        assert_eq!(
            Display::from_args(CMD_NAME, args),
            Ok(Display { subcommand: SubCommandEnum::Get(GetArgs { field: None }) })
        )
    }

    #[test]
    fn test_display_watch_cmd() {
        // Test input arguments are generated to according struct.
        let args = &["watch"];
        assert_eq!(
            Display::from_args(CMD_NAME, args),
            Ok(Display { subcommand: SubCommandEnum::Watch(WatchArgs {}) })
        )
    }
}
