// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::format_err;
use fidl::endpoints::ServerEnd;
use fidl::prelude::*;
use fidl_fuchsia_wlan_device_service::{
    self as fidl_svc, DeviceWatcherControlHandle, DeviceWatcherRequestStream,
};
use futures::channel::mpsc::{self, UnboundedReceiver, UnboundedSender};
use futures::prelude::*;
use futures::try_join;
use log::error;
use parking_lot::Mutex;
use std::collections::HashMap;
use std::convert::Infallible;
use std::sync::Arc;

use crate::watchable_map::{MapEvent, WatchableMap};

// In reality, P and I are always PhyDevice and IfaceDevice, respectively.
// They are generic solely for the purpose of mocking for tests.
pub fn serve_watchers<P, I>(
    phys: Arc<WatchableMap<u16, P>>,
    ifaces: Arc<WatchableMap<u16, I>>,
    phy_events: UnboundedReceiver<MapEvent<u16, P>>,
    iface_events: UnboundedReceiver<MapEvent<u16, I>>,
) -> (WatcherService<P, I>, impl Future<Output = Result<Infallible, anyhow::Error>>)
where
    P: 'static,
    I: 'static,
{
    let inner =
        Arc::new(Mutex::new(Inner { watchers: HashMap::new(), next_watcher_id: 0, phys, ifaces }));
    let (reaper_sender, reaper_receiver) = mpsc::unbounded();
    let s = WatcherService { inner: Arc::clone(&inner), reaper_queue: reaper_sender };

    let fut = async move {
        let phy_fut = notify_phy_watchers(phy_events, &inner);
        let iface_fut = notify_iface_watchers(iface_events, &inner);
        let reaper_fut = reap_watchers(&inner, reaper_receiver);
        try_join!(phy_fut, iface_fut, reaper_fut).map(|x: (Infallible, Infallible, Infallible)| x.0)
    };
    (s, fut)
}

pub struct WatcherService<P, I> {
    inner: Arc<Mutex<Inner<P, I>>>,
    reaper_queue: UnboundedSender<ReaperTask>,
}

// Manual clone impl since #derive uses incorrect trait bounds
impl<P, I> Clone for WatcherService<P, I> {
    fn clone(&self) -> Self {
        WatcherService { inner: self.inner.clone(), reaper_queue: self.reaper_queue.clone() }
    }
}

impl<P, I> WatcherService<P, I> {
    pub fn add_watcher(
        &self,
        endpoint: ServerEnd<fidl_svc::DeviceWatcherMarker>,
    ) -> Result<(), fidl::Error> {
        let stream = endpoint.into_stream()?;
        let handle = stream.control_handle();
        let mut guard = self.inner.lock();
        let inner = &mut *guard;
        self.reaper_queue
            .unbounded_send(ReaperTask {
                watcher_channel: stream,
                watcher_id: inner.next_watcher_id,
            })
            .expect("failed to submit a task to the watcher reaper: {}");
        inner.watchers.insert(
            inner.next_watcher_id,
            Watcher { handle, sent_phy_snapshot: false, sent_iface_snapshot: false },
        );
        inner.phys.request_snapshot();
        inner.ifaces.request_snapshot();
        inner.next_watcher_id += 1;
        Ok(())
    }
}

struct Inner<P, I> {
    watchers: HashMap<u64, Watcher>,
    next_watcher_id: u64,
    phys: Arc<WatchableMap<u16, P>>,
    ifaces: Arc<WatchableMap<u16, I>>,
}

struct Watcher {
    handle: DeviceWatcherControlHandle,
    sent_phy_snapshot: bool,
    sent_iface_snapshot: bool,
}

impl<P, I> Inner<P, I> {
    fn notify_watchers<F, G>(&mut self, sent_snapshot: F, send_event: G)
    where
        F: Fn(&Watcher) -> bool,
        G: Fn(&DeviceWatcherControlHandle) -> Result<(), fidl::Error>,
    {
        self.watchers.retain(|_, w| {
            if sent_snapshot(w) {
                let r = send_event(&w.handle);
                handle_send_result(&w.handle, r)
            } else {
                true
            }
        })
    }

