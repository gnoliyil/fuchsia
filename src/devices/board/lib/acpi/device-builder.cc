// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/board/lib/acpi/device-builder.h"

#include <fidl/fuchsia.hardware.spi.businfo/cpp/wire.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <zircon/compiler.h>

#include <fbl/string_printf.h>

#include "src/devices/board/lib/acpi/acpi.h"
#ifdef __Fuchsia__
#include "src/devices/board/lib/acpi/device.h"
#include "src/devices/board/lib/acpi/irq-fragment.h"
#else
#include "src/devices/board/lib/acpi/device-for-host.h"
#endif
#include "src/devices/board/lib/acpi/manager.h"
#include "src/devices/board/lib/acpi/resources.h"
#include "src/devices/lib/acpi/util.h"

namespace acpi {
namespace {
static const zx_bind_inst_t kSysmemFragment[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_SYSMEM),
};

static const zx_bind_inst_t kSysmemFidlFragment[]{
    BI_MATCH_IF(EQ, BIND_FIDL_PROTOCOL, ZX_FIDL_PROTOCOL_SYSMEM),
};

}  // namespace

acpi::status<> DeviceBuilder::GatherResources(acpi::Acpi* acpi, fidl::AnyArena& allocator,
                                              acpi::Manager* manager,
                                              GatherResourcesCallback callback) {
  if (!handle_ || !parent_) {
    // Skip the root device.
    return acpi::ok();
  }

  // Don't decode resources if the ENABLED bit is not set.
  // See ACPI v6.4 section 6.3.7
  if (!(state_ & ACPI_STA_DEVICE_ENABLED)) {
    return acpi::ok();
  }

  // TODO(fxbug.dev/78565): Handle other resources like serial buses.
  auto result = acpi->WalkResources(
      handle_, "_CRS",
      [this, acpi, manager, &allocator, &callback](ACPI_RESOURCE* res) -> acpi::status<> {
        ACPI_HANDLE bus_parent = nullptr;
        BusType type = BusType::kUnknown;
        DeviceChildEntry entry;
        uint16_t bus_id_prop;
        if (resource_is_spi(res)) {
          type = BusType::kSpi;
          auto result = resource_parse_spi(acpi, handle_, res, allocator, &bus_parent);
          if (result.is_error()) {
            zxlogf(WARNING, "Failed to parse SPI resource: %d", result.error_value());
            return result.take_error();
          }
          entry = result.value();
          bus_id_prop = BIND_SPI_BUS_ID;
          dev_props_.emplace_back(
              zx_device_prop_t{.id = BIND_SPI_CHIP_SELECT, .value = result.value().cs()});
        } else if (resource_is_i2c(res)) {
          type = BusType::kI2c;
          auto result = resource_parse_i2c(acpi, handle_, res, allocator, &bus_parent);
          if (result.is_error()) {
            zxlogf(WARNING, "Failed to parse I2C resource: %d", result.error_value());
            return result.take_error();
          }
          entry = result.value();
          bus_id_prop = BIND_I2C_BUS_ID;
          dev_props_.emplace_back(
              zx_device_prop_t{.id = BIND_I2C_ADDRESS, .value = result.value().address()});
        } else if (resource_is_irq(res)) {
          irq_count_++;
        }

        if (bus_parent) {
          size_t bus_index = callback(bus_parent, type, entry);
          DeviceBuilder* b = manager->LookupDevice(bus_parent);
          buses_.emplace_back(b, bus_index);
          dev_props_.emplace_back(zx_device_prop_t{.id = bus_id_prop, .value = b->GetBusId()});
          has_address_ = true;
        }
        return acpi::ok();
      });
  if (result.is_error() && result.zx_status_value() != ZX_ERR_NOT_FOUND) {
    return result.take_error();
  }

  auto info = acpi->GetObjectInfo(handle_);
  if (info.is_error()) {
    zxlogf(WARNING, "Failed to get object info: %d", info.status_value());
    return info.take_error();
  }

  // PCI is special, and PCI devices don't have an explicit resource. Instead, we need to check
  // _ADR for PCI addressing info.
  if (parent_->bus_type_ == BusType::kPci) {
    if (info->Valid & ACPI_VALID_ADR) {
      callback(parent_->handle_, BusType::kPci, DeviceChildEntry(info->Address));
      // Set up some bind properties for ourselves. callback() should set HasBusId.
      ZX_ASSERT(parent_->HasBusId());
      uint32_t bus_id = parent_->GetBusId();
      uint32_t device = (info->Address & (0xffff0000)) >> 16;
      uint32_t func = info->Address & 0x0000ffff;
      dev_props_.emplace_back(zx_device_prop_t{
          .id = BIND_PCI_TOPO,
          .value = BIND_PCI_TOPO_PACK(bus_id, device, func),
      });
      // Should we buses_.emplace_back() here? The PCI bus driver currently publishes PCI
      // composites, so having a device on a PCI bus that uses other buses resources can't be
      // represented. Such devices don't seem to exist, but if we ever encounter one, it will need
      // to be handled somehow.
      has_address_ = true;
    }
  }

  bool has_devicetree_cid = false;
  // Add HID and CID properties, if present.
  if (info->Valid & ACPI_VALID_HID) {
    if (!strcmp(info->HardwareId.String, kDeviceTreeLinkID)) {
      has_devicetree_cid = CheckForDeviceTreeCompatible(acpi);
    } else {
      str_props_.emplace_back(OwnedStringProp("fuchsia.acpi.hid", info->HardwareId.String));
    }
  }

  if (!has_devicetree_cid && info->Valid & ACPI_VALID_CID && info->CompatibleIdList.Count > 0) {
    auto& first = info->CompatibleIdList.Ids[0];
    if (!strcmp(first.String, kDeviceTreeLinkID)) {
      has_devicetree_cid = CheckForDeviceTreeCompatible(acpi);
    } else {
      // We only expose the first CID.
      str_props_.emplace_back(OwnedStringProp("fuchsia.acpi.first_cid", first.String));
    }
  }

  // If our parent has a bus type, and we have an address on that bus, then we'll expose it in our
  // bind properties.
  if (parent_->GetBusType() != BusType::kUnknown && has_address_) {
    dev_props_.emplace_back(zx_device_prop_t{
        .id = BIND_ACPI_BUS_TYPE,
        .value = parent_->GetBusType(),
    });
  }
  if (result.status_value() == AE_NOT_FOUND) {
    return acpi::ok();
  }
  return result;
}

