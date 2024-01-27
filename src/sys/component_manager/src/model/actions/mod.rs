// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The "Action" concept represents an asynchronous activity on a component that should eventually
//! complete.
//!
//! Actions decouple the "what" of what needs to happen to a component from the "how". Several
//! client APIs may induce operations on a component's state that complete asynchronously. These
//! operations could depend on each other in various ways.
//!
//! A key property of actions is idempotency. If two equal actions are registered on a component,
//! the work for that action is performed only once. This means that two distinct call sites can
//! register the same action, and be guaranteed the work is not repeated.
//!
//! Here are a couple examples:
//! - A `Shutdown` FIDL call must shut down every component instance in the tree, in
//!   dependency order. For this to happen every component must shut down, but not before its
//!   downstream dependencies have shut down.
//! - A `Realm.DestroyChild` FIDL call returns right after a child component is destroyed.
//!   However, in order to actually delete the child, a sequence of events must happen:
//!     * All instances in the component must be shut down (see above)
//!     * The component instance's persistent storage must be erased, if any.
//!     * The component's parent must remove it as a child.
//!
//! Note the interdependencies here -- destroying a component also requires shutdown, for example.
//!
//! These processes could be implemented through a chain of futures in the vicinity of the API
//! call. However, this doesn't scale well, because it requires distributed state handling and is
//! prone to races. Actions solve this problem by allowing client code to just specify the actions
//! that need to eventually be fulfilled. The actual business logic to perform the actions can be
//! implemented by the component itself in a coordinated manner.
//!
//! `DestroyChild()` is an example of how this can work. For simplicity, suppose it's called on a
//! component with no children of its own. This might cause a chain of events like the following:
//!
//! - Before it returns, the `DestroyChild` FIDL handler registers the `DeleteChild` action on the
//!   parent component for child being destroyed.
//! - This results in a call to `Action::handle` for the component. In response to
//!   `DestroyChild`, `Action::handle()` spawns a future that sets a `Destroy` action on the child.
//!   Note that `Action::handle()` is not async, it always spawns any work that might block
//!   in a future.
//! - `Action::handle()` is called on the child. In response to `Destroy`, it sets a `Shutdown`
//!   action on itself (the component instance must be stopped before it is destroyed).
//! - `Action::handle()` is called on the child again, in response to `Shutdown`. It turns out the
//!   instance is still running, so the `Shutdown` future tells the instance to stop. When this
//!   completes, the `Shutdown` action is finished.
//! - The future that was spawned for `Destroy` is notified that `Shutdown` completes, so it cleans
//!   up the instance's resources and finishes the `Destroy` action.
//! - When the work for `Destroy` completes, the future spawned for `DestroyChild` deletes the
//!   child and marks `DestroyChild` finished, which will notify the client that the action is
//!   complete.

mod destroy;
mod destroy_child;
mod discover;
mod resolve;
pub mod shutdown;
pub mod start;
mod stop;
mod unresolve;

// Re-export the actions
pub use {
    destroy_child::DestroyChildAction, discover::DiscoverAction, resolve::ResolveAction,
    shutdown::ShutdownAction, start::StartAction, stop::StopAction, unresolve::UnresolveAction,
};

// Limit visibility of internal actions
#[cfg(test)]
pub use destroy::DestroyAction;
#[cfg(not(test))]
pub(self) use destroy::DestroyAction;

use {
    crate::model::component::ComponentInstance,
    async_trait::async_trait,
    cm_moniker::IncarnationId,
    fuchsia_async as fasync,
    futures::{
        channel::oneshot,
        future::{pending, BoxFuture, FutureExt, Shared},
        task::{Context, Poll},
        Future,
    },
    moniker::ChildMoniker,
    std::any::Any,
    std::collections::HashMap,
    std::fmt::Debug,
    std::hash::Hash,
    std::pin::Pin,
    std::sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
};

/// A action on a component that must eventually be fulfilled.
#[async_trait]
pub trait Action: Send + Sync + 'static {
    type Output: Send + Sync + Clone + Debug;
    async fn handle(&self, component: &Arc<ComponentInstance>) -> Self::Output;
    fn key(&self) -> ActionKey;
}

/// A key that uniquely identifies an action.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum ActionKey {
    Discover,
    Resolve,
    Unresolve,
    Start,
    Stop,
    Shutdown,
    DestroyChild(ChildMoniker, IncarnationId),
    Destroy,
}

