use pin_project::pin_project;
use std::marker::PhantomPinned;

#[pin_project]
struct S<T> {
    #[pin]
    f: T,
}

struct __S {}

impl Unpin for __S {}

fn is_unpin<T: Unpin>() {}

fn main() {
    is_unpin::<S<PhantomPinned>>(); //~ ERROR E0277
}
