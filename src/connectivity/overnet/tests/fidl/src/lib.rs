// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use anyhow::Error;
use fidl::HandleBased;
use fuchsia_async::Task;
use futures::prelude::*;
use overnet_core::{log_errors, LinkReceiver, LinkSender, NodeId, NodeIdGenerator, Router};
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

mod channel;
mod socket;

struct Service(futures::channel::mpsc::Sender<fidl::Channel>, String);

impl fidl_fuchsia_overnet::ServiceProviderProxyInterface for Service {
    fn connect_to_service(
        &self,
        chan: fidl::Channel,
        _connection_info: fidl_fuchsia_overnet::ConnectionInfo,
    ) -> std::result::Result<(), fidl::Error> {
        let test_name = self.1.clone();
        tracing::info!(%test_name, "got connection {:?}", chan);
        let mut sender = self.0.clone();
        Task::spawn(log_errors(
            async move {
                tracing::info!(%test_name, "sending the thing");
                sender.send(chan).await?;
                tracing::info!(%test_name, "sent the thing");
                Ok(())
            },
            format!("{} failed to send incoming request handle", self.1),
        ))
        .detach();
        Ok(())
    }
}

struct Fixture {
    dist_a_to_b: fidl::Channel,
    dist_b: fidl::AsyncChannel,
    dist_a_to_c: fidl::Channel,
    dist_c: fidl::AsyncChannel,
    test_name: String,
    _service_task: Task<()>,
}

async fn forward(mut sender: LinkSender, mut receiver: LinkReceiver) -> Result<(), Error> {
    while let Some(mut packet) = sender.next_send().await {
        packet.drop_inner_locks();
        receiver.received_frame(packet.bytes_mut()).await;
    }
    Ok(())
}

async fn link(a: Arc<Router>, b: Arc<Router>) {
    let (ab_tx, ab_rx) = a.new_link(Default::default(), Box::new(|| None));
    let (ba_tx, ba_rx) = b.new_link(Default::default(), Box::new(|| None));
    futures::future::try_join(forward(ab_tx, ba_rx), forward(ba_tx, ab_rx)).await.map(drop).unwrap()
}

#[derive(Clone, Copy, Debug)]
enum Target {
    A,
    B,
    C,
}

const FIXTURE_INCREMENT: u64 = 100000;
static NEXT_FIXTURE_ID: AtomicU64 = AtomicU64::new(100 + FIXTURE_INCREMENT);

impl Fixture {
    async fn new(mut node_id_gen: NodeIdGenerator) -> Fixture {
        let test_name = node_id_gen.test_desc();
        let fixture_id = NEXT_FIXTURE_ID.fetch_add(FIXTURE_INCREMENT, Ordering::Relaxed);
        let router1 = node_id_gen.new_router().unwrap();
        let router2 = node_id_gen.new_router().unwrap();
        let router3 = node_id_gen.new_router().unwrap();
        let l1 = link(router1.clone(), router2.clone());
        let l2 = link(router2.clone(), router3.clone());
        let l3 = link(router3.clone(), router1.clone());
        let service_task = Task::spawn(futures::future::join3(l1, l2, l3).map(drop));
        let service = format!("distribute_handle_for_{}", test_name);
        let (send_handle, mut recv_handle) = futures::channel::mpsc::channel(1);
        tracing::info!(%test_name, %fixture_id, "register 2");
        router2
            .register_raw_service(
                service.clone(),
                Box::new(Service(send_handle.clone(), test_name.clone())),
            )
            .await
            .unwrap();
        tracing::info!(%test_name, %fixture_id, "register 3");
        router3
            .register_raw_service(
                service.clone(),
                Box::new(Service(send_handle, test_name.clone())),
            )
            .await
            .unwrap();
        // Wait til we can see both peers in the service map before progressing.
        let lpc = router1.new_list_peers_context();
        loop {
            let peers = lpc.list_peers().await.unwrap();
            let has_peer = |node_id: NodeId| {
                peers
                    .iter()
                    .find(|peer| {
                        node_id == peer.id.into()
                            && peer
                                .description
                                .services
                                .as_ref()
                                .unwrap()
                                .iter()
                                .find(|&s| *s == service)
                                .is_some()
                    })
                    .is_some()
            };
            if has_peer(router2.node_id()) && has_peer(router3.node_id()) {
                break;
            }
        }
        let (dist_a_to_b, dist_b) = fidl::Channel::create();
        let (dist_a_to_c, dist_c) = fidl::Channel::create();
        tracing::info!(%test_name, %fixture_id, "connect 2");
        router1.connect_to_service(router2.node_id(), &service, dist_b).await.unwrap();
        tracing::info!(%test_name, %fixture_id,"get 2");
        let dist_b = recv_handle.next().await.unwrap();
        tracing::info!(%test_name, %fixture_id, "connect 3");
        router1.connect_to_service(router3.node_id(), &service, dist_c).await.unwrap();
        tracing::info!(%test_name, %fixture_id, "get 3");
        let dist_c = recv_handle.next().await.unwrap();
        let dist_b = fidl::AsyncChannel::from_channel(dist_b).unwrap();
        let dist_c = fidl::AsyncChannel::from_channel(dist_c).unwrap();
        Fixture { dist_a_to_b, dist_b, dist_a_to_c, dist_c, test_name, _service_task: service_task }
    }

    async fn distribute_handle<H: HandleBased>(&self, h: H, target: Target) -> H {
        let h = h.into_handle();
        tracing::info!(test_name = %self.test_name, "distribute_handle: make {:?} on {:?}", h, target);
        let (dist_local, dist_remote) = match target {
            Target::A => return H::from_handle(h),
            Target::B => (&self.dist_a_to_b, &self.dist_b),
            Target::C => (&self.dist_a_to_c, &self.dist_c),
        };
        assert!(dist_local.write(&[], &mut vec![h]) == Ok(()));
        let mut msg = fidl::MessageBufEtc::new();
        dist_remote.recv_etc_msg(&mut msg).await.unwrap();
        let (bytes, handles) = msg.split_mut();
        assert_eq!(bytes.len(), 0);
        assert_eq!(handles.len(), 1);
        let h = std::mem::replace(handles, vec![]).into_iter().next().unwrap();
        tracing::info!(test_name = %self.test_name, "distribute_handle: remote is {:?}", h);
        return H::from_handle(h.handle);
    }

    pub fn log(&mut self, msg: &str) {
        tracing::info!(test_name = %self.test_name, "{}", msg);
    }
}
