// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "src/connectivity/weave/weavestack/app.h"

#include <lib/async/cpp/task.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <poll.h>
#include <zircon/types.h>

#pragma GCC diagnostic push
#include <Weave/DeviceLayer/PlatformManager.h>
#pragma GCC diagnostic pop

#include "src/connectivity/weave/lib/core/trait_updater.h"

namespace weavestack {
namespace {
using nl::Weave::DeviceLayer::PlatformMgr;
using nl::Weave::DeviceLayer::PlatformMgrImpl;
using nl::Weave::DeviceLayer::WeaveDeviceEvent;
}  // namespace

// TODO(fxbug.dev/59699): Implement system/platform timer in openweave
// and revert this to 10 seconds.
constexpr struct timeval MAX_SELECT_SLEEP_TIME = {.tv_sec = 1};

App::~App() { Quit(); }

void App::ClearWaiters() {
  waiters_.clear();
  if (sleep_task_ != nullptr) {
    sleep_task_->Cancel();
  }
}

void App::ClearFds() {
  FD_ZERO(&fds_.read_fds);
  FD_ZERO(&fds_.write_fds);
  FD_ZERO(&fds_.except_fds);
  fds_.num_fds = 0;
}

void App::Quit() {
  if (!initialized_) {
    return;
  }
  loop_.Quit();
  loop_.JoinThreads();
  ClearWaiters();
  ClearFds();
  PlatformMgrImpl().ShutdownWeaveStack();
  PlatformMgr().RemoveEventHandler(&App::OnPlatformEvent, reinterpret_cast<intptr_t>(this));
  initialized_ = false;
}

void App::OnPlatformEvent(const WeaveDeviceEvent* event, intptr_t arg) {
  switch (event->Type) {
    case nl::Weave::DeviceLayer::kShutdownRequest:
      // The adaptation layer may request a shutdown of weavestack. Platform events
      // are posted on the dispatcher, so shutting down the loop here will prevent
      // processing any socket data that might be in-flight.
      FX_LOGS(WARNING) << "Received shutdown request.";
      App* app = reinterpret_cast<App*>(arg);
      FX_DCHECK(app && app->loop());
      app->loop()->Quit();
      break;
  }
}

void App::TrampolineDoClose(int fd, intptr_t arg) {
  if (arg == 0) {
    FX_LOGS(ERROR) << "Invalid argument";
    return;
  }
  App* app = reinterpret_cast<App*>(arg);
  app->DoClose(fd);
}

void App::DoClose(int fd) {
  if (fd <= 0) {
    FX_LOGS(ERROR) << "Invalid fd: " << fd;
    return;
  }
  // Here |waiters_| is valid only during tunnel restart, after pairing is complete.
  // It is empty in other scenarios.
  if (waiters_.erase(fd) == 0) {
    FX_LOGS(DEBUG) << "Couldnt find fd " << fd << " in waiters_";
    return;
  }

  FD_CLR(fd, &fds_.read_fds);
  FD_CLR(fd, &fds_.write_fds);
  FD_CLR(fd, &fds_.except_fds);
}

zx_status_t App::Init() {
  zx_status_t status;
  fuchsia_logging::SetTags({"weavestack"});

  if (initialized_) {
    return ZX_ERR_BAD_STATE;
  }

  auto component_context = PlatformMgrImpl().GetComponentContextForProcess();

  // Bootstrap fidl is initialized first since if Bootstrap is serving then
  // WeaveStack is in bootstrap mode and should not fully initalize since normal
  // operation may interfere with the bootstrap.
  bootstrap_impl_ = std::make_unique<BootstrapImpl>(component_context);
  auto bootstrap_status = bootstrap_impl_->Init();
  if (bootstrap_status.has_value()) {
    status = bootstrap_status.value();
    initialized_ = true;
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "BootstrapImpl Init() failed with status = "
                     << zx_status_get_string(status);
    }
    return status;
  }

  // Set dispatcher before initializing the stack, which may reach out to other
  // components on the system to acquire initial state.
  PlatformMgrImpl().SetDispatcher(loop_.dispatcher());

  WEAVE_ERROR err = PlatformMgr().InitWeaveStack();
  if (err != WEAVE_NO_ERROR) {
    FX_LOGS(ERROR) << "InitWeaveStack() failed: " << nl::ErrorStr(err);
    return ZX_ERR_INTERNAL;
  }

  err = nl::Weave::DeviceLayer::TraitUpdater().Init();
  if (err != WEAVE_NO_ERROR) {
    FX_LOGS(ERROR) << "TraitUpdater init failed: " << nl::ErrorStr(err);
    return ZX_ERR_INTERNAL;
  }

