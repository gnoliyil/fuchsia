// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

use crate::PublishError;
use diagnostics_log_encoding::{
    encode::{
        EncodedSpanArguments, Encoder, EncodingError, MutableBuffer, TestRecord, TracingEvent,
        WriteEventParams,
    },
    Metatag,
};
use fidl_fuchsia_logger::{LogSinkProxy, MAX_DATAGRAM_LEN_BYTES};
use fuchsia_runtime as rt;
use fuchsia_zircon::{self as zx, AsHandleRef};
use std::{
    collections::HashSet,
    io::Cursor,
    sync::atomic::{AtomicU32, Ordering},
};
use tracing::{span, subscriber::Subscriber, Event};
use tracing_subscriber::{
    layer::{Context, Layer},
    registry::LookupSpan,
};

thread_local! {
    static PROCESS_ID: zx::Koid =
        rt::process_self().get_koid().expect("couldn't read our own process koid");
    static THREAD_ID: zx::Koid = rt::thread_self()
        .get_koid()
        .expect("couldn't read our own thread id");
}

pub(crate) struct Sink {
    socket: zx::Socket,
    pub tags: Vec<String>,
    pub metatags: HashSet<Metatag>,
    num_events_dropped: AtomicU32,
}

impl Sink {
    pub fn new(
        log_sink: &LogSinkProxy,
        tags: &[&str],
        metatags: HashSet<Metatag>,
    ) -> Result<Self, PublishError> {
        let (socket, remote_socket) = zx::Socket::create_datagram();
        log_sink.connect_structured(remote_socket).map_err(PublishError::SendSocket)?;

        Ok(Self {
            socket,
            tags: tags.iter().map(|s| s.to_string()).collect(),
            metatags,
            num_events_dropped: AtomicU32::new(0),
        })
    }
}

impl Sink {
    fn encode_and_send(
        &self,
        encode: impl FnOnce(&mut Encoder<Cursor<&mut [u8]>>, u32) -> Result<(), EncodingError>,
    ) {
        let ordering = Ordering::Relaxed;
        let previously_dropped = self.num_events_dropped.swap(0, ordering);
        let restore_and_increment_dropped_count = || {
            self.num_events_dropped.fetch_add(previously_dropped + 1, ordering);
        };

        let mut buf = [0u8; MAX_DATAGRAM_LEN_BYTES as _];
        let mut encoder = Encoder::new(Cursor::new(&mut buf[..]));

        if encode(&mut encoder, previously_dropped).is_err() {
            restore_and_increment_dropped_count();
            return;
        }

        let end = encoder.inner().cursor();
        let packet = &encoder.inner().get_ref()[..end];
        match self.socket.write(packet) {
            Ok(_) => (),
            Err(zx::Status::SHOULD_WAIT) |
            // TODO(fxbug.dev/56043) handle socket closure separately from buffer full
            Err(_) => restore_and_increment_dropped_count(),
        }
    }

    pub fn event_for_testing(&self, record: TestRecord<'_>) {
        self.encode_and_send(move |encoder, previously_dropped| {
            encoder.write_event(WriteEventParams {
                event: record,
                tags: &self.tags,
                metatags: std::iter::empty(),
                pid: PROCESS_ID.with(|p| *p),
                tid: THREAD_ID.with(|t| *t),
                dropped: previously_dropped,
            })
        });
    }
}

