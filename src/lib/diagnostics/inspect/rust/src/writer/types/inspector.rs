// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::{private::InspectTypeInternal, Error, Heap, Node, State};
use diagnostics_hierarchy::{
    testing::{DiagnosticsHierarchyGetter, JsonGetter},
    DiagnosticsHierarchy,
};
use inspect_format::{constants, Container};
use std::{borrow::Cow, cmp::max, default::Default, fmt, sync::Arc};
use tracing::error;

#[cfg(target_os = "fuchsia")]
use {
    fuchsia_zircon::{self as zx, HandleBased},
    mapped_vmo::Mapping,
};

#[cfg(not(target_os = "fuchsia"))]
use std::sync::Mutex;

/// Root of the Inspect API. Through this API, further nodes can be created and inspect can be
/// served.
#[derive(Clone)]
pub struct Inspector {
    /// The root node.
    root_node: Arc<Node>,

    /// The VMO backing the inspector
    #[cfg(target_os = "fuchsia")]
    pub(crate) vmo: Option<Arc<zx::Vmo>>,
}

impl fmt::Debug for Inspector {
    fn fmt(&self, fmt: &mut fmt::Formatter<'_>) -> fmt::Result {
        let result = if fmt.alternate() { self.get_pretty_json() } else { self.get_json() };

        fmt.write_str(&result)
    }
}

impl DiagnosticsHierarchyGetter<String> for Inspector {
    fn get_diagnostics_hierarchy(&self) -> Cow<'_, DiagnosticsHierarchy> {
        let hierarchy = futures::executor::block_on(async move { crate::reader::read(self).await })
            .expect("failed to get hierarchy");
        Cow::Owned(hierarchy)
    }
}

#[cfg(target_os = "fuchsia")]
impl Inspector {
    /// Returns a duplicate of the underlying VMO for this Inspector.
    ///
    /// The duplicated VMO will be read-only, and is suitable to send to clients over FIDL.
    pub fn duplicate_vmo(&self) -> Option<zx::Vmo> {
        self.vmo.as_ref().and_then(|vmo| {
            vmo.duplicate_handle(
                zx::Rights::BASIC | zx::Rights::READ | zx::Rights::MAP | zx::Rights::GET_PROPERTY,
            )
            .ok()
        })
    }

    /// This produces a copy-on-write VMO with a generation count marked as
    /// VMO_FROZEN. The resulting VMO is read-only.
    ///
    /// Failure
    /// This function returns `None` for failure. That can happen for
    /// a few reasons.
    ///   1) It is a semantic error to freeze a VMO while an atomic transaction
    ///      is in progress, because that transaction is supposed to be atomic.
    ///   2) VMO errors. This can include running out of space or debug assertions.
    ///
    /// Note: the generation count for the original VMO is updated immediately. Since
    /// the new VMO is page-by-page copy-on-write, at least the first page of the
    /// VMO will immediately do a true copy. The practical implications of this
    /// depend on implementation details like how large a VMO is versus page size.
    pub fn frozen_vmo_copy(&self) -> Option<zx::Vmo> {
        self.state()?.try_lock().ok().and_then(|mut state| state.frozen_vmo_copy().ok()).flatten()
    }

    /// Returns a VMO holding a copy of the data in this inspector.
    ///
    /// The copied VMO will be read-only.
    pub fn copy_vmo(&self) -> Option<zx::Vmo> {
        self.copy_vmo_data().and_then(|data| {
            if let Ok(vmo) = zx::Vmo::create(data.len() as u64) {
                vmo.write(&data, 0).ok().map(|_| vmo)
            } else {
                None
            }
        })
    }

    /// Returns Ok(()) if VMO is frozen, and the generation count if it is not.
    /// Very unsafe. Propagates unrelated errors by panicking.
    #[cfg(test)]
    pub fn is_frozen(&self) -> Result<(), u64> {
        use inspect_format::Block;
        let vmo = self.vmo.as_ref().unwrap();
        let mut buffer: [u8; 16] = [0; 16];
        vmo.read(&mut buffer, 0).unwrap();
        let block = Block::new(&buffer[..16], 0);
        if block.header_generation_count().unwrap() == constants::VMO_FROZEN {
            Ok(())
        } else {
            Err(block.header_generation_count().unwrap())
        }
    }
}

