// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Result};
use fidl_fuchsia_input;
use fidl_fuchsia_ui_input3;

/// A codepoint returned by [hid_usage_to_code_point] for HID usages that do
/// not have an associated code point, e.g. Alt.
pub(crate) const EMPTY_CODEPOINT: u32 = 0;

/// Standard [qwerty] keymap.
///
/// The value of this array at index `u`, where `u` is the usage, can be:
///
///  * `None` if the key maps to no `char` (Esc key)
///  * `Some((c, None))` if the key maps to `c`, but does not map to any `char` when shift is pressed
///  * `Some((c, Some(cs)))` if the key maps to `c` when shift is not pressed and to `cs` when it is
///    pressed
///
/// [qwerty]: https://en.wikipedia.org/wiki/Keyboard_layout#QWERTY-based_Latin-script_keyboard_layouts
pub const QWERTY_MAP: &[Option<(char, Option<char>)>] = &[
    // 0x00
    None,
    None,
    None,
    None,
    // HID_USAGE_KEY_A
    Some(('a', Some('A'))),
    Some(('b', Some('B'))),
    Some(('c', Some('C'))),
    Some(('d', Some('D'))),
    // 0x08
    Some(('e', Some('E'))),
    Some(('f', Some('F'))),
    Some(('g', Some('G'))),
    Some(('h', Some('H'))),
    // 0x0c
    Some(('i', Some('I'))),
    Some(('j', Some('J'))),
    Some(('k', Some('K'))),
    Some(('l', Some('L'))),
    // 0x10
    Some(('m', Some('M'))),
    Some(('n', Some('N'))),
    Some(('o', Some('O'))),
    Some(('p', Some('P'))),
    // 0x14
    Some(('q', Some('Q'))),
    Some(('r', Some('R'))),
    Some(('s', Some('S'))),
    Some(('t', Some('T'))),
    // 0x18
    Some(('u', Some('U'))),
    Some(('v', Some('V'))),
    Some(('w', Some('W'))),
    Some(('x', Some('X'))),
    // 0x1c
    Some(('y', Some('Y'))),
    Some(('z', Some('Z'))),
    Some(('1', Some('!'))),
    Some(('2', Some('@'))),
    // 0x20
    Some(('3', Some('#'))),
    Some(('4', Some('$'))),
    Some(('5', Some('%'))),
    Some(('6', Some('^'))),
    // 0x24
    Some(('7', Some('&'))),
    Some(('8', Some('*'))),
    Some(('9', Some('('))),
    Some(('0', Some(')'))),
    // 0x28
    None,
    None,
    None,
    None,
    // 0x2c
    Some((' ', Some(' '))),
    Some(('-', Some('_'))),
    Some(('=', Some('+'))),
    Some(('[', Some('{'))),
    // 0x30
    Some((']', Some('}'))),
    Some(('\\', Some('|'))),
    None,
    Some((';', Some(':'))),
    // 0x34
    Some(('\'', Some('"'))),
    Some(('`', Some('~'))),
    Some((',', Some('<'))),
    Some(('.', Some('>'))),
    // 0x38
    Some(('/', Some('?'))),
    None,
    None,
    None,
    // 0x3c
    None,
    None,
    None,
    None,
    // 0x40
    None,
    None,
    None,
    None,
    // 0x44
    None,
    None,
    None,
    None,
    // 0x48
    None,
    None,
    None,
    None,
    // 0x4c
    None,
    None,
    None,
    None,
    // 0x50
    None,
    None,
    None,
    None,
    // 0x54
    Some(('/', None)),
    Some(('*', None)),
    Some(('-', None)),
    Some(('+', None)),
    // 0x58
    None,
    Some(('1', None)),
    Some(('2', None)),
    Some(('3', None)),
    // 0x5c
    Some(('4', None)),
    Some(('5', None)),
    Some(('6', None)),
    Some(('7', None)),
    // 0x60
    Some(('8', None)),
    Some(('9', None)),
    Some(('0', None)),
    Some(('.', None)),
];

/// Tracks the current state of "significant" modifier keys.
///
/// Currently, a modifier key is "significant" if it affects the mapping of a
/// Fuchsia key to a key meaning.
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ModifierState {
    /// Whether the Caps Lock level modifier is active.  Caps Lock level modifier
    /// may be active even if the key itself is not actuated.
    pub caps_lock: bool,
    /// Whether the left shift modifier key is active.  Shift keys are normally
    /// active only while they are actuated (held pressed).
    pub left_shift: bool,
    /// Same as `left_shift`, but for the right shift key.
    pub right_shift: bool,
}

impl ModifierState {
    /// Update the modifier tracker state with this event.
    /// An error is returned in the case the input is completely unexpectedly broken.
    pub fn update(
        &mut self,
        event: fidl_fuchsia_ui_input3::KeyEventType,
        key: fidl_fuchsia_input::Key,
    ) {
        match event {
            fidl_fuchsia_ui_input3::KeyEventType::Pressed => match key {
                fidl_fuchsia_input::Key::LeftShift => self.left_shift = true,
                fidl_fuchsia_input::Key::RightShift => self.right_shift = true,
                fidl_fuchsia_input::Key::CapsLock => self.caps_lock = !self.caps_lock,
                _ => {}
            },
            fidl_fuchsia_ui_input3::KeyEventType::Released => match key {
                fidl_fuchsia_input::Key::LeftShift => self.left_shift = false,
                fidl_fuchsia_input::Key::RightShift => self.right_shift = false,
                _ => {}
            },
            _ => {
                panic!(
                    "ModifierState::update: unexpected event: {:?} - this is a programmer error",
                    event
                );
            }
        }
    }