zx::result<zx_device_t*> DeviceBuilder::Build(acpi::Manager* manager,
                                              async_dispatcher_t* device_dispatcher) {
  if (parent_->zx_device_ == nullptr) {
    zxlogf(ERROR, "Parent has not been added to the tree yet!");
    return zx::error(ZX_ERR_BAD_STATE);
  }
  if (zx_device_ != nullptr) {
    zxlogf(ERROR, "This device (%s) has already been built!", name());
    return zx::error(ZX_ERR_BAD_STATE);
  }
  DeviceArgs device_args(parent_->zx_device_, manager, device_dispatcher, handle_);
  if (HasBusId() && bus_type_ != BusType::kPci) {
    zx::result<std::vector<uint8_t>> metadata = FidlEncodeMetadata();
    if (metadata.is_error()) {
      zxlogf(ERROR, "Error while encoding metadata for '%s': %d", name(), metadata.status_value());
      return metadata.take_error();
    }
    device_args.SetBusMetadata(std::move(*metadata), bus_type_, GetBusId());
  }
  auto device = std::make_unique<Device>(std::move(device_args));

  // Narrow our custom type down to zx_device_str_prop_t.
  // Any strings in zx_device_str_prop_t will still point at their equivalents
  // in the original str_props_ array.
  std::vector<zx_device_str_prop_t> str_props_for_ddkadd;
  for (auto& str_prop : str_props_) {
    str_props_for_ddkadd.emplace_back(str_prop);
  }

  uint32_t add_flags = DEVICE_ADD_MUST_ISOLATE;
  if ((state_ & (ACPI_STA_DEVICE_FUNCTIONING | ACPI_STA_DEVICE_PRESENT)) ==
      ACPI_STA_DEVICE_FUNCTIONING) {
    // Don't bind drivers to this device if it is functioning but not present.
    // See ACPI 6.4 section 6.3.7.
    add_flags |= DEVICE_ADD_NON_BINDABLE;
  }

  auto result = device->AddDevice(name(), cpp20::span(dev_props_),
                                  cpp20::span(str_props_for_ddkadd), add_flags);
  if (result.is_error()) {
    zxlogf(ERROR, "failed to publish acpi device '%s' (parent=%s): %d", name(), parent_->name(),
           result.status_value());
    return result.take_error();
  }

  auto* acpi_dev = device.release();
  zx_device_ = acpi_dev->zxdev();

  for (uint32_t i = 0; i < irq_count_; i++) {
#ifdef __Fuchsia__
    auto result = IrqFragment::Create(device_dispatcher, *acpi_dev, i, device_id_);
    if (result.is_error()) {
      zxlogf(ERROR, "Failed to construct IRQ fragment: %d", result.status_value());
      return result.take_error();
    }
#endif
  }

  auto status = BuildComposite(manager, str_props_for_ddkadd, device_dispatcher);
  if (status.is_error()) {
    zxlogf(WARNING, "failed to publish composite acpi device '%s-composite': %d", name(),
           status.error_value());
    return status.take_error();
  }

  return zx::ok(zx_device_);
}

