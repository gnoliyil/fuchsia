// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/boot-options/boot-options.h>
#include <lib/counters.h>
#include <trace.h>
#include <zircon/syscalls-next.h>

#include <lk/init.h>
#include <object/pager_dispatcher.h>
#include <object/pager_proxy.h>
#include <object/thread_dispatcher.h>

#define LOCAL_TRACE 0

KCOUNTER(dispatcher_pager_overtime_wait_count, "dispatcher.pager.overtime_waits")
KCOUNTER(dispatcher_pager_total_request_count, "dispatcher.pager.total_requests")
KCOUNTER(dispatcher_pager_succeeded_request_count, "dispatcher.pager.succeeded_requests")
KCOUNTER(dispatcher_pager_failed_request_count, "dispatcher.pager.failed_requests")
KCOUNTER(dispatcher_pager_timed_out_request_count, "dispatcher.pager.timed_out_requests")

namespace {

const PageSourceProperties kProperties{
    .is_user_pager = true,
    .is_preserving_page_content = true,
    .is_providing_specific_physical_pages = false,
    .is_handling_free = false,
};

}  // namespace

PagerProxy::PagerProxy(PagerDispatcher* dispatcher, fbl::RefPtr<PortDispatcher> port, uint64_t key,
                       uint32_t options)
    : pager_(dispatcher), port_(ktl::move(port)), key_(key), options_(options) {
  LTRACEF("%p key %lx options %x\n", this, key_, options_);
}

PagerProxy::~PagerProxy() {
  LTRACEF("%p\n", this);
  // In error paths shortly after construction, we can destruct without page_source_closed_ becoming
  // true.
  DEBUG_ASSERT(!complete_pending_);
}

const PageSourceProperties& PagerProxy::properties() const { return kProperties; }

void PagerProxy::SendAsyncRequest(PageRequest* request) {
  Guard<Mutex> guard{&mtx_};
  ASSERT(!page_source_closed_);

  QueuePacketLocked(request);
}

void PagerProxy::QueuePacketLocked(PageRequest* request) {
  if (packet_busy_) {
    pending_requests_.push_back(request);
    return;
  }

  DEBUG_ASSERT(active_request_ == nullptr);
  packet_busy_ = true;
  active_request_ = request;

  uint64_t offset, length;
  uint16_t cmd;
  if (request != &complete_request_) {
    switch (GetRequestType(request)) {
      case page_request_type::READ:
        cmd = ZX_PAGER_VMO_READ;
        break;
      case page_request_type::DIRTY:
        DEBUG_ASSERT(options_ & kTrapDirty);
        cmd = ZX_PAGER_VMO_DIRTY;
        break;
      default:
        // Not reached
        ASSERT(false);
    }
    offset = GetRequestOffset(request);
    length = GetRequestLen(request);

    // The vm subsystem should guarantee this
    uint64_t unused;
    DEBUG_ASSERT(!add_overflow(offset, length, &unused));

    // Trace flow events require an enclosing duration.
    VM_KTRACE_DURATION(1, "page_request_queue", ("offset", offset), ("length", length));
    VM_KTRACE_FLOW_BEGIN(1, "page_request_queue", reinterpret_cast<uintptr_t>(&packet_));
  } else {
    offset = length = 0;
    cmd = ZX_PAGER_VMO_COMPLETE;
  }

  zx_port_packet_t packet = {};
  packet.key = key_;
  packet.type = ZX_PKT_TYPE_PAGE_REQUEST;
  packet.page_request.command = cmd;
  packet.page_request.offset = offset;
  packet.page_request.length = length;

  packet_.packet = packet;

  // We can treat ZX_ERR_BAD_HANDLE as if the packet was queued
  // but the pager service never responds.
  // TODO: Bypass the port's max queued packet count to prevent ZX_ERR_SHOULD_WAIT
  ASSERT(port_->Queue(&packet_, ZX_SIGNAL_NONE) != ZX_ERR_SHOULD_WAIT);
}

