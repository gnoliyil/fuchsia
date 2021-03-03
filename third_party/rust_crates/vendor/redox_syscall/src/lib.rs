#![feature(asm)]
#![feature(llvm_asm)]
#![feature(const_fn)] // see https://github.com/rust-lang/rfcs/pull/2632
#![cfg_attr(not(test), no_std)]

#[cfg(test)]
extern crate core;

pub use self::arch::*;
pub use self::call::*;
pub use self::data::*;
pub use self::error::*;
pub use self::flag::*;
pub use self::io::*;
pub use self::number::*;
pub use self::scheme::*;

#[cfg(all(any(target_os = "none", target_os = "redox"), target_arch = "arm"))]
#[path="arch/arm.rs"]
mod arch;

#[cfg(all(any(target_os = "none", target_os = "redox"), target_arch = "aarch64"))]
#[path="arch/aarch64.rs"]
mod arch;

#[cfg(all(any(target_os = "none", target_os = "redox"), target_arch = "x86"))]
#[path="arch/x86.rs"]
mod arch;

#[cfg(all(any(target_os = "none", target_os = "redox"), target_arch = "x86_64"))]
#[path="arch/x86_64.rs"]
mod arch;

#[cfg(not(any(target_os = "none", target_os = "redox")))]
#[path="arch/nonredox.rs"]
mod arch;

/// Function definitions
pub mod call;

/// Complex structures that are used for some system calls
pub mod data;

/// All errors that can be generated by a system call
pub mod error;

/// Flags used as an argument to many system calls
pub mod flag;

/// Functions for low level hardware control
pub mod io;

/// Call numbers used by each system call
pub mod number;

/// A trait useful for scheme handlers
pub mod scheme;

#[cfg(test)]
mod tests;
