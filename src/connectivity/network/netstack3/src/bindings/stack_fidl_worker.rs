// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use core::ops::DerefMut;

use super::{
    util::{IntoFidl, TryFromFidlWithContext as _, TryIntoCore as _},
    Ctx,
};

use fidl_fuchsia_net as fidl_net;
use fidl_fuchsia_net_stack::{
    self as fidl_net_stack, ForwardingEntry, StackRequest, StackRequestStream,
};
use futures::{TryFutureExt as _, TryStreamExt as _};
use log::{debug, error};
use netstack3_core::{add_route, del_route, ip::types::AddableEntryEither};

pub(crate) struct StackFidlWorker {
    netstack: crate::bindings::Netstack,
}

struct LockedFidlWorker<C> {
    ctx: C,
}

impl StackFidlWorker {
    fn lock_worker(&self) -> LockedFidlWorker<impl DerefMut<Target = Ctx> + '_> {
        let ctx = self.netstack.ctx.clone();
        LockedFidlWorker { ctx }
    }

    pub(crate) async fn serve(
        netstack: crate::bindings::Netstack,
        stream: StackRequestStream,
    ) -> Result<(), fidl::Error> {
        stream
            .try_fold(Self { netstack }, |worker, req| async {
                match req {
                    StackRequest::AddForwardingEntry { entry, responder } => {
                        responder_send!(
                            responder,
                            &mut worker.lock_worker().fidl_add_forwarding_entry(entry)
                        );
                    }
                    StackRequest::DelForwardingEntry {
                        entry:
                            fidl_net_stack::ForwardingEntry {
                                subnet,
                                device_id: _,
                                next_hop: _,
                                metric: _,
                            },
                        responder,
                    } => {
                        responder_send!(
                            responder,
                            &mut worker.lock_worker().fidl_del_forwarding_entry(subnet)
                        );
                    }
                    StackRequest::SetInterfaceIpForwardingDeprecated {
                        id: _,
                        ip_version: _,
                        enabled: _,
                        responder,
                    } => {
                        // TODO(https://fxbug.dev/76987): Support configuring
                        // per-NIC forwarding.
                        responder_send!(responder, &mut Err(fidl_net_stack::Error::NotSupported));
                    }
                    StackRequest::GetDnsServerWatcher { watcher, control_handle: _ } => {
                        let () = watcher
                            .close_with_epitaph(fuchsia_zircon::Status::NOT_SUPPORTED)
                            .unwrap_or_else(|e| {
                                debug!("failed to close DNS server watcher {:?}", e)
                            });
                    }
                    StackRequest::SetDhcpClientEnabled { responder, id: _, enable } => {
                        // TODO(https://fxbug.dev/81593): Remove this once
                        // DHCPv4 client is implemented out-of-stack.
                        if enable {
                            error!("TODO(https://fxbug.dev/111066): Support starting DHCP client");
                        }
                        responder_send!(responder, &mut Ok(()));
                    }
                    StackRequest::BridgeInterfaces{ interfaces: _, bridge, control_handle: _ } => {
                        error!("TODO(https://fxbug.dev/86661): Support bridging in NS3, probably via a new API");
                        bridge.close_with_epitaph(fuchsia_zircon::Status::NOT_SUPPORTED)
                        .unwrap_or_else(|e| {
                            debug!("failed to close bridge control {:?}", e)
                        });
                    }
                }
                Ok(worker)
            })
            .map_ok(|Self { netstack: _ }| ())
            .await
    }
}

impl<C: DerefMut<Target = Ctx>> LockedFidlWorker<C> {
    fn fidl_add_forwarding_entry(
        mut self,
        entry: ForwardingEntry,
    ) -> Result<(), fidl_net_stack::Error> {
        let Ctx { sync_ctx, non_sync_ctx } = self.ctx.deref_mut();

        let entry = match AddableEntryEither::try_from_fidl_with_ctx(&non_sync_ctx, entry) {
            Ok(entry) => entry,
            Err(e) => return Err(e.into()),
        };
        add_route(sync_ctx, non_sync_ctx, entry).map_err(IntoFidl::into_fidl)
    }

    fn fidl_del_forwarding_entry(
        mut self,
        subnet: fidl_net::Subnet,
    ) -> Result<(), fidl_net_stack::Error> {
        let Ctx { sync_ctx, non_sync_ctx } = self.ctx.deref_mut();

        if let Ok(subnet) = subnet.try_into_core() {
            del_route(sync_ctx, non_sync_ctx, subnet).map_err(IntoFidl::into_fidl)
        } else {
            Err(fidl_net_stack::Error::InvalidArgs)
        }
    }
}