  // Set handlers after initializing the weavestack, which resets its initial
  // handler variables on startup.
  PlatformMgr().AddEventHandler(&App::OnPlatformEvent, reinterpret_cast<intptr_t>(this));
  PlatformMgrImpl().GetInetLayer().SetPlatformSocketCloseHandler(TrampolineDoClose,
                                                                 reinterpret_cast<intptr_t>(this));

  // Kick FD handler to start the task loop.
  sleep_task_ = std::make_unique<async::TaskClosure>([this] { FdHandler(ZX_OK, 0); });

  // The stack implementation should remain the last member to be fully
  // initialized, as it will begin accepting FIDL requests after initialization
  // is complete, potentially interacting with the rest of WeaveStack.
  stack_impl_ = std::make_unique<StackImpl>(component_context);
  status = stack_impl_->Init();
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "StackImpl Init() failed with status = " << zx_status_get_string(status);
    return status;
  }

  stack_provider_impl_ = std::make_unique<StackProviderImpl>(component_context);
  status = stack_provider_impl_->Init();
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "StackProviderImpl Init() failed with status = "
                   << zx_status_get_string(status);
    return status;
  }

  initialized_ = true;
  return ZX_OK;
}

zx_status_t App::WaitForFd(int fd, uint32_t events) {
  auto waiter = std::make_unique<fsl::FDWaiter>(loop_.dispatcher());
  bool waited = waiter->Wait([this](zx_status_t status, uint32_t zero) { FdHandler(status, zero); },
                             fd, events);
  if (!waited) {
    FX_LOGS(ERROR) << "failed to wait for events on fd = " << fd;
  }
  waiters_[fd] = std::move(waiter);

  return ZX_OK;
}

// TODO(fxbug.dev/47096): tracks the integration test.
zx_status_t App::StartFdWaiters() {
  struct timeval sleep_time;
  memcpy(&sleep_time, &MAX_SELECT_SLEEP_TIME, sizeof(sleep_time));
  ClearFds();
  PlatformMgrImpl().GetSystemLayer().PrepareSelect(fds_.num_fds, &fds_.read_fds, &fds_.write_fds,
                                                   &fds_.except_fds, sleep_time);
  PlatformMgrImpl().GetInetLayer().PrepareSelect(fds_.num_fds, &fds_.read_fds, &fds_.write_fds,
                                                 &fds_.except_fds, sleep_time);

  for (auto fd = 0; fd < fds_.num_fds; ++fd) {
    uint32_t events = 0;
    if (FD_ISSET(fd, &fds_.read_fds)) {
      events |= POLLIN;
    }
    if (FD_ISSET(fd, &fds_.write_fds)) {
      events |= POLLOUT;
    }
    if (FD_ISSET(fd, &fds_.except_fds)) {
      events |= POLLERR;
    }
    if (events == 0) {
      continue;
    }
    zx_status_t status = WaitForFd(fd, events);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "error waiting for fd " << fd << ": " << status;
      return status;
    }
  }

  zx::duration duration(ZX_SEC(sleep_time.tv_sec) + ZX_USEC(sleep_time.tv_usec));
  return sleep_task_->PostDelayed(loop_.dispatcher(), duration);
}

void App::FdHandler(zx_status_t status, uint32_t zero) {
  if (status == ZX_ERR_CANCELED) {
    FX_VLOGS(1) << "waiter cancelled, doing nothing";
    return;
  }

  struct timeval sleep_time;
  memset(&sleep_time, 0, sizeof(sleep_time));
  int res = select(fds_.num_fds, &fds_.read_fds, &fds_.write_fds, &fds_.except_fds, &sleep_time);
  if (res < 0) {
    FX_LOGS(ERROR) << "failed to select on fds: " << strerror(errno);
    loop_.Quit();
    return;
  }

  // HandleSelectResult may respond by closing fds passed to it. The |waiters_| list
  // of FDWaiters must be cleared while the socket is still open to avoid assertion failures.
  ClearWaiters();

  PlatformMgrImpl().GetSystemLayer().HandleSelectResult(res, &fds_.read_fds, &fds_.write_fds,
                                                        &fds_.except_fds);
  PlatformMgrImpl().GetInetLayer().HandleSelectResult(res, &fds_.read_fds, &fds_.write_fds,
                                                      &fds_.except_fds);
  // Wait for the next set of events.
  status = StartFdWaiters();
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "failed to wait for next packet: " << status;
    loop_.Quit();
  }
}

zx_status_t App::Run(zx::time deadline, bool once) {
  zx_status_t status = async::PostTask(loop_.dispatcher(), [this]() {
    if (bootstrap_impl_->IsServing()) {
      return;
    }
    zx_status_t status = StartFdWaiters();
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "failed to wait for first packet: " << status;
      loop_.Quit();
    }
  });
  if (status != ZX_OK) {
    return status;
  }
  return loop_.Run(deadline, once);
}

}  // namespace weavestack
