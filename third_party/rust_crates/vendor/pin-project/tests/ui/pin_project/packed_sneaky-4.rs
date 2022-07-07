// https://github.com/taiki-e/pin-project/issues/342

#![allow(unaligned_references)]

use auxiliary_macro::hidden_repr2;
use pin_project::pin_project;

#[pin_project] //~ ERROR reference to packed field is unaligned
#[hidden_repr2]
struct A {
    #[pin]
    f: u32,
}

fn main() {}
