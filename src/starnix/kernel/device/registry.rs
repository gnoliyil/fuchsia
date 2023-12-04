// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    device::kobject::{
        KObject, KObjectDeviceAttribute, KObjectHandle, KType, UEventAction, UEventContext,
    },
    fs::{
        devtmpfs::{devtmpfs_create_device, devtmpfs_remove_child},
        sysfs::{BlockDeviceDirectory, ClassCollectionDirectory, DeviceDirectory, SysFsDirectory},
    },
    task::CurrentTask,
    vfs::{FileOps, FsNode, FsNodeOps, FsStr},
};
use assert_matches::assert_matches;
use starnix_logging::log_error;
use starnix_uapi::{
    device_type::{DeviceType, DYN_MAJOR},
    errno, error,
    errors::Errno,
    open_flags::OpenFlags,
};

use starnix_lock::{MappedMutexGuard, Mutex, MutexGuard, RwLock};
use std::{
    collections::btree_map::{BTreeMap, Entry},
    marker::{Send, Sync},
    sync::{Arc, Weak},
};

use dyn_clone::{clone_trait_object, DynClone};
use range_map::RangeMap;

const CHRDEV_MINOR_MAX: u32 = 256;
const BLKDEV_MINOR_MAX: u32 = 2u32.pow(20);

/// The mode or category of the device driver.
#[derive(Copy, Clone, Ord, PartialOrd, Eq, PartialEq, Debug)]
pub enum DeviceMode {
    Char,
    Block,
}

pub trait DeviceOps: DynClone + Send + Sync + 'static {
    fn open(
        &self,
        _current_task: &CurrentTask,
        _id: DeviceType,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno>;
}

clone_trait_object!(DeviceOps);

// This is a newtype instead of an alias so we can implement traits for it.
#[derive(Clone)]
struct DeviceOpsHandle(Arc<dyn DeviceOps>);

impl PartialEq for DeviceOpsHandle {
    fn eq(&self, _other: &Self) -> bool {
        false
    }
}
impl Eq for DeviceOpsHandle {}

/// Allows directly using a function or closure as an implementation of DeviceOps, avoiding having
/// to write a zero-size struct and an impl for it.
impl<F> DeviceOps for F
where
    F: Clone
        + Send
        + Sync
        + Clone
        + Fn(&CurrentTask, DeviceType, &FsNode, OpenFlags) -> Result<Box<dyn FileOps>, Errno>
        + 'static,
{
    fn open(
        &self,
        current_task: &CurrentTask,
        id: DeviceType,
        node: &FsNode,
        flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        self(current_task, id, node, flags)
    }
}

/// A simple `DeviceOps` function for any device that implements `FileOps + Default`.
pub fn simple_device_ops<T: Default + FileOps + 'static>(
    _current_task: &CurrentTask,
    _id: DeviceType,
    _node: &FsNode,
    _flags: OpenFlags,
) -> Result<Box<dyn FileOps>, Errno> {
    Ok(Box::new(T::default()))
}

// TODO(fxb/128798): It's ideal to support all registered device nodes.
pub fn create_unknown_device(
    _current_task: &CurrentTask,
    _id: DeviceType,
    _node: &FsNode,
    _flags: OpenFlags,
) -> Result<Box<dyn FileOps>, Errno> {
    error!(ENODEV)
}

/// Keys returned by the registration method for `DeviceListener`s that allows to unregister a
/// listener.
pub type DeviceListenerKey = u64;

/// A listener for uevents on devices.
pub trait DeviceListener: Send + Sync {
    fn on_device_event(&self, action: UEventAction, kobject: KObjectHandle, context: UEventContext);
}

struct MajorDevices {
    /// Maps device major number to device implementation.
    map: BTreeMap<u32, RangeMap<u32, DeviceOpsHandle>>,
    mode: DeviceMode,
}

impl MajorDevices {
    fn new(mode: DeviceMode) -> Self {
        Self { map: Default::default(), mode }
    }