size_t DeviceBuilder::AddBusChild(DeviceChildEntry d) {
  return std::visit(
      [this](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        // If we haven't initialised the vector yet, populate it.
        auto pval_empty = std::get_if<std::monostate>(&bus_children_);
        if (pval_empty) {
          auto tmp = DeviceChildData(std::vector<T>());
          bus_children_.swap(tmp);
        }

        auto pval = std::get_if<std::vector<T>>(&bus_children_);
        ZX_ASSERT_MSG(pval, "Bus %s had unexpected child type vector", name());
        pval->emplace_back(arg);
        return pval->size() - 1;
      },
      d);
}

zx::result<std::vector<uint8_t>> DeviceBuilder::FidlEncodeMetadata() {
#ifdef __Fuchsia__
  using SpiChannel = fuchsia_hardware_spi_businfo::wire::SpiChannel;
  using I2CChannel = fuchsia_hardware_i2c_businfo::wire::I2CChannel;
  fidl::Arena<> allocator;
  return std::visit(
      [this, &allocator](auto&& arg) -> zx::result<std::vector<uint8_t>> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return zx::ok(std::vector<uint8_t>());
        } else if constexpr (std::is_same_v<T, std::vector<SpiChannel>>) {
          ZX_ASSERT(HasBusId());  // Bus ID should get set when a child device is added.
          fuchsia_hardware_spi_businfo::wire::SpiBusMetadata metadata(allocator);
          for (auto& chan : arg) {
            chan.set_bus_id(GetBusId());
          }
          auto channels = fidl::VectorView<SpiChannel>::FromExternal(arg);
          metadata.set_channels(allocator, channels);
          return zx::result<std::vector<uint8_t>>{
              fidl::Persist(metadata).map_error(std::mem_fn(&fidl::Error::status))};
        } else if constexpr (std::is_same_v<T, std::vector<I2CChannel>>) {
          ZX_ASSERT(HasBusId());  // Bus ID should get set when a child device is added.
          fuchsia_hardware_i2c_businfo::wire::I2CBusMetadata metadata(allocator);
          for (auto& chan : arg) {
            chan.set_bus_id(GetBusId());
          }
          auto channels = fidl::VectorView<I2CChannel>::FromExternal(arg);
          metadata.set_channels(allocator, channels);
          return zx::result<std::vector<uint8_t>>{
              fidl::Persist(metadata).map_error(std::mem_fn(&fidl::Error::status))};
        } else {
          return zx::error(ZX_ERR_NOT_SUPPORTED);
        }
      },
      bus_children_);