    fn send_snapshot<F, G, T>(
        &mut self,
        sent_snapshot: F,
        send_on_add: G,
        snapshot: Arc<HashMap<u16, T>>,
    ) where
        F: Fn(&mut Watcher) -> &mut bool,
        G: Fn(&DeviceWatcherControlHandle, u16) -> Result<(), fidl::Error>,
    {
        self.watchers.retain(|_, w| {
            if !*sent_snapshot(w) {
                for key in snapshot.keys() {
                    let r = send_on_add(&w.handle, *key);
                    if !handle_send_result(&w.handle, r) {
                        return false;
                    }
                }
                *sent_snapshot(w) = true;
            }
            true
        })
    }
}

fn handle_send_result(handle: &DeviceWatcherControlHandle, r: Result<(), fidl::Error>) -> bool {
    if let Err(e) = r.as_ref() {
        error!("Error sending event to watcher: {}", e);
        handle.shutdown();
    }
    r.is_ok()
}

async fn notify_phy_watchers<P, I>(
    mut events: UnboundedReceiver<MapEvent<u16, P>>,
    inner: &Mutex<Inner<P, I>>,
) -> Result<Infallible, anyhow::Error> {
    while let Some(e) = events.next().await {
        match e {
            MapEvent::KeyInserted(id) => {
                inner.lock().notify_watchers(|w| w.sent_phy_snapshot, |h| h.send_on_phy_added(id))
            }
            MapEvent::KeyRemoved(id) => {
                inner.lock().notify_watchers(|w| w.sent_phy_snapshot, |h| h.send_on_phy_removed(id))
            }
            MapEvent::Snapshot(s) => inner.lock().send_snapshot(
                |w| &mut w.sent_phy_snapshot,
                |h, id| h.send_on_phy_added(id),
                s,
            ),
        }
    }
    Err(format_err!("stream of events from the phy device map has ended unexpectedly"))
}

async fn notify_iface_watchers<P, I>(
    mut events: UnboundedReceiver<MapEvent<u16, I>>,
    inner: &Mutex<Inner<P, I>>,
) -> Result<Infallible, anyhow::Error> {
    while let Some(e) = events.next().await {
        match e {
            MapEvent::KeyInserted(id) => inner
                .lock()
                .notify_watchers(|w| w.sent_iface_snapshot, |h| h.send_on_iface_added(id)),
            MapEvent::KeyRemoved(id) => inner
                .lock()
                .notify_watchers(|w| w.sent_iface_snapshot, |h| h.send_on_iface_removed(id)),
            MapEvent::Snapshot(s) => inner.lock().send_snapshot(
                |w| &mut w.sent_iface_snapshot,
                |h, id| h.send_on_iface_added(id),
                s,
            ),
        }
    }
    Err(format_err!("stream of events from the iface device map has ended unexpectedly"))
}

struct ReaperTask {
    watcher_channel: DeviceWatcherRequestStream,
    watcher_id: u64,
}

