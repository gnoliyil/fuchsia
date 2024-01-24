// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    device::{
        kobject::DeviceMetadata, loop_device::create_loop_control_device, mem::DevRandom,
        simple_device_ops, uinput::create_uinput_device, DeviceMode,
    },
    fs::sysfs::DeviceDirectory,
    task::CurrentTask,
    vfs::{create_stub_device_with_bug, fuse::open_fuse_device},
};
use starnix_uapi::device_type::DeviceType;

pub fn misc_device_init(current_task: &CurrentTask) {
    let kernel = current_task.kernel();
    let registry = &kernel.device_registry;
    let misc_class = registry.get_or_create_class("misc".into(), registry.virtual_bus());
    registry.add_and_register_device(
        current_task,
        "hwrng".into(),
        DeviceMetadata::new("hwrng".into(), DeviceType::HW_RANDOM, DeviceMode::Char),
        misc_class.clone(),
        DeviceDirectory::new,
        simple_device_ops::<DevRandom>,
    );
    registry.add_and_register_device(
        current_task,
        "fuse".into(),
        DeviceMetadata::new("fuse".into(), DeviceType::FUSE, DeviceMode::Char),
        misc_class.clone(),
        DeviceDirectory::new,
        open_fuse_device,
    );
    registry.add_and_register_device(
        current_task,
        "device-mapper".into(),
        DeviceMetadata::new("mapper/control".into(), DeviceType::DEVICE_MAPPER, DeviceMode::Char),
        misc_class.clone(),
        DeviceDirectory::new,
        create_stub_device_with_bug("device mapper control", "https://fxbug.dev/297432471"),
    );
    registry.add_and_register_device(
        current_task,
        "loop-control".into(),
        DeviceMetadata::new("loop-control".into(), DeviceType::LOOP_CONTROL, DeviceMode::Char),
        misc_class.clone(),
        DeviceDirectory::new,
        create_loop_control_device,
    );
    registry.add_and_register_device(
        current_task,
        "uinput".into(),
        DeviceMetadata::new("uinput".into(), DeviceType::UINPUT, DeviceMode::Char),
        misc_class,
        DeviceDirectory::new,
        create_uinput_device,
    );
}
