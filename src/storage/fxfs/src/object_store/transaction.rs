// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        lsm_tree::types::Item,
        object_store::{
            allocator::AllocatorItem,
            constants::INVALID_OBJECT_ID,
            record::{ObjectItem, ObjectKey, ObjectValue},
        },
    },
    anyhow::Error,
    async_trait::async_trait,
    futures::future::poll_fn,
    serde::{Deserialize, Serialize},
    std::{
        any::Any,
        cmp::Ordering,
        collections::{
            hash_map::{Entry, HashMap},
            BTreeSet,
        },
        sync::{Arc, Mutex},
        task::{Poll, Waker},
        vec::Vec,
    },
};

#[async_trait]
pub trait TransactionHandler: Send + Sync {
    /// Initiates a new transaction.  Implementations should check to see that a transaction can be
    /// created (for example, by checking to see that the journaling system can accept more
    /// transactions), and then call Transaction::new.
    async fn new_transaction<'a>(
        self: Arc<Self>,
        lock_keys: &[LockKey],
    ) -> Result<Transaction<'a>, Error>;

    /// Implementations should perform any required journaling and then apply the mutations via
    /// ObjectManager's apply_mutation method.  Any mutations within the transaction should be
    /// removed so that drop_transaction can tell that the transaction was committed.
    async fn commit_transaction(&self, transaction: Transaction<'_>);

    /// Drops a transaction (rolling back if not committed).  Committing a transaction should have
    /// removed the mutations.  This is called automatically when Transaction is dropped, which is
    /// why this isn't async.
    fn drop_transaction(&self, transaction: &mut Transaction<'_>);

    /// Acquires a read lock for the given keys.  Read locks are only blocked whilst a transaction
    /// is being committed for the same locks.  They are only necessary where consistency is
    /// required between different mutations within a transaction.  For example, a write might
    /// change the size and extents for an object, in which case a read lock is required so that
    /// observed size and extents are seen together or not at all.  Implementations should call
    /// through to LockManager's read_lock implementation.
    async fn read_lock<'a>(&'a self, lock_keys: &[LockKey]) -> ReadGuard<'a>;
}

/// The journal consists of these records which will be replayed at mount time.  Within a a
/// transaction, these are stored as a set which allows some mutations to be deduplicated and found
/// (and we require custom comparison functions below).  For example, we need to be able to find
/// object size changes.
#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd, Serialize, Deserialize)]
pub enum Mutation {
    ObjectStore(ObjectStoreMutation),
    Allocator(AllocatorMutation),
    // Seal the mutable layer and create a new one.
    TreeSeal,
    // Discards all non-mutable layers.
    TreeCompact,
}

impl Mutation {
    pub fn insert_object(key: ObjectKey, value: ObjectValue) -> Self {
        Mutation::ObjectStore(ObjectStoreMutation {
            item: Item::new(key, value),
            op: Operation::Insert,
        })
    }

    pub fn replace_or_insert_object(key: ObjectKey, value: ObjectValue) -> Self {
        Mutation::ObjectStore(ObjectStoreMutation {
            item: Item::new(key, value),
            op: Operation::ReplaceOrInsert,
        })
    }

    pub fn merge_object(key: ObjectKey, value: ObjectValue) -> Self {
        Mutation::ObjectStore(ObjectStoreMutation {
            item: Item::new(key, value),
            op: Operation::Merge,
        })
    }

    pub fn allocation(item: AllocatorItem) -> Self {
        Mutation::Allocator(AllocatorMutation(item))
    }
}

// We have custom comparison functions for mutations that just use the key, rather than the key and
// value that would be used by default so that we can deduplicate and find mutations (see
// get_object_mutation below).

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ObjectStoreMutation {
    pub item: ObjectItem,
    pub op: Operation,
}

// The different LSM tree operations that can be performed as part of a mutation.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum Operation {
    Insert,
    ReplaceOrInsert,
    Merge,
}

impl Ord for ObjectStoreMutation {
    fn cmp(&self, other: &Self) -> Ordering {
        self.item.key.cmp(&other.item.key)
    }
}