/// A set of actions on a component that must be completed.
///
/// Each action is mapped to a future that returns when the action is complete.
pub struct ActionSet {
    rep: HashMap<ActionKey, Box<dyn Any + Send + Sync>>,
}

/// A future bound to a particular action that completes when that action completes.
///
/// Cloning this type will not duplicate the action, but generate another future that waits on the
/// same action.
#[derive(Debug)]
pub struct ActionNotifier<Output: Send + Sync + Clone + Debug> {
    /// The inner future.
    fut: Shared<BoxFuture<'static, Output>>,
    /// How many clones of this ActionNotifer are live, useful for testing.
    refcount: Arc<AtomicUsize>,
}

impl<Output: Send + Sync + Clone + Debug> ActionNotifier<Output> {
    /// Instantiate an `ActionNotifier` wrapping `fut`.
    pub fn new(fut: BoxFuture<'static, Output>) -> Self {
        Self { fut: fut.shared(), refcount: Arc::new(AtomicUsize::new(1)) }
    }
}

impl<Output: Send + Sync + Clone + Debug> Clone for ActionNotifier<Output> {
    fn clone(&self) -> Self {
        self.refcount.fetch_add(1, Ordering::Relaxed);
        Self { fut: self.fut.clone(), refcount: self.refcount.clone() }
    }
}

impl<Output: Send + Sync + Clone + Debug> Drop for ActionNotifier<Output> {
    fn drop(&mut self) {
        self.refcount.fetch_sub(1, Ordering::Relaxed);
    }
}

impl<Output: Send + Sync + Clone + Debug> Future for ActionNotifier<Output> {
    type Output = Output;
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let fut = Pin::new(&mut self.fut);
        fut.poll(cx)
    }
}

/// Represents a task that implements an action.
pub(crate) struct ActionTask<A>
where
    A: Action,
{
    tx: oneshot::Sender<A::Output>,
    fut: BoxFuture<'static, A::Output>,
}

impl<A> ActionTask<A>
where
    A: Action,
{
    fn new(tx: oneshot::Sender<A::Output>, fut: BoxFuture<'static, A::Output>) -> Self {
        Self { tx, fut }
    }

    /// Runs the action in a separate task and signals the `ActionNotifier` when it completes.
    pub fn spawn(self) {
        fasync::Task::spawn(async move {
            self.tx.send(self.fut.await).unwrap_or(()); // Ignore closed receiver.
        })
        .detach();
    }
}

impl ActionSet {
    pub fn new() -> Self {
        ActionSet { rep: HashMap::new() }
    }

    pub fn contains(&self, key: &ActionKey) -> bool {
        self.rep.contains_key(key)
    }

    #[cfg(test)]
    pub fn mock_result<O>(&mut self, key: ActionKey, result: O)
    where
        O: Send + Sync + Clone + Debug + 'static,
    {
        let notifier = ActionNotifier::new(async move { result }.boxed());
        self.rep.insert(key, Box::new(notifier));
    }

    #[cfg(test)]
    pub fn remove_notifier(&mut self, key: ActionKey) {
        self.rep.remove(&key).expect("No notifier found with that key");
    }

    /// Registers an action in the set, returning when the action is finished (which may represent
    /// a task that's already running for this action).
    pub async fn register<A>(component: Arc<ComponentInstance>, action: A) -> A::Output
    where
        A: Action,
    {
        let rx = {
            let mut actions = component.lock_actions().await;
            actions.register_no_wait(&component, action)
        };
        rx.await
    }

    /// Registers an action in the set, but does not wait for it to complete, instead returning a
    /// future that can be used to wait on the task. This function is a no-op if the task is
    /// already registered.
    ///
    /// REQUIRES: `self` is the `ActionSet` contained in `component`.
    pub fn register_no_wait<A>(
        &mut self,
        component: &Arc<ComponentInstance>,
        action: A,
    ) -> impl Future<Output = A::Output>
    where
        A: Action,
    {
        let (task, rx) = self.register_inner(component, action);
        if let Some(task) = task {
            task.spawn();
        }
        rx
    }

