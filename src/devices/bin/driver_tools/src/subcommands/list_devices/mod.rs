// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod args;

use {
    crate::common::{node_property_key_to_string, node_property_value_to_string},
    anyhow::{anyhow, Result},
    args::ListDevicesCommand,
    fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_device_manager as fdm,
    fidl_fuchsia_driver_development as fdd,
    fuchsia_driver_dev::{self, DFv1Device, DFv2Node, Device},
};

trait DevicePrinter {
    fn print(&self) -> Result<()>;
    fn print_verbose(&self) -> Result<()>;
}

impl DevicePrinter for DFv1Device {
    fn print(&self) -> Result<()> {
        if let Some(ref topo_path) = self.0.topological_path {
            println!("{}", topo_path);
        }
        Ok(())
    }

    fn print_verbose(&self) -> Result<()> {
        let topo_path = self
            .0
            .topological_path
            .as_deref()
            .map(|s| s.strip_prefix("/dev/").unwrap().to_string())
            .unwrap_or("".to_string());
        let (_, name) = topo_path.rsplit_once('/').unwrap_or(("", &topo_path));
        println!("{0: <9}: {1}", "Name", name);
        println!("{0: <9}: {1}", "Topo Path", topo_path);
        println!("{0: <9}: {1}", "Driver", self.0.bound_driver_libname.as_deref().unwrap_or(""));
        println!(
            "{0: <9}: {1:?}",
            "Flags",
            self.0.flags.as_ref().unwrap_or(&fdd::DeviceFlags::empty())
        );
        if let Some(protocol_id) = self.0.protocol_id {
            println!(
                "{0: <9}: {1} ({2})",
                "Proto",
                self.0.protocol_name.as_deref().unwrap_or("none"),
                protocol_id
            );
        }
        if let Some(ref property_list) = self.0.property_list {
            let count = property_list.props.len();
            println!("{} Properties", count);
            let mut idx = 1;
            for prop in property_list.props.iter() {
                let id_name = bind::compiler::get_deprecated_key_identifiers()
                    .get(&(prop.id as u32))
                    .map(std::clone::Clone::clone)
                    .unwrap_or_else(|| format!("{:#08}", prop.id));
                println!(
                    "[{0: >2}/ {1: >2}] : Key {2:30} Value {3:#08x}",
                    idx, count, id_name, prop.value,
                );
                idx += 1;
            }
            let count = property_list.str_props.len();
            println!("{} String Properties", count);
            idx = 1;
            for prop in property_list.str_props.iter() {
                println!(
                    "[{0: >2}/ {1: >2}] : Key {2:30} Value {3:?}",
                    idx,
                    count,
                    prop.key,
                    match prop.value {
                        fdm::PropertyValue::IntValue(value) => format!("{:#08x}", value),
                        fdm::PropertyValue::StrValue(ref value) => format!("{}", value),
                        fdm::PropertyValue::BoolValue(value) => value.to_string(),
                        fdm::PropertyValue::EnumValue(ref value) => format!("Enum({})", value),
                    }
                );
                idx += 1;
            }
        } else {
            println!("0 Properties");
            println!("0 String Properties");
        }
        println!("");
        Ok(())
    }
}

impl DevicePrinter for DFv2Node {
    fn print(&self) -> Result<()> {
        println!("{}", self.0.moniker.as_ref().expect("DFv2 node does not have a moniker"));
        Ok(())
    }

    fn print_verbose(&self) -> Result<()> {
        let moniker = self.0.moniker.as_deref().expect("DFv2 node does not have a moniker");
        let (_, name) = moniker.rsplit_once('.').unwrap_or(("", &moniker));
        println!("{0: <9}: {1}", "Name", name);
        println!("{0: <9}: {1}", "Moniker", moniker);
        println!("{0: <9}: {1}", "Driver", &self.0.bound_driver_url.as_deref().unwrap_or("None"));
        if let Some(ref node_property_list) = self.0.node_property_list {
            println!("{} Properties", node_property_list.len());
            for i in 0..node_property_list.len() {
                let node_property = &node_property_list[i];
                println!(
                    "[{:>2}/ {:>2}] : Key {:30} Value {}",
                    i + 1,
                    node_property_list.len(),
                    node_property
                        .key
                        .as_ref()
                        .map(|key| node_property_key_to_string(key))
                        .unwrap_or("None".to_owned()),
                    node_property
                        .value
                        .as_ref()
                        .map(|value| node_property_value_to_string(value))
                        .unwrap_or("None".to_owned()),
                );
            }
        } else {
            println!("0 Properties");
        }

        if let Some(ref offer_list) = self.0.offer_list {
            println!("{} Offers", offer_list.len());
            for i in 0..offer_list.len() {
                if let fdecl::Offer::Service(service) = &offer_list[i] {
                    println!(
                        "Service: {}",
                        service.target_name.as_ref().unwrap_or(&"<unknown>".to_string())
                    );
                    if let Some(fdecl::Ref::Child(ref source)) = service.source.as_ref() {
                        println!("  Source: {}", source.name);
                    }
                    if let Some(filter) = &service.source_instance_filter {
                        println!("  Instances: {}", filter.join(" "));
                    }
                }
            }
        } else {
            println!("0 Offers");
        }
        println!("");
        Ok(())
    }
}

impl DevicePrinter for Device {
    fn print(&self) -> Result<()> {
        match self {
            Device::V1(device) => device.print(),
            Device::V2(node) => node.print(),
        }
    }

    fn print_verbose(&self) -> Result<()> {
        match self {
            Device::V1(device) => device.print_verbose(),
            Device::V2(node) => node.print_verbose(),
        }
    }
}

pub async fn list_devices(
    cmd: ListDevicesCommand,
    driver_development_proxy: fdd::DriverDevelopmentProxy,
) -> Result<()> {
    let devices: Vec<Device> = match cmd.device {
        Some(device) => {
            fuchsia_driver_dev::get_device_info(&driver_development_proxy, &[device], cmd.exact)
                .await?
        }
        None => {
            fuchsia_driver_dev::get_device_info(&driver_development_proxy, &[], cmd.exact).await?
        }
    }
    .into_iter()
    .map(|device_info| Device::from(device_info))
    .collect();

    if devices.len() > 0 {
        if cmd.verbose {
            for device in devices {
                device.print_verbose()?;
            }
        } else {
            for device in devices {
                device.print()?;
            }
        }
    } else {
        if cmd.fail_on_missing {
            return Err(anyhow!("No devices found."));
        } else {
            println!("No devices found.");
        }
    }

    // TODO(fxb/118660): Remove this warning once ffx driver supports unbound composites.
    if !driver_development_proxy.is_dfv2().await? {
        println!(
            "\n\n\
            PSA: Unbound composites are no longer added to the topology in DFv1. \
            Use ffx driver list-composites to see information on the composites."
        );
    }

    Ok(())
}
