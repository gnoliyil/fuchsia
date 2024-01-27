// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remote_characteristic.h"

#include "client.h"
#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/slab_allocator.h"

namespace bt::gatt {

RemoteCharacteristic::PendingNotifyRequest::PendingNotifyRequest(ValueCallback value_cb,
                                                                 NotifyStatusCallback status_cb)
    : value_callback(std::move(value_cb)), status_callback(std::move(status_cb)) {
  BT_DEBUG_ASSERT(value_callback);
  BT_DEBUG_ASSERT(status_callback);
}

RemoteCharacteristic::RemoteCharacteristic(Client::WeakPtr client, const CharacteristicData& info)
    : info_(info),
      discovery_error_(false),
      ccc_handle_(att::kInvalidHandle),
      ext_prop_handle_(att::kInvalidHandle),
      next_notify_handler_id_(1u),
      client_(std::move(client)),
      weak_self_(this) {
  BT_DEBUG_ASSERT(client_.is_alive());
}

RemoteCharacteristic::~RemoteCharacteristic() {
  ResolvePendingNotifyRequests(ToResult(HostError::kFailed));

  // Clear the CCC if we have enabled notifications and destructor was not called as a result of a
  // Service Changed notification.
  if (!notify_handlers_.empty()) {
    notify_handlers_.clear();
    // Don't disable notifications if the service changed as this characteristic may no longer
    // exist, may have been changed, or may have moved. If the characteristic is still valid, the
    // server may continue to send notifications, but they will be ignored until a new handler is
    // registered.
    if (!service_changed_) {
      DisableNotificationsInternal();
    }
  }
}

void RemoteCharacteristic::UpdateDataWithExtendedProperties(ExtendedProperties ext_props) {
  // |CharacteristicData| is an immutable snapshot into the data associated with this
  // Characteristic. Update |info_| with the most recent snapshot - the only new member is the
  // recently read |ext_props|.
  info_ =
      CharacteristicData(info_.properties, ext_props, info_.handle, info_.value_handle, info_.type);
}

void RemoteCharacteristic::DiscoverDescriptors(att::Handle range_end,
                                               att::ResultFunction<> callback) {
  BT_DEBUG_ASSERT(client_.is_alive());
  BT_DEBUG_ASSERT(callback);
  BT_DEBUG_ASSERT(range_end >= info().value_handle);

  discovery_error_ = false;
  descriptors_.clear();

  if (info().value_handle == range_end) {
    callback(fit::ok());
    return;
  }

  auto self = weak_self_.GetWeakPtr();
  auto desc_cb = [self](const DescriptorData& desc) {
    if (!self.is_alive())
      return;

    if (self->discovery_error_)
      return;

    if (desc.type == types::kClientCharacteristicConfig) {
      if (self->ccc_handle_ != att::kInvalidHandle) {
        bt_log(DEBUG, "gatt", "characteristic has more than one CCC descriptor!");
        self->discovery_error_ = true;
        return;
      }
      self->ccc_handle_ = desc.handle;
    } else if (desc.type == types::kCharacteristicExtProperties) {
      if (self->ext_prop_handle_ != att::kInvalidHandle) {
        bt_log(DEBUG, "gatt", "characteristic has more than one Extended Prop descriptor!");
        self->discovery_error_ = true;
        return;
      }

      // If the characteristic properties has the ExtendedProperties bit set, then
      // update the handle.
      if (self->properties() & Property::kExtendedProperties) {
        self->ext_prop_handle_ = desc.handle;
      } else {
        bt_log(DEBUG, "gatt", "characteristic extended properties not set");
      }
    }

    // As descriptors must be strictly increasing, this emplace should always succeed
    auto [_unused, success] = self->descriptors_.try_emplace(DescriptorHandle(desc.handle), desc);
    BT_DEBUG_ASSERT(success);
  };

  auto status_cb = [self, cb = std::move(callback)](att::Result<> status) mutable {
    if (!self.is_alive()) {
      cb(ToResult(HostError::kFailed));
      return;
    }

    if (self->discovery_error_) {
      status = ToResult(HostError::kFailed);
    }

    if (status.is_error()) {
      self->descriptors_.clear();
      cb(status);
      return;
    }

    // If the characteristic contains the ExtendedProperties descriptor, perform a Read operation
    // to get the extended properties before notifying the callback.
    if (self->ext_prop_handle_ != att::kInvalidHandle) {
      auto read_cb = [self, cb = std::move(cb)](att::Result<> status, const ByteBuffer& data,
                                                bool /*maybe_truncated*/) {
        if (status.is_error()) {
          cb(status);
          return;
        }

        // The ExtendedProperties descriptor value is a |uint16_t| representing the
        // ExtendedProperties bitfield. If the retrieved |data| is malformed, respond with an error
        // and return early.
        if (data.size() != sizeof(uint16_t)) {
          cb(ToResult(HostError::kPacketMalformed));
          return;
        }

        auto ext_props = le16toh(data.To<uint16_t>());
        self->UpdateDataWithExtendedProperties(ext_props);

        cb(status);
      };

      self->client_->ReadRequest(self->ext_prop_handle_, std::move(read_cb));
      return;
    }

    cb(status);
  };

  client_->DiscoverDescriptors(info().value_handle + 1, range_end, std::move(desc_cb),
                               std::move(status_cb));
}

void RemoteCharacteristic::EnableNotifications(ValueCallback value_callback,
                                               NotifyStatusCallback status_callback) {
  BT_DEBUG_ASSERT(client_.is_alive());
  BT_DEBUG_ASSERT(value_callback);
  BT_DEBUG_ASSERT(status_callback);

  if (!(info().properties & (Property::kNotify | Property::kIndicate))) {
    bt_log(DEBUG, "gatt", "characteristic does not support notifications");
    status_callback(ToResult(HostError::kNotSupported), kInvalidId);
    return;
  }

  // If notifications are already enabled then succeed right away.
  if (!notify_handlers_.empty()) {
    BT_DEBUG_ASSERT(pending_notify_reqs_.empty());

    IdType id = next_notify_handler_id_++;
    notify_handlers_[id] = std::move(value_callback);
    status_callback(fit::ok(), id);
    return;
  }

  pending_notify_reqs_.emplace(std::move(value_callback), std::move(status_callback));

  // If there are other pending requests to enable notifications then we'll wait
  // until the descriptor write completes.
  if (pending_notify_reqs_.size() > 1u)
    return;

  // It is possible for some characteristics that support notifications or indications to not have a
  // CCC descriptor. Such characteristics do not need to be directly configured to consider
  // notifications to have been enabled.
  if (ccc_handle_ == att::kInvalidHandle) {
    bt_log(TRACE, "gatt", "notications enabled without characteristic configuration");
    ResolvePendingNotifyRequests(fit::ok());
    return;
  }

  StaticByteBuffer<2> ccc_value;
  ccc_value.SetToZeros();

  // Enable indications if supported. Otherwise enable notifications.
  if (info().properties & Property::kIndicate) {
    ccc_value[0] = static_cast<uint8_t>(kCCCIndicationBit);
  } else {
    ccc_value[0] = static_cast<uint8_t>(kCCCNotificationBit);
  }

  auto self = weak_self_.GetWeakPtr();
  auto ccc_write_cb = [self](att::Result<> status) {
    bt_log(DEBUG, "gatt", "CCC write status (enable): %s", bt_str(status));
    if (self.is_alive()) {
      self->ResolvePendingNotifyRequests(status);
    }
  };

  client_->WriteRequest(ccc_handle_, ccc_value, std::move(ccc_write_cb));
}

bool RemoteCharacteristic::DisableNotifications(IdType handler_id) {
  BT_DEBUG_ASSERT(client_.is_alive());

  auto handler_iter = notify_handlers_.find(handler_id);
  if (handler_iter == notify_handlers_.end()) {
    bt_log(TRACE, "gatt", "notify handler not found (id: %lu)", handler_id);
    return false;
  }

  // Don't modify handlers map while handlers are being notified.
  if (notifying_handlers_) {
    handlers_pending_disable_.push_back(handler_id);
    return true;
  }
  notify_handlers_.erase(handler_iter);

  if (!notify_handlers_.empty())
    return true;

  DisableNotificationsInternal();
  return true;
}

void RemoteCharacteristic::DisableNotificationsInternal() {
  if (ccc_handle_ == att::kInvalidHandle) {
    // Nothing to do.
    return;
  }

  if (!client_.is_alive()) {
    bt_log(TRACE, "gatt", "client bearer invalid!");
    return;
  }

  // Disable notifications.
  StaticByteBuffer<2> ccc_value;
  ccc_value.SetToZeros();

  auto ccc_write_cb = [](att::Result<> status) {
    bt_log(DEBUG, "gatt", "CCC write status (disable): %s", bt_str(status));
  };

  // We send the request without handling the status as there is no good way to
  // recover from failing to disable notifications. If the peer continues to
  // send notifications, they will be dropped as no handlers are registered.
  client_->WriteRequest(ccc_handle_, ccc_value, std::move(ccc_write_cb));
}

void RemoteCharacteristic::ResolvePendingNotifyRequests(att::Result<> status) {
  // Don't iterate requests as callbacks can add new requests.
  while (!pending_notify_reqs_.empty()) {
    auto req = std::move(pending_notify_reqs_.front());
    pending_notify_reqs_.pop();

    IdType id = kInvalidId;

    if (status.is_ok()) {
      id = next_notify_handler_id_++;
      // Add handler to map before calling status callback in case callback removes the handler.
      notify_handlers_[id] = std::move(req.value_callback);
    }

    req.status_callback(status, id);
  }
}

void RemoteCharacteristic::HandleNotification(const ByteBuffer& value, bool maybe_truncated) {
  BT_DEBUG_ASSERT(client_.is_alive());

  notifying_handlers_ = true;
  for (auto& iter : notify_handlers_) {
    auto& handler = iter.second;
    handler(value, maybe_truncated);
  }
  notifying_handlers_ = false;

  // If handlers disabled themselves when notified, remove them from the map.
  for (IdType handler_id : handlers_pending_disable_) {
    notify_handlers_.erase(handler_id);
  }
  handlers_pending_disable_.clear();
}

}  // namespace bt::gatt