#else  // __Fuchsia__
  return zx::error(ZX_ERR_NOT_SUPPORTED);
#endif
}

zx::result<> DeviceBuilder::BuildComposite(acpi::Manager* manager,
                                           std::vector<zx_device_str_prop_t>& str_props,
                                           async_dispatcher_t* device_dispatcher) {
  if (parent_->GetBusType() == BusType::kPci) {
    // If a device is on a PCI bus, the PCI bus driver will publish a composite device, so we
    // don't try to publish a composite.
    return zx::ok();
  }

  size_t fragment_count = buses_.size() + irq_count_ + 3;
  // Bookkeeping.
  // We use fixed-size arrays here rather than std::vector because we don't want
  // pointers to array members to become invalidated when the vector resizes.
  // While we could use vector.reserve(), there's no way to guarantee that future bugs won't be
  // caused by someone adding an extra fragment without first updating the reserve() call.
  auto bind_insns = std::make_unique<std::vector<zx_bind_inst_t>[]>(fragment_count);
  auto fragment_names = std::make_unique<fbl::String[]>(fragment_count);
  auto fragment_parts = std::make_unique<device_fragment_part_t[]>(fragment_count);
  auto fragments = std::make_unique<device_fragment_t[]>(fragment_count);
  std::unordered_map<BusType, uint32_t> parent_types;

  size_t bus_index = 0;
  // Generate fragments for every device we use.
  for (auto& pair : buses_) {
    DeviceBuilder* parent = pair.first;
    size_t child_index = pair.second;
    BusType type = parent->GetBusType();
    // Fragments are named <protocol>NNN, e.g. "i2c000", "i2c001".
    fragment_names[bus_index] = fbl::StringPrintf("%s%03u", BusTypeToString(type),
                                                  parent_types.emplace(type, 0).first->second++);

    std::vector<zx_bind_inst_t> insns = parent->GetFragmentBindInsnsForChild(child_index);
    bind_insns[bus_index] = std::move(insns);
    fragment_parts[bus_index] = device_fragment_part_t{
        .instruction_count = static_cast<uint32_t>(bind_insns[bus_index].size()),
        .match_program = bind_insns[bus_index].data(),
    };
    fragments[bus_index] = device_fragment_t{
        .name = fragment_names[bus_index].data(),
        .parts_count = 1,
        .parts = &fragment_parts[bus_index],
    };

    bus_index++;
  }

  for (uint32_t i = 0; i < irq_count_; i++) {
    fragment_names[bus_index] = fbl::StringPrintf("irq%03u", i);

    // Make sure that the bind rules here stay in sync
    // with the properties in irq-fragment.cc
    // LINT.IfChange
    std::vector<zx_bind_inst_t> insns{
        BI_ABORT_IF(NE, BIND_ACPI_ID, device_id_),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_INTERRUPT_ID, i + 1),
        BI_MATCH(),
    };
    // LINT.ThenChange(irq-fragment.cc)
    bind_insns[bus_index] = std::move(insns);

    fragment_parts[bus_index] = device_fragment_part_t{
        .instruction_count = static_cast<uint32_t>(bind_insns[bus_index].size()),
        .match_program = bind_insns[bus_index].data(),
    };
    fragments[bus_index] = device_fragment_t{
        .name = fragment_names[bus_index].data(),
        .parts_count = 1,
        .parts = &fragment_parts[bus_index],
    };

    bus_index++;
  }

  // Generate the ACPI fragment.
  std::vector<zx_bind_inst_t> insns = GetFragmentBindInsnsForSelf();
  bind_insns[bus_index] = std::move(insns);
  fragment_parts[bus_index] = device_fragment_part_t{
      .instruction_count = static_cast<uint32_t>(bind_insns[bus_index].size()),
      .match_program = bind_insns[bus_index].data(),
  };
  fragments[bus_index] = device_fragment_t{
      .name = "acpi",
      .parts_count = 1,
      .parts = &fragment_parts[bus_index],
  };
  bus_index++;

  // Generate the sysmem fragment.
  fragment_parts[bus_index] = device_fragment_part_t{
      .instruction_count = sizeof(kSysmemFragment) / sizeof(kSysmemFragment[0]),
      .match_program = kSysmemFragment,
  };
  fragments[bus_index] = device_fragment_t{
      .name = "sysmem",
      .parts_count = 1,
      .parts = &fragment_parts[bus_index],
  };
  bus_index++;

  // Generate the sysmem-fidl fragment.
  fragment_parts[bus_index] = device_fragment_part_t{
      .instruction_count = sizeof(kSysmemFidlFragment) / sizeof(kSysmemFidlFragment[0]),
      .match_program = kSysmemFidlFragment,
  };
  fragments[bus_index] = device_fragment_t{
      .name = "sysmem-fidl",
      .parts_count = 1,
      .parts = &fragment_parts[bus_index],
  };

  [[maybe_unused]] composite_device_desc_t composite_desc = {
      .props = dev_props_.data(),
      .props_count = dev_props_.size(),
      .str_props = str_props.data(),
      .str_props_count = str_props.size(),
      .fragments = fragments.get(),
      .fragments_count = fragment_count,
      .primary_fragment = "acpi",
      .spawn_colocated = true,
  };

  zx_status_t status = ZX_OK;