    /// Returns a future that waits for the given action to complete, if one exists.
    pub fn wait<A>(&self, action: A) -> Option<impl Future<Output = A::Output>>
    where
        A: Action,
    {
        let key = action.key();
        if let Some(rx) = self.rep.get(&key) {
            let rx = rx
                .downcast_ref::<ActionNotifier<A::Output>>()
                .expect("action notifier has unexpected type");
            let rx = rx.clone();
            Some(rx)
        } else {
            None
        }
    }

    /// Removes an action from the set, completing it.
    async fn finish<'a>(component: &Arc<ComponentInstance>, key: &'a ActionKey) {
        let mut action_set = component.lock_actions().await;
        action_set.rep.remove(key);
    }

    /// Registers, but does not execute, an action.
    ///
    /// Returns:
    /// - An object that implements the action if it was scheduled for the first time. The caller
    ///   should call spawn() on it.
    /// - A future to listen on the completion of the action.
    #[must_use]
    pub(crate) fn register_inner<'a, A>(
        &'a mut self,
        component: &Arc<ComponentInstance>,
        action: A,
    ) -> (Option<ActionTask<A>>, ActionNotifier<A::Output>)
    where
        A: Action,
    {
        let key = action.key();
        // If this Action is already running, just subscribe to the result
        if let Some(rx) = self.rep.get(&key) {
            let rx = rx
                .downcast_ref::<ActionNotifier<A::Output>>()
                .expect("action notifier has unexpected type");
            let rx = rx.clone();
            return (None, rx);
        }

        // Otherwise we spin up the new Action
        let prereq = self.get_prereq_action(&action);

        let component = component.clone();

        let action_fut = async move {
            prereq.await;
            let res = action.handle(&component).await;
            Self::finish(&component, &action.key()).await;
            res
        }
        .boxed();

        let (tx, rx) = oneshot::channel();
        let task = ActionTask::new(tx, action_fut);
        let notifier = ActionNotifier::new(
            async move {
                match rx.await {
                    Ok(res) => res,
                    Err(_) => {
                        // Normally we won't get here but this can happen if the sender's task
                        // is cancelled because, for example, component manager exited and the
                        // executor was torn down.
                        let () = pending().await;
                        unreachable!();
                    }
                }
            }
            .boxed(),
        );
        self.rep.insert(key, Box::new(notifier.clone()));
        (Some(task), notifier)
    }

    /// Return a future that waits for any Action that must be waited on before
    /// executing the target Action. If none is required the returned future is
    /// empty.
    fn get_prereq_action<'a, A: Action>(
        &'a mut self,
        action: &'a A,
    ) -> Pin<Box<dyn Future<Output = ()> + Send>> {
        // If the current action is Stop/Shutdown, ensure that
        // we block on the completion of Shutdown/Stop if it is
        // currently in progress.
        let prereq_action = match action.key() {
            ActionKey::Shutdown => self.rep.get(&ActionKey::Stop),
            ActionKey::Stop => self.rep.get(&ActionKey::Shutdown),
            _ => None,
        };

        if let Some(prereq_action) = prereq_action {
            let prereq_action = prereq_action
                .downcast_ref::<ActionNotifier<A::Output>>()
                .expect("action notifier has unexpected type")
                .clone();
            async move {
                prereq_action.await;
            }
            .boxed()
        } else {
            async {}.boxed()
        }
    }
}

#[cfg(test)]
pub mod tests {
    use {
        super::*,
        crate::model::{
            actions::{destroy::DestroyAction, shutdown::ShutdownAction},
            error::ModelError,
            testing::test_helpers::ActionsTest,
        },
        assert_matches::assert_matches,
        fuchsia_async as fasync,
    };

    async fn register_action_in_new_task<A>(
        action: A,
        component: Arc<ComponentInstance>,
        responder: oneshot::Sender<A::Output>,
        res: A::Output,
    ) where
        A: Action,
    {
        let (starter_tx, starter_rx) = oneshot::channel();
        fasync::Task::spawn(async move {
            let mut action_set = component.lock_actions().await;

            // Register action, and get the future. Use `register_inner` so that we can control
            // when to notify the listener.
            let (task, rx) = action_set.register_inner(&component, action);

            // Signal to test that action is registered.
            starter_tx.send(()).unwrap();

            // Drop `action_set` to release the lock.
            drop(action_set);

            if let Some(task) = task {
                // Notify the listeners, but don't actually run the action since this test tests
                // action registration and not the actions themselves.
                task.tx.send(res).unwrap();
            }
            let res = rx.await;

            // If the future completed successfully then we will get to this point.
            responder.send(res).expect("failed to send response");
        })
        .detach();
        starter_rx.await.expect("Unable to receive start signal");
    }

