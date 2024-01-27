// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::responding_channel as responding,
    async_utils::{
        hanging_get::error::HangingGetServerError,
        stream::{StreamItem, WithEpitaph},
    },
    core::hash::Hash,
    futures::{channel::mpsc, select, SinkExt, StreamExt},
    std::collections::HashMap,
};

/// Default value that can be passed to `HangingGetBroker::new` by clients.
/// If passed in, this will be used for all MPSC channels created by the broker.
pub const DEFAULT_CHANNEL_SIZE: usize = 128;

/// A `Send` wrapper for a `HangingGet` that can receive messages via an async channel.
/// The `HangingGetBroker` is the primary way of implementing server side hanging get using
/// this library. It manages all state and reacts to inputs sent over channels.
///
/// ### Example Usage:
///
/// Assuming some fidl protocol with a hanging get method:
///
/// ```fidl
/// protocol SheepCounter {
///     /// Returns the current number of sheep that have jumped the fence
///     /// when that number changes.
///     WatchCount() -> (uint64 count);
/// }
/// ```
///
/// A server implementation might include the following:
///
/// ```rust
/// let broker = HangingGetBroker::new(
///     0u64, // Initial state
///     |s, o: SheepCounterWatchCountResponder| {
///         o.send(s.clone()).unwrap();
///         true
///     }, // notify function with fidl auto-generated responder
///     DEFAULT_CHANNEL_SIZE, // Size of channels used by Publishers and Subscribers
/// );
///
/// // Create a new publisher that can be used to publish updates to the state
/// let mut publisher = broker.new_publisher();
/// // Create a new registrar that can be used to register subscribers
/// let mut registrar = broker.new_registrar();
///
/// // Spawn broker as an async task that will run until there are not any more
/// // `SubscriptionRegistrar`, `Publisher`, or `Subscriber` objects that can update the system.
/// fuchsia_async::Task::spawn(broker.run()).detach();
///
/// // Spawn a background task to count sheep
/// fuchsia_async::Task::spawn(async move {
///     let interval = fuchsia_async::Interval::new(1.second);
///     loop {
///         interval.next.await();
///         publisher.update(|sheep_count| *sheep_count += 1);
///     }
/// }).detach();
///
/// // Create a new `ServiceFs` and register SheepCounter fidl service
/// let mut fs = ServiceFs::new();
/// fs.dir("svc").add_fidl_service(|s: SheepCounterRequestStream| s);
///
/// // SubscriptionRegistrar new client connections sequentially
/// while let Some(mut stream) = fs.next().await {
///
///     // Create a new subscriber associated with this client's request stream
///     let mut subscriber = registrar.new_subscriber().await.unwrap();
///
///     // SubscriptionRegistrar requests from this client by registering new observers
///     fuchsia_async::Task::spawn(async move {
///         while let Some(Ok(SheepCounterWatchCountRequest { responder })) = stream.next().await {
///             subscriber.register(responder).await.unwrap();
///         }
///     }).detach();
/// }
/// ```
pub struct HangingGetBroker<S, O: Unpin + 'static, F: Fn(&S, O) -> bool> {
    inner: HangingGet<S, subscriber_key::Key, O, F>,
    publisher: Publisher<S>,
    updates: mpsc::Receiver<UpdateFn<S>>,
    registrar: SubscriptionRegistrar<O>,
    subscription_requests: responding::Receiver<(), Subscriber<O>>,
    /// A `subscriber_key::Key` held by the broker to track the next unique key that the broker can
    /// assign to a `Subscriber`.
    subscriber_key_generator: subscriber_key::Generator,
    channel_size: usize,
}