void PagerProxy::ClearAsyncRequest(PageRequest* request) {
  Guard<Mutex> guard{&mtx_};
  ASSERT(!page_source_closed_);

  if (request == active_request_) {
    if (request != &complete_request_) {
      // Trace flow events require an enclosing duration.
      VM_KTRACE_DURATION(
          1, "page_request_queue",
          ("request offset",
           KTRACE_ANNOTATED_VALUE(AssertHeld(mtx_), GetRequestOffset(active_request_))),
          ("request len",
           KTRACE_ANNOTATED_VALUE(AssertHeld(mtx_), GetRequestLen(active_request_))));
      VM_KTRACE_FLOW_END(1, "page_request_queue", reinterpret_cast<uintptr_t>(&packet_));
    }
    // This request is being taken back by the PageSource, so we can't hold a reference to it
    // anymore. This will remain null until OnPacketFreedLocked is called (and a new packet gets
    // queued as a result), either by us here below or by PagerProxy::Free, since packet_busy_ is
    // true and will be true until OnPacketFreedLocked is called.
    active_request_ = nullptr;
    // Condition on whether or not we actually cancel the packet, to make sure
    // we don't race with a call to PagerProxy::Free.
    if (port_->CancelQueued(&packet_)) {
      OnPacketFreedLocked();
    }
  } else if (fbl::InContainer<PageProviderTag>(*request)) {
    pending_requests_.erase(*request);
  }
}

void PagerProxy::SwapAsyncRequest(PageRequest* old, PageRequest* new_req) {
  Guard<Mutex> guard{&mtx_};
  ASSERT(!page_source_closed_);

  if (fbl::InContainer<PageProviderTag>(*old)) {
    pending_requests_.insert(*old, new_req);
    pending_requests_.erase(*old);
  } else if (old == active_request_) {
    active_request_ = new_req;
  }
}

bool PagerProxy::DebugIsPageOk(vm_page_t* page, uint64_t offset) { return true; }

void PagerProxy::OnDetach() {
  Guard<Mutex> guard{&mtx_};
  ASSERT(!page_source_closed_);

  complete_pending_ = true;
  QueuePacketLocked(&complete_request_);
}

void PagerProxy::OnClose() {
  fbl::RefPtr<PagerProxy> self_ref;
  fbl::RefPtr<PageSource> self_src;
  Guard<Mutex> guard{&mtx_};
  ASSERT(!page_source_closed_);

  page_source_closed_ = true;
  // If there isn't a complete packet pending, we're free to clean up our ties with the PageSource
  // and PagerDispatcher right now, as we're not expecting a PagerProxy::Free to perform final
  // delayed clean up later. The PageSource is closing, so it won't need to send us any more
  // requests. The PagerDispatcher doesn't need to refer to us anymore as we won't be queueing any
  // more pager requests.
  if (!complete_pending_) {
    // We know PagerDispatcher::on_zero_handles hasn't been invoked, since that would
    // have already closed this pager proxy via OnDispatcherClose. Therefore we are free to
    // immediately clean up.
    DEBUG_ASSERT(!pager_dispatcher_closed_);
    self_ref = pager_->ReleaseProxy(this);
    self_src = ktl::move(page_source_);
  } else {
    // There is still a pending complete message that we would like to wait to be received and so we
    // do not perform CancelQueued like OnDispatcherClose does. However, we must leave the reference
    // to ourselves in pager_ so that OnDispatcherClose (and the forced packet cancelling) can
    // happen if needed. Otherwise final delayed cleanup will happen in PagerProxy::Free.
  }
}