    #[fuchsia::test]
    async fn action_set() {
        let test = ActionsTest::new("root", vec![], None).await;
        let component = test.model.root().clone();

        let (tx1, rx1) = oneshot::channel();
        register_action_in_new_task(DestroyAction::new(), component.clone(), tx1, Ok(())).await;
        let (tx2, rx2) = oneshot::channel();
        register_action_in_new_task(
            ShutdownAction::new(),
            component.clone(),
            tx2,
            Err(ModelError::ContextNotFound), // Some random error.
        )
        .await;
        let (tx3, rx3) = oneshot::channel();
        register_action_in_new_task(DestroyAction::new(), component.clone(), tx3, Ok(())).await;

        // Complete actions, while checking notifications.
        ActionSet::finish(&component, &ActionKey::Destroy).await;
        assert_matches!(rx1.await.expect("Unable to receive result of Notification"), Ok(()));
        assert_matches!(rx3.await.expect("Unable to receive result of Notification"), Ok(()));

        ActionSet::finish(&component, &ActionKey::Shutdown).await;
        assert_matches!(
            rx2.await.expect("Unable to receive result of Notification"),
            Err(ModelError::ContextNotFound)
        );
    }
}

#[cfg(test)]
pub(crate) mod test_utils {
    use {
        crate::model::component::{ComponentInstance, InstanceState},
        moniker::{AbsoluteMonikerBase, ChildMoniker},
        routing::component_instance::ComponentInstanceInterface,
    };

    pub async fn is_executing(component: &ComponentInstance) -> bool {
        component.lock_execution().await.runtime.is_some()
    }

    /// Verifies that a child component is deleted by checking its InstanceState and verifying that
    /// it does not exist in the InstanceState of its parent. Assumes the parent is not destroyed
    /// yet.
    pub async fn is_child_deleted(parent: &ComponentInstance, child: &ComponentInstance) -> bool {
        let instanced_moniker =
            child.instanced_moniker().leaf().expect("Root component cannot be destroyed");

        // Verify the parent-child relationship
        assert_eq!(
            parent.instanced_moniker().child(instanced_moniker.clone()),
            *child.instanced_moniker()
        );

        let parent_state = parent.lock_state().await;
        let parent_resolved_state = match *parent_state {
            InstanceState::Resolved(ref s) => s,
            _ => panic!("not resolved"),
        };

        let child_state = child.lock_state().await;
        let child_execution = child.lock_execution().await;
        let found_child = parent_resolved_state.get_child(child.child_moniker().unwrap());

        found_child.is_none()
            && matches!(*child_state, InstanceState::Destroyed)
            && child_execution.runtime.is_none()
            && child_execution.is_shut_down()
    }

    pub async fn is_stopped(component: &ComponentInstance, moniker: &ChildMoniker) -> bool {
        match *component.lock_state().await {
            InstanceState::Resolved(ref s) => match s.get_child(moniker) {
                Some(child) => {
                    let child_execution = child.lock_execution().await;
                    child_execution.runtime.is_none()
                }
                None => false,
            },
            InstanceState::Destroyed => false,
            InstanceState::New | InstanceState::Unresolved => {
                panic!("not resolved")
            }
        }
    }

    pub async fn is_destroyed(component: &ComponentInstance) -> bool {
        let state = component.lock_state().await;
        let execution = component.lock_execution().await;
        matches!(*state, InstanceState::Destroyed)
            && execution.runtime.is_none()
            && execution.is_shut_down()
    }

    pub async fn is_resolved(component: &ComponentInstance) -> bool {
        let state = component.lock_state().await;
        matches!(*state, InstanceState::Resolved(_))
    }

    pub async fn is_discovered(component: &ComponentInstance) -> bool {
        let state = component.lock_state().await;
        matches!(*state, InstanceState::Unresolved)
    }

    pub async fn is_unresolved(component: &ComponentInstance) -> bool {
        let state = component.lock_state().await;
        let execution = component.lock_execution().await;
        execution.runtime.is_none()
            && matches!(*state, InstanceState::New | InstanceState::Unresolved)
    }
}