    /// Register a `DeviceOps` for a range of minors [`base_minor`, `base_minor` + `minor_count`).
    fn register(
        &mut self,
        major: u32,
        base_minor: u32,
        minor_count: u32,
        ops: impl DeviceOps,
    ) -> Result<(), Errno> {
        let range = base_minor..(base_minor + minor_count);
        let minor_map = self.map.entry(major).or_insert(RangeMap::new());
        if minor_map.intersection(range.clone()).count() == 0 {
            minor_map.insert(range, DeviceOpsHandle(Arc::new(ops)));
            Ok(())
        } else {
            log_error!("Range {:?} overlaps with existing entries in the map.", range);
            error!(EINVAL)
        }
    }

    /// Unregister a range of minors [`base_minor`, `base_minor` + `minor_count`).
    fn unregister(&mut self, major: u32, base_minor: u32, minor_count: u32) -> Result<(), Errno> {
        let range = base_minor..(base_minor + minor_count);
        match self.map.entry(major) {
            Entry::Occupied(minor_map) => {
                minor_map.into_mut().remove(&range);
                Ok(())
            }
            Entry::Vacant(_) => {
                log_error!("No major {} entry registered in the map", major);
                error!(EINVAL)
            }
        }
    }

    /// Register a `DeviceOps` for all minor devices in the `major`.
    fn register_major(&mut self, major: u32, ops: impl DeviceOps) -> Result<(), Errno> {
        let minor_count: u32 = match self.mode {
            DeviceMode::Char => CHRDEV_MINOR_MAX,
            DeviceMode::Block => BLKDEV_MINOR_MAX,
        };
        self.register(major, 0, minor_count, ops)
    }

    /// Unregister all minor devices in the `major`.
    fn unregister_major(&mut self, major: u32) -> Result<(), Errno> {
        let minor_count: u32 = match self.mode {
            DeviceMode::Char => CHRDEV_MINOR_MAX,
            DeviceMode::Block => BLKDEV_MINOR_MAX,
        };
        self.unregister(major, 0, minor_count)
    }

    fn get(&self, id: DeviceType) -> Result<Arc<dyn DeviceOps>, Errno> {
        match self.map.get(&id.major()) {
            Some(minor_map) => match minor_map.get(&id.minor()) {
                Some(entry) => Ok(Arc::clone(&entry.1 .0)),
                None => error!(ENODEV),
            },
            None => error!(ENODEV),
        }
    }
}

/// The kernel's registry of drivers.
pub struct DeviceRegistry {
    root_kobject: KObjectHandle,
    class_subsystem_kobject: KObjectHandle,
    state: Mutex<DeviceRegistryState>,
}

struct DeviceRegistryState {
    char_devices: MajorDevices,
    block_devices: MajorDevices,
    dyn_devices: Arc<RwLock<DynRegistry>>,
    next_anon_minor: u32,
    listeners: BTreeMap<u64, Box<dyn DeviceListener>>,
    next_listener_id: u64,
    next_event_id: u64,
}

