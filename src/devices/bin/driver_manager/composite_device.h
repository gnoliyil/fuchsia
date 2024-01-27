// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_COMPOSITE_DEVICE_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_COMPOSITE_DEVICE_H_

#include <fidl/fuchsia.device.manager/cpp/wire.h>
#include <lib/ddk/binding.h>

#include <memory>
#include <string_view>

#include <fbl/array.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/string.h>

#include "driver.h"
#include "metadata.h"

// Forward declaration
class CompositeDevice;
class Coordinator;
class Device;
class DriverHost;

// Tags used for container membership identification
namespace internal {
struct CdfListTag {};
struct CdfDeviceListTag {};
}  // namespace internal

using StrPropertyValue = std::variant<uint32_t, std::string, bool, std::string>;

enum StrPropValueType { Integer = 0, String = 1, Bool = 2, Enum = 3 };

struct StrProperty {
  std::string key;
  StrPropertyValue value;
};

// This class represents a fragment of a composite device.
// It holds information like the bind instructions, and if a device has been
// bound to the fragment.
// It also helps create proxies for the composite.
class CompositeDeviceFragment
    : public fbl::ContainableBaseClasses<
          fbl::TaggedDoublyLinkedListable<std::unique_ptr<CompositeDeviceFragment>,
                                          internal::CdfListTag,
                                          fbl::NodeOptions::AllowMultiContainerUptr>,
          fbl::TaggedDoublyLinkedListable<CompositeDeviceFragment*, internal::CdfDeviceListTag>> {
 public:
  using ListTag = internal::CdfListTag;
  using DeviceListTag = internal::CdfDeviceListTag;

  CompositeDeviceFragment(CompositeDevice* composite, std::string name, uint32_t index,
                          fbl::Array<const zx_bind_inst_t> bind_rules);

  CompositeDeviceFragment(CompositeDeviceFragment&&) = delete;
  CompositeDeviceFragment& operator=(CompositeDeviceFragment&&) = delete;

  CompositeDeviceFragment(const CompositeDeviceFragment&) = delete;
  CompositeDeviceFragment& operator=(const CompositeDeviceFragment&) = delete;

  ~CompositeDeviceFragment();

  // Attempt to match this fragment against |dev|.  Returns true if the match
  // was successful.
  bool TryMatch(const fbl::RefPtr<Device>& dev) const;

  // Bind this fragment to the given device.
  zx_status_t Bind(const fbl::RefPtr<Device>& dev);

  // Unbind this fragment.
  void Unbind();

  bool IsBound() const { return bound_device_ != nullptr; }

  // This is true if this fragment is ready for assembly. That means that it
  // is bound and its fragment driver (or fidl proxy) is ready.
  bool IsReady();

  // Create a proxy for this fragment in the specific driver host.
  // This may create a Banjo or FIDL proxy depending on the device bound to this
  // fragment.
  zx_status_t CreateProxy(fbl::RefPtr<DriverHost> driver_host);

  std::string_view name() const { return name_; }
  uint32_t index() const { return index_; }
  CompositeDevice* composite() const { return composite_; }

  // This is the device that the fragment matches with.
  const fbl::RefPtr<Device>& bound_device() const { return bound_device_; }

  // This is the device created by the fragment driver (fragment.so).
  // It is the same driver host as `bound_device()`.
  // This does only exists if `bound_device()` needs banjo proxying.
  const fbl::RefPtr<Device>& fragment_device() const { return fragment_device_; }

  // This is the proxy device that lives in the driver host of the composite
  // device.
  // If `bound_device()` does banjo proxying this is a device from fragment.proxy.so.
  // If `bound_device()` does FIDL proxying this is a fidl proxy.
  // If `bound_device()` is the primary parent and the composite is colocated
  // this the same as `bound_device()`.
  const fbl::RefPtr<Device>& proxy_device() const { return proxy_device_; }

  // Registers (or unregisters) the fragment device (i.e. an instance of the
  // "fragment" driver) that bound to bound_device().
  void set_fragment_device(fbl::RefPtr<Device> device) { fragment_device_ = std::move(device); }

  void set_proxy_device(fbl::RefPtr<Device> device) { proxy_device_ = std::move(device); }

  bool uses_fragment_driver() const { return uses_fragment_driver_; }

 private:
  // The CompositeDevice that this is a part of.
  CompositeDevice* const composite_;

  // The name of this fragment within its CompositeDevice.
  const std::string name_;

  // The index of this fragment within its CompositeDevice.
  const uint32_t index_;

  // Bind rules for the fragment.
  const fbl::Array<const zx_bind_inst_t> bind_rules_;

  // This is the device that the composite fragment has matched to.
  // If this is null then this fragment does not have a device yet.
  //
  // Note that if this device needs to be Banjo proxied, then we will create
  // `fragment_device_` to do the proxying.
  fbl::RefPtr<Device> bound_device_ = nullptr;

  // This is the device created by the fragment driver (fragment.so) that
  // was bound to `bound_device_`.
  //
  // Note that this is fragment.so, not fragment.proxy.so, this device lives
  // in the same driver host as `bound_device_`.
  //
  // This only exists if `bound_device_` needs to be Banjo proxied.
  fbl::RefPtr<Device> fragment_device_ = nullptr;

  // This is the device that lives in the driver_host of the final composite device.
  // It may be a Banjo proxied device, it may be a FIDL proxied device, or it may
  // be `bound_device_` if the composite is colocated.
  fbl::RefPtr<Device> proxy_device_ = nullptr;

  bool uses_fragment_driver_ = false;
};