#if !defined(IS_TEST)
  // TODO(fxbug.dev/79923): re-enable this in tests once mock_ddk supports composites.
  auto composite_name = fbl::StringPrintf("%s-composite", name());
  // Don't worry about any metadata, since it's present in the "acpi" parent.
  DeviceArgs args(parent_->zx_device_, manager, device_dispatcher, handle_);
  auto composite_device = std::make_unique<Device>(args);
  status = composite_device->DdkAddComposite(composite_name.data(), &composite_desc);

  if (status == ZX_OK) {
    // The DDK takes ownership of the device, but only if DdkAddComposite succeeded.
    [[maybe_unused]] auto unused = composite_device.release();
  }
#endif

  return zx::make_result(status);
}

std::vector<zx_bind_inst_t> DeviceBuilder::GetFragmentBindInsnsForChild(size_t child_index) {
  std::vector<zx_bind_inst_t> ret;
  switch (bus_type_) {
    case BusType::kPci:
      ret.push_back(BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PCI));
      break;
    case BusType::kI2c:
      // No Banjo protocol needed for I2C.
      break;
    case BusType::kSpi:
      ret.push_back(BI_ABORT_IF(NE, BIND_FIDL_PROTOCOL, ZX_FIDL_PROTOCOL_SPI));
      break;
    case BusType::kUnknown:
      ZX_ASSERT(bus_type_ != BusType::kUnknown);
  }

  std::visit(
      [&ret, child_index](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        using SpiChannel = fuchsia_hardware_spi_businfo::wire::SpiChannel;
        using I2CChannel = fuchsia_hardware_i2c_businfo::wire::I2CChannel;
        if constexpr (std::is_same_v<T, std::monostate>) {
          ZX_ASSERT_MSG(false, "bus should have children");
        } else if constexpr (std::is_same_v<T, std::vector<SpiChannel>>) {
          SpiChannel& chan = arg[child_index];
          ret.push_back(BI_ABORT_IF(NE, BIND_SPI_BUS_ID, chan.bus_id()));
          ret.push_back(BI_ABORT_IF(NE, BIND_SPI_CHIP_SELECT, chan.cs()));
        } else if constexpr (std::is_same_v<T, std::vector<I2CChannel>>) {
          I2CChannel& chan = arg[child_index];
          ret.push_back(BI_ABORT_IF(NE, BIND_I2C_BUS_ID, chan.bus_id()));
          ret.push_back(BI_ABORT_IF(NE, BIND_I2C_ADDRESS, chan.address()));
          ret.push_back(BI_ABORT_IF(NE, BIND_FIDL_PROTOCOL, ZX_FIDL_PROTOCOL_I2C));
        }
      },
      bus_children_);

  // Only bind to the non-composite device.
  ret.push_back(BI_ABORT_IF(NE, BIND_COMPOSITE, 0));
  ret.push_back(BI_MATCH());

  return ret;
}