impl DeviceRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    /// Returns the root kobject.
    pub fn root_kobject(&self) -> KObjectHandle {
        self.root_kobject.clone()
    }

    pub fn class_subsystem_kobject(&self) -> KObjectHandle {
        self.class_subsystem_kobject.clone()
    }

    pub fn block_collection(&self) -> KObjectHandle {
        self.class_subsystem_kobject.get_or_create_child(
            b"block",
            KType::Collection,
            ClassCollectionDirectory::new,
        )
    }

    /// Returns the virtual bus kobject where all virtual and pseudo devices are stored.
    pub fn virtual_bus(&self) -> KObjectHandle {
        self.root_kobject.get_or_create_child(b"virtual", KType::Bus, SysFsDirectory::new)
    }

    pub fn add_class(&self, name: &FsStr, bus: KObjectHandle) -> KObjectHandle {
        let class = bus.get_or_create_child(name, KType::Class, SysFsDirectory::new);
        self.class_subsystem_kobject.get_or_create_child(
            name,
            KType::Collection,
            ClassCollectionDirectory::new,
        );
        class
    }

    pub fn add_device(
        &self,
        current_task: &CurrentTask,
        dev_attr: KObjectDeviceAttribute,
    ) -> KObjectHandle {
        match dev_attr.device.mode {
            DeviceMode::Char => {
                self.add_device_with_directory(current_task, dev_attr, DeviceDirectory::new)
            }
            DeviceMode::Block => {
                self.add_device_with_directory(current_task, dev_attr, BlockDeviceDirectory::new)
            }
        }
    }

    pub fn add_device_with_directory<F, N>(
        &self,
        current_task: &CurrentTask,
        dev_attr: KObjectDeviceAttribute,
        create_fs_node_ops: F,
    ) -> KObjectHandle
    where
        F: Fn(Weak<KObject>) -> N + Send + Sync + 'static,
        N: FsNodeOps,
    {
        let ktype = KType::Device(dev_attr.device.clone());
        let kobj_device =
            dev_attr.class.get_or_create_child(&dev_attr.name, ktype, create_fs_node_ops);
        self.class_subsystem_kobject
            .get_child(&dev_attr.class.name())
            .expect("no associated collection exists")
            .insert_child(kobj_device.clone());

        if let Err(err) = devtmpfs_create_device(current_task, dev_attr.device.clone()) {
            log_error!("Cannot add device {:?} in devtmpfs ({:?})", dev_attr.device, err);
        }

        self.dispatch_uevent(UEventAction::Add, kobj_device.clone());
        kobj_device
    }

    pub fn add_and_register_device(
        &self,
        current_task: &CurrentTask,
        dev_attr: KObjectDeviceAttribute,
        dev_ops: impl DeviceOps,
    ) -> KObjectHandle {
        if let Err(err) = self.register_device(
            dev_attr.device.device_type.major(),
            dev_attr.device.device_type.minor(),
            1,
            dev_ops,
            dev_attr.device.mode,
        ) {
            log_error!("Cannot register device {:?} ({:?})", dev_attr.device, err);
        }
        self.add_device(current_task, dev_attr)
    }

    pub fn remove_device(&self, current_task: &CurrentTask, device: KObjectHandle) {
        assert_matches!(device.ktype(), KType::Device(_));
        if let KType::Device(metadata) = device.ktype() {
            device.remove();
            self.class_subsystem_kobject
                .get_child(&metadata.class.upgrade().expect("class kobject should be valid").name())
                .expect("associated collection should exist")
                .remove_child(&device.name());
            self.dispatch_uevent(UEventAction::Remove, device.clone());

            devtmpfs_remove_child(current_task, &metadata.name);
        }
    }

    fn major_devices(&self, mode: DeviceMode) -> MappedMutexGuard<'_, MajorDevices> {
        MutexGuard::map(self.state.lock(), |state| match mode {
            DeviceMode::Char => &mut state.char_devices,
            DeviceMode::Block => &mut state.block_devices,
        })
    }

    pub fn register_device(
        &self,
        major: u32,
        base_minor: u32,
        minor_count: u32,
        ops: impl DeviceOps,
        mode: DeviceMode,
    ) -> Result<(), Errno> {
        self.major_devices(mode).register(major, base_minor, minor_count, ops)
    }

    pub fn unregister_device(
        &self,
        major: u32,
        base_minor: u32,
        minor_count: u32,
        mode: DeviceMode,
    ) -> Result<(), Errno> {
        self.major_devices(mode).unregister(major, base_minor, minor_count)
    }

    pub fn register_major(
        &self,
        major: u32,
        device: impl DeviceOps,
        mode: DeviceMode,
    ) -> Result<(), Errno> {
        self.major_devices(mode).register_major(major, device)
    }

    pub fn unregister_major(&self, major: u32, mode: DeviceMode) -> Result<(), Errno> {
        self.major_devices(mode).unregister_major(major)
    }

    pub fn register_dyn_chrdev(&self, device: impl DeviceOps) -> Result<DeviceType, Errno> {
        self.state.lock().dyn_devices.write().register(device)
    }

    pub fn next_anonymous_dev_id(&self) -> DeviceType {
        let mut state = self.state.lock();
        let id = DeviceType::new(0, state.next_anon_minor);
        state.next_anon_minor += 1;
        id
    }

    /// Register a new listener for uevents on devices.
    ///
    /// Returns a key used to unregister the listener.
    pub fn register_listener(&self, listener: impl DeviceListener + 'static) -> DeviceListenerKey {
        let mut state = self.state.lock();
        let key = state.next_listener_id;
        state.next_listener_id += 1;
        state.listeners.insert(key, Box::new(listener));
        key
    }

    /// Unregister a listener previously registered through `register_listener`.
    pub fn unregister_listener(&self, key: &DeviceListenerKey) {
        self.state.lock().listeners.remove(key);
    }

    /// Dispatch an uevent for the given `device`.
    pub fn dispatch_uevent(&self, action: UEventAction, kobject: KObjectHandle) {
        let mut state = self.state.lock();
        let event_id = state.next_event_id;
        state.next_event_id += 1;
        let context = UEventContext { seqnum: event_id };
        for listener in state.listeners.values() {
            listener.on_device_event(action, kobject.clone(), context);
        }
    }

    /// Opens a device file corresponding to the device identifier `dev`.
    pub fn open_device(
        &self,
        current_task: &CurrentTask,
        node: &FsNode,
        flags: OpenFlags,
        id: DeviceType,
        mode: DeviceMode,
    ) -> Result<Box<dyn FileOps>, Errno> {
        let device_ops = {
            // Access the actual devices in a nested scope to avoid holding the state lock while
            // creating the FileOps from any single DeviceOps.
            let state = self.state.lock();
            let devices = match mode {
                DeviceMode::Char => &state.char_devices,
                DeviceMode::Block => &state.block_devices,
            };
            devices.get(id)?
        };
        device_ops.open(current_task, id, node, flags)
    }
}