/// A future that removes watchers from device maps when their FIDL channels get
/// closed. Performing this clean up solely when notification fails is not
/// sufficient: in the scenario where devices are not being added or removed,
/// but new clients come and go, the watcher list could grow without bound.
async fn reap_watchers<P, I>(
    inner: &Mutex<Inner<P, I>>,
    watchers: UnboundedReceiver<ReaperTask>,
) -> Result<Infallible, anyhow::Error> {
    const REAP_CONCURRENT_LIMIT: usize = 10000;
    watchers
        .for_each_concurrent(REAP_CONCURRENT_LIMIT, move |w| {
            // Wait for the other side to close the channel (or an error to occur)
            // and remove the watcher from the maps
            async move {
                w.watcher_channel.map(|_| ()).collect::<()>().await;
                inner.lock().watchers.remove(&w.watcher_id);
            }
        })
        .await;
    Err(format_err!("stream of watcher channels has ended unexpectedly"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_wlan_device_service::DeviceWatcherEvent;
    use fuchsia_async as fasync;
    use fuchsia_zircon as zx;
    use futures::{pin_mut, task::Poll};
    use std::mem;

    #[test]
    fn reap_watchers() {
        let exec = &mut fasync::TestExecutor::new().expect("Failed to create an executor");
        let (helper, future) = setup();
        pin_mut!(future);
        assert_eq!(0, helper.service.inner.lock().watchers.len());
        let (client_end, server_end) =
            fidl::endpoints::create_endpoints().expect("Failed to create endpoints");

        // Add a watcher and check that it was added to the map
        helper.service.add_watcher(server_end).expect("add_watcher failed");
        assert_eq!(1, helper.service.inner.lock().watchers.len());

        // Run the reaper and make sure the watcher is still there
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("future returned an error (1): {:?}", e);
        }
        assert_eq!(1, helper.service.inner.lock().watchers.len());

        // Drop the client end of the channel and run the reaper again
        mem::drop(client_end);
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("future returned an error (1): {:?}", e);
        }
        assert_eq!(0, helper.service.inner.lock().watchers.len());
    }

    #[test]
    fn add_remove_phys() {
        let exec = &mut fasync::TestExecutor::new().expect("Failed to create an executor");
        let (helper, future) = setup();
        pin_mut!(future);
        let (proxy, server_end) =
            fidl::endpoints::create_proxy().expect("Failed to create endpoints");
        helper.service.add_watcher(server_end).expect("add_watcher failed");

        helper.phys.insert(20, 2000);
        helper.phys.insert(30, 3000);
        helper.phys.remove(&20);

        // Run the server future to propagate the events to FIDL clients
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("server future returned an error: {:?}", e);
        }

        let events = fetch_events(exec, proxy.take_event_stream());
        assert_eq!(3, events.len());
        // Sadly, generated Event struct doesn't implement PartialEq
        match &events[0] {
            &DeviceWatcherEvent::OnPhyAdded { phy_id: 20 } => {}
            other => panic!("Expected OnPhyAdded(20), got {:?}", other),
        }
        match &events[1] {
            &DeviceWatcherEvent::OnPhyAdded { phy_id: 30 } => {}
            other => panic!("Expected OnPhyAdded(30), got {:?}", other),
        }
        match &events[2] {
            &DeviceWatcherEvent::OnPhyRemoved { phy_id: 20 } => {}
            other => panic!("Expected OnPhyRemoved(20), got {:?}", other),
        }
    }

    #[test]
    fn add_remove_ifaces() {
        let exec = &mut fasync::TestExecutor::new().expect("Failed to create an executor");
        let (helper, future) = setup();
        pin_mut!(future);
        let (proxy, server_end) =
            fidl::endpoints::create_proxy().expect("Failed to create endpoints");
        helper.service.add_watcher(server_end).expect("add_watcher failed");

        helper.ifaces.insert(50, 5000);
        helper.ifaces.remove(&50);

        // Run the server future to propagate the events to FIDL clients
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("server future returned an error: {:?}", e);
        }

        let events = fetch_events(exec, proxy.take_event_stream());
        assert_eq!(2, events.len());
        match &events[0] {
            &DeviceWatcherEvent::OnIfaceAdded { iface_id: 50 } => {}
            other => panic!("Expected OnIfaceAdded(50), got {:?}", other),
        }
        match &events[1] {
            &DeviceWatcherEvent::OnIfaceRemoved { iface_id: 50 } => {}
            other => panic!("Expected OnIfaceRemoved(50), got {:?}", other),
        }
    }

    #[test]
    fn snapshot_phys() {
        let exec = &mut fasync::TestExecutor::new().expect("Failed to create an executor");
        let (helper, future) = setup();
        pin_mut!(future);

        // Add and remove phys before we the watcher is added
        helper.phys.insert(20, 2000);
        helper.phys.insert(30, 3000);
        helper.phys.remove(&20);

        // Now add the watcher and pump the events
        let (proxy, server_end) =
            fidl::endpoints::create_proxy().expect("Failed to create endpoints");
        helper.service.add_watcher(server_end).expect("add_watcher failed");
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("server future returned an error: {:?}", e);
        }

        // The watcher should only see phy #30 being "added"
        let events = fetch_events(exec, proxy.take_event_stream());
        assert_eq!(1, events.len());
        match &events[0] {
            &DeviceWatcherEvent::OnPhyAdded { phy_id: 30 } => {}
            other => panic!("Expected OnPhyAdded(30), got {:?}", other),
        }
    }

    #[test]
    fn snapshot_ifaces() {
        let exec = &mut fasync::TestExecutor::new().expect("Failed to create an executor");
        let (helper, future) = setup();
        pin_mut!(future);

        // Add and remove ifaces before we the watcher is added
        helper.ifaces.insert(20, 2000);
        helper.ifaces.insert(30, 3000);
        helper.ifaces.remove(&20);

        // Now add the watcher and pump the events
        let (proxy, server_end) =
            fidl::endpoints::create_proxy().expect("Failed to create endpoints");
        helper.service.add_watcher(server_end).expect("add_watcher failed");
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("server future returned an error: {:?}", e);
        }

        // The watcher should only see iface #30 being "added"
        let events = fetch_events(exec, proxy.take_event_stream());
        assert_eq!(1, events.len());
        match &events[0] {
            &DeviceWatcherEvent::OnIfaceAdded { iface_id: 30 } => {}
            other => panic!("Expected OnIfaceAdded(30), got {:?}", other),
        }
    }

    #[test]
    fn two_watchers() {
        let exec = &mut fasync::TestExecutor::new().expect("Failed to create an executor");
        let (helper, future) = setup();
        pin_mut!(future);

        helper.ifaces.insert(20, 2000);

        // Add first watcher
        let (proxy_one, server_end_one) =
            fidl::endpoints::create_proxy().expect("Failed to create endpoints");
        helper.service.add_watcher(server_end_one).expect("add_watcher failed (1)");

        // Add second watcher
        let (proxy_two, server_end_two) =
            fidl::endpoints::create_proxy().expect("Failed to create endpoints");
        helper.service.add_watcher(server_end_two).expect("add_watcher failed (2)");

        // Deliver events
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("server future returned an error: {:?}", e);
        }

        // Each should only receive a single snapshot, despite two snapshots being
        // requested
        let events_one = fetch_events(exec, proxy_one.take_event_stream());
        assert_eq!(1, events_one.len());
        let events_two = fetch_events(exec, proxy_two.take_event_stream());
        assert_eq!(1, events_two.len());
    }

    #[test]
    fn remove_watcher_on_send_error() {
        let exec = &mut fasync::TestExecutor::new().expect("Failed to create an executor");
        let (helper, future) = setup();
        pin_mut!(future);

        let (client_chan, server_chan) = zx::Channel::create();
        // Make a channel without a WRITE permission to make sure sending an event fails
        let server_handle: zx::Handle = server_chan.into();
        let reduced_chan: zx::Channel =
            server_handle.replace(zx::Rights::READ | zx::Rights::WAIT).unwrap().into();

        helper.service.add_watcher(ServerEnd::new(reduced_chan)).expect("add_watcher failed");
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("future returned an error (1): {:?}", e);
        }
        assert_eq!(1, helper.service.inner.lock().watchers.len());

        // Not add a phy to trigger an event
        helper.phys.insert(20, 2000);

        // The watcher should be now removed since sending the event fails
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut future) {
            panic!("future returned an error (1): {:?}", e);
        }
        assert_eq!(0, helper.service.inner.lock().watchers.len());

        // Make sure the client endpoint is only dropped at the end, so that the watcher
        // is not removed by the reaper thread
        mem::drop(client_chan);
    }

    struct Helper {
        phys: Arc<WatchableMap<u16, i32>>,
        ifaces: Arc<WatchableMap<u16, i32>>,
        service: WatcherService<i32, i32>,
    }

    fn setup() -> (Helper, impl Future<Output = Result<Infallible, anyhow::Error>>) {
        let (phys, phy_events) = WatchableMap::new();
        let (ifaces, iface_events) = WatchableMap::new();
        let phys = Arc::new(phys);
        let ifaces = Arc::new(ifaces);
        let (service, future) =
            serve_watchers(phys.clone(), ifaces.clone(), phy_events, iface_events);
        let helper = Helper { phys, ifaces, service };
        (helper, future)
    }

    fn fetch_events(
        exec: &mut fasync::TestExecutor,
        stream: fidl_svc::DeviceWatcherEventStream,
    ) -> Vec<DeviceWatcherEvent> {
        let events = Arc::new(Mutex::new(Some(Vec::new())));
        let events_two = events.clone();
        let mut event_fut = stream
            .try_for_each(move |e| future::ready(Ok(events_two.lock().as_mut().unwrap().push(e))));
        if let Poll::Ready(Err(e)) = exec.run_until_stalled(&mut event_fut) {
            panic!("event stream future returned an error: {:?}", e);
        }
        let events = events.lock().take().unwrap();
        events
    }
}
