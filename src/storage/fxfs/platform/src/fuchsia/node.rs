// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::fuchsia::{directory::FxDirectory, file::FxFile},
    anyhow::Error,
    async_trait::async_trait,
    futures::future::poll_fn,
    fxfs::object_handle::ObjectProperties,
    std::{
        any::TypeId,
        collections::{btree_map::Entry, BTreeMap},
        iter::Iterator,
        sync::{Arc, Mutex, Weak},
        task::{Poll, Waker},
        vec::Vec,
    },
    vfs::common::IntoAny,
};

/// FxNode is a node in the filesystem hierarchy (either a file or directory).
#[async_trait]
pub trait FxNode: IntoAny + Send + Sync + 'static {
    fn object_id(&self) -> u64;
    fn parent(&self) -> Option<Arc<FxDirectory>>;
    fn set_parent(&self, parent: Arc<FxDirectory>);
    fn open_count_add_one(&self);
    fn open_count_sub_one(&self);
    async fn get_properties(&self) -> Result<ObjectProperties, Error>;
}

struct PlaceholderInner {
    object_id: u64,
    waker_sequence: u64,
    wakers: Vec<Waker>,
}

struct Placeholder(Mutex<PlaceholderInner>);

#[async_trait]
impl FxNode for Placeholder {
    fn object_id(&self) -> u64 {
        self.0.lock().unwrap().object_id
    }
    fn parent(&self) -> Option<Arc<FxDirectory>> {
        unreachable!();
    }
    fn set_parent(&self, _parent: Arc<FxDirectory>) {
        unreachable!();
    }
    fn open_count_add_one(&self) {}
    fn open_count_sub_one(&self) {}
    async fn get_properties(&self) -> Result<ObjectProperties, Error> {
        unreachable!();
    }
}

/// PlaceholderOwner is a reserved slot in the node cache.
pub struct PlaceholderOwner<'a> {
    inner: Arc<Placeholder>,
    committed: bool,
    cache: &'a NodeCache,
}

impl PlaceholderOwner<'_> {
    /// Commits a node to the cache, replacing the placeholder and unblocking any waiting callers.
    pub fn commit(mut self, node: &Arc<dyn FxNode>) {
        let this_object_id = self.inner.object_id();
        assert_eq!(node.object_id(), this_object_id);
        self.committed = true;
        self.cache.commit(node.clone());
    }
}

impl Drop for PlaceholderOwner<'_> {
    fn drop(&mut self) {
        let mut p = self.inner.0.lock().unwrap();
        if !self.committed {
            // If the placeholder is dropped before it was committed, remove the cache entry so that
            // another caller blocked in NodeCache::get_or_reserve can take the slot.
            self.cache.0.lock().unwrap().map.remove(&p.object_id);
        }
        for waker in p.wakers.drain(..) {
            waker.wake();
        }
    }
}

/// See NodeCache::get_or_reserve.
pub enum GetResult<'a> {
    Placeholder(PlaceholderOwner<'a>),
    Node(Arc<dyn FxNode>),
}

impl<'a> GetResult<'a> {
    pub fn placeholder(self) -> Option<PlaceholderOwner<'a>> {
        match self {
            GetResult::Placeholder(placeholder) => Some(placeholder),
            _ => None,
        }
    }
}

struct NodeCacheInner {
    map: BTreeMap<u64, Weak<dyn FxNode>>,
    next_waker_sequence: u64,
}

/// NodeCache is an in-memory cache of weak node references.
pub struct NodeCache(Mutex<NodeCacheInner>);

/// Iterates over all files in the cache (skipping directories).
pub struct FileIter<'a> {
    cache: &'a NodeCache,
    object_id: Option<u64>,
}

impl<'a> Iterator for FileIter<'a> {
    type Item = Arc<FxFile>;
    fn next(&mut self) -> Option<Self::Item> {
        let cache = self.cache.0.lock().unwrap();
        let range = match self.object_id {
            None => cache.map.range(0..),
            Some(oid) => cache.map.range(oid + 1..),
        };
        for (object_id, file) in range {
            if let Some(file) = file.upgrade().and_then(|f| f.into_any().downcast::<FxFile>().ok())
            {
                self.object_id = Some(*object_id);
                return Some(file);
            }
        }
        None
    }
}

impl NodeCache {
    pub fn new() -> Self {
        Self(Mutex::new(NodeCacheInner { map: BTreeMap::new(), next_waker_sequence: 0 }))
    }