impl<S, O, F> HangingGetBroker<S, O, F>
where
    S: Clone + Send,
    O: Send + Unpin + 'static,
    F: Fn(&S, O) -> bool,
{
    /// Create a new broker.
    /// `state` is the initial state of the HangingGet
    /// `notify` is a `Fn` that is used to notify observers of state.
    /// `channel_size` is the maximum queue size of unprocessed messages from an individual object.
    pub fn new(state: S, notify: F, channel_size: usize) -> Self {
        let (sender, updates) = mpsc::channel(channel_size);
        let publisher = Publisher { sender };
        let (sender, subscription_requests) = responding::channel(channel_size);
        let registrar = SubscriptionRegistrar { sender };
        Self {
            inner: HangingGet::new(state, notify),
            publisher,
            updates,
            registrar,
            subscription_requests,
            subscriber_key_generator: subscriber_key::Generator::default(),
            channel_size,
        }
    }

    /// Create a new `Publisher` that can be used to communicate state updates
    /// with this `HangingGetBroker` from another thread or async task.
    pub fn new_publisher(&self) -> Publisher<S> {
        self.publisher.clone()
    }

    /// Create a new `SubscriptionRegistrar` that can be used to register new subscribers
    /// with this `HangingGetBroker` from another thread or async task.
    pub fn new_registrar(&self) -> SubscriptionRegistrar<O> {
        self.registrar.clone()
    }

    /// Consume `HangingGetBroker`, returning a Future object that can be polled to drive updates
    /// to the HangingGet object. The Future completes when there are no remaining
    /// `SubscriptionRegistrars` for this object.
    pub async fn run(mut self) {
        // Drop internally held references to incoming registrar.
        // They are no longer externally reachable and will prevent the
        // select! macro from completing if they are not dropped.
        drop(self.publisher);
        drop(self.registrar);

        // A combined stream of all active subscribers which yields
        // `observer` objects from those subscribers as they request
        // observations.
        let mut subscriptions = futures::stream::SelectAll::new();

        loop {
            select! {
                // An update has been sent to the broker from a `Publisher`.
                update = self.updates.next() => {
                    if let Some(update) = update {
                        self.inner.update(update)
                    }
                }
                // A request for a new subscriber as been requested from a `SubscriptionRegistrar`.
                subscriber = self.subscription_requests.next() => {
                    if let Some((_, responder)) = subscriber {
                        let (sender, receiver) = responding::channel(self.channel_size);
                        let key = self.subscriber_key_generator.next().unwrap();
                        if let Ok(()) = responder.respond(sender.into()) {
                            subscriptions.push(receiver.map(move |o| (key, o)).with_epitaph(key));
                        }
                    }
                }
                // An observation request has been made by a `Subscriber`.
                observer = subscriptions.next() => {
                    match observer {
                        Some(StreamItem::Item((key, (observer, responder)))) => {
                            let _ = responder.respond(self.inner.subscribe(key, observer));
                        },
                        Some(StreamItem::Epitaph(key)) => {
                            self.inner.unsubscribe(key);
                        },
                        None => (),
                    }
                }
                // There are no live objects that can inject new inputs into the system.
                complete => break,
            }
        }
    }
}

/// A cheaply copyable handle that can be used to register new `Subscriber`s with
/// the `HangingGetBroker`.
pub struct SubscriptionRegistrar<O> {
    sender: responding::Sender<(), Subscriber<O>>,
}

impl<O> Clone for SubscriptionRegistrar<O> {
    fn clone(&self) -> Self {
        Self { sender: self.sender.clone() }
    }
}

impl<O> SubscriptionRegistrar<O> {
    /// Register a new subscriber
    pub async fn new_subscriber(&mut self) -> Result<Subscriber<O>, HangingGetServerError> {
        Ok(self.sender.request(()).await?)
    }
}

/// A `Subscriber` can be used to register observation requests with the `HangingGetBroker`.
/// These observations will be fulfilled when the state changes or immediately the first time
/// a `Subscriber` registers an observation.
pub struct Subscriber<O> {
    sender: responding::Sender<O, Result<(), HangingGetServerError>>,
}

impl<O> From<responding::Sender<O, Result<(), HangingGetServerError>>> for Subscriber<O> {
    fn from(sender: responding::Sender<O, Result<(), HangingGetServerError>>) -> Self {
        Self { sender }
    }
}

impl<O> Subscriber<O> {
    /// Register a new observation.
    /// Errors occur when:
    /// * A `Subscriber` is sending observation requests at too high a rate.
    /// * The `HangingGetBroker` has been dropped by the server.
    pub async fn register(&mut self, observation: O) -> Result<(), HangingGetServerError> {
        self.sender.request(observation).await?
    }
}

