// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SYSROOT_ZIRCON_DEVICE_BLOCK_H_
#define SYSROOT_ZIRCON_DEVICE_BLOCK_H_

// !!! IMPORTANT !!!
// Most of the definitions in this file need to be kept in sync with:
// - //sdk/banjo/fuchsia.hardware.block/block.fidl;
// - //src/lib/storage/block_client/rust/src/lib.rs.

#include <assert.h>
#include <limits.h>
#include <zircon/types.h>

#define BLOCK_FLAG_READONLY 0x00000001
#define BLOCK_FLAG_REMOVABLE 0x00000002
// Block device has bootdata partition map provided by device metadata.
#define BLOCK_FLAG_BOOTPART 0x00000004
#define BLOCK_FLAG_TRIM_SUPPORT 0x00000008
#define BLOCK_FLAG_FUA_SUPPORT 0x00000010

#define BLOCK_MAX_TRANSFER_UNBOUNDED 0xFFFFFFFF

typedef struct {
  uint64_t block_count;        // The number of blocks in this block device
  uint32_t block_size;         // The size of a single block
  uint32_t max_transfer_size;  // Max size in bytes per transfer.
                               // May be BLOCK_MAX_TRANSFER_UNBOUNDED if there
                               // is no restriction.
  uint32_t flags;
  uint32_t reserved;
} block_info_t;

typedef struct {
  uint64_t total_ops;     // Total number of block ops processed
  uint64_t total_blocks;  // Total number of blocks processed
  uint64_t total_reads;
  uint64_t total_blocks_read;
  uint64_t total_writes;
  uint64_t total_blocks_written;
} block_stats_t;

typedef uint16_t vmoid_t;

// Dummy vmoid value reserved for "invalid". Will never be allocated; can be
// used as a local value for unallocated / freed ID.
#define BLOCK_VMOID_INVALID 0

#define BLOCK_GUID_LEN 16
#define BLOCK_NAME_LEN 24
#define MAX_FVM_VSLICE_REQUESTS 16

typedef struct {
  size_t slice_count;
  uint8_t type[BLOCK_GUID_LEN];
  uint8_t guid[BLOCK_GUID_LEN];
  char name[BLOCK_NAME_LEN];
  // Refer to //sdk/fidl/fuchsia.hardware.block.volume/volume.fidl for options
  // here. (Currently only `ALLOCATE_PARTITION_FLAG_INACTIVE` is defined.)
  // Default is 0.
  uint32_t flags;
} alloc_req_t;

typedef struct {
  size_t offset;  // Both in units of "slice". "0" = slice 0, "1" = slice 1, etc...
  size_t length;
} extend_request_t;

typedef struct {
  size_t count;                                  // number of elements in vslice_start
  size_t vslice_start[MAX_FVM_VSLICE_REQUESTS];  // vslices to query from
} query_request_t;

// Multiple block I/O operations may be sent at once before a response is
// actually sent back. Block I/O ops may be sent concurrently to different
// vmoids, and they also may be sent to different groups at any point in time.
//
// `MAX_TXN_GROUP_COUNT` "groups" are pre-allocated lanes separated on the
// block server.  Using a group allows multiple message to be buffered at once
// on a single communication channel before receiving a response.
//
// Usage of groups is identified by `BLOCKIO_GROUP_ITEM`, and is optional.
//
// These groups may be referred to with a "groupid", in the range [0,
// `MAX_TXN_GROUP_COUNT`).
//
// The protocol to communicate with a single group is as follows:
// 1) SEND [N - 1] messages with an allocated groupid for any value of 1 <= N.
//    The `BLOCKIO_GROUP_ITEM` flag is set for these messages.
// 2) SEND a final Nth message with the same groupid. The `BLOCKIO_GROUP_ITEM
//    | BLOCKIO_GROUP_LAST` flags are set for this message.
// 3) RECEIVE a single response from the Block I/O server after all N requests
//    have completed. This response is sent once all operations either complete
//    or a single operation fails. At this point, step (1) may begin again for
//    the same groupid.
//
// For `BLOCKIO_READ` and `BLOCKIO_WRITE`, N may be greater than 1. Otherwise,
// N == 1 (skipping step (1) in the protocol above).
//
// Notes:
// - groupids may operate on any number of vmoids at once.
// - If additional requests are sent on the same groupid before step (3) has
//   completed, then the additional request will not be processed. If
//   `BLOCKIO_GROUP_LAST` is set, an error will be returned. Otherwise, the
//   request will be silently dropped.
// - Messages within a group are not guaranteed to be processed in any order
//   relative to each other.
// - All requests receive responses, except for ones with `BLOCKIO_GROUP_ITEM`
//   that do not have `BLOCKIO_GROUP_LAST` set.
//
// For example, the following is a valid sequence of transactions:
//
//   -> (groupid = 1, vmoid = 1, OP = Write | GroupItem,             reqid = 1)
//   -> (groupid = 1, vmoid = 2, OP = Write | GroupItem,             reqid = 2)
//   -> (groupid = 2, vmoid = 3, OP = Write | GroupItem | GroupLast, reqid = 0)
//   <- Response sent to groupid = 2, reqid = 0
//   -> (groupid = 1, vmoid = 1, OP = Read | GroupItem | GroupLast,  reqid = 3)
//   <- Response sent to groupid = 1, reqid = 3
//   -> (groupid = 3, vmoid = 1, OP = Write | GroupItem,             reqid = 4)
//   -> (groupid = don't care, vmoid = 1, OP = Read, reqid = 5)
//   <- Response sent to reqid = 5
//   -> (groupid = 3, vmoid = 1, OP = Read | GroupItem | GroupLast,  reqid = 6)
//   <- Response sent to groupid = 3, reqid = 6
//
// Each transaction reads or writes up to `length` blocks from the device,
// starting at `dev_offset` blocks, into the VMO associated with `vmoid`,
// starting at `vmo_offset` blocks.  If the transaction is out of range, for
// example if `length` is too large or if `dev_offset` is beyond the end of the
// device, `ZX_ERR_OUT_OF_RANGE` is returned.