impl Default for Inspector {
    fn default() -> Self {
        Inspector::new(InspectorConfig::default())
    }
}

impl Inspector {
    /// Initializes a new Inspect VMO object with the
    /// [`default maximum size`][constants::DEFAULT_VMO_SIZE_BYTES].
    pub fn new(conf: InspectorConfig) -> Self {
        conf.build()
    }

    /// Returns a copy of the bytes stored in the VMO for this inspector.
    ///
    /// The output will be truncated to only those bytes that are needed to accurately read the
    /// stored data.
    pub fn copy_vmo_data(&self) -> Option<Vec<u8>> {
        self.root_node.inner.inner_ref().and_then(|inner_ref| inner_ref.state.copy_vmo_bytes())
    }

    pub fn clone_heap_container(&self) -> Option<Container> {
        self.root_node.inner.inner_ref().map(|inner_ref| inner_ref.state.clone_container())
    }

    pub fn max_size(&self) -> Option<usize> {
        self.state()?.try_lock().ok().map(|state| state.stats().maximum_size)
    }

    /// True if the Inspector was created successfully (it's not No-Op)
    pub fn is_valid(&self) -> bool {
        // It is only necessary to check the root_node, because:
        //   1) If the Inspector was created as a no-op, the root node is not valid.
        //   2) If the creation of the Inspector failed, then the root_node is invalid. This
        //      is because `Inspector::new_root` returns the VMO and root node as a pair.
        self.root_node.is_valid()
    }

    /// Returns the root node of the inspect hierarchy.
    pub fn root(&self) -> &Node {
        &self.root_node
    }

    /// Takes a function to execute as under a single lock of the Inspect VMO. This function
    /// receives a reference to the root of the inspect hierarchy.
    pub fn atomic_update<F, R>(&self, update_fn: F) -> R
    where
        F: FnOnce(&Node) -> R,
    {
        self.root().atomic_update(update_fn)
    }

    pub(crate) fn state(&self) -> Option<State> {
        self.root().inner.inner_ref().map(|inner_ref| inner_ref.state.clone())
    }
}

/// Classic builder pattern object for constructing an `Inspector`.
pub struct InspectorConfig {
    is_no_op: bool,
    size: usize,

    #[cfg(target_os = "fuchsia")]
    vmo: Option<Arc<zx::Vmo>>,
}

impl Default for InspectorConfig {
    /// A default Inspector:
    ///     * Fully functional
    ///     * Size: `constants::DEFAULT_VMO_SIZE_BYTES`
    ///
    /// Because the default is so cheap to construct, there is
    /// no "empty" `InspectorConfig`.
    fn default() -> Self {
        Self {
            is_no_op: false,
            size: constants::DEFAULT_VMO_SIZE_BYTES,
            #[cfg(target_os = "fuchsia")]
            vmo: None,
        }
    }
}

impl InspectorConfig {
    /// A read-only Inspector.
    pub fn no_op(mut self) -> Self {
        self.is_no_op = true;
        self
    }

    /// Size of the VMO.
    pub fn size(mut self, max_size: usize) -> Self {
        self.size = max_size;
        self
    }

    fn create_no_op(self) -> Inspector {
        return Inspector {
            #[cfg(target_os = "fuchsia")]
            vmo: self.vmo,
            root_node: Arc::new(Node::new_no_op()),
        };
    }

    fn adjusted_buffer_size(max_size: usize) -> usize {
        let mut size = max(constants::MINIMUM_VMO_SIZE_BYTES, max_size);
        // If the size is not a multiple of 4096, round up.
        if size % constants::MINIMUM_VMO_SIZE_BYTES != 0 {
            size =
                (1 + size / constants::MINIMUM_VMO_SIZE_BYTES) * constants::MINIMUM_VMO_SIZE_BYTES;
        }

        size
    }
}

#[cfg(target_os = "fuchsia")]
impl InspectorConfig {
    /// An Inspector with a readable VMO.
    /// Implicitly no-op.
    pub fn vmo(mut self, vmo: Arc<zx::Vmo>) -> Self {
        self.vmo = Some(vmo);
        self.no_op()
    }

