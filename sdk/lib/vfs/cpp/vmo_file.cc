// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/vfs/cpp/vmo_file.h>

namespace vfs {

VmoFile::VmoFile(zx::unowned_vmo unowned_vmo, size_t offset, size_t length,
                 WriteOption write_option, Sharing vmo_sharing)
    : offset_(offset), length_(length), write_option_(write_option), vmo_sharing_(vmo_sharing) {
  unowned_vmo->duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_);
}

VmoFile::VmoFile(zx::vmo vmo, size_t offset, size_t length, WriteOption write_option,
                 Sharing vmo_sharing)
    : offset_(offset),
      length_(length),
      write_option_(write_option),
      vmo_sharing_(vmo_sharing),
      vmo_(std::move(vmo)) {}

VmoFile::~VmoFile() = default;

zx::vmo VmoFile::GetVmoForDescribe() {
  zx::vmo result_vmo;
  switch (vmo_sharing_) {
    case Sharing::NONE:
      break;
    case Sharing::DUPLICATE:
      // TODO(fxbug.dev/45287): As part of fxbug.dev/85334 it was discovered that Describe leaks
      // writable handles even if the connection lacks OPEN_RIGHT_WRITABLE. If duplicate handles
      // to the underlying VMO require specific rights, they should be obtained via GetBuffer(),
      // or we need to allow the VmoFile node itself query the connection rights (since these are
      // currently not available when handling the Describe call).
      //
      // If an exact/duplicate handle includes ZX_RIGHT_WRITE, we need to either track possible size
      // changes to the backing VMO, provide a writable child slice, or explicitly set the
      // content size and ensure the duplicate handle lacks ZX_RIGHT_SET_PROPERTY.
      vmo_.duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_READ | ZX_RIGHT_MAP | ZX_RIGHT_GET_PROPERTY,
                     &result_vmo);
      break;
    case Sharing::CLONE_COW:
      vmo_.create_child(ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE, offset_, length_, &result_vmo);
      break;
  }
  return result_vmo;
}

void VmoFile::Describe(fuchsia::io::NodeInfo* out_info) {
  zx::vmo client_vmo = GetVmoForDescribe();
  switch (vmo_sharing_) {
    case Sharing::NONE:
      out_info->set_file(fuchsia::io::FileObject());
      break;
    case Sharing::DUPLICATE:
    case Sharing::CLONE_COW:
      if (!client_vmo.is_valid()) {
        return;
      }
      out_info->vmofile() =
          fuchsia::io::Vmofile{.vmo = std::move(client_vmo), .offset = offset_, .length = length_};
      break;
  }
}

void VmoFile::Describe2(fuchsia::io::ConnectionInfo* out_info) {
  zx::vmo client_vmo = GetVmoForDescribe();
  switch (vmo_sharing_) {
    case Sharing::NONE:
      out_info->set_representation(fuchsia::io::Representation::WithFile(fuchsia::io::FileInfo()));
      break;
    case Sharing::DUPLICATE:
    case Sharing::CLONE_COW:
      if (!client_vmo.is_valid()) {
        return;
      }
      fuchsia::io::MemoryInfo mem_info;
      mem_info.set_buffer(
          fuchsia::mem::Range{.vmo = std::move(client_vmo), .offset = offset_, .size = length_});
      out_info->set_representation(fuchsia::io::Representation::WithMemory(std::move(mem_info)));
      break;
  }
}

zx_status_t VmoFile::ReadAt(uint64_t length, uint64_t offset, std::vector<uint8_t>* out_data) {
  if (length == 0u || offset >= length_) {
    return ZX_OK;
  }

  size_t remaining_length = length_ - offset;
  if (length > remaining_length) {
    length = remaining_length;
  }

  out_data->resize(length);
  return vmo_.read(out_data->data(), offset_ + offset, length);
}

zx_status_t VmoFile::WriteAt(std::vector<uint8_t> data, uint64_t offset, uint64_t* out_actual) {
  if (write_option_ != WriteOption::WRITABLE) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  size_t length = data.size();
  if (length == 0u) {
    *out_actual = 0u;
    return ZX_OK;
  }
  if (offset >= length_) {
    return ZX_ERR_NO_SPACE;
  }

  size_t remaining_length = length_ - offset;
  if (length > remaining_length) {
    length = remaining_length;
  }
  zx_status_t status = vmo_.write(data.data(), offset_ + offset, length);
  if (status == ZX_OK) {
    *out_actual = length;
  }
  return status;
}

zx_status_t VmoFile::Truncate(uint64_t length) { return ZX_ERR_NOT_SUPPORTED; }

size_t VmoFile::GetCapacity() { return length_; }

size_t VmoFile::GetLength() { return length_; }

zx_status_t VmoFile::GetAttr(fuchsia::io::NodeAttributes* out_attributes) const {
  fuchsia::io::OpenFlags flags = fuchsia::io::OpenFlags::RIGHT_READABLE;
  if (write_option_ == WriteOption::WRITABLE) {
    flags |= fuchsia::io::OpenFlags::RIGHT_WRITABLE;
  }
  out_attributes->mode = fuchsia::io::MODE_TYPE_FILE | static_cast<uint32_t>(flags);
  out_attributes->id = fuchsia::io::INO_UNKNOWN;
  out_attributes->content_size = length_;
  out_attributes->storage_size = length_;
  out_attributes->link_count = 1;
  out_attributes->creation_time = 0;
  out_attributes->modification_time = 0;
  return ZX_OK;
}

NodeKind::Type VmoFile::GetKind() const {
  return File::GetKind() | NodeKind::kVmo | NodeKind::kReadable |
         (write_option_ == WriteOption::WRITABLE ? NodeKind::kWritable : 0);
}

}  // namespace vfs