    /// Gets a node in the cache, or reserves a placeholder in the cache to fill.
    ///
    /// Only the first caller will receive a placeholder result; all callers after that will block
    /// until the placeholder is filled (or the placeholder is dropped, at which point the next
    /// caller would get a placeholder). Callers that receive a placeholder should later commit a
    /// node with NodeCache::commit.
    pub async fn get_or_reserve<'a>(&'a self, object_id: u64) -> GetResult<'a> {
        let mut waker_sequence = 0;
        let mut waker_index = 0;
        poll_fn(|cx| {
            let mut this = self.0.lock().unwrap();
            if let Some(node) = this.map.get(&object_id) {
                if let Some(node) = node.upgrade() {
                    if let Ok(placeholder) = node.clone().into_any().downcast::<Placeholder>() {
                        let mut inner = placeholder.0.lock().unwrap();
                        if inner.waker_sequence == waker_sequence {
                            inner.wakers[waker_index] = cx.waker().clone();
                        } else {
                            waker_index = inner.wakers.len();
                            waker_sequence = inner.waker_sequence;
                            inner.wakers.push(cx.waker().clone());
                        }
                        return Poll::Pending;
                    } else {
                        return Poll::Ready(GetResult::Node(node));
                    }
                }
            }
            this.next_waker_sequence += 1;
            let inner = Arc::new(Placeholder(Mutex::new(PlaceholderInner {
                object_id,
                waker_sequence: this.next_waker_sequence,
                wakers: vec![],
            })));
            this.map.insert(object_id, Arc::downgrade(&inner) as Weak<dyn FxNode>);
            Poll::Ready(GetResult::Placeholder(PlaceholderOwner {
                inner,
                committed: false,
                cache: self,
            }))
        })
        .await
    }

    /// Removes a node from the cache. Calling this on a placeholder is an error; instead, the
    /// placeholder should simply be dropped.
    pub fn remove(&self, node: &dyn FxNode) {
        let mut this = self.0.lock().unwrap();
        if let Entry::Occupied(o) = this.map.entry(node.object_id()) {
            // If this method is called when a node is being dropped, then upgrade will fail and
            // it's possible the cache has been populated with another node, so to avoid that race,
            // we must check that the node in the cache is the node we want to remove.
            //
            // Note this ugly cast in place of `std::ptr::eq(o.get().as_ptr(), node)` here is
            // to ensure we don't compare vtable pointers, which are not strictly guaranteed to be
            // the same across casts done in different code generation units at compilation time.
            if o.get().as_ptr() as *const dyn FxNode as *const u8
                == node as *const dyn FxNode as *const u8
            {
                o.remove();
            }
        }
    }

    /// Returns the given node if present in the cache.
    pub fn get(&self, object_id: u64) -> Option<Arc<dyn FxNode>> {
        self.0.lock().unwrap().map.get(&object_id).and_then(Weak::upgrade)
    }

    /// Returns an iterator over all files in the cache.
    pub fn files(&self) -> FileIter<'_> {
        FileIter { cache: self, object_id: None }
    }

    /// Does nothing anymore, but may again in the future.
    pub fn clear(&self) {}

    fn commit(&self, node: Arc<dyn FxNode>) {
        let mut this = self.0.lock().unwrap();
        this.map.insert(node.object_id(), Arc::downgrade(&node));
    }
}

// Wraps a node with an open count.
pub struct OpenedNode<N: FxNode + ?Sized>(Arc<N>);

impl<N: FxNode + ?Sized> OpenedNode<N> {
    pub fn new(node: Arc<N>) -> Self {
        node.open_count_add_one();
        OpenedNode(node)
    }

    /// Downcasts to something that implements FxNode.
    pub fn downcast<T: FxNode>(self) -> Result<OpenedNode<T>, Self> {
        if self.is::<T>() {
            Ok(OpenedNode(
                self.take().into_any().downcast::<T>().unwrap_or_else(|_| unreachable!()),
            ))
        } else {
            Err(self)
        }
    }

    /// Takes the wrapped node.  The caller takes responsibility for dropping the open count.
    pub fn take(self) -> Arc<N> {
        let this = std::mem::ManuallyDrop::new(self);
        unsafe { std::ptr::read(&this.0) }
    }

    /// Returns true if this is an instance of T.
    pub fn is<T: 'static>(&self) -> bool {
        self.0.as_ref().type_id() == TypeId::of::<T>()
    }
}

impl<N: FxNode + ?Sized> Drop for OpenedNode<N> {
    fn drop(&mut self) {
        self.0.open_count_sub_one();
    }
}

impl<N: FxNode + ?Sized> std::ops::Deref for OpenedNode<N> {
    type Target = Arc<N>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

#[cfg(test)]
mod tests {
    use {
        crate::fuchsia::{
            directory::FxDirectory,
            node::{FxNode, GetResult, NodeCache},
        },
        anyhow::Error,
        async_trait::async_trait,
        fuchsia_async as fasync,
        futures::future::join_all,
        fxfs::object_handle::ObjectProperties,
        std::{
            sync::{
                atomic::{AtomicU64, Ordering},
                Arc, Mutex,
            },
            time::Duration,
        },
    };

