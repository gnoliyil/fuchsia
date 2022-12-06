// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_host/devfs_vnode.h"

#include <lib/ddk/device.h>

#include <string_view>

#include "src/devices/bin/driver_host/driver_host_context.h"
#include "src/devices/bin/driver_host/zx_device.h"
#include "src/devices/lib/fidl/transaction.h"
#include "src/devices/lib/log/log.h"

DeviceServer::DeviceServer(fbl::RefPtr<zx_device> dev, async_dispatcher_t* dispatcher)
    : dev_(std::move(dev)), dispatcher_(dispatcher) {}

void DeviceServer::ConnectToController(fidl::ServerEnd<fuchsia_device::Controller> server_end) {
  Serve(server_end.TakeChannel(), &controller_);
}

void DeviceServer::ConnectToDeviceFidl(zx::channel channel) { Serve(std::move(channel), &device_); }

void DeviceServer::ServeMultiplexed(zx::channel channel) {
  Serve(std::move(channel), &multiplexed_);
}

void DeviceServer::CloseAllConnections(fit::callback<void()> callback) {
  async::PostTask(dispatcher_, [this, callback = std::move(callback)]() mutable {
    if (bindings_.empty()) {
      if (callback != nullptr) {
        callback();
      }
      return;
    }
    if (callback != nullptr) {
      ZX_ASSERT(!std::exchange(callback_, std::move(callback)).has_value());
    }
    for (auto& [handle, binding] : bindings_) {
      binding.Unbind();
    }
  });
}

void DeviceServer::Serve(zx::channel channel, fidl::internal::IncomingMessageDispatcher* impl) {
  fidl::ServerEnd<TypeErasedProtocol> server_end(std::move(channel));
  async::PostTask(dispatcher_, [this, server_end = std::move(server_end), impl]() mutable {
    const zx_handle_t key = server_end.channel().get();
    const fidl::ServerBindingRef binding = fidl::internal::BindServerTypeErased<TypeErasedProtocol>(
        dispatcher_, std::move(server_end), impl,
        fidl::internal::ThreadingPolicy::kCreateAndTeardownFromDispatcherThread,
        [this, key](fidl::internal::IncomingMessageDispatcher*, fidl::UnbindInfo,
                    fidl::internal::AnyTransport) {
          DeviceServer& self = *this;
          size_t erased = self.bindings_.erase(key);
          ZX_ASSERT_MSG(erased == 1, "erased=%zu", erased);
          if (self.bindings_.empty()) {
            if (std::optional callback = std::exchange(self.callback_, {}); callback.has_value()) {
              callback.value()();
            }
          }
        });

    auto [it, inserted] = bindings_.try_emplace(key, binding);
    ZX_ASSERT_MSG(inserted, "handle=%d", key);
  });
}

DeviceServer::Node::Node(DeviceServer& parent) : parent_(parent) {}