void PagerProxy::OnDispatcherClose() {
  fbl::RefPtr<PageSource> self_src;
  Guard<Mutex> guard{&mtx_};

  // The PagerDispatcher is going away and there won't be a way to service any pager requests. Close
  // the PageSource from our end so that no more requests can be sent. Closing the PageSource will
  // clear/cancel any outstanding requests that it had forwarded, i.e. any requests except the
  // complete request (which is owned by us and is not visible to the PageSource).
  if (!page_source_closed_) {
    // page_source_ is only reset to nullptr if we already closed it.
    DEBUG_ASSERT(page_source_);
    self_src = page_source_;
    // Call Close without the lock to
    //  * Not violate lock ordering
    //  * Allow it to call back into ::OnClose
    guard.CallUnlocked([&self_src]() mutable { self_src->Close(); });
  }

  // The pager dispatcher's reference to this object is the only one we completely control. Now
  // that it's gone, we need to make sure that port_ doesn't end up with an invalid pointer
  // to packet_ if all external RefPtrs to this object go away.
  // As the Pager dispatcher is going away, we are not content to keep these objects alive
  // indefinitely until messages are read, instead we want to cancel everything as soon as possible
  // to avoid memory leaks. Therefore we will attempt to cancel any queued final packet.
  if (complete_pending_) {
    if (port_->CancelQueued(&packet_)) {
      // We successfully cancelled the message, so we don't have to worry about
      // PagerProxy::Free being called, and can immediately break the refptr cycle.
      complete_pending_ = false;
    } else {
      // If we failed to cancel the message, then there is a pending call to PagerProxy::Free. It
      // will cleanup the RefPtr cycle, although only if page_source_closed_ is true, which should
      // be the case since we performed the Close step earlier.
      DEBUG_ASSERT(page_source_closed_);
    }
  } else {
    // Either the complete message had already been dispatched when this object was closed or
    // PagerProxy::Free was called between this object being closed and this method taking the
    // lock. In either case, the port no longer has a reference, any RefPtr cycles have been broken
    // and cleanup is already done.
    DEBUG_ASSERT(!page_source_);
  }
  // The pager dispatcher calls OnDispatcherClose when it is going away on zero handles, and it's
  // not safe to dereference pager_ anymore. Remember that pager_ is now closed.
  pager_dispatcher_closed_ = true;
}

void PagerProxy::Free(PortPacket* packet) {
  fbl::RefPtr<PagerProxy> self_ref;
  fbl::RefPtr<PageSource> self_src;

  Guard<Mutex> guard{&mtx_};
  if (active_request_ != &complete_request_) {
    // This request is still active, i.e. it has not been taken back by the PageSource with
    // ClearAsyncRequest. So we are responsible for relinquishing ownership of the request.
    if (active_request_ != nullptr) {
      // Trace flow events require an enclosing duration.
      VM_KTRACE_DURATION(
          1, "page_request_queue",
          ("request offset",
           KTRACE_ANNOTATED_VALUE(AssertHeld(mtx_), GetRequestOffset(active_request_))),
          ("request len",
           KTRACE_ANNOTATED_VALUE(AssertHeld(mtx_), GetRequestLen(active_request_))));
      VM_KTRACE_FLOW_END(1, "page_request_queue", reinterpret_cast<uintptr_t>(packet));
      active_request_ = nullptr;
    }
    OnPacketFreedLocked();
  } else {
    // Freeing the complete_request_ indicates we have completed a pending action that might have
    // been delaying cleanup.
    complete_pending_ = false;
    // If the source is closed, we need to do delayed cleanup. Make sure we are not still in the
    // pager's proxy list (if the pager is not closed yet), and then break our refptr cycle.
    if (page_source_closed_) {
      DEBUG_ASSERT(page_source_);
      // If the PagerDispatcher is already closed, the proxy has already been released.
      if (!pager_dispatcher_closed_) {
        // self_ref could be a nullptr if we have ended up racing with
        // PagerDispatcher::on_zero_handles which calls PagerProxy::OnDispatcherClose *after*
        // removing the proxy from its list. This is fine as the proxy will be removed from the
        // pager's proxy list either way.
        self_ref = pager_->ReleaseProxy(this);
      }
      self_src = ktl::move(page_source_);
    }
  }
}

void PagerProxy::OnPacketFreedLocked() {
  // We are here because the active request has been freed. And packet_busy_ is still true, so no
  // new request will have become active yet.
  DEBUG_ASSERT(active_request_ == nullptr);
  packet_busy_ = false;
  if (!pending_requests_.is_empty()) {
    QueuePacketLocked(pending_requests_.pop_front());
  }
}

void PagerProxy::SetPageSourceUnchecked(fbl::RefPtr<PageSource> src) {
  // SetPagerSource is a private function and is only called by the PagerDispatcher just after
  // construction, unfortunately it needs to be called under the PagerDispatcher lock and lock
  // ordering is always PagerProxy->PagerDispatcher, and so we cannot acquire the lock here.
  auto func = [this, &src]() TA_NO_THREAD_SAFETY_ANALYSIS { page_source_ = ktl::move(src); };
  func();
}

