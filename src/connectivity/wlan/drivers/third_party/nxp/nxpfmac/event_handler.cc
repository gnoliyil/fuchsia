// Copyright (c) 2022 The Fuchsia Authors
//
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without
// fee is hereby granted, provided that the above copyright notice and this permission notice
// appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
// SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
// AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
// OF THIS SOFTWARE.

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/event_handler.h"

#include <lib/ddk/debug.h>
#include <zircon/status.h>

namespace wlan::nxpfmac {

void EventHandler::RegisterForEvent(mlan_event_id event_id, EventCallback&& callback) {
  std::lock_guard lock(mutex_);

  callbacks_[event_id].emplace_back(Callback{.callback = std::move(callback)});
}

void EventHandler::RegisterForInterfaceEvent(mlan_event_id event_id, uint32_t bss_idx,
                                             EventCallback&& callback) {
  std::lock_guard lock(mutex_);

  callbacks_[event_id].emplace_back(Callback{.bss_idx = bss_idx, .callback = std::move(callback)});
}

void EventHandler::OnEvent(pmlan_event event) {
  std::lock_guard lock(mutex_);
  auto callbacks = callbacks_.find(event->event_id);
  if (callbacks == callbacks_.end()) {
    // No callbacks registered, everything is fine.
    return;
  }

  for (auto& callback : callbacks->second) {
    // If the callback has a bss_idx we'll get it and compare it to the event's bss_idx. Otherwise
    // we return the same index we're looking for because this callback should trigger for all
    // bss_indexes.
    if (callback.bss_idx.value_or(event->bss_index) == event->bss_index) {
      callback.callback(event);
    }
  }
}

}  // namespace wlan::nxpfmac
