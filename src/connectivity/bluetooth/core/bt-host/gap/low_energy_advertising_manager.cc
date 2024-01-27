// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "low_energy_advertising_manager.h"

#include "low_energy_address_manager.h"
#include "peer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/random.h"
#include "src/connectivity/bluetooth/core/bt-host/common/slab_allocator.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/util.h"
#include "src/connectivity/bluetooth/lib/cpp-string/string_printf.h"

namespace bt::gap {

namespace {

// Returns the matching minimum and maximum advertising interval values in controller timeslices.
hci::AdvertisingIntervalRange GetIntervalRange(AdvertisingInterval interval) {
  switch (interval) {
    case AdvertisingInterval::FAST1:
      return {kLEAdvertisingFastIntervalMin1, kLEAdvertisingFastIntervalMax1};
    case AdvertisingInterval::FAST2:
      return {kLEAdvertisingFastIntervalMin2, kLEAdvertisingFastIntervalMax2};
    case AdvertisingInterval::SLOW:
      return {kLEAdvertisingSlowIntervalMin, kLEAdvertisingSlowIntervalMax};
  }

  BT_PANIC("unexpected advertising interval value");
  return {kLEAdvertisingSlowIntervalMin, kLEAdvertisingSlowIntervalMax};
}

}  // namespace

AdvertisementInstance::AdvertisementInstance() : id_(kInvalidAdvertisementId) {}

AdvertisementInstance::AdvertisementInstance(AdvertisementId id,
                                             WeakSelf<LowEnergyAdvertisingManager>::WeakPtr owner)
    : id_(id), owner_(std::move(owner)) {
  BT_DEBUG_ASSERT(id_ != kInvalidAdvertisementId);
  BT_DEBUG_ASSERT(owner_.is_alive());
}

AdvertisementInstance::~AdvertisementInstance() { Reset(); }

void AdvertisementInstance::Move(AdvertisementInstance* other) {
  // Destroy the old advertisement instance if active and clear the contents.
  Reset();

  // Transfer the old data over and clear |other| so that it no longer owns its advertisement.
  owner_ = std::move(other->owner_);
  id_ = other->id_;
  other->id_ = kInvalidAdvertisementId;
}

void AdvertisementInstance::Reset() {
  if (owner_.is_alive() && id_ != kInvalidAdvertisementId) {
    owner_->StopAdvertising(id_);
  }

  owner_.reset();
  id_ = kInvalidAdvertisementId;
}

class LowEnergyAdvertisingManager::ActiveAdvertisement final {
 public:
  // TODO(fxbug.dev/863): Don't randomly generate the ID of an advertisement.
  // Instead use a counter like other internal IDs once this ID is not visible
  // outside of bt-host.
  explicit ActiveAdvertisement(const DeviceAddress& address)
      : address_(address), id_(RandomPeerId().value()) {}

  ~ActiveAdvertisement() = default;

  const DeviceAddress& address() const { return address_; }
  AdvertisementId id() const { return id_; }

 private:
  DeviceAddress address_;
  AdvertisementId id_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ActiveAdvertisement);
};

LowEnergyAdvertisingManager::LowEnergyAdvertisingManager(
    hci::LowEnergyAdvertiser* advertiser, hci::LocalAddressDelegate* local_addr_delegate)
    : advertiser_(advertiser), local_addr_delegate_(local_addr_delegate), weak_self_(this) {
  BT_DEBUG_ASSERT(advertiser_);
  BT_DEBUG_ASSERT(local_addr_delegate_);
}

LowEnergyAdvertisingManager::~LowEnergyAdvertisingManager() {
  // Turn off all the advertisements!
  for (const auto& ad : advertisements_) {
    advertiser_->StopAdvertising(ad.second->address());
  }
}

void LowEnergyAdvertisingManager::StartAdvertising(AdvertisingData data, AdvertisingData scan_rsp,
                                                   ConnectionCallback connect_callback,
                                                   AdvertisingInterval interval, bool anonymous,
                                                   bool include_tx_power_level,
                                                   AdvertisingStatusCallback status_callback) {
  // Can't be anonymous and connectable
  if (anonymous && connect_callback) {
    bt_log(DEBUG, "gap-le", "can't advertise anonymously and connectable!");
    status_callback(AdvertisementInstance(), ToResult(HostError::kInvalidParameters));
    return;
  }

  // v5.1, Vol 3, Part C, Appendix A recommends the FAST1 parameters for connectable advertising and
  // FAST2 parameters for non-connectable advertising. Some Bluetooth controllers reject the FAST1
  // parameters for non-connectable advertising, hence we fall back to FAST2 in that case.
  if (interval == AdvertisingInterval::FAST1 && !connect_callback) {
    interval = AdvertisingInterval::FAST2;
  }
  hci::LowEnergyAdvertiser::AdvertisingOptions options(GetIntervalRange(interval), anonymous,
                                                       AdvFlag::kLEGeneralDiscoverableMode,
                                                       include_tx_power_level);

  auto self = weak_self_.GetWeakPtr();

  // TODO(fxbug.dev/1335): The address used for legacy advertising must be
  // coordinated via |local_addr_delegate_| however a unique address can be
  // generated and assigned to each advertising set when the controller
  // supports 5.0 extended advertising. hci::LowEnergyAdvertiser needs to be
  // revised to not use device addresses to distinguish between advertising
  // instances especially since |local_addr_delegate_| is likely to return the
  // same address if called frequently.
  //
  // Revisit this logic when multi-advertising is supported.
  local_addr_delegate_->EnsureLocalAddress(
      [self, data = std::move(data), scan_rsp = std::move(scan_rsp), options,
       connect_cb = std::move(connect_callback),
       status_cb = std::move(status_callback)](const auto& address) mutable {
        if (!self.is_alive()) {
          return;
        }

        auto ad_ptr = std::make_unique<ActiveAdvertisement>(address);
        hci::LowEnergyAdvertiser::ConnectionCallback adv_conn_cb;
        if (connect_cb) {
          adv_conn_cb = [self, id = ad_ptr->id(), connect_cb = std::move(connect_cb)](auto link) {
            bt_log(DEBUG, "gap-le", "received new connection");

            if (!self.is_alive()) {
              return;
            }

            // remove the advertiser because advertising has stopped
            self->advertisements_.erase(id);
            connect_cb(id, std::move(link));
          };
        }
        auto status_cb_wrapper = [self, ad_ptr = std::move(ad_ptr),
                                  status_cb = std::move(status_cb)](hci::Result<> status) mutable {
          if (!self.is_alive()) {
            return;
          }

          if (status.is_error()) {
            status_cb(AdvertisementInstance(), status);
            return;
          }

          auto id = ad_ptr->id();
          self->advertisements_.emplace(id, std::move(ad_ptr));
          status_cb(AdvertisementInstance(id, self), status);
        };

        // Call StartAdvertising, with the callback
        self->advertiser_->StartAdvertising(address, data, scan_rsp, options,
                                            std::move(adv_conn_cb), std::move(status_cb_wrapper));
      });
}

bool LowEnergyAdvertisingManager::StopAdvertising(AdvertisementId advertisement_id) {
  auto it = advertisements_.find(advertisement_id);
  if (it == advertisements_.end()) {
    return false;
  }

  advertiser_->StopAdvertising(it->second->address());
  advertisements_.erase(it);
  return true;
}

}  // namespace bt::gap