zx_status_t PagerProxy::WaitOnEvent(Event* event) {
  ThreadDispatcher::AutoBlocked by(ThreadDispatcher::Blocked::PAGER);
  kcounter_add(dispatcher_pager_total_request_count, 1);
  uint32_t waited = 0;
  // declare a lambda to calculate our deadline to avoid an excessively large statement in our
  // loop condition.
  auto make_deadline = []() {
    if (gBootOptions->userpager_overtime_wait_seconds == 0) {
      return Deadline::infinite();
    } else {
      return Deadline::after(ZX_SEC(gBootOptions->userpager_overtime_wait_seconds));
    }
  };
  zx_status_t result;
  while ((result = event->Wait(make_deadline())) == ZX_ERR_TIMED_OUT) {
    waited++;
    // We might trigger this loop multiple times as we exceed multiples of the overtime counter, but
    // we only want to count each unique overtime event in the kcounter.
    if (waited == 1) {
      dispatcher_pager_overtime_wait_count.Add(1);
    }

    // Error out if we've been waiting for longer than the specified timeout, to allow the rest of
    // the system to make progress (if possible).
    if (gBootOptions->userpager_overtime_timeout_seconds > 0 &&
        waited * gBootOptions->userpager_overtime_wait_seconds >=
            gBootOptions->userpager_overtime_timeout_seconds) {
      Guard<Mutex> guard{&mtx_};
      printf("ERROR Page source %p has been blocked for %" PRIu64
             " seconds. Page request timed out.\n",
             page_source_.get(), gBootOptions->userpager_overtime_timeout_seconds);
      Thread::Current::Dump(false);
      kcounter_add(dispatcher_pager_timed_out_request_count, 1);
      return ZX_ERR_TIMED_OUT;
    }

    // Determine whether we have any requests that have not yet been received off of the port.
    fbl::RefPtr<PageSource> src;
    bool active;
    {
      Guard<Mutex> guard{&mtx_};
      active = !!active_request_;
      src = page_source_;
    }
    printf("WARNING Page source %p has been blocked for %" PRIu64
           " seconds with%s message waiting on port.\n",
           src.get(), waited * gBootOptions->userpager_overtime_wait_seconds, active ? "" : " no");
    // Dump out the rest of the state of the oustanding requests.
    if (src) {
      src->Dump(0);
    }
  }

  if (result == ZX_OK) {
    kcounter_add(dispatcher_pager_succeeded_request_count, 1);
  } else {
    // Only counts failures that are *not* pager timeouts. Timeouts are tracked with
    // dispatcher_pager_timed_out_request_count, which is updated above when we
    // return early with ZX_ERR_TIMED_OUT.
    kcounter_add(dispatcher_pager_failed_request_count, 1);
  }

  return result;
}

void PagerProxy::Dump(uint depth) {
  Guard<Mutex> guard{&mtx_};
  for (uint i = 0; i < depth; ++i) {
    printf("  ");
  }
  printf("pager_proxy %p pager_dispatcher %p page_source %p key %lu\n", this, pager_,
         page_source_.get(), key_);

  for (uint i = 0; i < depth; ++i) {
    printf("  ");
  }
  printf("  source closed %d pager closed %d packet_busy %d complete_pending %d\n",
         page_source_closed_, pager_dispatcher_closed_, packet_busy_, complete_pending_);

  for (uint i = 0; i < depth; ++i) {
    printf("  ");
  }
  if (active_request_) {
    printf("  active %s request on pager port [0x%lx, 0x%lx)\n",
           PageRequestTypeToString(GetRequestType(active_request_)),
           GetRequestOffset(active_request_),
           GetRequestOffset(active_request_) + GetRequestLen(active_request_));
  } else {
    printf("  no active request on pager port\n");
  }

  if (pending_requests_.is_empty()) {
    for (uint i = 0; i < depth; ++i) {
      printf("  ");
    }
    printf("  no pending requests to queue on pager port\n");
    return;
  }

  for (auto& req : pending_requests_) {
    for (uint i = 0; i < depth; ++i) {
      printf("  ");
    }
    printf("  pending %s req to queue on pager port [0x%lx, 0x%lx)\n",
           PageRequestTypeToString(GetRequestType(&req)), GetRequestOffset(&req),
           GetRequestOffset(&req) + GetRequestLen(&req));
  }
}