    fn build(self) -> Inspector {
        if self.is_no_op {
            return Self::create_no_op(self);
        }

        match Self::new_root(self.size) {
            Ok((vmo, root_node)) => Inspector { vmo: Some(vmo), root_node: Arc::new(root_node) },
            Err(e) => {
                error!("Failed to create root node. Error: {:?}", e);
                return Self::create_no_op(self);
            }
        }
    }

    /// Allocates a new VMO and initializes it.
    fn new_root(max_size: usize) -> Result<(Arc<zx::Vmo>, Node), Error> {
        let size = Self::adjusted_buffer_size(max_size);
        let (mapping, vmo) = Mapping::allocate_with_name(size, "InspectHeap")
            .map_err(|status| Error::AllocateVmo(status))?;
        let vmo = Arc::new(vmo);
        let heap = Heap::new(Arc::new(mapping)).map_err(|e| Error::CreateHeap(Box::new(e)))?;
        let state =
            State::create(heap, vmo.clone()).map_err(|e| Error::CreateState(Box::new(e)))?;
        Ok((vmo, Node::new_root(state)))
    }
}

#[cfg(not(target_os = "fuchsia"))]
impl InspectorConfig {
    fn build(self) -> Inspector {
        if self.is_no_op {
            return Self::create_no_op(self);
        }

        match Self::new_root(self.size) {
            Ok(root_node) => Inspector { root_node: Arc::new(root_node) },
            Err(e) => {
                error!("Failed to create root node. Error: {:?}", e);
                return Self::create_no_op(self);
            }
        }
    }

    fn new_root(max_size: usize) -> Result<Node, Error> {
        let size = Self::adjusted_buffer_size(max_size);
        let mut backer = Vec::<u8>::new();
        backer.resize(size, 0);
        let heap =
            Heap::new(Arc::new(Mutex::new(backer))).map_err(|e| Error::CreateHeap(Box::new(e)))?;
        let state = State::create(heap).map_err(|e| Error::CreateState(Box::new(e)))?;
        Ok(Node::new_root(state))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use futures::FutureExt;

    #[fuchsia::test]
    fn debug_impl() {
        let inspector = Inspector::default();
        inspector.root().record_int("name", 5);

        assert_eq!(format!("{:?}", &inspector), r#"{"root":{"name":5}}"#);

        let pretty = r#"{
  "root": {
    "name": 5
  }
}"#;
        assert_eq!(format!("{:#?}", &inspector), pretty);

        let two = inspector.root().create_child("two");
        two.record_lazy_child("two_child", || {
            let insp = Inspector::default();
            insp.root().record_double("double", 1.0);

            async move { Ok(insp) }.boxed()
        });

        let pretty = r#"{
  "root": {
    "name": 5,
    "two": {
      "two_child": {
        "double": 1.0
      }
    }
  }
}"#;
        assert_eq!(format!("{:#?}", &inspector), pretty);
    }

    #[fuchsia::test]
    fn inspector_new() {
        let test_object = Inspector::default();
        assert_eq!(test_object.max_size().unwrap(), constants::DEFAULT_VMO_SIZE_BYTES);
    }

    #[fuchsia::test]
    fn inspector_copy_data() {
        let test_object = Inspector::default();

        assert_eq!(test_object.max_size().unwrap(), constants::DEFAULT_VMO_SIZE_BYTES);

        // The copy will be a single page, since that is all that is used.
        assert_eq!(test_object.copy_vmo_data().unwrap().len(), 4096);
    }

    #[fuchsia::test]
    fn no_op() {
        let inspector = Inspector::new(InspectorConfig::default().size(4096));
        // Make the VMO full.
        let nodes = (0..127).map(|_| inspector.root().create_child("test")).collect::<Vec<Node>>();

        assert!(nodes.iter().all(|node| node.is_valid()));
        let no_op_node = inspector.root().create_child("no-op-child");
        assert!(!no_op_node.is_valid());
    }

