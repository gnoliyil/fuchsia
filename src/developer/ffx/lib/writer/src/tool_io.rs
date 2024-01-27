// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::fmt::Display;
use std::io::Write;

use crate::Result;

/// ToolIO defines the necessary functions to perform output from a tool,
/// potentially including type-safe machine output if required.
///
/// There are two provided implementations, [`crate::MachineWriter`] and
/// [`crate::SimpleWriter`], which provide either type-safe or string-only
/// output respectively.
pub trait ToolIO: Write + Sized {
    /// The type of object that is expected for the [`Self::item`] call (or
    /// any machine output writing functions that may be added by an
    /// implementation)
    type OutputItem;

    /// Whether this can theoretically support machine output given the right configuration.
    fn is_machine_supported() -> bool;

    /// Returns true if the receiver was configured to output for machines.
    fn is_machine(&self) -> bool;

    /// Returns an error stream that errors can be written to.
    fn stderr(&mut self) -> &'_ mut Box<dyn Write>;

    /// Writes the value to standard output without a newline.
    ///
    /// This is a no-op if `is_machine` returns true.
    fn print(&mut self, value: impl std::fmt::Display) -> Result<()> {
        if !self.is_machine() {
            write!(self, "{value}")?;
        }
        Ok(())
    }

    /// Writes the value to standard output with a newline.
    ///
    /// This is a no-op if `is_machine` returns true.
    fn line(&mut self, value: impl std::fmt::Display) -> Result<()> {
        if !self.is_machine() {
            writeln!(self, "{value}")?;
        }
        Ok(())
    }

    /// Displays the item in whatever formatted style is most appropriate based
    /// on is_machine and the underlying implementation
    fn item(&mut self, value: &Self::OutputItem) -> Result<()>
    where
        Self::OutputItem: Display;
}
