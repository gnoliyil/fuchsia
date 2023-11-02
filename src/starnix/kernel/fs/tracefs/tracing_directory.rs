// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::fs::buffers::InputBuffer;
use crate::fs::{
    fileops_impl_delegate_read_and_seek, DynamicFile, DynamicFileBuf, DynamicFileSource,
    FileObject, FileOps, FsNodeOps, SimpleFileNode,
};
use crate::task::CurrentTask;
use crate::types::Errno;

use std::collections::HashMap;
use std::sync::Mutex;

use fuchsia_trace::TraceCategoryContext;
use fuchsia_zircon as zx;
use fuchsia_zircon::sys::zx_ticks_t;

/// trace_marker, used by applications to write trace events
struct TraceMarkerFileSource;

impl DynamicFileSource for TraceMarkerFileSource {
    fn generate(&self, _sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        Ok(())
    }
}

pub struct TraceMarkerFile {
    source: DynamicFile<TraceMarkerFileSource>,
    event_stacks: Mutex<HashMap<u64, Vec<(String, zx_ticks_t)>>>,
}

impl TraceMarkerFile {
    pub fn new_node() -> impl FsNodeOps {
        SimpleFileNode::new(move || {
            Ok(Self {
                source: DynamicFile::new(TraceMarkerFileSource {}),
                event_stacks: Mutex::new(HashMap::new()),
            })
        })
    }
}

impl FileOps for TraceMarkerFile {
    fileops_impl_delegate_read_and_seek!(self, self.source);

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        if let Some(context) =
            TraceCategoryContext::acquire(fuchsia_trace::cstr!(trace_category_atrace!()))
        {
            let bytes = data.read_all()?;
            if let Ok(mut event_stacks) = self.event_stacks.lock() {
                let now = zx::ticks_get();
                if let Some(atrace_event) = ATraceEvent::parse(&String::from_utf8_lossy(&bytes)) {
                    match atrace_event {
                        ATraceEvent::Begin { pid, name } => {
                            event_stacks
                                .entry(pid)
                                .or_insert_with(Vec::new)
                                .push((name.to_string(), now));
                        }
                        ATraceEvent::End { pid } => {
                            if let Some(stack) = event_stacks.get_mut(&pid) {
                                if let Some((name, start_time)) = stack.pop() {
                                    context.write_duration_with_inline_name(&name, start_time, &[]);
                                }
                            }
                        }
                    }
                }
            }
            Ok(bytes.len())
        } else {
            Ok(data.drain())
        }
    }
}

enum ATraceEvent<'a> {
    Begin { pid: u64, name: &'a str },
    End { pid: u64 },
}

impl<'a> ATraceEvent<'a> {
    // Arbitrary data is allowed to be written to tracefs, and we only care about identifying ATrace
    // events to forward to Fuchsia tracing. Since we would throw away any detailed parsing error, this
    // function returns an Option rather than a Result. If we did return a Result, this could be
    // put in a TryFrom impl, if desired.
    fn parse(s: &'a str) -> Option<Self> {
        let chunks: Vec<_> = s.split('|').collect();
        if chunks.len() >= 3 && chunks[0] == "B" {
            if let Ok(pid) = chunks[1].parse::<u64>() {
                return Some(ATraceEvent::Begin { pid, name: chunks[2] });
            }
        } else if chunks.len() >= 2 && chunks[0] == "E" {
            if let Ok(pid) = chunks[1].parse::<u64>() {
                return Some(ATraceEvent::End { pid });
            }
        }
        None
    }
}