void DeviceServer::Node::NotImplemented_(const std::string& name, fidl::CompleterBase& completer) {
  char buf[fuchsia_device::wire::kMaxDevicePathLen + 1];
  size_t actual;
  zx_status_t status =
      parent_.dev_->driver_host_context()->GetTopoPath(parent_.dev_, buf, sizeof(buf), &actual);
  LOGF(ERROR, "%s: unsupported call to %s", name.c_str(),
       status == ZX_OK ? buf : zx_status_get_string(status));

  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

void DeviceServer::Node::Close(CloseCompleter::Sync& completer) {
  completer.ReplySuccess();
  completer.Close(ZX_OK);
}

void DeviceServer::Node::Query(QueryCompleter::Sync& completer) {
  const std::string_view kProtocol = fuchsia_io::wire::kNodeProtocolName;
  uint8_t* data = reinterpret_cast<uint8_t*>(const_cast<char*>(kProtocol.data()));
  completer.Reply(fidl::VectorView<uint8_t>::FromExternal(data, kProtocol.size()));
}

void DeviceServer::Node::Clone(CloneRequestView request, CloneCompleter::Sync& completer) {
  if (request->flags != fuchsia_io::wire::OpenFlags::kCloneSameRights) {
    char buf[fuchsia_device::wire::kMaxDevicePathLen + 1];
    size_t actual;
    zx_status_t status =
        parent_.dev_->driver_host_context()->GetTopoPath(parent_.dev_, buf, sizeof(buf), &actual);
    LOGF(ERROR, "%s: unsupported clone flags=0x%x",
         status == ZX_OK ? buf : zx_status_get_string(status),
         static_cast<uint32_t>(request->flags));

    request->object.Close(ZX_ERR_NOT_SUPPORTED);
    return;
  }
  parent_.ServeMultiplexed(request->object.TakeChannel());
}

DeviceServer::Controller::Controller(DeviceServer& parent) : parent_(parent) {}

void DeviceServer::Controller::ConnectToDeviceFidl(ConnectToDeviceFidlRequestView request,
                                                   ConnectToDeviceFidlCompleter::Sync& completer) {
  parent_.ConnectToDeviceFidl(std::move(request->server));
}

void DeviceServer::Controller::Bind(BindRequestView request, BindCompleter::Sync& completer) {
  zx_status_t status = device_bind(parent_.dev_, std::string(request->driver.get()).c_str());
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  parent_.dev_->set_bind_conn([completer = completer.ToAsync()](zx_status_t status) mutable {
    completer.Reply(zx::make_result(status));
  });
}

void DeviceServer::Controller::GetCurrentPerformanceState(
    GetCurrentPerformanceStateCompleter::Sync& completer) {
  completer.Reply(parent_.dev_->current_performance_state());
}

void DeviceServer::Controller::Rebind(RebindRequestView request, RebindCompleter::Sync& completer) {
  parent_.dev_->set_rebind_drv_name(std::string(request->driver.get()).c_str());
  zx_status_t status = device_rebind(parent_.dev_.get());
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  // These will be set, until device is unbound and then bound again.
  parent_.dev_->set_rebind_conn([completer = completer.ToAsync()](zx_status_t status) mutable {
    completer.Reply(zx::make_result(status));
  });
}

void DeviceServer::Controller::UnbindChildren(UnbindChildrenCompleter::Sync& completer) {
  zx::result<bool> scheduled_unbind = device_schedule_unbind_children(parent_.dev_);
  if (scheduled_unbind.is_error()) {
    completer.ReplyError(scheduled_unbind.status_value());
    return;
  }

  // Handle case where we have no children to unbind (otherwise the callback below will never
  // fire).
  if (!scheduled_unbind.value()) {
    completer.ReplySuccess();
    return;
  }

  // Asynchronously respond to the unbind request once all children have been unbound.
  // The unbind children conn will be set until all the children of this device are unbound.
  parent_.dev_->set_unbind_children_conn(
      [completer = completer.ToAsync()](zx_status_t status) mutable {
        completer.Reply(zx::make_result(status));
      });
}

void DeviceServer::Controller::ScheduleUnbind(ScheduleUnbindCompleter::Sync& completer) {
  zx_status_t status = device_schedule_remove(parent_.dev_, true /* unbind_self */);
  completer.Reply(zx::make_result(status));
}

void DeviceServer::Controller::GetTopologicalPath(GetTopologicalPathCompleter::Sync& completer) {
  char buf[fuchsia_device::wire::kMaxDevicePathLen + 1];
  size_t actual;
  zx_status_t status =
      parent_.dev_->driver_host_context()->GetTopoPath(parent_.dev_, buf, sizeof(buf), &actual);
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  if (actual > 0) {
    // Remove the accounting for the null byte
    actual--;
  }
  auto path = fidl::StringView(buf, actual);
  completer.ReplySuccess(path);
}

void DeviceServer::Controller::GetMinDriverLogSeverity(
    GetMinDriverLogSeverityCompleter::Sync& completer) {
  if (!parent_.dev_->driver) {
    completer.Reply(ZX_ERR_UNAVAILABLE, fuchsia_logger::wire::LogLevelFilter::kNone);
    return;
  }
  fx_log_severity_t severity = fx_logger_get_min_severity(parent_.dev_->zx_driver()->logger());
  completer.Reply(ZX_OK, static_cast<fuchsia_logger::wire::LogLevelFilter>(severity));
}

void DeviceServer::Controller::SetMinDriverLogSeverity(
    SetMinDriverLogSeverityRequestView request, SetMinDriverLogSeverityCompleter::Sync& completer) {
  if (!parent_.dev_->driver) {
    completer.Reply(ZX_ERR_UNAVAILABLE);
    return;
  }
  auto status = parent_.dev_->zx_driver()->set_driver_min_log_severity(
      static_cast<fx_log_severity_t>(request->severity));
  completer.Reply(status);
}

void DeviceServer::Controller::SetPerformanceState(SetPerformanceStateRequestView request,
                                                   SetPerformanceStateCompleter::Sync& completer) {
  uint32_t out_state;
  zx_status_t status = parent_.dev_->driver_host_context()->DeviceSetPerformanceState(
      parent_.dev_, request->requested_state, &out_state);
  completer.Reply(status, out_state);
}

DeviceServer::MessageDispatcher::MessageDispatcher(DeviceServer& parent, bool multiplexing)
    : parent_(parent), multiplexing_(multiplexing) {}

void DeviceServer::MessageDispatcher::dispatch_message(
    fidl::IncomingHeaderAndMessage&& msg, fidl::Transaction* txn,
    fidl::internal::MessageStorageViewBase* storage_view) {
  // If the device is unbound it shouldn't receive messages so close the channel.
  if (parent_.dev_->Unbound()) {
    txn->Close(ZX_ERR_IO_NOT_PRESENT);
    return;
  }

  if (multiplexing_) {
    if (fidl::WireTryDispatch(&parent_.node_, msg, txn) == fidl::DispatchResult::kFound) {
      return;
    }
    if (fidl::WireTryDispatch(&parent_.controller_, msg, txn) == fidl::DispatchResult::kFound) {
      return;
    }
  }

  fidl_incoming_msg_t c_msg = std::move(msg).ReleaseToEncodedCMessage();
  auto ddk_txn = MakeDdkInternalTransaction(txn);
  zx_status_t status = parent_.dev_->MessageOp(&c_msg, ddk_txn.Txn());
  if (status != ZX_OK && status != ZX_ERR_ASYNC) {
    // Close the connection on any error
    txn->Close(status);
  }
}