impl PartialOrd for ObjectStoreMutation {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl PartialEq for ObjectStoreMutation {
    fn eq(&self, other: &Self) -> bool {
        self.item.key.eq(&other.item.key)
    }
}

impl Eq for ObjectStoreMutation {}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct AllocatorMutation(pub AllocatorItem);

impl Ord for AllocatorMutation {
    fn cmp(&self, other: &Self) -> Ordering {
        self.0.key.cmp(&other.0.key)
    }
}

impl PartialOrd for AllocatorMutation {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl PartialEq for AllocatorMutation {
    fn eq(&self, other: &Self) -> bool {
        self.0.key.eq(&other.0.key)
    }
}

impl Eq for AllocatorMutation {}

/// When creating a transaction, locks typically need to be held to prevent two or more writers
/// trying to make conflicting mutations at the same time.  LockKeys are used for this.
/// TODO(csuter): At the moment, these keys only apply to writers, but there needs to be some
/// support for readers, since there are races that can occur whilst a transaction is being
/// committed.
#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum LockKey {
    /// Used to lock changes to a particular object attribute (e.g. writes).
    ObjectAttribute { store_object_id: u64, object_id: u64, attribute_id: u64 },

    /// Used to lock changes to a particular object (e.g. adding a child to a directory).
    Object { store_object_id: u64, object_id: u64 },

    /// Used to lock changes to the root volume (e.g. adding or removing a volume).
    RootVolume,
}

impl LockKey {
    pub fn object_attribute(store_object_id: u64, object_id: u64, attribute_id: u64) -> Self {
        LockKey::ObjectAttribute { store_object_id, object_id, attribute_id }
    }

    pub fn object(store_object_id: u64, object_id: u64) -> Self {
        LockKey::Object { store_object_id, object_id }
    }
}

// Mutations can be associated with an object so that when mutations are applied, updates can be
// applied to in-memory strucutres.  For example, we cache object sizes, so when a size change is
// applied, we can update the cached object size.
pub type AssociatedObject<'a> = &'a (dyn Any + Send + Sync);

#[derive(Clone)]
pub struct TxnMutation<'a> {
    // This, at time of writing, is either the object ID of an object store, or the object ID of the
    // allocator.  In the case of an object mutation, there's another object ID in the mutation
    // record that would be for the object actually being changed.
    pub object_id: u64,

    // The actual mutation.  This gets serialized to the journal.
    pub mutation: Mutation,

    // An optional associated object for the mutation.  During replay, there will always be no
    // associated object.
    pub associated_object: Option<AssociatedObject<'a>>,
}

// We store TxnMutation in a set, and for that, we only use object_id and mutation and not the
// associated object.
impl Ord for TxnMutation<'_> {
    fn cmp(&self, other: &Self) -> Ordering {
        self.object_id.cmp(&other.object_id).then_with(|| self.mutation.cmp(&other.mutation))
    }
}

impl PartialOrd for TxnMutation<'_> {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl PartialEq for TxnMutation<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.object_id.eq(&other.object_id) && self.mutation.eq(&other.mutation)
    }
}

impl Eq for TxnMutation<'_> {}

/// A transaction groups mutation records to be commited as a group.
pub struct Transaction<'a> {
    handler: Arc<dyn TransactionHandler>,

    /// The mutations that make up this transaction.
    pub mutations: BTreeSet<TxnMutation<'a>>,

    /// The locks that this transaction currently holds.
    pub lock_keys: Vec<LockKey>,
}