impl Default for DeviceRegistry {
    fn default() -> Self {
        let mut state = DeviceRegistryState {
            char_devices: MajorDevices::new(DeviceMode::Char),
            block_devices: MajorDevices::new(DeviceMode::Block),
            dyn_devices: Default::default(),
            next_anon_minor: 1,
            listeners: Default::default(),
            next_listener_id: 0,
            next_event_id: 0,
        };
        state
            .char_devices
            .register_major(DYN_MAJOR, Arc::clone(&state.dyn_devices))
            .expect("Failed to register DYN_MAJOR");
        Self {
            root_kobject: KObject::new_root(),
            class_subsystem_kobject: KObject::new_root(),
            state: Mutex::new(state),
        }
    }
}

#[derive(Default)]
struct DynRegistry {
    dyn_devices: BTreeMap<u32, Box<dyn DeviceOps>>,
    next_dynamic_minor: u32,
}

impl DynRegistry {
    fn register(&mut self, device: impl DeviceOps) -> Result<DeviceType, Errno> {
        // TODO(quiche): Emit a `uevent` to notify userspace of the new device.

        let minor = self.next_dynamic_minor;
        if minor > 255 {
            return error!(ENOMEM);
        }
        self.next_dynamic_minor += 1;
        self.dyn_devices.insert(minor, Box::new(device));
        Ok(DeviceType::new(DYN_MAJOR, minor))
    }
}