#define MAX_TXN_GROUP_COUNT 8

// The Request ID allowing callers to correspond requests with responses.
// This field is entirely for client-side bookkeeping; there is no obligation
// to make request IDs unique.
typedef uint32_t reqid_t;
typedef uint16_t groupid_t;

// Reads from the Block device into the VMO
#define BLOCKIO_READ 0x00000001

// Writes to the Block device from the VMO
#define BLOCKIO_WRITE 0x00000002

// Writes any cached data to nonvolatile storage.
#define BLOCKIO_FLUSH 0x00000003

// Marks data on the backing storage as invalid.
#define BLOCKIO_TRIM 0x00000004

// Detaches the VMO from the block device.
#define BLOCKIO_CLOSE_VMO 0x00000005
#define BLOCKIO_OP_MASK 0x000000FF

// Associate the following request with |group|.
#define BLOCKIO_GROUP_ITEM 0x00000400

// Only respond after this request (and all previous within group) have completed.
// Only valid with BLOCKIO_GROUP_ITEM.
#define BLOCKIO_GROUP_LAST 0x00000800

// Mark this operation as "Force Unit Access" (FUA), indicating that
// it should not complete until the data is written to the non-volatile
// medium (write), and that reads should bypass any on-device caches.
#define BLOCKIO_FL_FORCE_ACCESS 0x00001000
// The pre-flush flag generates a flush operation before running this operation
// and flushes on-device caches. It ensures that previous data is written to
// the non-volatile medium. Applying the preflush to an operation in a group
// will only apply to the operation with the flag and not all the other
// operations in the group. The preflush only applies to an operation where the
// preflush flag is set. It does not affect other operations and can proceed in
// parallel. If the flush operation fails, the operation will not proceed.
#define BLOCKIO_FL_PREFLUSH 0x00002000
#define BLOCKIO_FLAG_MASK 0x0000FF00

typedef struct {
  uint32_t opcode;
  reqid_t reqid;    // Transmitted in the block_fifo_response_t.
  groupid_t group;  // Only used if opcode & BLOCKIO_GROUP_ITEM.
  vmoid_t vmoid;
  uint32_t length;
  uint64_t vmo_offset;
  uint64_t dev_offset;
  uint64_t trace_flow_id;
} block_fifo_request_t;

typedef struct {
  zx_status_t status;
  reqid_t reqid;
  groupid_t group;  // Only valid if transmitted in request.
  vmoid_t reserved0;
  uint32_t count;  // The number of messages in the transaction completed by the block server.
  uint64_t reserved1;
  uint64_t reserved2;
  uint64_t reserved3;
} block_fifo_response_t;

static_assert(sizeof(block_fifo_request_t) == sizeof(block_fifo_response_t),
              "FIFO messages are the same size in both directions");

#define BLOCK_FIFO_ESIZE (sizeof(block_fifo_request_t))
#define BLOCK_FIFO_MAX_DEPTH (4096 / BLOCK_FIFO_ESIZE)

#endif  // SYSROOT_ZIRCON_DEVICE_BLOCK_H_