impl<S> Layer<S> for Sink
where
    for<'lookup> S: Subscriber + LookupSpan<'lookup>,
{
    fn on_event(&self, event: &Event<'_>, cx: Context<'_, S>) {
        self.encode_and_send(|encoder, previously_dropped| {
            encoder.write_event(WriteEventParams {
                event: TracingEvent::new(event, cx),
                tags: &self.tags,
                metatags: self.metatags.iter(),
                pid: PROCESS_ID.with(|p| *p),
                tid: THREAD_ID.with(|t| *t),
                dropped: previously_dropped,
            })
        });
    }

    fn on_new_span(&self, attrs: &span::Attributes<'_>, id: &span::Id, ctx: Context<'_, S>) {
        let span = ctx.span(id).expect("span exists. Internal tracing bug if it doesn't");
        let mut extensions = span.extensions_mut();
        if extensions.get_mut::<EncodedSpanArguments>().is_none() {
            if let Ok(encoded) = EncodedSpanArguments::new(attrs) {
                extensions.insert(encoded);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use diagnostics_log_encoding::{parse::parse_record, Severity};
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_diagnostics_stream::{Argument, Record, Value};
    use fidl_fuchsia_logger::{LogSinkMarker, LogSinkRequest};
    use futures::stream::StreamExt;
    use tracing::{debug, error, info, info_span, trace, warn};
    use tracing_subscriber::{layer::SubscriberExt, Registry};

    const TARGET: &str = "diagnostics_log_lib_test::fuchsia::sink::tests";

    async fn init_sink(tags: &[&str], metatags: &[Metatag]) -> fidl::Socket {
        let (proxy, mut requests) = create_proxy_and_stream::<LogSinkMarker>().unwrap();
        let mut sink = Sink::new(&proxy, &[], HashSet::default()).unwrap();
        sink.tags = tags.iter().copied().map(str::to_string).collect();
        sink.metatags = metatags.iter().copied().collect();
        tracing::subscriber::set_global_default(Registry::default().with(sink)).unwrap();

        match requests.next().await.unwrap().unwrap() {
            LogSinkRequest::ConnectStructured { socket, .. } => socket,
            _ => panic!("sink ctor sent the wrong message"),
        }
    }

    fn arg_prefix() -> Vec<Argument> {
        vec![
            Argument {
                name: "pid".into(),
                value: Value::UnsignedInt(PROCESS_ID.with(|p| p.raw_koid()) as _),
            },
            Argument {
                name: "tid".into(),
                value: Value::UnsignedInt(THREAD_ID.with(|t| t.raw_koid() as _)),
            },
        ]
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn packets_are_sent() {
        let socket = init_sink(&[], &[Metatag::Target]).await;
        let mut buf = [0u8; MAX_DATAGRAM_LEN_BYTES as _];
        let mut next_message = || {
            let len = socket.read(&mut buf).unwrap();
            let (record, _) = parse_record(&buf[..len]).unwrap();
            assert_eq!(socket.outstanding_read_bytes().unwrap(), 0, "socket must be empty");
            record
        };

        // emit some expected messages and then we'll retrieve them for parsing
        trace!(count = 123, "whoa this is noisy");
        let observed_trace = next_message();
        debug!(maybe = true, "don't try this at home");
        let observed_debug = next_message();
        info!("this is a message");
        let observed_info = next_message();
        warn!(reason = "just cuz", "this is a warning");
        let observed_warn = next_message();
        error!(e = "something went pretty wrong", "this is an error");
        let error_line = line!() - 1;
        let metatag = Argument { name: "tag".into(), value: Value::Text(TARGET.into()) };
        let observed_error = next_message();

        // TRACE
        {
            let mut expected_trace = Record {
                timestamp: observed_trace.timestamp,
                severity: Severity::Trace,
                arguments: arg_prefix(),
            };
            expected_trace.arguments.push(metatag.clone());
            expected_trace.arguments.push(Argument {
                name: "message".into(),
                value: Value::Text("whoa this is noisy".into()),
            });
            expected_trace
                .arguments
                .push(Argument { name: "count".into(), value: Value::SignedInt(123) });
            assert_eq!(observed_trace, expected_trace);
        }

        // DEBUG
        {
            let mut expected_debug = Record {
                timestamp: observed_debug.timestamp,
                severity: Severity::Debug,
                arguments: arg_prefix(),
            };
            expected_debug.arguments.push(metatag.clone());
            expected_debug.arguments.push(Argument {
                name: "message".into(),
                value: Value::Text("don't try this at home".into()),
            });
            expected_debug
                .arguments
                .push(Argument { name: "maybe".into(), value: Value::Boolean(true) });
            assert_eq!(observed_debug, expected_debug);
        }

        // INFO
        {
            let mut expected_info = Record {
                timestamp: observed_info.timestamp,
                severity: Severity::Info,
                arguments: arg_prefix(),
            };
            expected_info.arguments.push(metatag.clone());
            expected_info.arguments.push(Argument {
                name: "message".into(),
                value: Value::Text("this is a message".into()),
            });
            assert_eq!(observed_info, expected_info);
        }

        // WARN
        {
            let mut expected_warn = Record {
                timestamp: observed_warn.timestamp,
                severity: Severity::Warn,
                arguments: arg_prefix(),
            };
            expected_warn.arguments.push(metatag.clone());
            expected_warn.arguments.push(Argument {
                name: "message".into(),
                value: Value::Text("this is a warning".into()),
            });
            expected_warn
                .arguments
                .push(Argument { name: "reason".into(), value: Value::Text("just cuz".into()) });
            assert_eq!(observed_warn, expected_warn);
        }

        // ERROR
        {
            let mut expected_error = Record {
                timestamp: observed_error.timestamp,
                severity: Severity::Error,
                arguments: arg_prefix(),
            };
            expected_error.arguments.push(Argument {
                name: "file".into(),
                value: Value::Text("src/lib/diagnostics/log/rust/src/fuchsia/sink.rs".into()),
            });
            expected_error
                .arguments
                .push(Argument { name: "line".into(), value: Value::UnsignedInt(error_line as _) });
            expected_error.arguments.push(metatag);
            expected_error.arguments.push(Argument {
                name: "message".into(),
                value: Value::Text("this is an error".into()),
            });
            expected_error.arguments.push(Argument {
                name: "e".into(),
                value: Value::Text("something went pretty wrong".into()),
            });
            assert_eq!(observed_error, expected_error);
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn tags_are_sent() {
        let socket = init_sink(&["tags_are_sent"], &[]).await;
        let mut buf = [0u8; MAX_DATAGRAM_LEN_BYTES as _];
        let mut next_message = || {
            let len = socket.read(&mut buf).unwrap();
            let (record, _) = parse_record(&buf[..len]).unwrap();
            assert_eq!(socket.outstanding_read_bytes().unwrap(), 0, "socket must be empty");
            record
        };

        info!("this should have a tag");
        let observed = next_message();

        let mut expected = Record {
            timestamp: observed.timestamp,
            severity: Severity::Info,
            arguments: arg_prefix(),
        };
        expected.arguments.push(Argument {
            name: "message".into(),
            value: Value::Text("this should have a tag".into()),
        });
        expected
            .arguments
            .push(Argument { name: "tag".into(), value: Value::Text("tags_are_sent".into()) });
        assert_eq!(observed, expected);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn spans_are_supported() {
        let socket = init_sink(&[], &[]).await;
        let mut buf = [0u8; MAX_DATAGRAM_LEN_BYTES as _];
        let mut next_message = || {
            let len = socket.read(&mut buf).unwrap();
            let (record, _) = parse_record(&buf[..len]).unwrap();
            assert_eq!(socket.outstanding_read_bytes().unwrap(), 0, "socket must be empty");
            record
        };

        let span = info_span!("span 1", tag = "foo");
        let _s1 = span.enter();
        let span2 = info_span!("span 2", key = 2);
        let _s2 = span2.enter();
        info!("this should have span fields");
        let observed = next_message();

        let mut expected = Record {
            timestamp: observed.timestamp,
            severity: Severity::Info,
            arguments: arg_prefix(),
        };
        expected.arguments.push(Argument { name: "tag".into(), value: Value::Text("foo".into()) });
        expected.arguments.push(Argument { name: "key".into(), value: Value::SignedInt(2) });
        expected.arguments.push(Argument {
            name: "message".into(),
            value: Value::Text("this should have span fields".into()),
        });
        assert_eq!(observed, expected);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn drop_count_is_tracked() {
        let socket = init_sink(&[], &[]).await;
        let mut buf = [0u8; MAX_DATAGRAM_LEN_BYTES as _];
        const MESSAGE_SIZE: usize = 104;
        const MESSAGE_SIZE_WITH_DROPS: usize = 136;
        const NUM_DROPPED: usize = 100;

        let socket_capacity = || {
            let info = socket.info().unwrap();
            info.rx_buf_max - info.rx_buf_size
        };
        let emit_message = || info!("it's-a-me, a message-o");
        let mut drain_message = |with_drops| {
            let len = socket.read(&mut buf).unwrap();

            let expected_len = if with_drops { MESSAGE_SIZE_WITH_DROPS } else { MESSAGE_SIZE };
            assert_eq!(len, expected_len, "constant message size is used to calculate thresholds");

            let (record, _) = parse_record(&buf[..len]).unwrap();
            let mut expected_args = arg_prefix();

            if with_drops {
                expected_args.push(Argument {
                    name: "num_dropped".into(),
                    value: Value::UnsignedInt(NUM_DROPPED as _),
                });
            }

            expected_args.push(Argument {
                name: "message".into(),
                value: Value::Text("it's-a-me, a message-o".into()),
            });

            assert_eq!(
                record,
                Record {
                    timestamp: record.timestamp,
                    severity: Severity::Info,
                    arguments: expected_args
                }
            );
        };

        // fill up the socket
        let mut num_emitted = 0;
        while socket_capacity() > MESSAGE_SIZE {
            emit_message();
            num_emitted += 1;
            assert_eq!(
                socket.info().unwrap().rx_buf_size,
                num_emitted * MESSAGE_SIZE,
                "incorrect bytes stored after {} messages sent",
                num_emitted
            );
        }

        // drop messages
        for _ in 0..NUM_DROPPED {
            emit_message();
        }

        // make space for a message to convey the drop count
        // we drain two messages here because emitting the drop count adds to the size of the packet
        // if we only drain one message then we're relying on the kernel's buffer size to satisfy
        //   (rx_buf_max_size % MESSAGE_SIZE) > (MESSAGE_SIZE_WITH_DROPS - MESSAGE_SIZE)
        // this is true at the time of writing of this test but we don't know whether that's a
        // guarantee.
        drain_message(false);
        drain_message(false);
        // we use this count below to drain the rest of the messages
        num_emitted -= 2;
        // convey the drop count, it's now at the tail of the socket
        emit_message();
        // drain remaining "normal" messages ahead of the drop count
        for _ in 0..num_emitted {
            drain_message(false);
        }
        // verify that messages were dropped
        drain_message(true);

        // check that we return to normal after reporting the drops
        emit_message();
        drain_message(false);
        assert_eq!(socket.outstanding_read_bytes().unwrap(), 0, "must drain all messages");
    }
}
