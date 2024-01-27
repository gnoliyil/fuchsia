// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Type-safe bindings for Zircon kernel
//! [syscalls](https://fuchsia.dev/fuchsia-src/reference/syscalls).

pub mod sys {
    pub use fuchsia_zircon_sys::*;
}

// Tuning constants for get_info_vec(). pub(crate) to support unit tests.
pub(crate) const INFO_VEC_SIZE_INITIAL: usize = 16;
const INFO_VEC_SIZE_PAD: usize = 2;

// Implements the HandleBased traits for a Handle newtype struct
macro_rules! impl_handle_based {
    ($type_name:path) => {
        impl AsHandleRef for $type_name {
            fn as_handle_ref(&self) -> HandleRef<'_> {
                self.0.as_handle_ref()
            }
        }

        impl From<Handle> for $type_name {
            fn from(handle: Handle) -> Self {
                $type_name(handle)
            }
        }

        impl From<$type_name> for Handle {
            fn from(x: $type_name) -> Handle {
                x.0
            }
        }

        impl HandleBased for $type_name {}
    };
}

/// Convenience macro for creating get/set property functions on an object.
///
/// This is for use when the underlying property type is a simple raw type.
/// It creates an empty 'tag' struct to implement the relevant PropertyQuery*
/// traits against. One, or both, of a getter and setter may be defined
/// depending upon what the property supports. Example usage is
/// unsafe_handle_propertyes!(ObjectType[get_foo_prop,set_foo_prop:FooPropTag,FOO,u32;]);
/// unsafe_handle_properties!(object: Foo,
///     props: [
///         {query_ty: FOO_BAR, tag: FooBarTag, prop_ty: usize, get:get_bar},
///         {query_ty: FOO_BAX, tag: FooBazTag, prop_ty: u32, set:set_baz},
///     ]
/// );
/// And will create
/// Foo::get_bar(&self) -> Result<usize, Status>
/// Foo::set_baz(&self, val: &u32) -> Result<(), Status>
/// Using Property::FOO as the underlying property.
///
///  # Safety
///
/// This macro will implement unsafe traits on your behalf and any combination
/// of query_ty and prop_ty must respect the Safety requirements detailed on the
/// PropertyQuery trait.
macro_rules! unsafe_handle_properties {
    (
        object: $object_ty:ty,
        props: [$( {
            query_ty: $query_ty:ident,
            tag: $query_tag:ident,
            prop_ty: $prop_ty:ty
            $(,get: $get:ident)*
            $(,set: $set:ident)*
            $(,)*
        }),*$(,)*]
    ) => {
        $(
            struct $query_tag {}
            unsafe impl PropertyQuery for $query_tag {
                const PROPERTY: Property = Property::$query_ty;
                type PropTy = $prop_ty;
            }

            $(
                impl $object_ty {
                    pub fn $get(&self) -> Result<$prop_ty, Status> {
                        object_get_property::<$query_tag>(self.as_handle_ref())
                    }
                }
            )*

            $(
                impl $object_ty {
                    pub fn $set(&self, val: &$prop_ty) -> Result<(), Status> {
                        object_set_property::<$query_tag>(self.as_handle_ref(), val)
                    }
                }
            )*
        )*
    }
}

