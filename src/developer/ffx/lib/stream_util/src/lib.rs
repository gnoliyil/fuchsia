// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use futures::{
    future::FusedFuture,
    stream::{FuturesUnordered, StreamExt, TryStream},
    Future,
};
use pin_project::pin_project;
use std::{
    mem,
    num::NonZeroUsize,
    pin::Pin,
    task::{Context, Poll},
};

impl<T: ?Sized + TryStream> TryStreamUtilExt for T where T: TryStream {}

pub trait TryStreamUtilExt: TryStream {
    fn try_for_each_concurrent_while_connected<Fut, F>(
        self,
        limit: impl Into<Option<usize>>,
        f: F,
    ) -> TryForEachConcurrentWhileConnected<Self, Fut, F>
    where
        F: FnMut(Self::Ok) -> Fut,
        Fut: Future<Output = Result<(), Self::Error>>,
        Self: Sized,
    {
        TryForEachConcurrentWhileConnected::new(self, limit.into(), f)
    }
}

#[pin_project]
#[must_use = "futures do nothing unless you `.await` or poll them"]
pub struct TryForEachConcurrentWhileConnected<Strm, Fut, F> {
    #[pin]
    stream: Option<Strm>,
    f: F,
    futures: FuturesUnordered<Fut>,
    limit: Option<NonZeroUsize>,
}

impl<Strm, Fut, F> FusedFuture for TryForEachConcurrentWhileConnected<Strm, Fut, F>
where
    Strm: TryStream,
    F: FnMut(Strm::Ok) -> Fut,
    Fut: Future<Output = Result<(), Strm::Error>>,
{
    fn is_terminated(&self) -> bool {
        self.stream.is_none() && self.futures.is_empty()
    }
}

impl<Strm, Fut, F> TryForEachConcurrentWhileConnected<Strm, Fut, F>
where
    Strm: TryStream,
    F: FnMut(Strm::Ok) -> Fut,
    Fut: Future<Output = Result<(), Strm::Error>>,
{
    pub fn new(
        stream: Strm,
        limit: Option<usize>,
        f: F,
    ) -> TryForEachConcurrentWhileConnected<Strm, Fut, F> {
        TryForEachConcurrentWhileConnected {
            stream: Some(stream),
            // Note: `limit` = 0 gets ignored.
            limit: limit.and_then(NonZeroUsize::new),
            f,
            futures: FuturesUnordered::new(),
        }
    }
}

impl<Strm, Fut, F> Future for TryForEachConcurrentWhileConnected<Strm, Fut, F>
where
    Strm: TryStream,
    F: FnMut(Strm::Ok) -> Fut,
    Fut: Future<Output = Result<(), Strm::Error>>,
{
    type Output = Result<(), Strm::Error>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let mut this = self.project();
        loop {
            let mut made_progress_this_iter = false;

            // Check if we've already created a number of futures greater than `limit`
            if this.limit.map(|limit| limit.get() > this.futures.len()).unwrap_or(true) {
                let poll_res = match this.stream.as_mut().as_pin_mut() {
                    Some(stream) => stream.try_poll_next(cx),
                    None => Poll::Ready(None),
                };

                let elem = match poll_res {
                    Poll::Pending => None,
                    Poll::Ready(Some(Ok(elem))) => {
                        made_progress_this_iter = true;
                        Some(elem)
                    }
                    Poll::Ready(None) => {
                        this.stream.set(None);
                        drop(mem::replace(this.futures, FuturesUnordered::new()));
                        return Poll::Ready(Ok(()));
                    }
                    Poll::Ready(Some(Err(e))) => {
                        this.stream.set(None);
                        drop(mem::replace(this.futures, FuturesUnordered::new()));
                        return Poll::Ready(Err(e));
                    }
                };

                if let Some(elem) = elem {
                    this.futures.push((this.f)(elem));
                }
            }

            match this.futures.poll_next_unpin(cx) {
                Poll::Ready(Some(Ok(()))) => made_progress_this_iter = true,
                Poll::Ready(None) => {
                    if this.stream.is_none() {
                        return Poll::Ready(Ok(()));
                    }
                }
                Poll::Pending => {}
                Poll::Ready(Some(Err(e))) => {
                    // Empty the stream and futures so that we know
                    // the future has completed.
                    this.stream.set(None);
                    drop(mem::replace(this.futures, FuturesUnordered::new()));
                    return Poll::Ready(Err(e));
                }
            }

            if !made_progress_this_iter {
                return Poll::Pending;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::anyhow;
    use fuchsia_async::Timer;
    use futures::{
        future::ready,
        stream::{iter, once},
    };
    use std::time::Duration;

    async fn sleep_for_a_year() {
        // Attempts to sleep for an entire year.
        Timer::new(Duration::from_secs(3153600000)).await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn single_element_stream_drops_when_terminated() {
        let s = once(ready(Result::<_>::Ok(2u64)));
        s.try_for_each_concurrent_while_connected(None, |_| async {
            sleep_for_a_year().await;
            panic!("this should never be fired")
        })
        .await
        .unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn single_element_stream_errs_before_end() {
        let s = once(ready(Result::<_>::Err(anyhow!("test err")))).chain(once(async {
            sleep_for_a_year().await;
            Result::<_>::Ok(1u64)
        }));
        let res = s
            .try_for_each_concurrent_while_connected(None, |_| {
                #[allow(unreachable_code)]
                async {
                    panic!("this should never be fired");
                    Ok(())
                }
            })
            .await;
        assert!(res.is_err());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn stream_exits_with_internal_err() {
        let s = iter(0u64..100).map(Result::<_>::Ok).chain(once(async {
            sleep_for_a_year().await;
            Ok(1000u64)
        }));
        let res = s
            .try_for_each_concurrent_while_connected(Some(3), |i| async move {
                if i == 63 {
                    Err(anyhow!("test error"))
                } else {
                    Ok(())
                }
            })
            .await;
        assert!(res.is_err());
    }
}