// A device composed of other devices.
class CompositeDevice : public fbl::DoublyLinkedListable<std::unique_ptr<CompositeDevice>> {
 public:
  // Only public because of make_unique.  You probably want Create().
  CompositeDevice(fbl::String name, fbl::Array<const zx_device_prop_t> properties,
                  fbl::Array<const StrProperty> str_properties, uint32_t fragments_count,
                  uint32_t primary_fragment_index, bool spawn_colocated,
                  fbl::Array<std::unique_ptr<Metadata>> metadata, bool from_driver_index);

  CompositeDevice(CompositeDevice&&) = delete;
  CompositeDevice& operator=(CompositeDevice&&) = delete;

  CompositeDevice(const CompositeDevice&) = delete;
  CompositeDevice& operator=(const CompositeDevice&) = delete;

  ~CompositeDevice();

  static zx_status_t Create(std::string_view name,
                            fuchsia_device_manager::wire::CompositeDeviceDescriptor comp_desc,
                            std::unique_ptr<CompositeDevice>* out);

  static std::unique_ptr<CompositeDevice> CreateFromDriverIndex(
      MatchedCompositeDriverInfo driver, uint32_t primary_index,
      fbl::Array<std::unique_ptr<Metadata>> metadata);

  const fbl::String& name() const { return name_; }
  const fbl::Array<const zx_device_prop_t>& properties() const { return properties_; }
  const fbl::Array<const StrProperty>& str_properties() const { return str_properties_; }
  uint32_t fragments_count() const { return fragments_count_; }

  // Returns a reference to the constructed composite device, if it exists.
  fbl::RefPtr<Device> device() const { return device_; }

  // Attempt to match and bind any of the unbound fragments against |dev|.
  zx_status_t TryMatchBindFragments(const fbl::RefPtr<Device>& dev);

  // Bind the fragment with the given index to the specified device
  zx_status_t BindFragment(size_t index, const fbl::RefPtr<Device>& dev);

  // Mark the given fragment as unbound.  Note that since we don't expose
  // this device's fragments in the API, this method can only be invoked by
  // CompositeDeviceFragment
  void UnbindFragment(CompositeDeviceFragment* fragment);

  // Creates the actual device and orchestrates the creation of the composite
  // device in a driver_host.
  // Returns ZX_ERR_SHOULD_WAIT if some fragment is not fully ready (i.e. has
  // either not been matched or the fragment driver that bound to it has not
  // yet published its device).
  zx_status_t TryAssemble();

  // Forget about the composite device that was constructed.  If TryAssemble()
  // is invoked after this, it will reassemble the device.
  void Remove();

  using FragmentList = fbl::TaggedDoublyLinkedList<std::unique_ptr<CompositeDeviceFragment>,
                                                   CompositeDeviceFragment::ListTag>;
  FragmentList& fragments() { return fragments_; }
  const FragmentList& fragments() const { return fragments_; }

  CompositeDeviceFragment* primary_fragment() {
    for (auto& fragment : fragments_) {
      if (fragment.index() == primary_fragment_index_) {
        return &fragment;
      }
    }
    return nullptr;
  }
  const CompositeDeviceFragment* primary_fragment() const {
    for (auto& fragment : fragments_) {
      if (fragment.index() == primary_fragment_index_) {
        return &fragment;
      }
    }
    return nullptr;
  }

 private:
  // Get the driver host that the composite device will live in.
  // If the composite device does not have a driver host yet, this function
  // will create a new one.
  zx::result<fbl::RefPtr<DriverHost>> GetDriverHost();

  // Returns true if a fragment matches |dev|. Sets |*index_out| will be set to the
  // matching fragment.
  bool IsFragmentMatch(const fbl::RefPtr<Device>& dev, size_t* index_out) const;

  const fbl::String name_;
  const fbl::Array<const zx_device_prop_t> properties_;
  const fbl::Array<const StrProperty> str_properties_;

  const uint32_t fragments_count_;
  const uint32_t primary_fragment_index_;

  const bool spawn_colocated_;
  const fbl::Array<std::unique_ptr<Metadata>> metadata_;

  // Driver index variables. |driver_index_driver_| is set by CreateFromDriverIndex().
  const bool from_driver_index_;
  MatchedDriverInfo driver_index_driver_;

  FragmentList fragments_;

  // The driver host that the composite device will be placed into.
  // This will only be set once in GetDriverHost().
  fbl::RefPtr<DriverHost> driver_host_;

  // Once the composite has been assembled, this refers to the constructed
  // device.
  fbl::RefPtr<Device> device_;
};

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_COMPOSITE_DEVICE_H_
