// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/boot-options/boot-options.h>
#include <lib/boot-options/types.h>
#include <lib/console.h>
#include <lib/debuglog.h>
#include <lib/io.h>
#include <lib/ktrace.h>
#include <lib/mtrace.h>
#include <lib/persistent-debuglog.h>
#include <lib/syscalls/forward.h>
#include <lib/user_copy/user_ptr.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/syscalls/debug.h>
#include <zircon/types.h>

#include <object/handle.h>
#include <object/process_dispatcher.h>
#include <object/resource.h>
#include <platform/debug.h>

#define LOCAL_TRACE 0

constexpr uint32_t kMaxDebugWriteSize = 256u;

// zx_status_t zx_debug_read
zx_status_t sys_debug_read(zx_handle_t handle, user_out_ptr<char> ptr, size_t max_len,
                           user_out_ptr<size_t> len) {
  LTRACEF("ptr %p\n", ptr.get());

  if (gBootOptions->enable_serial_syscalls != SerialDebugSyscalls::kEnabled) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status;
  if ((status = validate_ranged_resource(handle, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_DEBUG_BASE,
                                         1)) != ZX_OK) {
    return status;
  }

  size_t idx = 0;
  for (; idx < max_len; ++idx) {
    char c;
    // Wait only on the first character.
    // The API for this function can return any number of characters up to the supplied buffer
    // length, however there is no notification mechanism for when there are bytes to read.
    // Hence, we need to read at least one character or applications will be forced to spin poll.
    // We avoid reading all the characters so that interactive applications can stay responsive
    // without losing efficiency by being forced to read one character at a time.
    bool wait = (idx == 0);
    int err = platform_dgetc(&c, wait);
    if (err < 0) {
      return err;
    } else if (err == 0) {
      break;
    }

    if (c == '\r') {
      c = '\n';
    }

    status = ptr.copy_array_to_user(&c, 1, idx);
    if (status != ZX_OK) {
      return status;
    }
  }
  return len.copy_to_user(idx);
}

// zx_status_t zx_debug_write
zx_status_t sys_debug_write(user_in_ptr<const char> ptr, size_t len) {
  LTRACEF("ptr %p, len %zu\n", ptr.get(), len);

  if (gBootOptions->enable_serial_syscalls != SerialDebugSyscalls::kEnabled &&
      gBootOptions->enable_serial_syscalls != SerialDebugSyscalls::kOutputOnly) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (len > kMaxDebugWriteSize)
    len = kMaxDebugWriteSize;

  char buf[kMaxDebugWriteSize];
  if (ptr.copy_array_from_user(buf, len) != ZX_OK)
    return ZX_ERR_INVALID_ARGS;

  // Dump what we can into the persistent dlog, if we have one.
  persistent_dlog_write({buf, len});

  // This path to serial out arbitrates with the debug log
  // drainer and/or kernel ll debug path to minimize interleaving
  // of serial output between various sources
  dlog_serial_write({buf, len});

  return ZX_OK;
}

// zx_status_t zx_debug_send_command
zx_status_t sys_debug_send_command(zx_handle_t handle, user_in_ptr<const char> ptr, size_t len) {
  LTRACEF("ptr %p, len %zu\n", ptr.get(), len);

  if (!gBootOptions->enable_debugging_syscalls) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status;
  if ((status = validate_ranged_resource(handle, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_DEBUG_BASE,
                                         1)) != ZX_OK) {
    return status;
  }

  if (len > kMaxDebugWriteSize)
    return ZX_ERR_INVALID_ARGS;

  char buf[kMaxDebugWriteSize + 2];
  if (ptr.copy_array_from_user(buf, len) != ZX_OK)
    return ZX_ERR_INVALID_ARGS;

  buf[len] = '\n';
  buf[len + 1] = 0;
  return console_run_script(buf);
}

// zx_status_t zx_ktrace_read
zx_status_t sys_ktrace_read(zx_handle_t handle, user_out_ptr<void> _data, uint32_t offset,
                            size_t len, user_out_ptr<size_t> _actual) {
  // See also ktrace_init() in zircon/kernel/lib/ktrace/ktrace.cc.
  if (!gBootOptions->enable_debugging_syscalls) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status;
  if ((status = validate_ranged_resource(handle, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_DEBUG_BASE,
                                         1)) != ZX_OK) {
    return status;
  }

  ssize_t result = ktrace_read_user(_data, offset, len);
  if (result < 0)
    return static_cast<zx_status_t>(result);

  return _actual.copy_to_user(static_cast<size_t>(result));
}

// zx_status_t zx_ktrace_control
zx_status_t sys_ktrace_control(zx_handle_t handle, uint32_t action, uint32_t options,
                               user_inout_ptr<void> _ptr) {
  // See also ktrace_init() in zircon/kernel/lib/ktrace/ktrace.cc.
  if (!gBootOptions->enable_debugging_syscalls) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status;
  if ((status = validate_ranged_resource(handle, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_DEBUG_BASE,
                                         1)) != ZX_OK) {
    return status;
  }

  switch (action) {
    case KTRACE_ACTION_NEW_PROBE: {
      char name[ZX_MAX_NAME_LEN];
      if (_ptr.reinterpret<char>().copy_array_from_user(name, sizeof(name) - 1) != ZX_OK)
        return ZX_ERR_INVALID_ARGS;
      name[sizeof(name) - 1] = 0;
      return ktrace_control(action, options, name);
    }
    default:
      return ktrace_control(action, options, nullptr);
  }
}

// zx_status_t zx_ktrace_write
zx_status_t sys_ktrace_write(zx_handle_t handle, uint32_t event_id, uint32_t arg0, uint32_t arg1) {
  // See also ktrace_init() in zircon/kernel/lib/ktrace/ktrace.cc.
  if (!gBootOptions->enable_debugging_syscalls) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status;
  if ((status = validate_ranged_resource(handle, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_DEBUG_BASE,
                                         1)) != ZX_OK) {
    return status;
  }

  if (event_id > 0x7FF) {
    return ZX_ERR_INVALID_ARGS;
  }

  ktrace_thunks::fxt_instant(
      "kernel:probe"_category, ktrace_timestamp(), ThreadRefFromContext(TraceContext::Cpu),
      fxt::StringRef{static_cast<uint16_t>(event_id | 0x4000)},
      fxt::Argument{"arg0"_stringref, arg0}, fxt::Argument{"arg1"_stringref, arg1});
  return ZX_OK;
}

// zx_status_t zx_mtrace_control
zx_status_t sys_mtrace_control(zx_handle_t handle, uint32_t kind, uint32_t action, uint32_t options,
                               user_inout_ptr<void> ptr, size_t size) {
  if (!gBootOptions->enable_debugging_syscalls) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status;
  if ((status = validate_ranged_resource(handle, ZX_RSRC_KIND_SYSTEM, ZX_RSRC_SYSTEM_DEBUG_BASE,
                                         1)) != ZX_OK) {
    return status;
  }

  return mtrace_control(kind, action, options, ptr, size);
}
