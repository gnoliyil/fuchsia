// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_async as fasync;
use fuchsia_zircon_status as zx_status;
use fuchsia_zircon_types::{
    zx_handle_t, zx_packet_signal_t, zx_signals_t, zx_status_t, zx_time_t, ZX_ERR_NOT_SUPPORTED,
    ZX_OK,
};
use futures::channel::oneshot;
use parking_lot::Mutex;
use std::time::Duration;

struct EPtr(*mut Executor);
unsafe impl Send for EPtr {}
unsafe impl Sync for EPtr {}

impl EPtr {
    unsafe fn as_ref(&self) -> &Executor {
        self.0.as_ref().unwrap()
    }

    unsafe fn as_mut(&mut self) -> &mut Executor {
        self.0.as_mut().unwrap()
    }
}

#[cfg(target_os = "fuchsia")]
mod fuchsia_details {

    use super::*;
    use fuchsia_zircon as zx;

    // The callback holder... gets registered with the executor and receives a packet when
    // trigger signals are observed. Needs to hold its own registration as dropping that would cause
    // deregistration.
    pub(crate) struct WaitEnder {
        wait: *mut async_wait_t,
        dispatcher: *mut std::ffi::c_void,
        registration: Mutex<Registration>,
    }
    // Registration state machine:
    // Unregistered -+-> Registered -+-> Finished
    //               |               |
    //               +---------------+
    // i.e. we start at Unregistered, move to Finished, and for a time we may be Registered, or not.
    // The or not case happens when we get the receive_packet callback *before* we record the registration on
    // the WaitEnder instance.
    enum Registration {
        // Wait registration not yet recorded.
        Unregistered,
        // Wait registered. We expect to receive_packet sometime in the future.
        Registered(fasync::ReceiverRegistration<WaitEnder>),
        // We have gotten a receive_packet callback.
        Finished,
    }
    unsafe impl Send for WaitEnder {}
    unsafe impl Sync for WaitEnder {}
    impl fasync::PacketReceiver for WaitEnder {
        fn receive_packet(&self, packet: zx::Packet) {
            // Record that the receive_packet call has been received... this finishes the
            // registration state machine.
            // Lock only needs to be held as long as it takes to note this fact.
            *self.registration.lock() = Registration::Finished;
            if let zx::PacketContents::SignalOne(sig) = packet.contents() {
                unsafe {
                    ((*self.wait).handler)(
                        self.dispatcher,
                        self.wait,
                        packet.status(),
                        sig.raw_packet(),
                    )
                }
            } else {
                panic!("Expected a signal packet");
            }
        }
    }
    impl WaitEnder {
        pub(crate) fn new(dispatcher: *mut std::ffi::c_void, wait: *mut async_wait_t) -> WaitEnder {
            WaitEnder { wait, dispatcher, registration: Mutex::new(Registration::Unregistered) }
        }

        pub(crate) fn record_registration(
            &self,
            registration: fasync::ReceiverRegistration<WaitEnder>,
        ) {
            let mut lock = self.registration.lock();
            if let Registration::Finished =
                std::mem::replace(&mut *lock, Registration::Registered(registration))
            {
                // Need to be able to cope with the callback coming before we reach this code and finish
                // the cycle holding our registration.
                *lock = Registration::Finished;
            }
        }
    }
}

pub struct Executor {
    executor: Mutex<fasync::LocalExecutor>,
    quit_tx: Mutex<Option<oneshot::Sender<()>>>,
    start: fasync::Time,
    cb_executor: *mut std::ffi::c_void,
}

impl Executor {
    fn new(cb_executor: *mut std::ffi::c_void) -> Box<Executor> {
        Box::new(Executor {
            executor: Mutex::new(fasync::LocalExecutor::new()),
            quit_tx: Mutex::new(None),
            start: fasync::Time::now(),
            cb_executor,
        })
    }

    fn run_singlethreaded(&self) {
        let (tx, rx) = oneshot::channel();

        // We make sure the quit message channel is `None` before replacing with
        // the new channel. This ensures that even if it panics, the executor
        // can still be successfully quit afterwards.
        let mut quit_tx = self.quit_tx.lock();
        if quit_tx.is_none() {
            *quit_tx = Some(tx);
            drop(quit_tx);
        } else {
            // `parking_lot::Mutex` doesn't poison on panic, but dropping the
            // guard before panicking is good practice in case we migrate away
            // from it in the future.
            drop(quit_tx);
            panic!("run_singlethreaded called but the executor is already running");
        }

        self.executor.lock().run_singlethreaded(async move { rx.await }).unwrap();
    }