/// `FnOnce` type that can be used by library consumers to make in-place modifications to
/// the hanging get state.
type UpdateFn<S> = Box<dyn FnOnce(&mut S) -> bool + Send + 'static>;

/// A `Publisher` is used to make changes to the state contained within a `HangingGetBroker`.
/// It is designed to be cheaply cloned and `Send`.
pub struct Publisher<S> {
    sender: mpsc::Sender<UpdateFn<S>>,
}

impl<S> Clone for Publisher<S> {
    fn clone(&self) -> Self {
        Publisher { sender: self.sender.clone() }
    }
}

impl<S> Publisher<S>
where
    S: Send + 'static,
{
    /// `set` is a specialized form of `update` that sets the hanging get state to the value
    /// passed in as the `state` parameter.
    pub async fn set(&mut self, state: S) -> Result<(), HangingGetServerError> {
        Ok(self
            .sender
            .send(Box::new(move |s| {
                *s = state;
                true
            }))
            .await?)
    }

    /// Pass a function to the hanging get that can update the hanging get state in place.
    /// `update` should return false if the state was not updated.
    pub async fn update<F>(&mut self, update: F) -> Result<(), HangingGetServerError>
    where
        F: FnOnce(&mut S) -> bool + Send + 'static,
    {
        Ok(self.sender.send(Box::new(update)).await?)
    }
}

/// *Deprecated*: New code should use [`async_utils::hanging_get::server::HangingGet`].
///
/// TODO(<https://fxbug.dev/104570>): Remove this struct.
///
/// A `HangingGet` object manages some internal state `S` and notifies observers of type `O`
/// when their view of the state is outdated.
///
/// `S` - the type of State to be watched
/// `O` - the type of Observers of `S`
/// `F` - the type of observer notification behavior, where `F: Fn(&S, O)`
/// `K` - the Key by which Observers are identified
///
/// While it _can_ be used directly, most API consumers will want to use the higher level
/// `HangingGetBroker` object. `HangingGetBroker` and its companion types provide `Send`
/// for use from multiple threads or async tasks.
pub struct HangingGet<S, K, O, F: Fn(&S, O) -> bool> {
    state: S,
    notify: F,
    observers: HashMap<K, Window<O>>,
}

impl<S, K, O, F> HangingGet<S, K, O, F>
where
    K: Eq + Hash,
    F: Fn(&S, O) -> bool,
{
    fn notify_all(&mut self) {
        for window in self.observers.values_mut() {
            window.notify(&self.notify, &self.state);
        }
    }

    /// Create a new `HangingGet`.
    /// `state` is the initial state of the HangingGet
    /// `notify` is a `Fn` that is used to notify observers of state.
    pub fn new(state: S, notify: F) -> Self {
        Self { state, notify, observers: HashMap::new() }
    }

    /// Set the internal state value to `state` and notify all observers.
    /// Note that notification is not conditional on the _value_ set by calling the `set` function.
    /// Notification will occur regardless of whether the `state` value differs from the value
    /// currently stored by `HangingGet`.
    pub fn set(&mut self, state: S) {
        self.state = state;
        self.notify_all();
    }

    /// Modify the internal state in-place using the `state_update` function. Notify all
    /// observers if `state_update` returns true.
    pub fn update(&mut self, state_update: impl FnOnce(&mut S) -> bool) {
        if state_update(&mut self.state) {
            self.notify_all();
        }
    }

    /// Register an observer as a subscriber of the state. Observers are grouped by key and
    /// all observers will the same key are assumed to have received the latest state update.
    /// If an observer with a previously unseen key subscribes, it is immediately notified
    /// to the stated. If an observer with a known key subscribes, it will only be
    /// notified when the state is updated since last sent to an observer with the same
    /// key. All unresolved observers will be resolved to the same value immediately after the state
    /// is updated.
    pub fn subscribe(&mut self, key: K, observer: O) -> Result<(), HangingGetServerError> {
        self.observers.entry(key).or_insert_with(Window::new).observe(
            observer,
            &self.notify,
            &self.state,
        )
    }

    /// Deregister all observers that subscribed with `key`. If an observer is subsequently
    /// subscribed with the same `key` value, it will be treated as a previously unseen `key`.
    pub fn unsubscribe(&mut self, key: K) {
        drop(self.observers.remove(&key));
    }
}