impl DeviceOps for Arc<RwLock<DynRegistry>> {
    fn open(
        &self,
        current_task: &CurrentTask,
        id: DeviceType,
        node: &FsNode,
        flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        let state = self.read();
        let device = state.dyn_devices.get(&id.minor()).ok_or_else(|| errno!(ENODEV))?;
        device.open(current_task, id, node, flags)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{device::mem::DevNull, testing::*, vfs::*};
    use starnix_uapi::device_type::{INPUT_MAJOR, MEM_MAJOR};

    #[::fuchsia::test]
    fn registry_fails_to_add_duplicate_device() {
        let registry = DeviceRegistry::default();
        registry
            .register_major(MEM_MAJOR, simple_device_ops::<DevNull>, DeviceMode::Char)
            .expect("registers once");
        registry
            .register_major(123, simple_device_ops::<DevNull>, DeviceMode::Char)
            .expect("registers unique");
        registry
            .register_major(MEM_MAJOR, simple_device_ops::<DevNull>, DeviceMode::Char)
            .expect_err("fail to register duplicate");
    }

    #[::fuchsia::test]
    async fn registry_opens_device() {
        let (_kernel, current_task) = create_kernel_and_task();

        let registry = DeviceRegistry::default();
        registry
            .register_major(MEM_MAJOR, simple_device_ops::<DevNull>, DeviceMode::Char)
            .expect("registers unique");

        let node = FsNode::new_root(PanickingFsNode);

        // Fail to open non-existent device.
        assert!(registry
            .open_device(
                &current_task,
                &node,
                OpenFlags::RDONLY,
                DeviceType::NONE,
                DeviceMode::Char
            )
            .is_err());

        // Fail to open in wrong mode.
        assert!(registry
            .open_device(
                &current_task,
                &node,
                OpenFlags::RDONLY,
                DeviceType::NULL,
                DeviceMode::Block
            )
            .is_err());

        // Open in correct mode.
        let _ = registry
            .open_device(
                &current_task,
                &node,
                OpenFlags::RDONLY,
                DeviceType::NULL,
                DeviceMode::Char,
            )
            .expect("opens device");
    }

    #[::fuchsia::test]
    async fn test_dynamic_misc() {
        let (_kernel, current_task) = create_kernel_and_task();

        fn create_test_device(
            _current_task: &CurrentTask,
            _id: DeviceType,
            _node: &FsNode,
            _flags: OpenFlags,
        ) -> Result<Box<dyn FileOps>, Errno> {
            Ok(Box::new(PanickingFile))
        }

        let registry = DeviceRegistry::default();
        let device_type = registry.register_dyn_chrdev(create_test_device).unwrap();
        assert_eq!(device_type.major(), DYN_MAJOR);

        let node = FsNode::new_root(PanickingFsNode);
        let _ = registry
            .open_device(&current_task, &node, OpenFlags::RDONLY, device_type, DeviceMode::Char)
            .expect("opens device");
    }

    #[::fuchsia::test]
    async fn test_add_class() {
        let (kernel, current_task) = create_kernel_and_task();
        let registry = &kernel.device_registry;

        let input_class = registry.add_class(b"input", registry.virtual_bus());
        registry.add_device(
            &current_task,
            KObjectDeviceAttribute::new(
                input_class,
                b"mice",
                b"mice",
                DeviceType::new(INPUT_MAJOR, 0),
                DeviceMode::Char,
            ),
        );

        assert!(registry.class_subsystem_kobject().has_child(b"input"));
        assert!(registry
            .class_subsystem_kobject()
            .get_child(b"input")
            .and_then(|collection| collection.get_child(b"mice"))
            .is_some());
    }

    #[::fuchsia::test]
    async fn registry_remove_device() {
        let (kernel, current_task) = create_kernel_and_task();
        let registry = &kernel.device_registry;

        let input_class = registry.add_class(b"input", registry.virtual_bus());
        let mice_dev = registry.add_device(
            &current_task,
            KObjectDeviceAttribute::new(
                input_class.clone(),
                b"mice",
                b"mice",
                DeviceType::new(INPUT_MAJOR, 0),
                DeviceMode::Char,
            ),
        );

        registry.remove_device(&current_task, mice_dev);
        assert!(!input_class.has_child(b"mice"));
        assert!(!registry
            .class_subsystem_kobject()
            .get_child(b"input")
            .expect("get input collection")
            .has_child(b"mice"));
    }

    #[::fuchsia::test]
    async fn registery_unregister_device() {
        let (kernel, current_task) = create_kernel_and_task();
        let registry = &kernel.device_registry;
        let node = FsNode::new_root(PanickingFsNode);
        let _ = registry
            .open_device(
                &current_task,
                &node,
                OpenFlags::RDONLY,
                DeviceType::NULL,
                DeviceMode::Char,
            )
            .expect("opens device");

        let _ = registry.unregister_major(MEM_MAJOR, DeviceMode::Char);
        assert!(registry
            .open_device(
                &current_task,
                &node,
                OpenFlags::RDONLY,
                DeviceType::NULL,
                DeviceMode::Char,
            )
            .is_err());
    }
}