// Creates associated constants of TypeName of the form
// `pub const NAME: TypeName = TypeName(path::to::value);`
// and provides a private `assoc_const_name` method and a `Debug` implementation
// for the type based on `$name`.
// If multiple names match, the first will be used in `name` and `Debug`.
#[macro_export]
macro_rules! assoc_values {
    ($typename:ident, [$($(#[$attr:meta])* $name:ident = $value:path;)*]) => {
        #[allow(non_upper_case_globals)]
        impl $typename {
            $(
                $(#[$attr])*
                pub const $name: $typename = $typename($value);
            )*

            fn assoc_const_name(&self) -> Option<&'static str> {
                match self.0 {
                    $(
                        $(#[$attr])*
                        $value => Some(stringify!($name)),
                    )*
                    _ => None,
                }
            }
        }

        impl ::std::fmt::Debug for $typename {
            fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {
                f.write_str(concat!(stringify!($typename), "("))?;
                match self.assoc_const_name() {
                    Some(name) => f.write_str(&name)?,
                    None => ::std::fmt::Debug::fmt(&self.0, f)?,
                }
                f.write_str(")")
            }
        }
    }
}

mod bti;
mod channel;
mod clock;
mod clock_update;
mod cprng;
mod debuglog;
mod event;
mod eventpair;
mod exception;
mod fifo;
mod guest;
mod handle;
mod info;
mod interrupt;
mod iommu;
mod job;
mod pager;
mod pmt;
mod port;
mod process;
mod profile;
mod property;
mod resource;
mod rights;
mod signals;
mod socket;
mod stream;
mod task;
mod thread;
mod time;
mod vcpu;
mod version;
mod vmar;
mod vmo;

pub use self::bti::*;
pub use self::channel::*;
pub use self::clock::*;
pub use self::clock_update::{ClockUpdate, ClockUpdateBuilder};
pub use self::cprng::*;
pub use self::debuglog::*;
pub use self::event::*;
pub use self::eventpair::*;
pub use self::exception::*;
pub use self::fifo::*;
pub use self::guest::*;
pub use self::handle::*;
pub use self::info::*;
pub use self::interrupt::*;
pub use self::iommu::*;
pub use self::job::*;
pub use self::pager::*;
pub use self::pmt::*;
pub use self::port::*;
pub use self::process::*;
pub use self::profile::*;
pub use self::property::*;
pub use self::resource::*;
pub use self::rights::*;
pub use self::signals::*;
pub use self::socket::*;
pub use self::stream::*;
pub use self::task::*;
pub use self::thread::*;
pub use self::time::*;
pub use self::vcpu::*;
pub use self::version::*;
pub use self::vmar::*;
pub use self::vmo::*;
pub use fuchsia_zircon_status::*;

/// Prelude containing common utility traits.
/// Designed for use like `use fuchsia_zircon::prelude::*;`
pub mod prelude {
    pub use crate::{AsHandleRef, DurationNum, HandleBased, Peered};
}

/// Convenience re-export of `Status::ok`.
pub fn ok(raw: sys::zx_status_t) -> Result<(), Status> {
    Status::ok(raw)
}

/// A packet sent through a port. This is a type-safe wrapper for
/// [zx_port_packet_t](https://fuchsia.dev/fuchsia-src/reference/syscalls/port_wait.md).
#[derive(PartialEq, Eq, Debug)]
pub struct Packet(sys::zx_port_packet_t);

/// A "wait item" containing a handle reference and information about what signals
/// to wait on, and, on return from `object_wait_many`, which are pending.
#[repr(C)]
#[derive(Debug)]
pub struct WaitItem<'a> {
    /// The handle to wait on.
    pub handle: HandleRef<'a>,
    /// A set of signals to wait for.
    pub waitfor: Signals,
    /// The set of signals pending, on return of `object_wait_many`.
    pub pending: Signals,
}

/// Wait on multiple handles.
/// The success return value is a bool indicating whether one or more of the
/// provided handle references was closed during the wait.
///
/// Wraps the
/// [zx_object_wait_many](https://fuchsia.dev/fuchsia-src/reference/syscalls/object_wait_many.md)
/// syscall.
pub fn object_wait_many(items: &mut [WaitItem<'_>], deadline: Time) -> Result<bool, Status> {
    let items_ptr = items.as_mut_ptr() as *mut sys::zx_wait_item_t;
    let status = unsafe { sys::zx_object_wait_many(items_ptr, items.len(), deadline.into_nanos()) };
    if status == sys::ZX_ERR_CANCELED {
        return Ok(true);
    }
    ok(status).map(|()| false)
}

/// Query information about a zircon object.
/// Returns `(num_returned, num_remaining)` on success.
pub fn object_get_info<Q: ObjectQuery>(
    handle: HandleRef<'_>,
    out: &mut [Q::InfoTy],
) -> Result<(usize, usize), Status> {
    let mut actual = 0;
    let mut avail = 0;
    let status = unsafe {
        sys::zx_object_get_info(
            handle.raw_handle(),
            *Q::TOPIC,
            out.as_mut_ptr() as *mut u8,
            std::mem::size_of_val(out),
            &mut actual as *mut usize,
            &mut avail as *mut usize,
        )
    };
    ok(status).map(|_| (actual, avail - actual))
}

/// Query multiple records of information about a zircon object.
/// Returns a vec of Q::InfoTy on success.
/// Intended for calls that return multiple small objects.
pub fn object_get_info_vec<Q: ObjectQuery>(
    handle: HandleRef<'_>,
) -> Result<Vec<Q::InfoTy>, Status> {
    // Start with a few slots
    let mut out = Vec::<Q::InfoTy>::with_capacity(INFO_VEC_SIZE_INITIAL);
    let mut actual = 0;
    let mut avail = 0;
    loop {
        let status = unsafe {
            let uninit = out.spare_capacity_mut();
            sys::zx_object_get_info(
                handle.raw_handle(),
                *Q::TOPIC,
                uninit.as_mut_ptr() as *mut u8,
                std::mem::size_of_val(uninit),
                &mut actual as *mut usize,
                &mut avail as *mut usize,
            )
        };
        match Status::ok(status) {
            Err(status) => return Err(status),
            Ok(()) if actual == avail => {
                unsafe { out.set_len(actual) };
                return Ok(out);
            }
            Ok(()) => {
                if avail < out.capacity() {
                    // This should only happen if there's a bug somewhere
                    return Err(Status::INTERNAL);
                }
                // The number of records may increase between retries; reserve space for that.
                let needed_space = avail * INFO_VEC_SIZE_PAD;
                out.reserve_exact(/* amount to grow */ needed_space - out.capacity());
            }
        }
    }
}

/// Get a property on a zircon object
pub fn object_get_property<P: PropertyQuery>(handle: HandleRef<'_>) -> Result<P::PropTy, Status> {
    // this is safe due to the contract on the P::PropTy type in the ObjectProperty trait.
    let mut out = ::std::mem::MaybeUninit::<P::PropTy>::uninit();
    let status = unsafe {
        sys::zx_object_get_property(
            handle.raw_handle(),
            *P::PROPERTY,
            out.as_mut_ptr() as *mut u8,
            std::mem::size_of::<P::PropTy>(),
        )
    };
    ok(status).map(|_| unsafe { out.assume_init() })
}

/// Set a property on a zircon object
pub fn object_set_property<P: PropertyQuery>(
    handle: HandleRef<'_>,
    val: &P::PropTy,
) -> Result<(), Status> {
    let status = unsafe {
        sys::zx_object_set_property(
            handle.raw_handle(),
            *P::PROPERTY,
            val as *const P::PropTy as *const u8,
            std::mem::size_of::<P::PropTy>(),
        )
    };
    ok(status)
}

/// Retrieve the system memory page size in bytes.
///
/// Wraps the
/// [zx_system_get_page_size](https://fuchsia.dev/fuchsia-src/reference/syscalls/system_get_page_size.md)
/// syscall.
pub fn system_get_page_size() -> u32 {
    unsafe { sys::zx_system_get_page_size() }
}

/// Get the amount of physical memory on the system, in bytes.
///
/// Wraps the
/// [zx_system_get_physmem](https://fuchsia.dev/fuchsia-src/reference/syscalls/system_get_physmem)
/// syscall.
pub fn system_get_physmem() -> u64 {
    unsafe { sys::zx_system_get_physmem() }
}

/// Get number of logical processors on the system.
///
/// Wraps the
/// [zx_system_get_num_cpus](https://fuchsia.dev/fuchsia-src/reference/syscalls/system_get_num_cpus)
/// syscall.
pub fn system_get_num_cpus() -> u32 {
    unsafe { sys::zx_system_get_num_cpus() }
}

#[cfg(test)]
mod tests {
    #[allow(unused_imports)]
    use super::prelude::*;
    use super::*;

    #[test]
    fn wait_and_signal() {
        let event = Event::create();
        let ten_ms = 10.millis();

        // Waiting on it without setting any signal should time out.
        assert_eq!(event.wait_handle(Signals::USER_0, Time::after(ten_ms)), Err(Status::TIMED_OUT));

        // If we set a signal, we should be able to wait for it.
        assert!(event.signal_handle(Signals::NONE, Signals::USER_0).is_ok());
        assert_eq!(
            event.wait_handle(Signals::USER_0, Time::after(ten_ms)).unwrap(),
            Signals::USER_0
        );

        // Should still work, signals aren't automatically cleared.
        assert_eq!(
            event.wait_handle(Signals::USER_0, Time::after(ten_ms)).unwrap(),
            Signals::USER_0
        );

        // Now clear it, and waiting should time out again.
        assert!(event.signal_handle(Signals::USER_0, Signals::NONE).is_ok());
        assert_eq!(event.wait_handle(Signals::USER_0, Time::after(ten_ms)), Err(Status::TIMED_OUT));
    }

    #[test]
    fn wait_many_and_signal() {
        let ten_ms = 10.millis();
        let e1 = Event::create();
        let e2 = Event::create();

        // Waiting on them now should time out.
        let mut items = vec![
            WaitItem {
                handle: e1.as_handle_ref(),
                waitfor: Signals::USER_0,
                pending: Signals::NONE,
            },
            WaitItem {
                handle: e2.as_handle_ref(),
                waitfor: Signals::USER_1,
                pending: Signals::NONE,
            },
        ];
        assert_eq!(object_wait_many(&mut items, Time::after(ten_ms)), Err(Status::TIMED_OUT));
        assert_eq!(items[0].pending, Signals::NONE);
        assert_eq!(items[1].pending, Signals::NONE);

        // Signal one object and it should return success.
        assert!(e1.signal_handle(Signals::NONE, Signals::USER_0).is_ok());
        assert!(object_wait_many(&mut items, Time::after(ten_ms)).is_ok());
        assert_eq!(items[0].pending, Signals::USER_0);
        assert_eq!(items[1].pending, Signals::NONE);

        // Signal the other and it should return both.
        assert!(e2.signal_handle(Signals::NONE, Signals::USER_1).is_ok());
        assert!(object_wait_many(&mut items, Time::after(ten_ms)).is_ok());
        assert_eq!(items[0].pending, Signals::USER_0);
        assert_eq!(items[1].pending, Signals::USER_1);

        // Clear signals on both; now it should time out again.
        assert!(e1.signal_handle(Signals::USER_0, Signals::NONE).is_ok());
        assert!(e2.signal_handle(Signals::USER_1, Signals::NONE).is_ok());
        assert_eq!(object_wait_many(&mut items, Time::after(ten_ms)), Err(Status::TIMED_OUT));
        assert_eq!(items[0].pending, Signals::NONE);
        assert_eq!(items[1].pending, Signals::NONE);
    }
}

pub fn usize_into_u32(n: usize) -> Result<u32, ()> {
    if n > ::std::u32::MAX as usize || n < ::std::u32::MIN as usize {
        return Err(());
    }
    Ok(n as u32)
}

pub fn size_to_u32_sat(n: usize) -> u32 {
    if n > ::std::u32::MAX as usize {
        return ::std::u32::MAX;
    }
    if n < ::std::u32::MIN as usize {
        return ::std::u32::MIN;
    }
    n as u32
}
