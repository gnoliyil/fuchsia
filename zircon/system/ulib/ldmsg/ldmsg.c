// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl/txn_header.h>
#include <string.h>

#include <ldmsg/ldmsg.h>

// TODO(fxbug.dev/121817): These handwritten encoding/decoding functions should
// be replaced with generated FIDL bindings after the linked bug about libc
// improvements.

static_assert(sizeof(ldmsg_req_t) == 1024, "Loader service requests can be at most 1024 bytes.");

static size_t FidlAlign(size_t offset) {
  const size_t alignment_mask = FIDL_ALIGNMENT - 1;
  return (offset + alignment_mask) & ~alignment_mask;
}

zx_status_t ldmsg_req_encode(uint64_t ordinal, ldmsg_req_t* req, size_t* req_len_out,
                             const char* data, size_t len) {
  fidl_init_txn_header(&req->header, 0, ordinal, 0);
  size_t offset;
  switch (ordinal) {
    case LDMSG_OP_DONE:
      *req_len_out = sizeof(fidl_message_header_t);
      return ZX_OK;
    case LDMSG_OP_CLONE:
      *req_len_out = sizeof(fidl_message_header_t) + sizeof(ldmsg_clone_t);
      memset(&req->clone, 0, sizeof(req->clone));
      req->clone = (ldmsg_clone_t){
          .object = FIDL_HANDLE_PRESENT,
      };
      return ZX_OK;
    case LDMSG_OP_LOAD_OBJECT:
    case LDMSG_OP_CONFIG:
      offset = sizeof(fidl_string_t);
      break;
    default:
      return ZX_ERR_INVALID_ARGS;
  }

  // Reserve one byte for the null terminator on the receiving side.
  if (LDMSG_MAX_PAYLOAD - offset - 1 < len) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  req->common = (ldmsg_common_t){
      .string =
          {
              .size = len,
              .data = (char*)FIDL_ALLOC_PRESENT,
          },
  };
  memcpy(req->data + offset, data, len);

  // Make sure to zero out the extra bytes required by alignment constraints.
  size_t req_len_unaligned = sizeof(fidl_message_header_t) + offset + len;
  *req_len_out = FidlAlign(req_len_unaligned);
  memset((char*)req + req_len_unaligned, 0, *req_len_out - req_len_unaligned);
  return ZX_OK;
}

zx_status_t ldmsg_req_decode(ldmsg_req_t* req, size_t req_len, const char** data_out,
                             size_t* len_out) {
  size_t offset = 0;
  switch (req->header.ordinal) {
    case LDMSG_OP_DONE:
      if (req_len != sizeof(fidl_message_header_t))
        return ZX_ERR_INVALID_ARGS;
      *data_out = 0;
      *len_out = 0;
      return ZX_OK;
    case LDMSG_OP_CLONE:
      if (req_len != sizeof(fidl_message_header_t) + sizeof(ldmsg_clone_t) ||
          req->clone.object != FIDL_HANDLE_PRESENT)
        return ZX_ERR_INVALID_ARGS;
      *data_out = 0;
      *len_out = 0;
      return ZX_OK;
    case LDMSG_OP_LOAD_OBJECT:
    case LDMSG_OP_CONFIG:
      if ((uintptr_t)req->common.string.data != FIDL_ALLOC_PRESENT)
        return ZX_ERR_INVALID_ARGS;
      offset = sizeof(fidl_string_t);
      break;
    default:
      return ZX_ERR_INVALID_ARGS;
  }

  size_t size = req->common.string.size;
  if (LDMSG_MAX_PAYLOAD - offset - 1 < size ||
      req_len != FidlAlign(sizeof(fidl_message_header_t) + offset + size))
    return ZX_ERR_INVALID_ARGS;

  // Null terminate the string. The message isn't required to have a null
  // terminated string, but we have enough space in our buffer for the null
  // terminator and adding it makes life easier for our caller.
  req->data[offset + size] = '\0';

  *data_out = req->data + offset;
  *len_out = size;
  return ZX_OK;
}

size_t ldmsg_rsp_get_size(ldmsg_rsp_t* rsp) {
  switch (rsp->header.ordinal) {
    case LDMSG_OP_LOAD_OBJECT:
    case LDMSG_OP_CONFIG:
    case LDMSG_OP_CLONE:
      return sizeof(ldmsg_rsp_t);
    case LDMSG_OP_DONE:
    default:
      return 0;
  }
}
