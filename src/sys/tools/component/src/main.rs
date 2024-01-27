// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod args;

use crate::args::*;
use anyhow::Result;
use component_debug::cli::*;
use component_debug::copy::copy_cmd;
use fidl_fuchsia_sys2 as fsys;
use fuchsia_component::client::connect_to_protocol_at_path;

pub async fn exec() -> Result<()> {
    let args: ComponentArgs = argh::from_env();

    let writer = std::io::stdout();
    let realm_explorer = connect_to_protocol_at_path::<fsys::RealmExplorerMarker>(
        "/svc/fuchsia.sys2.RealmExplorer.root",
    )?;
    let realm_query =
        connect_to_protocol_at_path::<fsys::RealmQueryMarker>("/svc/fuchsia.sys2.RealmQuery.root")?;
    let route_validator = connect_to_protocol_at_path::<fsys::RouteValidatorMarker>(
        "/svc/fuchsia.sys2.RouteValidator.root",
    )?;
    let lifecycle_controller = connect_to_protocol_at_path::<fsys::LifecycleControllerMarker>(
        "/svc/fuchsia.sys2.LifecycleController.root",
    )?;

    match args.subcommand {
        ComponentSubcommand::Show(args) => {
            show_cmd_print(args.query, realm_query, realm_explorer, writer).await
        }
        ComponentSubcommand::Create(args) => {
            create_cmd(args.url, args.moniker, lifecycle_controller, writer).await
        }
        ComponentSubcommand::Destroy(args) => {
            destroy_cmd(args.query, lifecycle_controller, realm_query, writer).await
        }
        ComponentSubcommand::Resolve(args) => {
            resolve_cmd(args.query, lifecycle_controller, realm_query, writer).await
        }
        ComponentSubcommand::Reload(args) => {
            reload_cmd(args.query, lifecycle_controller, realm_query, writer).await
        }
        ComponentSubcommand::Start(args) => {
            start_cmd(args.query, lifecycle_controller, realm_query, writer).await
        }
        ComponentSubcommand::Stop(args) => {
            stop_cmd(args.query, lifecycle_controller, realm_query, writer).await
        }
        ComponentSubcommand::Doctor(args) => {
            doctor_cmd_print(args.query, route_validator, realm_query, writer).await
        }
        ComponentSubcommand::Capability(args) => {
            capability_cmd(args.capability_name, realm_query, writer).await
        }
        ComponentSubcommand::List(args) => {
            list_cmd_print(args.filter, args.verbose, realm_query, writer).await
        }
        ComponentSubcommand::Graph(args) => {
            graph_cmd(args.filter, args.orientation, realm_query, writer).await
        }
        ComponentSubcommand::Run(args) => {
            run_cmd(args.moniker, args.url, args.recreate, lifecycle_controller, writer).await
        }
        ComponentSubcommand::Copy(args) => copy_cmd(&realm_query, args.paths, args.verbose).await,
        ComponentSubcommand::Storage(args) => match args.subcommand {
            StorageSubcommand::Copy(copy_args) => {
                storage_copy_cmd(
                    args.provider,
                    args.capability,
                    copy_args.source_path,
                    copy_args.destination_path,
                    lifecycle_controller,
                )
                .await
            }
            StorageSubcommand::Delete(delete_args) => {
                storage_delete_cmd(
                    args.provider,
                    args.capability,
                    delete_args.path,
                    lifecycle_controller,
                )
                .await
            }
            StorageSubcommand::List(list_args) => {
                storage_list_cmd(
                    args.provider,
                    args.capability,
                    list_args.path,
                    lifecycle_controller,
                    writer,
                )
                .await
            }
            StorageSubcommand::MakeDirectory(make_dir_args) => {
                storage_make_directory_cmd(
                    args.provider,
                    args.capability,
                    make_dir_args.path,
                    lifecycle_controller,
                )
                .await
            }
        },
    }
}

#[fuchsia::main(logging = false)]
async fn main() {
    if let Err(e) = exec().await {
        eprintln!("{}", e);
        std::process::exit(1);
    }
}