impl<'a> Transaction<'a> {
    /// Creates a new transaction.  This should typically be called by a TransactionHandler's
    /// implementation of new_transaction.
    pub fn new(handler: Arc<dyn TransactionHandler>, lock_keys: Vec<LockKey>) -> Transaction<'a> {
        Transaction { handler, mutations: BTreeSet::new(), lock_keys }
    }

    /// Adds a mutation to this transaction.
    pub fn add(&mut self, object_id: u64, mutation: Mutation) {
        assert!(object_id != INVALID_OBJECT_ID);
        self.mutations.replace(TxnMutation { object_id, mutation, associated_object: None });
    }

    /// Adds a mutation with an assoicated object.
    pub fn add_with_object(
        &mut self,
        object_id: u64,
        mutation: Mutation,
        associated_object: AssociatedObject<'a>,
    ) {
        assert!(object_id != INVALID_OBJECT_ID);
        self.mutations.replace(TxnMutation {
            object_id,
            mutation,
            associated_object: Some(associated_object),
        });
    }

    /// Returns true if this transaction has no mutations.
    pub fn is_empty(&self) -> bool {
        self.mutations.is_empty()
    }

    /// Searches for an existing object mutation within the transaction that has the given key and
    /// returns it if found.
    pub fn get_object_mutation(
        &self,
        object_id: u64,
        key: ObjectKey,
    ) -> Option<&ObjectStoreMutation> {
        if let Some(TxnMutation { mutation: Mutation::ObjectStore(mutation), .. }) =
            self.mutations.get(&TxnMutation {
                object_id,
                mutation: Mutation::insert_object(key, ObjectValue::None),
                associated_object: None,
            })
        {
            Some(mutation)
        } else {
            None
        }
    }

    /// Commits a transaction.
    pub async fn commit(self) {
        self.handler.clone().commit_transaction(self).await;
    }
}

impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        // Call the TransactionHandler implementation of drop_transaction which should, as a
        // minimum, call LockManager's drop_transaction to ensure the locks are released.
        self.handler.clone().drop_transaction(self);
    }
}

/// LockManager holds the locks that transactions might have taken.  A TransactionManager
/// implementation would typically have one of these.
pub struct LockManager {
    locks: Mutex<Locks>,
}

struct Locks {
    sequence: u64,
    keys: HashMap<LockKey, LockEntry>,
}

#[derive(Debug)]
struct LockEntry {
    sequence: u64,
    read_count: u64,
    state: LockState,
    wakers: Vec<Waker>,
}

#[derive(Debug)]
enum LockState {
    Unlocked,
    Locked,
    Committing(Waker),
}

impl LockManager {
    pub fn new() -> Self {
        LockManager { locks: Mutex::new(Locks { sequence: 0, keys: HashMap::new() }) }
    }

    /// Acquires the locks.  To avoid deadlocks, the locks should be sorted.  It is the caller's
    /// responsibility to ensure that drop_transaction is called when a transaction is dropped i.e.
    /// implementers of TransactionHandler's drop_transaction method should call LockManager's
    /// drop_transaction method.
    pub async fn lock(&self, lock_keys: &[LockKey]) {
        for lock in lock_keys {
            let mut waker_sequence = 0;
            let mut waker_index = 0;
            poll_fn(|cx| {
                let mut locks = self.locks.lock().unwrap();
                let Locks { sequence, keys } = &mut *locks;
                match keys.entry(lock.clone()) {
                    Entry::Vacant(vacant) => {
                        *sequence += 1;
                        vacant.insert(LockEntry {
                            sequence: *sequence,
                            read_count: 0,
                            state: LockState::Locked,
                            wakers: Vec::new(),
                        });
                        Poll::Ready(())
                    }
                    Entry::Occupied(mut occupied) => {
                        let entry = occupied.get_mut();
                        if let LockState::Unlocked = entry.state {
                            entry.state = LockState::Locked;
                            Poll::Ready(())
                        } else {
                            if entry.sequence == waker_sequence {
                                entry.wakers[waker_index] = cx.waker().clone();
                            } else {
                                waker_index = entry.wakers.len();
                                waker_sequence = *sequence;
                                entry.wakers.push(cx.waker().clone());
                            }
                            Poll::Pending
                        }
                    }
                }
            })
            .await;
        }
    }

    /// This should be called by a TransactionHandler drop_transaction implementation.
    pub fn drop_transaction(&self, transaction: &mut Transaction<'_>) {
        let mut locks = self.locks.lock().unwrap();
        for lock in transaction.lock_keys.drain(..) {
            let Locks { sequence: _, keys } = &mut *locks;
            match keys.entry(lock.clone()) {
                Entry::Vacant(_) => unreachable!(),
                Entry::Occupied(mut occupied) => {
                    let entry = occupied.get_mut();
                    let wakers = std::mem::take(&mut entry.wakers);
                    match entry.state {
                        LockState::Committing(_) => {
                            occupied.remove_entry();
                        }
                        LockState::Locked => {
                            // If the lock is dropped before it is committed, there might be active
                            // readers referencing the same lock key, so we shouldn't remove it from
                            // the lock-set yet.
                            // The last reader will remove the entry (See ReadGuard::drop).
                            if entry.read_count == 0 {
                                occupied.remove_entry();
                            } else {
                                entry.state = LockState::Unlocked;
                            }
                        }
                        LockState::Unlocked => unreachable!(),
                    }
                    for waker in wakers {
                        waker.wake();
                    }
                }
            }
        }
    }

