// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};
use fidl::{
    endpoints::{create_proxy, Proxy},
    AsHandleRef,
};
use fidl_fuchsia_element as felement;
use fidl_fuchsia_math as fmath;
use fidl_fuchsia_ui_composition as ui_comp;
use fidl_fuchsia_ui_views as ui_views;
use fuchsia_async as fasync;
use futures::TryStreamExt;
use tracing::*;

use crate::{
    event::{ChildViewEvent, Event, EventSender, ViewSpecHolder},
    window::WindowId,
};

/// Defines a type to hold an id to a child view. This implementation uses the value of
/// [ViewportCreationToken] to be the child view id.
#[derive(Debug, Copy, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ChildViewId(u64);

impl ChildViewId {
    pub fn from_viewport_creation_token(token: &ui_views::ViewportCreationToken) -> Self {
        ChildViewId(token.value.raw_handle().into())
    }
}

/// Defines a struct to hold state for ChildView.
#[derive(Debug)]
pub struct ChildView {
    id: ChildViewId,
    flatland: ui_comp::FlatlandProxy,
    viewport_content_id: ui_comp::ContentId,
    view_ref: Option<ui_views::ViewRef>,
    _window_id: WindowId,
    _event_sender: EventSender,
    _annotations: Option<Vec<felement::Annotation>>,
    _running_tasks: Vec<fasync::Task<()>>,
}

impl Drop for ChildView {
    fn drop(&mut self) {
        // Release the childview's viewport.
        let _fut = self.flatland.release_viewport(&mut self.viewport_content_id);
    }
}

impl ChildView {
    pub(crate) fn new(
        flatland: ui_comp::FlatlandProxy,
        window_id: WindowId,
        viewport_content_id: ui_comp::ContentId,
        mut view_spec_holder: ViewSpecHolder,
        width: u32,
        height: u32,
        event_sender: EventSender,
    ) -> Result<Self, Error> {
        let mut viewport_creation_token = match view_spec_holder.view_spec.viewport_creation_token {
            Some(token) => token,
            None => {
                return Err(format_err!("Ignoring non-flatland client's attempt to present."));
            }
        };

        let child_view_id = ChildViewId::from_viewport_creation_token(&viewport_creation_token);

        let (child_view_watcher_proxy, child_view_watcher_request) =
            create_proxy::<ui_comp::ChildViewWatcherMarker>()?;

        flatland.create_viewport(
            &mut viewport_content_id.clone(),
            &mut viewport_creation_token,
            ui_comp::ViewportProperties {
                logical_size: Some(fmath::SizeU { width, height }),
                ..Default::default()
            },
            child_view_watcher_request,
        )?;

        if let Some(responder) = view_spec_holder.responder {
            responder.send(&mut Ok(())).expect("Failed to respond to GraphicalPresent.present")
        }

        let child_view_watcher_fut = Self::start_child_view_watcher(
            child_view_watcher_proxy,
            child_view_id,
            window_id,
            event_sender.clone(),
        );

        let running_tasks = if let Some(view_controller_request) =
            view_spec_holder.view_controller_request.take()
        {
            let view_watcher_fut = Self::serve_view_controller(
                view_controller_request
                    .into_stream()
                    .expect("Failed to convert to ViewControllerRequestStream"),
                child_view_id,
                window_id,
                event_sender.clone(),
            );
            vec![fasync::Task::spawn(child_view_watcher_fut), fasync::Task::spawn(view_watcher_fut)]
        } else {
            vec![fasync::Task::spawn(child_view_watcher_fut)]
        };

        Ok(ChildView {
            id: child_view_id,
            flatland: flatland.clone(),
            viewport_content_id,
            view_ref: None,
            _window_id: window_id,
            _event_sender: event_sender,
            _annotations: view_spec_holder.view_spec.annotations.take(),
            _running_tasks: running_tasks,
        })
    }

    pub fn get_content_id(&self) -> ui_comp::ContentId {
        self.viewport_content_id.clone()
    }

    pub fn id(&self) -> ChildViewId {
        self.id
    }

    pub fn set_view_ref(&mut self, view_ref: ui_views::ViewRef) {
        self.view_ref = Some(view_ref);
    }

    pub fn get_view_ref(&self) -> Option<ui_views::ViewRef> {
        if let Some(view_ref) = self.view_ref.as_ref() {
            return fuchsia_scenic::duplicate_view_ref(view_ref).ok();
        }
        None
    }

    pub fn set_size(&mut self, width: u32, height: u32) -> Result<(), Error> {
        self.flatland.set_viewport_properties(
            &mut self.viewport_content_id,
            ui_comp::ViewportProperties {
                logical_size: Some(fmath::SizeU { width, height }),
                ..Default::default()
            },
        )?;
        Ok(())
    }

    async fn start_child_view_watcher(
        child_view_watcher_proxy: ui_comp::ChildViewWatcherProxy,
        child_view_id: ChildViewId,
        window_id: WindowId,
        event_sender: EventSender,
    ) {
        match child_view_watcher_proxy.get_status().await {
            Ok(_) => event_sender.send(Event::ChildViewEvent {
                child_view_id,
                window_id,
                event: ChildViewEvent::Available,
            }),
            Err(err) => error!("ChildViewWatcher.get_status return error: {:?}", err),
        }
        match child_view_watcher_proxy.get_view_ref().await {
            Ok(view_ref) => event_sender.send(Event::ChildViewEvent {
                child_view_id,
                window_id,
                event: ChildViewEvent::Attached { view_ref },
            }),
            Err(err) => error!("ChildViewWatcher.get_view_ref returned error: {:?}", err),
        }

        // After retrieving status and viewRef, we can only wait for the channel to close. This is a
        // useful signal when the child view's component exits or crashes or does not use
        // [felement::ViewController]'s dismiss method.
        let _ = child_view_watcher_proxy.on_closed().await;
        event_sender.send(Event::ChildViewEvent {
            child_view_id,
            window_id,
            event: ChildViewEvent::Detached,
        });
    }

    async fn serve_view_controller(
        mut request_stream: felement::ViewControllerRequestStream,
        child_view_id: ChildViewId,
        window_id: WindowId,
        event_sender: EventSender,
    ) {
        if let Ok(Some(request)) = request_stream.try_next().await {
            match request {
                felement::ViewControllerRequest::Dismiss { .. } => {
                    event_sender.send(Event::ChildViewEvent {
                        child_view_id,
                        window_id,
                        event: ChildViewEvent::Dismissed,
                    });
                }
            }
        }
    }
}