    #[fuchsia::test]
    fn inspector_new_with_size() {
        let test_object = Inspector::new(InspectorConfig::default().size(8192));
        assert_eq!(test_object.max_size().unwrap(), 8192);

        // If size is not a multiple of 4096, it'll be rounded up.
        let test_object = Inspector::new(InspectorConfig::default().size(10000));
        assert_eq!(test_object.max_size().unwrap(), 12288);

        // If size is less than the minimum size, the minimum will be set.
        let test_object = Inspector::new(InspectorConfig::default().size(2000));
        assert_eq!(test_object.max_size().unwrap(), 4096);
    }

    #[fuchsia::test]
    async fn atomic_update() {
        let insp = Inspector::default();
        let gen = insp.state().unwrap().generation_count();
        insp.atomic_update(|n| {
            n.record_int("", 1);
            n.record_int("", 2);
            n.record_uint("", 3);
            n.record_string("", "abcd");
        });

        assert_eq!(gen + 2, insp.state().unwrap().generation_count());
    }
}

// These tests exercise Fuchsia-specific APIs for Inspector.
#[cfg(all(test, target_os = "fuchsia"))]
mod fuchsia_tests {
    use super::*;
    use fuchsia_zircon::AsHandleRef;
    use std::ffi::CString;

    #[fuchsia::test]
    fn inspector_duplicate_vmo() {
        let test_object = Inspector::default();
        assert_eq!(
            test_object.vmo.as_ref().unwrap().get_size().unwrap(),
            constants::DEFAULT_VMO_SIZE_BYTES as u64
        );
        assert_eq!(
            test_object.duplicate_vmo().unwrap().get_size().unwrap(),
            constants::DEFAULT_VMO_SIZE_BYTES as u64
        );
    }

    #[fuchsia::test]
    fn inspector_new_root() {
        // Note, the small size we request should be rounded up to a full 4kB page.
        let (vmo, root_node) = InspectorConfig::new_root(100).unwrap();
        assert_eq!(vmo.get_size().unwrap(), 4096);
        let inner = root_node.inner.inner_ref().unwrap();
        assert_eq!(inner.block_index, 0);
        assert_eq!(CString::new("InspectHeap").unwrap(), vmo.get_name().expect("Has name"));
    }

    #[fuchsia::test]
    fn freeze_vmo_works() {
        let inspector = Inspector::default();
        let initial = inspector.state().unwrap().header.header_generation_count().unwrap();
        let vmo = inspector.frozen_vmo_copy();

        let is_frozen_result = inspector.is_frozen();
        assert!(is_frozen_result.is_err());

        assert_eq!(initial + 2, is_frozen_result.err().unwrap());
        assert!(is_frozen_result.err().unwrap() % 2 == 0);

        let frozen_insp =
            Inspector::new(InspectorConfig::default().no_op().vmo(Arc::new(vmo.unwrap())));
        assert!(frozen_insp.is_frozen().is_ok());
    }

    #[fuchsia::test]
    fn transactions_block_freezing() {
        let inspector = Inspector::default();
        inspector.atomic_update(|_| assert!(inspector.frozen_vmo_copy().is_none()));
    }

    #[fuchsia::test]
    fn transactions_block_copying() {
        let inspector = Inspector::default();
        inspector.atomic_update(|_| assert!(inspector.copy_vmo().is_none()));
        inspector.atomic_update(|_| assert!(inspector.copy_vmo_data().is_none()));
    }

    #[fuchsia::test]
    fn inspector_new_with_size() {
        let test_object = Inspector::new(InspectorConfig::default().size(8192));
        assert_eq!(test_object.max_size().unwrap(), 8192);

        assert_eq!(
            CString::new("InspectHeap").unwrap(),
            test_object.vmo.as_ref().unwrap().get_name().expect("Has name")
        );

        // If size is not a multiple of 4096, it'll be rounded up.
        let test_object = Inspector::new(InspectorConfig::default().size(10000));
        assert_eq!(test_object.max_size().unwrap(), 12288);

        // If size is less than the minimum size, the minimum will be set.
        let test_object = Inspector::new(InspectorConfig::default().size(2000));
        assert_eq!(test_object.max_size().unwrap(), 4096);
    }
}