    // Returns true if the "shift" level modifier is active.
    pub fn is_shift_active(&self) -> bool {
        self.caps_lock | self.left_shift | self.right_shift
    }
}

/// Converts a HID usage for a key to a Unicode code point where such a code point exists, based on
/// a US QWERTY keyboard layout.  Returns EMPTY_CODEPOINT if a code point does not exist (e.g. Alt),
/// and an error in case the mapping somehow fails.
pub fn hid_usage_to_code_point(hid_usage: u32, modifier_state: &ModifierState) -> Result<u32> {
    if (hid_usage as usize) < QWERTY_MAP.len() {
        if let Some(map_entry) = QWERTY_MAP[hid_usage as usize] {
            if modifier_state.is_shift_active() {
                map_entry
                    .1
                    .and_then(|shifted_char| Some(shifted_char as u32))
                    .ok_or(format_err!("Invalid USB HID code: {:?}", hid_usage))
            } else {
                Ok(map_entry.0 as u32)
            }
        } else {
            Ok(EMPTY_CODEPOINT) // No code point provided by a keymap, e.g. Enter.
        }
    } else {
        Ok(EMPTY_CODEPOINT) // No code point available, e.g. Shift, Alt, etc.
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const HID_USAGE_KEY_A: u32 = 0x04;

    #[test]
    fn spotcheck_keymap() -> Result<()> {
        assert_eq!(
            'a' as u32,
            hid_usage_to_code_point(HID_USAGE_KEY_A, &ModifierState { ..Default::default() })?
        );
        assert_eq!(
            'A' as u32,
            hid_usage_to_code_point(
                HID_USAGE_KEY_A,
                &ModifierState { caps_lock: true, ..Default::default() }
            )?
        );
        assert_eq!(
            'A' as u32,
            hid_usage_to_code_point(
                HID_USAGE_KEY_A,
                &ModifierState { right_shift: true, ..Default::default() }
            )?
        );
        assert_eq!(
            'A' as u32,
            hid_usage_to_code_point(
                HID_USAGE_KEY_A,
                &ModifierState { left_shift: true, ..Default::default() }
            )?
        );
        Ok(())
    }

    #[test]
    fn test_modifier_tracker() {
        let mut modifier_state: ModifierState = Default::default();
        assert!(!modifier_state.is_shift_active());

        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Pressed,
            fidl_fuchsia_input::Key::LeftShift,
        );
        assert!(modifier_state.is_shift_active());
        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Released,
            fidl_fuchsia_input::Key::LeftShift,
        );
        assert!(!modifier_state.is_shift_active());

        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Pressed,
            fidl_fuchsia_input::Key::RightShift,
        );
        assert!(modifier_state.is_shift_active());
        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Released,
            fidl_fuchsia_input::Key::RightShift,
        );
        assert!(!modifier_state.is_shift_active());

        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Pressed,
            fidl_fuchsia_input::Key::CapsLock,
        );
        assert!(modifier_state.is_shift_active());
        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Released,
            fidl_fuchsia_input::Key::CapsLock,
        );
        assert!(modifier_state.is_shift_active());
        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Pressed,
            fidl_fuchsia_input::Key::CapsLock,
        );
        assert!(!modifier_state.is_shift_active());
        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Released,
            fidl_fuchsia_input::Key::CapsLock,
        );
        assert!(!modifier_state.is_shift_active());
    }

    // CapsLock  ________/""""""""""\___________________
    // LeftShift ____________/"""""""""""\______________
    #[test]
    fn test_interleaved_caps_lock_and_shift() {
        let mut modifier_state: ModifierState = Default::default();
        assert!(!modifier_state.is_shift_active());

        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Pressed,
            fidl_fuchsia_input::Key::CapsLock,
        );
        assert!(modifier_state.is_shift_active());

        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Pressed,
            fidl_fuchsia_input::Key::LeftShift,
        );
        assert!(modifier_state.is_shift_active());

        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Released,
            fidl_fuchsia_input::Key::CapsLock,
        );
        assert!(modifier_state.is_shift_active());

        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Released,
            fidl_fuchsia_input::Key::LeftShift,
        );
        // Caps Lock is still active...
        assert!(modifier_state.is_shift_active());

        // Press and release Caps Lock again.
        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Pressed,
            fidl_fuchsia_input::Key::CapsLock,
        );
        assert!(!modifier_state.is_shift_active());

        modifier_state.update(
            fidl_fuchsia_ui_input3::KeyEventType::Released,
            fidl_fuchsia_input::Key::CapsLock,
        );
        assert!(!modifier_state.is_shift_active());
    }
}