std::vector<zx_bind_inst_t> DeviceBuilder::GetFragmentBindInsnsForSelf() {
  std::vector<zx_bind_inst_t> ret;
  ret.push_back(BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_ACPI));
  for (auto& prop : dev_props_) {
    ret.push_back(BI_ABORT_IF(NE, static_cast<uint32_t>(prop.id), prop.value));
  }
  // Only bind to the non-composite device.
  ret.push_back(BI_ABORT_IF(NE, BIND_COMPOSITE, 0));
  ret.push_back(BI_MATCH());
  return ret;
}

bool DeviceBuilder::CheckForDeviceTreeCompatible(acpi::Acpi* acpi) {
  // UUID defined in "Device Properties UUID for _DSD", Revision 2.0, Section 2.1
  // https://uefi.org/sites/default/files/resources/_DSD-device-properties-UUID.pdf
  static constexpr Uuid kDevicePropertiesUuid =
      Uuid::Create(0xdaffd814, 0x6eba, 0x4d8c, 0x8a91, 0xbc9bbf4aa301);
  auto result = acpi->EvaluateObject(handle_, "_DSD", std::nullopt);
  if (result.is_error()) {
    if (result.zx_status_value() != ZX_ERR_NOT_FOUND) {
      zxlogf(WARNING, "Get _DSD for '%s' failed: %d", name(), result.error_value());
    }
    return false;
  }

  auto value = std::move(result.value());
  if (value->Type != ACPI_TYPE_PACKAGE) {
    zxlogf(WARNING, "'%s': Badly formed _DSD return value - wrong data type", name());
    return false;
  }

  // The package is an array of pairs. The first item in each pair is a UUID, and the second is the
  // value of that UUID.
  ACPI_OBJECT* properties = nullptr;
  for (size_t i = 0; (i + 1) < value->Package.Count; i += 2) {
    ACPI_OBJECT* uuid_buffer = &value->Package.Elements[i];
    if (uuid_buffer->Type != ACPI_TYPE_BUFFER || uuid_buffer->Buffer.Length != acpi::kUuidBytes) {
      zxlogf(WARNING, "'%s': _DSD entry %zu has invalid UUID.", name(), i);
      continue;
    }

    if (!memcmp(uuid_buffer->Buffer.Pointer, kDevicePropertiesUuid.bytes, acpi::kUuidBytes)) {
      properties = &value->Package.Elements[i + 1];
      break;
    }
  }

  if (!properties) {
    return false;
  }

  if (properties->Type != ACPI_TYPE_PACKAGE) {
    zxlogf(WARNING, "'%s': Device Properties _DSD value is not a package.", name());
    return false;
  }

  // properties should be a list of packages, which are each a key/value pair.
  for (size_t i = 0; i < properties->Package.Count; i++) {
    ACPI_OBJECT* pair = &properties->Package.Elements[i];
    if (pair->Type != ACPI_TYPE_PACKAGE || pair->Package.Count != 2) {
      continue;
    }

    ACPI_OBJECT* key = &pair->Package.Elements[0];
    ACPI_OBJECT* value = &pair->Package.Elements[1];
    if (key->Type != ACPI_TYPE_STRING || key->String.Length < sizeof("compatible") - 1) {
      continue;
    }

    if (!strcmp("compatible", key->String.Pointer) && value->Type == ACPI_TYPE_STRING) {
      str_props_.emplace_back(OwnedStringProp{"fuchsia.acpi.first_cid", value->String.Pointer});
      return true;
    }
  }
  return false;
}
}  // namespace acpi
