// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/memory/monitor/pressure_observer.h"

#include <fidl/fuchsia.kernel/cpp/wire.h>
#include <lib/async/cpp/task.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/directory.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <lib/zx/job.h>
#include <sys/stat.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include "src/developer/memory/monitor/pressure_notifier.h"

namespace monitor {

PressureObserver::PressureObserver(bool watch_for_changes, PressureNotifier* notifier)
    : notifier_(notifier) {
  if (InitMemPressureEvents() != ZX_OK) {
    return;
  }
  if (watch_for_changes) {
    // Set up a new thread (memory-pressure-loop) that watches for memory pressure changes from the
    // kernel. All this thread does is wait on memory pressure events in a loop, hence is kept
    // separate from memory_monitor's main dispatcher thread. Once the |PressureObserver| object has
    // been created, it is run entirely on the memory-pressure-loop thread.
    loop_.StartThread("memory-pressure-loop");
    watch_task_.Post(loop_.dispatcher());
  }
}

PressureObserver::~PressureObserver() {
  loop_.Quit();
  loop_.JoinThreads();
}

// Called from the constructor to set up waiting on kernel memory pressure events.
zx_status_t PressureObserver::InitMemPressureEvents() {
  auto client_end = component::Connect<fuchsia_kernel::RootJobForInspect>();
  if (!client_end.is_ok()) {
    return client_end.status_value();
  }

  auto result = fidl::WireCall(*client_end)->Get();
  if (result.status() != ZX_OK) {
    return result.status();
  }

  zx_status_t status;
  status = zx_system_get_event(result->job.get(), ZX_SYSTEM_EVENT_IMMINENT_OUT_OF_MEMORY,
                               events_[Level::kImminentOOM].reset_and_get_address());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "zx_system_get_event [IMMINENT-OOM] returned "
                   << zx_status_get_string(status);
    return status;
  }

  status = zx_system_get_event(result->job.get(), ZX_SYSTEM_EVENT_MEMORY_PRESSURE_CRITICAL,
                               events_[Level::kCritical].reset_and_get_address());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "zx_system_get_event [CRITICAL] returned " << zx_status_get_string(status);
    return status;
  }

  status = zx_system_get_event(result->job.get(), ZX_SYSTEM_EVENT_MEMORY_PRESSURE_WARNING,
                               events_[Level::kWarning].reset_and_get_address());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "zx_system_get_event [WARNING] returned " << zx_status_get_string(status);
    return status;
  }

  status = zx_system_get_event(result->job.get(), ZX_SYSTEM_EVENT_MEMORY_PRESSURE_NORMAL,
                               events_[Level::kNormal].reset_and_get_address());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "zx_system_get_event [NORMAL] returned " << zx_status_get_string(status);
    return status;
  }

  for (size_t i = 0; i < Level::kNumLevels; i++) {
    wait_items_[i].handle = events_[i].get();
    wait_items_[i].waitfor = ZX_EVENT_SIGNALED;
    wait_items_[i].pending = 0;
  }

  return ZX_OK;
}

void PressureObserver::WatchForChanges() {
  WaitOnLevelChange();

  watch_task_.Post(loop_.dispatcher());
}

void PressureObserver::WaitOnLevelChange() {
  // Wait on all events the first time around.
  size_t num_wait_items = level_initialized_ ? Level::kNumLevels - 1 : Level::kNumLevels;

  zx_status_t status = zx_object_wait_many(wait_items_.data(), num_wait_items, ZX_TIME_INFINITE);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "zx_object_wait_many returned " << zx_status_get_string(status);
    return;
  }

  for (size_t i = 0; i < Level::kNumLevels; i++) {
    if (wait_items_[i].pending) {
      wait_items_[i].pending = 0;
      OnLevelChanged(wait_items_[i].handle);

      // Move the event currently asserted to the end of the array.
      // Wait on only the first |kNumLevels| - 1 items next time around.
      std::swap(wait_items_[i].handle, wait_items_[Level::kNumLevels - 1].handle);
      break;
    }
  }
}

void PressureObserver::OnLevelChanged(zx_handle_t handle) {
  for (size_t i = 0; i < Level::kNumLevels; i++) {
    if (events_[i].get() == handle) {
      level_ = Level(i);
      break;
    }
  }

  if (unlikely(!level_initialized_)) {
    // Record that the level has been initialized if this is the first time. Before this, the
    // |PressureNotifier| will advertise the pressure level as Normal when watchers register. This
    // is fine because once the level is initialized, we will send out another pressure signal if
    // required (i.e. if the pressure level is not Normal). See comment near |level_| in class
    // definition.
    level_initialized_ = true;
  }

  if (notifier_ != nullptr) {
    // Notify the |PressureNotifier| that the level has changed. |PressureNotifier::Notify()| is a
    // lightweight call which simply causes a notification task to be queued on the
    // |PressureNotifier|'s thread. The notification task is not executed on our thread, whose only
    // job is to observe kernel memory pressure changes.
    notifier_->Notify();
  }
}

Level PressureObserver::GetCurrentLevelForWatcher() const {
  Level current_level = level_;
  // Watchers of the memory pressure service do not recognize the Imminent-OOM level, the highest
  // level that they can receive is Critical.
  return (current_level == Level::kImminentOOM) ? Level::kCritical : current_level;
}

}  // namespace monitor