/// Window tracks all observers for a given `key` and whether their view of the state is
/// `dirty` or not.
struct Window<O> {
    dirty: bool,
    observer: Option<O>,
}

impl<O> Window<O> {
    /// Create a new `Window` without an `observer` and an initial `dirty` value of `true`.
    pub fn new() -> Self {
        Window { dirty: true, observer: None }
    }

    /// Register a new observer. The observer will be notified immediately if the `Window`
    /// has a dirty view of the state. The observer will be stored for future notification
    /// if the `Window` does not have a dirty view.
    pub fn observe<S>(
        &mut self,
        observer: O,
        f: impl Fn(&S, O) -> bool,
        current_state: &S,
    ) -> Result<(), HangingGetServerError> {
        if self.observer.is_some() {
            return Err(HangingGetServerError::MultipleObservers);
        }
        self.observer = Some(observer);
        if self.dirty {
            self.notify(f, current_state);
        }
        Ok(())
    }

    /// Notify the observer _if and only if_ the `Window` has a dirty view of `state`.
    /// If an observer was present and notified, the `Window` no longer has a dirty view
    /// after this method returns.
    pub fn notify<S>(&mut self, f: impl Fn(&S, O) -> bool, state: &S) {
        match self.observer.take() {
            Some(observer) => {
                if f(state, observer) {
                    self.dirty = false;
                }
            }
            None => self.dirty = true,
        }
    }
}

/// Submodule used to keep the internals of `Key`s and `Generator`s inaccessable.
mod subscriber_key {
    /// Manages the creation and distribution of unique `Key`s
    pub struct Generator {
        next: Key,
    }

    impl Default for Generator {
        fn default() -> Self {
            Self { next: Key(0) }
        }
    }

    impl Generator {
        /// Get a unique key.
        /// Returns `None` if 2^64-2 keys have already been generated.
        pub fn next(&mut self) -> Option<Key> {
            let key = self.next.clone();
            if let Some(next) = self.next.0.checked_add(1) {
                self.next.0 = next;
                Some(key)
            } else {
                None
            }
        }
    }

    /// An internal per-subscriber key that is intended to be unique.
    #[derive(PartialEq, Eq, Hash, Debug, Clone, Copy)]
    pub struct Key(u64);
}

#[cfg(test)]
mod tests {
    use {
        super::*, async_utils::hanging_get::test_util::TestObserver, fuchsia_async as fasync,
        futures::channel::oneshot, std::task::Poll,
    };

    const TEST_CHANNEL_SIZE: usize = 128;

    #[test]
    fn subscriber_key_generator_creates_unique_keys() {
        let mut gen = subscriber_key::Generator::default();
        let key1 = gen.next();
        let key2 = gen.next();
        assert!(key1 != key2);
    }

    #[test]
    fn window_add_first_observer_notifies() {
        let state = 0;
        let mut window = Window::new();
        window.observe(TestObserver::expect_value(state), TestObserver::observe, &state).unwrap();
    }

    #[test]
    fn window_add_second_observer_does_not_notify() {
        let state = 0;
        let mut window = Window::new();
        window.observe(TestObserver::expect_value(state), TestObserver::observe, &state).unwrap();

        // Second observer added without updating the value
        window.observe(TestObserver::expect_no_value(), TestObserver::observe, &state).unwrap();
    }

    #[test]
    fn window_add_second_observer_notifies_after_notify_call() {
        let mut state = 0;
        let mut window = Window::new();
        window.observe(TestObserver::expect_value(state), TestObserver::observe, &state).unwrap();

        state = 1;
        window.notify(TestObserver::observe, &state);

        // Second observer added without updating the value
        window.observe(TestObserver::expect_value(state), TestObserver::observe, &state).unwrap();
    }

    #[test]
    fn window_add_multiple_observers_are_notified() {
        let mut state = 0;
        let mut window = Window::new();
        window.observe(TestObserver::expect_value(state), TestObserver::observe, &state).unwrap();

        // Second observer added without updating the value
        let o1 = TestObserver::expect_value(1);
        let o2 = TestObserver::expect_no_value();
        window.observe(o1.clone(), TestObserver::observe, &state).unwrap();
        let result = window.observe(o2.clone(), TestObserver::observe, &state);
        assert_eq!(result.unwrap_err(), HangingGetServerError::MultipleObservers);
        assert!(!o1.has_value());
        state = 1;
        window.notify(TestObserver::observe, &state);
    }