    fn quit(&self) {
        // These operations are separate to avoid poisoning the quit message
        // channel if the executor is not running.
        let quit_tx = self.quit_tx.lock().take();
        quit_tx.unwrap().send(()).unwrap();
    }

    #[cfg(target_os = "fuchsia")]
    unsafe fn begin_wait(&mut self, wait: *mut async_wait_t) -> Result<(), zx_status::Status> {
        // TODO: figure out how to change fasync::Executor such that this can be done without allocation.

        use fuchsia_details::*;
        use fuchsia_zircon as zx;
        use std::sync::Arc;
        use zx::AsHandleRef;

        let handle = zx::HandleRef::from_raw_handle((*wait).object);
        let dispatcher: *mut Executor = &mut *self;
        let wait_ender = Arc::new(WaitEnder::new(dispatcher as *mut std::ffi::c_void, wait));
        let registration = fasync::EHandle::local().register_receiver(wait_ender.clone());

        let signals =
            zx::Signals::from_bits((*wait).trigger).ok_or(zx_status::Status::INVALID_ARGS)?;
        let options =
            zx::WaitAsyncOpts::from_bits((*wait).options).ok_or(zx_status::Status::INVALID_ARGS)?;
        let result =
            handle.wait_async_handle(registration.port(), registration.key(), signals, options);

        wait_ender.record_registration(registration);

        result
    }

    #[cfg(not(target_os = "fuchsia"))]
    unsafe fn begin_wait(&mut self, wait: *mut async_wait_t) -> Result<(), zx_status::Status> {
        use fasync::emulated_handle::{Handle, Signals};
        use fasync::OnSignals;

        let wait_ptr = wait;
        let dispatcher: *mut Executor = &mut *self;
        let wait = &mut *wait;
        // TODO(sadmac): Make sure the handle is guaranteed to remain open for the duration of the
        // wait, or document what effect that has on correctness.
        let object = std::mem::ManuallyDrop::new(Handle::from_raw(wait.object));
        let trigger = Signals::from_bits_unchecked(wait.trigger);
        let handler = wait.handler;

        fasync::Task::local(async move {
            match OnSignals::new(&*object, trigger).await {
                Ok(sigs) => {
                    let packet = zx_packet_signal_t {
                        trigger: trigger.bits(),
                        observed: sigs.bits(),
                        count: 1,
                    };

                    handler(dispatcher as *mut std::ffi::c_void, wait_ptr, ZX_OK, &packet);
                }
                Err(x) => {
                    handler(
                        dispatcher as *mut std::ffi::c_void,
                        wait_ptr,
                        x.into_raw(),
                        std::ptr::null_mut(),
                    );
                }
            }
        })
        .detach();

        Ok(())
    }

    fn now(&self) -> zx_time_t {
        let dur = fasync::Time::now() - self.start;
        #[cfg(target_os = "fuchsia")]
        let r = dur.into_nanos();
        #[cfg(not(target_os = "fuchsia"))]
        let r = dur.as_nanos() as zx_time_t;
        r
    }
}

/// Duplicated from //zircon/system/ulib/async/include/lib/async/dispatcher.h
#[repr(C)]
struct async_state_t {
    _reserved: [usize; 2],
}

/// Duplicated from //zircon/system/ulib/async/include/lib/async/wait.h
#[repr(C)]
pub(crate) struct async_wait_t {
    _state: async_state_t,
    handler: extern "C" fn(
        *mut std::ffi::c_void,
        *mut async_wait_t,
        zx_status_t,
        *const zx_packet_signal_t,
    ),
    object: zx_handle_t,
    trigger: zx_signals_t,
    options: u32,
}

/// Duplicated from //zircon/system/ulib/async/include/lib/async/task.h
#[repr(C)]
struct async_task_t {
    _state: async_state_t,
    handler: extern "C" fn(*mut std::ffi::c_void, *mut async_task_t, zx_status_t),
    deadline: zx_time_t,
}

