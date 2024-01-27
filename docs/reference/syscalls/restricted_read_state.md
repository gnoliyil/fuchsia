<!--
Copyright 2022 The Fuchsia Authors. All rights reserved.
Use of this source code is governed by a BSD-style license that can be
found in the LICENSE file.

DO NOT EDIT. Generated from FIDL library zx by zither, a Fuchsia platform tool.

See //docs/reference/syscalls/README.md#documentation-generation for
regeneration instructions.
-->

# zx_restricted_read_state

## Summary

Set restricted mode state on the current thread.

## Declaration

```c
#include <zircon/syscalls-next.h>

zx_status_t zx_restricted_read(void *state,
                               size_t state_size);
```

## Description

Sets the restricted mode register state on the current thread according
to the structure passed.

The state structure is defined below (and in syscalls-next.h)
```c
   typedef struct zx_restricted_state {
#if __aarch64__
     uint64_t x[31];
     uint64_t sp;
     // Contains only the user-controllable upper 4-bits (NZCV).
     uint32_t cpsr;
     uint8_t padding1[4];
#elif __x86_64__
     // User space active registers
     uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax, rsp;
     uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
     uint64_t ip, flags;

     uint64_t fs_base, gs_base;
#endif
   } zx_restricted_state_t;
```

## Rights

None

## Return value

In the event of failure, a negative error value is returned.

## Errors

**ZX_ERR_INVALID_ARGS** Invalid pointer to state or length is incorrect.

## See also

- [`zx_restricted_enter()`]
- [`zx_restricted_write_state()`]

[`zx_restricted_enter()`]: restricted_enter.md
[`zx_restricted_write_state()`]: restricted_write_state.md