    #[test]
    fn window_dirty_flag_state() {
        let state = 0;
        let mut window = Window::new();
        let o = TestObserver::expect_value(state);
        window.observe(o, TestObserver::observe, &state).unwrap();
        assert!(window.observer.is_none());
        assert!(!window.dirty);
        window.notify(TestObserver::observe, &state);
        assert!(window.dirty);
        let o = TestObserver::expect_value(state);
        window.observe(o, TestObserver::observe, &state).unwrap();
        assert!(!window.dirty);
    }

    #[test]
    fn window_dirty_flag_respects_consumed_flag() {
        let state = 0;
        let mut window = Window::new();

        let o = TestObserver::expect_value(state);
        window.observe(o, TestObserver::observe_incomplete, &state).unwrap();
        assert!(window.dirty);
    }

    #[test]
    fn hanging_get_subscribe() {
        let mut hanging = HangingGet::new(0, TestObserver::observe);
        let o = TestObserver::expect_value(0);
        assert!(!o.has_value());
        hanging.subscribe(0, o.clone()).unwrap();
    }

    #[test]
    fn hanging_get_subscribe_then_set() {
        let mut hanging = HangingGet::new(0, TestObserver::observe);
        let o = TestObserver::expect_value(0);
        hanging.subscribe(0, o.clone()).unwrap();

        // No change without a new subscription
        hanging.set(1);
    }

    #[test]
    fn hanging_get_subscribe_twice_then_set() {
        let mut hanging = HangingGet::new(0, TestObserver::observe);
        hanging.subscribe(0, TestObserver::expect_value(0)).unwrap();

        hanging.subscribe(0, TestObserver::expect_value(1)).unwrap();
        hanging.set(1);
    }

    #[test]
    fn hanging_get_subscribe_multiple_then_set() {
        let mut hanging = HangingGet::new(0, TestObserver::observe);
        hanging.subscribe(0, TestObserver::expect_value(0)).unwrap();

        // A second subscription with the same client key should not be notified
        let o2 = TestObserver::expect_value(1);
        hanging.subscribe(0, o2.clone()).unwrap();
        assert!(!o2.has_value());

        // A third subscription will queue up along the other waiting observer
        let _ = hanging.subscribe(0, TestObserver::expect_no_value()).unwrap_err();

        // Set should notify all observers to the change
        hanging.set(1);
    }

    #[test]
    fn hanging_get_subscribe_with_two_clients_then_set() {
        let mut hanging = HangingGet::new(0, TestObserver::observe);
        hanging.subscribe(0, TestObserver::expect_value(0)).unwrap();
        hanging.subscribe(0, TestObserver::expect_value(1)).unwrap();
        hanging.subscribe(1, TestObserver::expect_value(0)).unwrap();
        hanging.subscribe(1, TestObserver::expect_value(1)).unwrap();
        hanging.set(1);
    }

    #[test]
    fn hanging_get_unsubscribe() {
        let mut hanging = HangingGet::new(0, TestObserver::observe);
        hanging.subscribe(0, TestObserver::expect_value(0)).unwrap();
        hanging.subscribe(0, TestObserver::expect_no_value()).unwrap();
        hanging.unsubscribe(0);
        hanging.set(1);
    }

    #[test]
    fn hanging_get_unsubscribe_one_of_many() {
        let mut hanging = HangingGet::new(0, TestObserver::observe);

        hanging.subscribe(0, TestObserver::expect_value(0)).unwrap();
        hanging.subscribe(0, TestObserver::expect_no_value()).unwrap();
        hanging.subscribe(1, TestObserver::expect_value(0)).unwrap();
        hanging.subscribe(1, TestObserver::expect_no_value()).unwrap();

        // Unsubscribe one of the two observers
        hanging.unsubscribe(0);
        assert!(!hanging.observers.contains_key(&0));
        assert!(hanging.observers.contains_key(&1));
    }

