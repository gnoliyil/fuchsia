// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod args;

use {
    anyhow::Result, args::LspciCommand, fidl_fuchsia_hardware_pci as fhpci, fidl_fuchsia_io as fio,
    lspci::bridge::Bridge, lspci::device::Device, lspci::Args,
};

pub async fn lspci(cmd: LspciCommand, dev: &fio::DirectoryProxy) -> Result<()> {
    let (bus, server) = fidl::endpoints::create_proxy::<fhpci::BusMarker>()?;
    let () = dev.open(
        fio::OpenFlags::empty(),
        fio::ModeType::empty(),
        &cmd.service,
        fidl::endpoints::ServerEnd::new(server.into_channel()),
    )?;
    let pci_ids = include_bytes!("../../../../../../../third_party/pciids/pci.ids.zst");
    // The capacity to 2 MB, because the decompressed data
    // should always be less than the capacity's bytes
    let pci_ids = zstd::decode_all(&pci_ids[..])?;
    let pci_ids = String::from_utf8(pci_ids)?;
    let db = Some((lspci::db::PciDb::new(&pci_ids))?);

    let args = Args {
        service: cmd.service,
        verbose: cmd.verbose,
        quiet: cmd.quiet,
        print_config: cmd.print_config,
        print_numeric: cmd.print_numeric,
        only_print_numeric: cmd.only_print_numeric,
        filter: cmd.filter,
        ..Default::default()
    };

    // Prints PCI Info
    for fidl_device in &bus.get_devices().await? {
        let device = Device::new(fidl_device, &db, &args);
        if let Some(filter) = &args.filter {
            if !filter.matches(&device) {
                continue;
            }
        }
        if device.cfg.header_type & 0x1 == 0x1 {
            print!("{}", Bridge::new(&device));
        } else {
            print!("{}", device);
        }
    }
    Ok(())
}
