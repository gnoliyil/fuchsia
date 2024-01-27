// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>

// The macros allow to call syscall with a random number of argument, and ensure
// that the correct number of arguments are present and that they are casted to
// the correct type.
#define _syscall(number, arg1, arg2, arg3, arg4, ...) \
  _syscall4(number, (intptr_t)(arg1), (intptr_t)(arg2), (intptr_t)(arg3), (intptr_t)(arg4))

#define syscall(number, ...) _syscall(number, __VA_ARGS__, 0, 0, 0, 0)

namespace {

intptr_t handle_error(intptr_t return_value) {
  if (return_value < 0) {
    // The return value contains the error number. Just drop it for now.
    return_value = -1;
  }
  return return_value;
}

// Generic syscall with 4 arguments.
intptr_t _syscall4(intptr_t syscall_number, intptr_t arg1, intptr_t arg2, intptr_t arg3,
                   intptr_t arg4) {
  intptr_t ret;
#if defined(__x86_64__)
  register intptr_t r10 asm("r10") = arg4;
  __asm__ volatile("syscall;"
                   : "=a"(ret)
                   : "a"(syscall_number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
                   : "rcx", "r11", "memory");
#elif defined(__aarch64__)
  register intptr_t x0 asm("x0") = arg1;
  register intptr_t x1 asm("x1") = arg2;
  register intptr_t x2 asm("x2") = arg3;
  register intptr_t x3 asm("x3") = arg4;
  register intptr_t number asm("x8") = syscall_number;

  __asm__ volatile("svc #0"
                   : "=r"(ret)
                   : "0"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(number)
                   : "memory");
#elif defined(__riscv) && __riscv_xlen == 64
  register intptr_t a0 asm("a0") = arg1;
  register intptr_t a1 asm("a1") = arg2;
  register intptr_t a2 asm("a2") = arg3;
  register intptr_t a3 asm("a3") = arg4;
  register intptr_t number asm("a7") = syscall_number;

  __asm__ volatile("ecall"
                   : "=r"(ret)
                   : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(number)
                   : "memory");
#endif
  return handle_error(ret);
}

void sleep(int count) {
  struct timespec ts = {.tv_sec = count, .tv_nsec = 0};
  syscall(__NR_nanosleep, &ts, 0);
}

pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage) {
  return static_cast<pid_t>(syscall(__NR_wait4, pid, wstatus, options, rusage));
}

void main() {
  for (;;) {
    if (wait4(-1, nullptr, 0, nullptr) == -1) {
      sleep(1);
    }
  }
}
}  // namespace

extern "C" {
#if defined(__x86_64__)
__attribute__((force_align_arg_pointer))
#endif
void _start() {
  main();
  syscall(__NR_exit_group, 0);
  __builtin_unreachable();
}
}