    /// Prepares to commit by waiting for readers to finish.
    pub async fn commit_prepare(&self, transaction: &Transaction<'_>) {
        for lock in &transaction.lock_keys {
            poll_fn(|cx| {
                let mut locks = self.locks.lock().unwrap();
                let entry = locks.keys.get_mut(&lock).expect("key missing!");
                entry.state = LockState::Committing(cx.waker().clone());
                if entry.read_count > 0 {
                    Poll::Pending
                } else {
                    Poll::Ready(())
                }
            })
            .await;
        }
    }

    pub async fn read_lock<'a>(&'a self, lock_keys: &[LockKey]) -> ReadGuard<'a> {
        let mut lock_keys: Vec<_> = lock_keys.iter().cloned().collect();
        lock_keys.sort_unstable();
        for lock in &lock_keys {
            let mut waker_sequence = 0;
            let mut waker_index = 0;
            poll_fn(|cx| {
                let mut locks = self.locks.lock().unwrap();
                let Locks { sequence, keys } = &mut *locks;
                match keys.entry(lock.clone()) {
                    Entry::Vacant(vacant) => {
                        *sequence += 1;
                        vacant.insert(LockEntry {
                            sequence: *sequence,
                            read_count: 1,
                            state: LockState::Unlocked,
                            wakers: Vec::new(),
                        });
                        Poll::Ready(())
                    }
                    Entry::Occupied(mut occupied) => {
                        let entry = occupied.get_mut();
                        if let LockState::Committing(_) = entry.state {
                            if entry.sequence == waker_sequence {
                                entry.wakers[waker_index] = cx.waker().clone();
                            } else {
                                waker_index = entry.wakers.len();
                                waker_sequence = *sequence;
                                entry.wakers.push(cx.waker().clone());
                            }
                            Poll::Pending
                        } else {
                            entry.read_count += 1;
                            Poll::Ready(())
                        }
                    }
                }
            })
            .await;
        }
        ReadGuard { manager: self, lock_keys }
    }
}

#[must_use]
pub struct ReadGuard<'a> {
    manager: &'a LockManager,
    lock_keys: Vec<LockKey>,
}

