// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "loader-service.h"

#include <lib/fidl/cpp/transaction_header.h>
#include <lib/zbi-format/internal/bootfs.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <cstring>

#include <fbl/string_buffer.h>
#include <ldmsg/ldmsg.h>

#include "bootfs.h"
#include "util.h"

void LoaderService::Config(std::string_view string) {
  exclusive_ = false;
  if (!string.empty() && string.back() == '!') {
    string.remove_suffix(1);
    exclusive_ = true;
  }
  if (string.size() >= subdir_.size() - 1) {
    fail(log_, "loader-service config string too long");
  }
  string.copy(subdir_.data(), subdir_.size());
  subdir_len_ = string.size();
}

zx::vmo LoaderService::TryLoadObject(std::string_view name, bool use_subdir) {
  fbl::StringBuffer<ZBI_BOOTFS_MAX_NAME_LEN> file;
  file.Append(kLoadObjectFileDir).Append('/');
  if (use_subdir && subdir_len_ > 0) {
    file.Append(subdir_.data(), subdir_len_).Append('/');
  }
  file.Append(name);
  return fs_->Open(root_, file, "shared library");
}

zx::vmo LoaderService::LoadObject(std::string_view name) {
  zx::vmo vmo = TryLoadObject(name, true);
  if (!vmo && subdir_len_ > 0 && !exclusive_) {
    vmo = TryLoadObject(name, false);
  }
  if (!vmo) {
    fail(log_, "cannot find shared library '%.*s'", static_cast<int>(name.size()), name.data());
  }
  return vmo;
}

bool LoaderService::HandleRequest(const zx::channel& channel) {
  ldmsg_req_t req;
  zx::vmo reqhandle;

  uint32_t size;
  uint32_t hcount;
  zx_status_t status =
      channel.read(0, &req, reqhandle.reset_and_get_address(), sizeof(req), 1, &size, &hcount);

  // This is the normal error for the other end going away,
  // which happens when the process dies.
  if (status == ZX_ERR_PEER_CLOSED) {
    printl(log_, "loader-service channel peer closed on read");
    return false;
  }

  check(log_, status, "zx_channel_read on loader-service channel failed");

  const char* string;
  size_t string_len;
  status = ldmsg_req_decode(&req, size, &string, &string_len);
  if (status != ZX_OK) {
    fail(log_, "loader-service request invalid");
  }

  ldmsg_rsp_t rsp;
  memset(&rsp, 0, sizeof(rsp));

  zx::vmo vmo;
  switch (req.header.ordinal) {
    case LDMSG_OP_DONE:
      printl(log_, "loader-service received DONE request");
      return false;

    case LDMSG_OP_CONFIG:
      Config({string, string_len});
      break;

    case LDMSG_OP_LOAD_OBJECT:
      vmo = LoadObject({string, string_len});
      break;

    case LDMSG_OP_CLONE:
      rsp.rv = ZX_ERR_NOT_SUPPORTED;
      goto error_reply;

    default:
      fail(log_, "loader-service received invalid opcode");
      break;
  }

  rsp.rv = ZX_OK;
  rsp.object = vmo ? FIDL_HANDLE_PRESENT : FIDL_HANDLE_ABSENT;
error_reply:
  fidl::InitTxnHeader(&rsp.header, req.header.txid, req.header.ordinal,
                      fidl::MessageDynamicFlags::kStrictMethod);

  if (vmo) {
    zx_handle_t handles[] = {vmo.release()};
    status = channel.write(0, &rsp, static_cast<uint32_t>(ldmsg_rsp_get_size(&rsp)), handles, 1);
  } else {
    status = channel.write(0, &rsp, static_cast<uint32_t>(ldmsg_rsp_get_size(&rsp)), nullptr, 0);
  }
  check(log_, status, "zx_channel_write on loader-service channel failed");

  return true;
}

void LoaderService::Serve(zx::channel channel) {
  printl(log_, "waiting for loader-service requests...");
  do {
    zx_signals_t signals;
    zx_status_t status = channel.wait_one(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                          zx::time::infinite(), &signals);
    if (status == ZX_ERR_BAD_STATE) {
      // This is the normal error for the other end going away,
      // which happens when the process dies.
      break;
    }
    check(log_, status, "zx_object_wait_one failed on loader-service channel");
    if (signals & ZX_CHANNEL_PEER_CLOSED) {
      printl(log_, "loader-service channel peer closed");
      break;
    }
    if (!(signals & ZX_CHANNEL_READABLE)) {
      fail(log_, "unexpected signal state on loader-service channel");
    }
  } while (HandleRequest(channel));
}