    #[fasync::run_until_stalled(test)]
    async fn publisher_set_value() {
        let (sender, mut receiver) = mpsc::channel(128);
        let mut p = Publisher { sender };
        let mut value = 1i32;
        p.set(2i32).await.unwrap();
        let f = receiver.next().await.unwrap();
        assert_eq!(true, f(&mut value));
        assert_eq!(value, 2);
    }

    #[fasync::run_until_stalled(test)]
    async fn publisher_update_value() {
        let (sender, mut receiver) = mpsc::channel(128);
        let mut p = Publisher { sender };
        let mut value = 1i32;
        p.update(|v| {
            *v += 1;
            true
        })
        .await
        .unwrap();
        let f = receiver.next().await.unwrap();
        assert_eq!(true, f(&mut value));
        assert_eq!(value, 2);
    }

    #[test]
    fn pub_sub_empty_completes() {
        let mut ex = fasync::TestExecutor::new();
        let broker = HangingGetBroker::new(
            0i32,
            |s, o: oneshot::Sender<_>| o.send(s.clone()).map(|()| true).unwrap(),
            TEST_CHANNEL_SIZE,
        );
        let publisher = broker.new_publisher();
        let registrar = broker.new_registrar();
        let broker_future = broker.run();
        futures::pin_mut!(broker_future);

        // Broker future is still pending when registrars are live.
        assert_eq!(ex.run_until_stalled(&mut broker_future), Poll::Pending);

        drop(publisher);
        drop(registrar);

        // Broker future completes when registrars are dropped.
        assert_eq!(ex.run_until_stalled(&mut broker_future), Poll::Ready(()));
    }

    #[fasync::run_until_stalled(test)]
    async fn pub_sub_updates_and_observes() {
        let broker = HangingGetBroker::new(
            0i32,
            |s, o: oneshot::Sender<_>| o.send(s.clone()).map(|()| true).unwrap(),
            TEST_CHANNEL_SIZE,
        );
        let mut publisher = broker.new_publisher();
        let mut registrar = broker.new_registrar();
        let fut = async move {
            let mut subscriber = registrar.new_subscriber().await.unwrap();

            // Initial observation is immediate
            let (sender, receiver) = oneshot::channel();
            subscriber.register(sender).await.unwrap();
            assert_eq!(receiver.await.unwrap(), 0);

            // Subsequent observations do not happen until after an update
            let (sender, mut receiver) = oneshot::channel();
            subscriber.register(sender).await.unwrap();
            assert!(receiver.try_recv().unwrap().is_none());
            publisher.set(1).await.unwrap();
            assert_eq!(receiver.await.unwrap(), 1);
        };

        // Broker future will complete when `fut` has complete
        futures::join!(fut, broker.run());
    }

    #[fasync::run_until_stalled(test)]
    async fn pub_sub_multiple_subscribers() {
        let broker = HangingGetBroker::new(
            0i32,
            |s, o: oneshot::Sender<_>| o.send(s.clone()).map(|()| true).unwrap(),
            TEST_CHANNEL_SIZE,
        );
        let mut publisher = broker.new_publisher();
        let mut registrar = broker.new_registrar();
        let fut = async move {
            let mut sub1 = registrar.new_subscriber().await.unwrap();
            let mut sub2 = registrar.new_subscriber().await.unwrap();

            // Initial observation for subscribers is immediate
            let (sender, receiver) = oneshot::channel();
            sub1.register(sender).await.unwrap();
            assert_eq!(receiver.await.unwrap(), 0);

            let (sender, receiver) = oneshot::channel();
            sub2.register(sender).await.unwrap();
            assert_eq!(receiver.await.unwrap(), 0);

            // Subsequent observations do not happen until after an update
            let (sender, mut recv1) = oneshot::channel();
            sub1.register(sender).await.unwrap();
            assert!(recv1.try_recv().unwrap().is_none());

            let (sender, mut recv2) = oneshot::channel();
            sub2.register(sender).await.unwrap();
            assert!(recv2.try_recv().unwrap().is_none());

            publisher.set(1).await.unwrap();
            assert_eq!(recv1.await.unwrap(), 1);
            assert_eq!(recv2.await.unwrap(), 1);
        };

        // Broker future will complete when `fut` has complete
        futures::join!(fut, broker.run());
    }
}