impl Drop for ReadGuard<'_> {
    fn drop(&mut self) {
        let mut locks = self.manager.locks.lock().unwrap();
        for lock in std::mem::take(&mut self.lock_keys) {
            if let Entry::Occupied(mut occupied) = locks.keys.entry(lock) {
                let entry = occupied.get_mut();
                entry.read_count -= 1;
                if entry.read_count == 0 {
                    match entry.state {
                        LockState::Unlocked => {
                            occupied.remove_entry();
                        }
                        LockState::Locked => {}
                        LockState::Committing(ref waker) => waker.wake_by_ref(),
                    }
                }
            } else {
                unreachable!(); // The entry *must* be in the HashMap.
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::{LockKey, Mutation, TransactionHandler},
        crate::{
            object_store::testing::fake_filesystem::FakeFilesystem,
            testing::fake_device::FakeDevice,
        },
        fuchsia_async as fasync,
        futures::{channel::oneshot::channel, join},
        std::{
            sync::{Arc, Mutex},
            time::Duration,
        },
    };

    #[fasync::run_singlethreaded(test)]
    async fn test_simple() {
        let device = Arc::new(FakeDevice::new(1024, 1024));
        let fs = FakeFilesystem::new(device);
        let mut t = fs.clone().new_transaction(&[]).await.expect("new_transaction failed");
        t.add(1, Mutation::TreeSeal);
        assert!(!t.is_empty());
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_locks() {
        let device = Arc::new(FakeDevice::new(1024, 1024));
        let fs = FakeFilesystem::new(device);
        let (send1, recv1) = channel();
        let (send2, recv2) = channel();
        let (send3, recv3) = channel();
        let done = Mutex::new(false);
        join!(
            async {
                let _t = fs
                    .clone()
                    .new_transaction(&[LockKey::object_attribute(1, 2, 3)])
                    .await
                    .expect("new_transaction failed");
                send1.send(()).unwrap(); // Tell the next future to continue.
                send3.send(()).unwrap(); // Tell the last future to continue.
                recv2.await.unwrap();
                // This is a halting problem so all we can do is sleep.
                fasync::Timer::new(Duration::from_millis(100)).await;
                assert!(!*done.lock().unwrap());
            },
            async {
                recv1.await.unwrap();
                // This should not block since it is a different key.
                let _t = fs
                    .clone()
                    .new_transaction(&[LockKey::object_attribute(2, 2, 3)])
                    .await
                    .expect("new_transaction failed");
                // Tell the first future to continue.
                send2.send(()).unwrap();
            },
            async {
                // This should block until the first future has completed.
                recv3.await.unwrap();
                let _t = fs.clone().new_transaction(&[LockKey::object_attribute(1, 2, 3)]).await;
                *done.lock().unwrap() = true;
            }
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_read_lock_after_write_lock() {
        let device = Arc::new(FakeDevice::new(1024, 1024));
        let fs = FakeFilesystem::new(device);
        let (send1, recv1) = channel();
        let (send2, recv2) = channel();
        let done = Mutex::new(false);
        join!(
            async {
                let t = fs
                    .clone()
                    .new_transaction(&[LockKey::object_attribute(1, 2, 3)])
                    .await
                    .expect("new_transaction failed");
                send1.send(()).unwrap(); // Tell the next future to continue.
                recv2.await.unwrap();
                t.commit().await;
                *done.lock().unwrap() = true;
            },
            async {
                recv1.await.unwrap();
                // Reads should not be blocked until the transaction is committed.
                let _guard = fs.read_lock(&[LockKey::object_attribute(1, 2, 3)]).await;
                // Tell the first future to continue.
                send2.send(()).unwrap();
                // It shouldn't proceed until we release our read lock, but it's a halting
                // problem, so sleep.
                fasync::Timer::new(Duration::from_millis(100)).await;
                assert!(!*done.lock().unwrap());
            },
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_write_lock_after_read_lock() {
        let device = Arc::new(FakeDevice::new(1024, 1024));
        let fs = FakeFilesystem::new(device);
        let (send1, recv1) = channel();
        let (send2, recv2) = channel();
        let done = Mutex::new(false);
        join!(
            async {
                // Reads should not be blocked until the transaction is committed.
                let _guard = fs.read_lock(&[LockKey::object_attribute(1, 2, 3)]).await;
                // Tell the next future to continue and then nwait.
                send1.send(()).unwrap();
                recv2.await.unwrap();
                // It shouldn't proceed until we release our read lock, but it's a halting
                // problem, so sleep.
                fasync::Timer::new(Duration::from_millis(100)).await;
                assert!(!*done.lock().unwrap());
            },
            async {
                recv1.await.unwrap();
                let t = fs
                    .clone()
                    .new_transaction(&[LockKey::object_attribute(1, 2, 3)])
                    .await
                    .expect("new_transaction failed");
                send2.send(()).unwrap(); // Tell the first future to continue;
                t.commit().await;
                *done.lock().unwrap() = true;
            },
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_drop_uncommitted_transaction() {
        let device = Arc::new(FakeDevice::new(1024, 1024));
        let fs = FakeFilesystem::new(device);
        let key = LockKey::object(1, 1);

        // Dropping while there's a reader.
        {
            let mut write_lock =
                fs.clone().new_transaction(&[key.clone()]).await.expect("new_transaction failed");
            let _read_lock = fs.read_lock(&[key.clone()]).await;
            fs.clone().drop_transaction(&mut write_lock);
        }
        // Dropping while there's no reader.
        let mut write_lock =
            fs.clone().new_transaction(&[key.clone()]).await.expect("new_transaction failed");
        fs.clone().drop_transaction(&mut write_lock);
        // Make sure we can take the lock again (i.e. it was actually released).
        fs.clone().new_transaction(&[key.clone()]).await.expect("new_transaction failed");
    }
}
