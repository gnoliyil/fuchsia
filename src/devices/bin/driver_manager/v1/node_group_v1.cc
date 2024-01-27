// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_manager/v1/node_group_v1.h"

#include "src/devices/lib/log/log.h"

namespace node_group {

zx::result<std::unique_ptr<NodeGroupV1>> NodeGroupV1::Create(
    NodeGroupCreateInfo create_info, fuchsia_device_manager::wire::NodeGroupDescriptor group_desc,
    DriverLoader* driver_loader) {
  ZX_ASSERT(driver_loader);

  fbl::Array<std::unique_ptr<Metadata>> metadata(
      new std::unique_ptr<Metadata>[group_desc.metadata.count()], group_desc.metadata.count());
  for (size_t i = 0; i < group_desc.metadata.count(); i++) {
    std::unique_ptr<Metadata> md;
    auto status = Metadata::Create(group_desc.metadata[i].data.count(), &md);
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to create metadata %s", zx_status_get_string(status));
      return zx::error(status);
    }

    md->type = group_desc.metadata[i].key;
    md->length = static_cast<uint32_t>(group_desc.metadata[i].data.count());
    memcpy(md->Data(), group_desc.metadata[i].data.data(), md->length);
    metadata[i] = std::move(md);
  }

  return zx::ok(
      std::make_unique<NodeGroupV1>(std::move(create_info), std::move(metadata), driver_loader));
}

NodeGroupV1::NodeGroupV1(NodeGroupCreateInfo create_info,
                         fbl::Array<std::unique_ptr<Metadata>> metadata,
                         DriverLoader* driver_loader)
    : NodeGroup(std::move(create_info)),
      metadata_(std::move(metadata)),
      driver_loader_(driver_loader) {
  ZX_ASSERT(driver_loader_);
}

zx::result<std::optional<DeviceOrNode>> NodeGroupV1::BindNodeImpl(
    fuchsia_driver_index::wire::MatchedNodeGroupInfo info, const DeviceOrNode& device_or_node) {
  auto device_ptr = std::get_if<std::weak_ptr<DeviceV1Wrapper>>(&device_or_node);
  ZX_ASSERT(device_ptr);
  auto owned = device_ptr->lock();
  if (!owned) {
    LOGF(ERROR, "DeviceV1Wrapper weak_ptr not available");
    return zx::error(ZX_ERR_INTERNAL);
  }

  if (!composite_device_) {
    SetCompositeDevice(info);
  }

  auto owned_device = owned->device;
  auto status = composite_device_->BindFragment(info.node_index(), owned_device);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to BindFragment for '%.*s': %s",
         static_cast<uint32_t>(owned_device->name().size()), owned_device->name().data(),
         zx_status_get_string(status));
    return zx::error(status);
  }

  if (owned_device->name() == "sysmem-fidl" || owned_device->name() == "sysmem-banjo") {
    LOGF(DEBUG, "Node '%s' matched node representation '%d' of node group '%s'",
         owned_device->name().c_str(), info.node_index(), std::string(info.name().get()).c_str());
  } else {
    LOGF(INFO, "Node '%s' matched node representation '%d' of node group '%s'",
         owned_device->name().c_str(), info.node_index(), std::string(info.name().get()).c_str());
  }

  return zx::ok(std::nullopt);
}

void NodeGroupV1::SetCompositeDevice(fuchsia_driver_index::wire::MatchedNodeGroupInfo info) {
  ZX_ASSERT(!composite_device_);
  ZX_ASSERT(info.has_composite() && info.composite().has_driver_info() &&
            info.composite().driver_info().has_url() && info.composite().has_composite_name());
  ZX_ASSERT(info.has_node_index() && info.has_num_nodes() && info.has_node_names() &&
            info.has_primary_index());

  auto node_names = std::vector<std::string>(info.node_names().count());
  for (size_t i = 0; i < info.node_names().count(); i++) {
    node_names[i] = std::string(info.node_names()[i].get());
  }

  MatchedCompositeDevice composite_info = {
      .node = info.node_index(),
      .num_nodes = info.num_nodes(),
      .name = std::string(info.composite().composite_name().get()),
      .node_names = std::move(node_names),
  };

  auto fidl_driver_info = info.composite().driver_info();
  MatchedDriverInfo matched_driver_info = {
      .driver = driver_loader_->LoadDriverUrl(std::string(fidl_driver_info.driver_url().get())),
      .colocate = fidl_driver_info.has_colocate() && fidl_driver_info.colocate(),
  };

  composite_device_ = CompositeDevice::CreateFromDriverIndex(
      MatchedCompositeDriverInfo{.composite = composite_info, .driver_info = matched_driver_info},
      info.primary_index(), std::move(metadata_));
  metadata_ = fbl::Array<std::unique_ptr<Metadata>>();
}

}  // namespace node_group