struct TaskPtr(*mut async_task_t);
unsafe impl Send for TaskPtr {}
unsafe impl Sync for TaskPtr {}
impl TaskPtr {
    unsafe fn as_ref(&self) -> &async_task_t {
        self.0.as_ref().unwrap()
    }
}

#[no_mangle]
pub extern "C" fn fasync_executor_create(cb_executor: *mut std::ffi::c_void) -> *mut Executor {
    Box::into_raw(Executor::new(cb_executor))
}

/// Runs the given executor in a single-threaded fashion.
///
/// This will block the calling thread until the executor is quit with
/// [`fasync_executor_quit`] or destroyed with [`fasync_executor_destroy`]. Note
/// that after calling this function, `executor` may be a dangling pointer.
///
/// # Panics
///
/// Panics if the given executor is already running, for example if another
/// thread is already calling this function.
///
/// # Safety
///
/// `executor` must be non-null, properly aligned, and point to an initialized
/// [`Executor`]. To guarantee these properties, `executor` should be a pointer
/// returned from [`fasync_executor_create`] that has not yet been passed to
/// [`fasync_executor_destroy`].
#[no_mangle]
pub unsafe extern "C" fn fasync_executor_run_singlethreaded(executor: *mut Executor) {
    EPtr(executor).as_ref().run_singlethreaded()
}

/// Signals the given executor to shut down.
///
/// This function does not wait for the executor to finish shutting down. After
/// calling this function, running tasks may continue to be processed until the
/// executor processes the shutdown signal. Once the executor shuts down,
/// [`fasync_executor_run_singlethreaded`] will return.
///
/// # Panics
///
/// Panics if the given executor is not currently running.
///
/// # Safety
///
/// `executor` must be non-null, properly aligned, and point to an initialized
/// [`Executor`]. To guarantee these properties, `executor` should be a pointer
/// returned from [`fasync_executor_create`] that has not yet been passed to
/// [`fasync_executor_destroy`].
#[no_mangle]
pub unsafe extern "C" fn fasync_executor_quit(executor: *mut Executor) {
    EPtr(executor).as_ref().quit()
}

/// Drops the given executor.
///
/// This will block the current thread until all tasks that are currently
/// spawned onto the executor have been dropped. After calling this function,
/// `executor` no longer points to an initialized [`Executor`].
///
/// # Safety
///
/// `executor` must be non-null, properly aligned, and point to an initialized
/// [`Executor`] that is not currently running. To guarantee these properties,
/// `executor` should be a pointer returned from [`fasync_executor_create`] that
/// has not yet been passed to [`fasync_executor_destroy`] and is not currently
/// running in a call to [`fasync_executor_run_singlethreaded`].
#[no_mangle]
pub unsafe extern "C" fn fasync_executor_destroy(executor: *mut Executor) {
    drop(Box::from_raw(executor))
}

#[no_mangle]
unsafe extern "C" fn fasync_executor_now(executor: *mut Executor) -> zx_time_t {
    EPtr(executor).as_ref().now()
}

#[no_mangle]
unsafe extern "C" fn fasync_executor_begin_wait(
    executor: *mut Executor,
    wait: *mut async_wait_t,
) -> zx_status_t {
    zx_status::Status::from_result(EPtr(executor).as_mut().begin_wait(wait)).into_raw()
}

#[no_mangle]
unsafe extern "C" fn fasync_executor_cancel_wait(
    _executor: *mut Executor,
    _wait: *mut async_wait_t,
) -> zx_status_t {
    ZX_ERR_NOT_SUPPORTED
}

#[no_mangle]
unsafe extern "C" fn fasync_executor_post_task(
    executor: *mut Executor,
    task: *mut async_task_t,
) -> zx_status_t {
    let executor = EPtr(executor);
    let task = TaskPtr(task);
    fasync::Task::spawn(async move {
        let deadline = Duration::from_nanos(task.as_ref().deadline as u64);
        let start = executor.as_ref().start;
        fasync::Timer::new(start + deadline.into()).await;
        (task.as_ref().handler)(executor.as_ref().cb_executor, task.0, ZX_OK)
    })
    .detach();
    ZX_OK
}

#[no_mangle]
unsafe extern "C" fn fasync_executor_cancel_task(
    _executor: *mut Executor,
    _task: *mut async_task_t,
) -> zx_status_t {
    ZX_ERR_NOT_SUPPORTED
}