    struct FakeNode(u64, Arc<NodeCache>);
    #[async_trait]
    impl FxNode for FakeNode {
        fn object_id(&self) -> u64 {
            self.0
        }
        fn parent(&self) -> Option<Arc<FxDirectory>> {
            unreachable!();
        }
        fn set_parent(&self, _parent: Arc<FxDirectory>) {
            unreachable!();
        }
        fn open_count_add_one(&self) {}
        fn open_count_sub_one(&self) {}
        async fn get_properties(&self) -> Result<ObjectProperties, Error> {
            unreachable!();
        }
    }
    impl Drop for FakeNode {
        fn drop(&mut self) {
            self.1.remove(self);
        }
    }

    #[fuchsia::test]
    async fn test_drop_placeholder() {
        let cache = Arc::new(NodeCache::new());
        let object_id = 0u64;
        match cache.get_or_reserve(object_id).await {
            GetResult::Node(_) => panic!("Unexpected node"),
            GetResult::Placeholder(_) => {}
        };
        match cache.get_or_reserve(object_id).await {
            GetResult::Node(_) => panic!("Unexpected node"),
            GetResult::Placeholder(_) => {}
        };
    }

    #[fuchsia::test]
    async fn test_simple() {
        let cache = Arc::new(NodeCache::new());
        let object_id = {
            let node = Arc::new(FakeNode(0, cache.clone()));
            match cache.get_or_reserve(node.object_id()).await {
                GetResult::Node(_) => panic!("Unexpected node"),
                GetResult::Placeholder(p) => {
                    p.commit(&(node.clone() as Arc<dyn FxNode>));
                }
            };
            match cache.get_or_reserve(node.object_id()).await {
                GetResult::Node(n) => assert_eq!(n.object_id(), node.object_id()),
                GetResult::Placeholder(_) => panic!("No node found"),
            };
            node.object_id()
        };
        match cache.get_or_reserve(object_id).await {
            GetResult::Node(_) => panic!("Unexpected node"),
            GetResult::Placeholder(_) => {}
        };
    }

    #[fuchsia::test(threads = 10)]
    async fn test_subsequent_callers_block() {
        let cache = Arc::new(NodeCache::new());
        let object_id = 0u64;
        let writes_to_cache = Arc::new(AtomicU64::new(0));
        let reads_from_cache = Arc::new(AtomicU64::new(0));
        let node = Arc::new(FakeNode(object_id, cache.clone()));
        join_all((0..10).map(|_| {
            let node = node.clone();
            let cache = cache.clone();
            let object_id = object_id.clone();
            let writes_to_cache = writes_to_cache.clone();
            let reads_from_cache = reads_from_cache.clone();
            async move {
                match cache.get_or_reserve(object_id).await {
                    GetResult::Node(node) => {
                        reads_from_cache.fetch_add(1, Ordering::SeqCst);
                        assert_eq!(node.object_id(), object_id);
                    }
                    GetResult::Placeholder(p) => {
                        writes_to_cache.fetch_add(1, Ordering::SeqCst);
                        // Add a delay to simulate doing some work (e.g. loading from disk).
                        fasync::Timer::new(Duration::from_millis(100)).await;
                        p.commit(&(node as Arc<dyn FxNode>));
                    }
                }
            }
        }))
        .await;
        assert_eq!(writes_to_cache.load(Ordering::SeqCst), 1);
        assert_eq!(reads_from_cache.load(Ordering::SeqCst), 9);
    }

    #[fuchsia::test(threads = 10)]
    async fn test_multiple_nodes() {
        const NUM_OBJECTS: usize = 5;
        const TASKS_PER_OBJECT: usize = 4;

        let cache = Arc::new(NodeCache::new());
        let writes = Arc::new(Mutex::new(vec![0u64; NUM_OBJECTS]));
        let reads = Arc::new(Mutex::new(vec![0u64; NUM_OBJECTS]));
        let nodes: Vec<_> = (0..NUM_OBJECTS as u64)
            .map(|object_id| Arc::new(FakeNode(object_id, cache.clone())))
            .collect();

        join_all((0..TASKS_PER_OBJECT).flat_map(|_| {
            nodes.iter().cloned().map(|node| {
                let cache = cache.clone();
                let writes = writes.clone();
                let reads = reads.clone();
                async move {
                    match cache.get_or_reserve(node.object_id()).await {
                        GetResult::Node(result) => {
                            assert_eq!(node.object_id(), result.object_id());
                            reads.lock().unwrap()[node.object_id() as usize] += 1;
                        }
                        GetResult::Placeholder(p) => {
                            writes.lock().unwrap()[node.object_id() as usize] += 1;
                            // Add a delay to simulate doing some work (e.g. loading from disk).
                            fasync::Timer::new(Duration::from_millis(100)).await;
                            p.commit(&(node as Arc<dyn FxNode>));
                        }
                    }
                }
            })
        }))
        .await;
        assert_eq!(*writes.lock().unwrap(), vec![1u64; NUM_OBJECTS]);
        assert_eq!(*reads.lock().unwrap(), vec![TASKS_PER_OBJECT as u64 - 1; NUM_OBJECTS]);
    }
}
