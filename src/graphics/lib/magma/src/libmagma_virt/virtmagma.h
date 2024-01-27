// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef _LINUX_VIRTMAGMA_H
#define _LINUX_VIRTMAGMA_H

#include <asm/ioctl.h>
#include <linux/types.h>

#define VIRTMAGMA_IOCTL_BASE 'm'
#define VIRTMAGMA_IO(nr) _IO(VIRTMAGMA_IOCTL_BASE, nr)
#define VIRTMAGMA_IOR(nr, type) _IOR(VIRTMAGMA_IOCTL_BASE, nr, type)
#define VIRTMAGMA_IOW(nr, type) _IOW(VIRTMAGMA_IOCTL_BASE, nr, type)
#define VIRTMAGMA_IOWR(nr, type) _IOWR(VIRTMAGMA_IOCTL_BASE, nr, type)
#define VIRTMAGMA_MAKE_VERSION(major, minor, patch) (((major) << 24) | ((minor) << 12) | (patch))
#define VIRTMAGMA_GET_VERSION(version, major, minor, patch)                                     \
  ((major = ((version) >> 24)), (minor = ((version) >> 12) & 0x3FF), (patch = (version)&0x3FF), \
   (version))

#define VIRTMAGMA_HANDSHAKE_SEND 0x46434853
#define VIRTMAGMA_HANDSHAKE_RECV 0x474F4F47
#define VIRTMAGMA_VERSION VIRTMAGMA_MAKE_VERSION(0, 1, 0)
struct virtmagma_ioctl_args_handshake {
  __u32 handshake_inout;
  __u32 version_out;
};

struct virtmagma_ioctl_args_magma_command {
  __u64 request_address;
  __u64 request_size;
  __u64 response_address;
  __u64 response_size;
};

struct virtmagma_command_descriptor {
  __u64 descriptor_size;
  __u64 descriptor;
  __u64 resource_size;
  __u64 resources;
  __u64 command_buffer_size;
  __u64 command_buffers;
  __u64 semaphore_size;
  __u64 semaphores;
};

struct virtmagma_create_image_wrapper {
  __u64 create_info;
  __u64 create_info_size;
};

struct virtmagma_get_image_info_wrapper {
  __u64 image_info_out;
  __u64 image_info_size;
};

#define VIRTMAGMA_IOCTL_HANDSHAKE VIRTMAGMA_IOWR(0x00, struct virtmagma_ioctl_args_handshake)
// Removed: VIRTMAGMA_IOCTL_GET_MMFD (0x01)
#define VIRTMAGMA_IOCTL_MAGMA_COMMAND \
  VIRTMAGMA_IOWR(0x02, struct virtmagma_ioctl_args_magma_command)

#endif /* _LINUX_VIRTMAGMA_H */
