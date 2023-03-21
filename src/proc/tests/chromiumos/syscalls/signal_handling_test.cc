// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h> /* Definition of O_* constants */
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>
#include <csignal>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

uintptr_t stack_ptr;

constexpr int kSigaltstackFailed = -1;
constexpr int kHandlerOnSigaltStack = 1;
constexpr int kHandlerOnMainStack = 2;
constexpr int kRaisedSIGSEGV = 3;
constexpr int kExitTestFailure = 4;

void overflow_handler(int sig) {
  stack_t current_altstack;
  int ret = sigaltstack(nullptr, &current_altstack);
  if (ret < 0) {
    exit(kSigaltstackFailed);
  }
  if (current_altstack.ss_flags == SA_ONSTACK) {
    exit(kHandlerOnSigaltStack);
  } else {
    exit(kHandlerOnMainStack);
  }
}

int setup_overflow_altstack(void *alt_stack) {
  stack_t ss = {
      .ss_sp = alt_stack,
      .ss_size = UINT64_MAX,
  };
  int ret = sigaltstack(&ss, nullptr);
  if (ret < 0) {
    return ret;
  }

  struct sigaction sa = {};
  sa.sa_handler = overflow_handler;
  sa.sa_flags = SA_ONSTACK;
  ret = sigaction(SIGUSR1, &sa, nullptr);
  if (ret < 0) {
    return ret;
  }

  ret = raise(SIGUSR1);
  if (ret != 0) {
    return ret;
  }
  return 0;
}

// Helper function for setting up a sig altstack.
void *setup_sigaltstack(size_t size) {
  void *altstack =
      mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_STACK | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (altstack == MAP_FAILED) {
    exit(kExitTestFailure);
  }

  stack_t ss = {
      .ss_sp = altstack,
      .ss_size = size,
  };

  if (sigaltstack(&ss, NULL)) {
    exit(kExitTestFailure);
  }

  return altstack;
}

TEST(SignalHandling, UseSigaltstackSucceeds) {
  constexpr size_t kStackSize = 0x20000;
  void *altstack = setup_sigaltstack(kStackSize);

  struct sigaction sa = {};
  sa.sa_handler = [](int) { stack_ptr = reinterpret_cast<uintptr_t>(__builtin_frame_address(0)); };
  sa.sa_flags = SA_ONSTACK;
  ASSERT_EQ(0, sigaction(SIGUSR1, &sa, nullptr));
  stack_ptr = 0;
  ASSERT_NE(-1, raise(SIGUSR1));
  // Check that the sigaltstack is used.
  uintptr_t alt_stack_base = reinterpret_cast<uintptr_t>(altstack);
  EXPECT_GE(stack_ptr, alt_stack_base);
  EXPECT_LE(stack_ptr, alt_stack_base + kStackSize);

  // Cleanup
  stack_t ss = {
      .ss_flags = SS_DISABLE,
  };
  ASSERT_EQ(0, sigaltstack(&ss, nullptr));
}

// Check that if the stack pointer calculation for the sigaltstack overflows,
// the main stack is used instead.
TEST(SignalHandling, UseMainStackOnSigaltstackOverflow) {
  constexpr size_t kStackSize = 0x20000;
  void *mmap1 = mmap(nullptr, kStackSize, PROT_WRITE | PROT_READ,
                     MAP_STACK | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, mmap1);
  void *mmap2 = mmap(nullptr, kStackSize, PROT_WRITE | PROT_READ,
                     MAP_STACK | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, mmap2);
  void *main_stack = std::min(mmap1, mmap2);
  void *alt_stack = std::max(mmap1, mmap2);

  uintptr_t main_stack_base = reinterpret_cast<uint64_t>(main_stack) + kStackSize;
  int pid =
      clone(setup_overflow_altstack, (void *)main_stack_base, CLONE_VFORK | SIGCHLD, alt_stack);
  ASSERT_NE(-1, pid);
  int status = 0;
  pid_t wait_pid = waitpid(pid, &status, 0);
  ASSERT_NE(-1, wait_pid);
  EXPECT_EQ(kHandlerOnMainStack, WEXITSTATUS(status));
}

TEST(SignalHandlingDeathTest, ExitsKilledBySignal) {
  std::vector<int> term_signals = {SIGABRT, SIGALRM, SIGBUS,  SIGFPE,  SIGHUP,  SIGILL,
                                   SIGINT,  SIGPIPE, SIGPOLL, SIGPROF, SIGQUIT, SIGSEGV,
                                   SIGSYS,  SIGTERM, SIGTRAP, SIGUSR1, SIGUSR2};

  for (const auto signal : term_signals) {
    EXPECT_EXIT(
        [signal]() {
          // Reset the action for this signal, the default should cause the
          // program to terminate.
          struct sigaction sa = {};
          sa.sa_handler = SIG_DFL;
          if (sigaction(signal, &sa, NULL)) {
            perror("sigaction");
            exit(EXIT_FAILURE);
          }
          raise(signal);
        }(),
        testing::KilledBySignal(signal), "");
  }

  // SIGKILL cannot be handled, so sigaction would fail.
  EXPECT_EXIT([]() { raise(SIGKILL); }(), testing::KilledBySignal(SIGKILL), "");

  // Generate signals by causing architectural exceptions.
  EXPECT_EXIT([]() { __builtin_trap(); }(), testing::KilledBySignal(SIGILL), "");
  EXPECT_EXIT([]() { __builtin_debugtrap(); }(), testing::KilledBySignal(SIGTRAP), "");

  EXPECT_EXIT(
      []() {
        // Write to a non-writable memory address to cause a SIGSEGV.
        void *res = mmap(NULL, 0x1000, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (res == MAP_FAILED) {
          exit(EXIT_FAILURE);
        }
        volatile uint8_t *buf = reinterpret_cast<volatile uint8_t *>(res);
        buf[0] = 0x1;  // Page Fault.
      }(),
      testing::KilledBySignal(SIGSEGV), "");
}

// Issues a `kill` syscall with a given signal, setting the stack to a given
// address.
// This is useful for testing how the signal stack gets setup.
void raise_with_stack(int signal, uintptr_t stack) {
  int pid = getpid();
  uint64_t new_rsp = stack;
#if defined(__x86_64__)
  __asm__ volatile(
      "movq %%rsp, %%r15\n"
      "movq %[new_rsp], %%rsp\n"
      "syscall\n"
      "movq %%r15, %%rsp\n"
      :
      : "A"(SYS_kill), "D"(pid), "S"(signal), [new_rsp] "r"(new_rsp)
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "r15", "cc", "memory");
#elif defined(__aarch64__)
  __asm__ volatile(
      "mov x19, sp\n"
      "mov sp, %[new_rsp]\n"
      "mov x0, %[pid]\n"
      "mov x1, %[signal]\n"
      "mov w8, %[sys_kill]\n"
      "svc #0\n"
      "mov sp, x19\n"
      :
      : [pid] "r"(pid), [signal] "r"(signal), [new_rsp] "r"(new_rsp), [sys_kill] "i"(SYS_kill)
      : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
        "x14", "x15", "x16", "x17", "x19", "cc", "memory");
#else
#error "unimplemented"
#endif
}

TEST(SignalHandlingDeathTest, GetSIGSEGVOnMainStackUnderflow) {
  // If we have an underflow while setting the signal stack,
  // we will receive a SIGSEGV.
  EXPECT_EXIT(
      []() {
        struct sigaction sa = {};
        sa.sa_handler = [](int) { exit(kExitTestFailure); };

        if (sigaction(SIGUSR1, &sa, 0)) {
          exit(kExitTestFailure);
        }

        raise_with_stack(SIGUSR1, 0x0);
      }(),
      testing::KilledBySignal(SIGSEGV), "");
}

// Check that if the stack pointer calculation for the main stack underflows,
// we get a SIGSEGV signal in the alt stack.
TEST(SignalHandling, GetSIGSEGVOnMainStackUnderflowHandledInAltStack) {
  EXPECT_EXIT(
      []() {
        // This function sets up two signal handlers:
        //  * One for SIGUSR1 in the main stack.
        //  * One for SIGSEGV in an alt stack.
        //
        // After doing that, it will set up a bogus stack
        // and raise SIGUSR1. This should cause an error
        // in the signal handling logic, which will then
        // send a SIGSEGV on the alt stack.
        //
        // The altstack handler will then call exit() if it
        // received a SIGSEGV.
        constexpr size_t kStackSize = 0x20000;
        setup_sigaltstack(kStackSize);

        // Set up a handler for SIGUSR1
        // Do not use the altstack.
        struct sigaction sa = {};
        sa.sa_handler = [](int) { exit(kExitTestFailure); };

        if (sigaction(SIGUSR1, &sa, 0)) {
          exit(kExitTestFailure);
        }

        // Set up a handler for SIGSEGV
        // Use the alt stack.
        struct sigaction sa_sigsegv = {};
        sa_sigsegv.sa_handler = [](int) { exit(kRaisedSIGSEGV); };
        sa_sigsegv.sa_flags = SA_ONSTACK;

        if (sigaction(SIGSEGV, &sa_sigsegv, 0)) {
          exit(kExitTestFailure);
        }

        // raise(SIGUSR1) with a bogus stack.
        raise_with_stack(SIGUSR1, 0x0);
        exit(kExitTestFailure);
      }(),
      testing::ExitedWithCode(kRaisedSIGSEGV), "");
}

// Check that if we fail to deliver a signal, and SIGSEGV is masked, we unmask
// it, and reset the SIGSEGV signal disposition as well.
TEST(SignalHandling, SignalFailureUnmasksSIGSEGVAndResetsHandler) {
  EXPECT_EXIT(
      []() {
        // This function sets up two signal handlers:
        //  * One for SIGUSR1 in the main stack.
        //  * One for SIGSEGV in an alternate stack.
        //
        // After doing that, it will mask SIGSEGV and it will set up a bogus stack
        // and raise SIGUSR1. This should cause an error in the signal handling
        // logic, which will unmask the SIGSEGV, reset the handler, and send a
        // SIGSEGV.
        //
        // The test checks that instead of executing the SIGSEGV handler that we had
        // setup, it uses the default action (kill the program).
        constexpr size_t kStackSize = 0x20000;
        setup_sigaltstack(kStackSize);

        // Set up a handler for SIGUSR1
        // Do not use the altstack.
        struct sigaction sa = {};
        sa.sa_handler = [](int) { exit(kExitTestFailure); };

        if (sigaction(SIGUSR1, &sa, 0)) {
          exit(kExitTestFailure);
        }

        // Set up a handler for SIGSEGV
        // Use the altstack.
        struct sigaction sa_sigsegv = {};
        sa_sigsegv.sa_handler = [](int) { exit(kRaisedSIGSEGV); };
        sa_sigsegv.sa_flags = SA_ONSTACK;

        if (sigaction(SIGSEGV, &sa_sigsegv, 0)) {
          exit(kExitTestFailure);
        }

        sigset_t newset;
        sigemptyset(&newset);
        sigaddset(&newset, SIGSEGV);

        if (sigprocmask(SIG_BLOCK, &newset, NULL)) {
          exit(kExitTestFailure);
        }

        // raise(SIGUSR1) with a bogus stack.
        raise_with_stack(SIGUSR1, 0x0);
        exit(kExitTestFailure);
      }(),
      testing::KilledBySignal(SIGSEGV), "");
}

TEST(SignalHandlingDeathTest, MaskedSIGSEGVGetsUnmasked) {
  EXPECT_EXIT(
      []() {
        sigset_t newset;
        sigemptyset(&newset);
        sigaddset(&newset, SIGSEGV);

        if (sigprocmask(SIG_BLOCK, &newset, NULL)) {
          perror("sigprocmask");
          exit(kExitTestFailure);
        }

        // Set up a handler for SIGUSR1
        // Will not get called.
        struct sigaction sa = {};
        sa.sa_handler = [](int) { exit(kExitTestFailure); };

        if (sigaction(SIGUSR1, &sa, 0)) {
          exit(kExitTestFailure);
        }

        raise_with_stack(SIGUSR1, 0x0);
      }(),
      testing::KilledBySignal(SIGSEGV), "");
}

TEST(SignalHandlingDeathTest, IgnoredSIGSEGVGetsUnignored) {
  EXPECT_EXIT(
      []() {
        // Set the SIGSEGV handler to IGNORE
        struct sigaction sa = {};
        sa.sa_handler = SIG_IGN;

        if (sigaction(SIGSEGV, &sa, 0)) {
          exit(kExitTestFailure);
        }

        // Set up a handler for SIGUSR1
        // Will not get called.
        sa = {};
        sa.sa_handler = [](int) { exit(kExitTestFailure); };

        if (sigaction(SIGUSR1, &sa, 0)) {
          exit(kExitTestFailure);
        }

        raise_with_stack(SIGUSR1, 0x0);
      }(),
      testing::KilledBySignal(SIGSEGV), "");
}

TEST(SignalHandlingDeathTest, SIGSEGVWithHanderFailsCausesSIGSEGV) {
  EXPECT_EXIT(
      []() {
        // Set a handler for SIGSEGV and SIGUSR1.
        struct sigaction sa = {};
        sa.sa_handler = [](int) { exit(kExitTestFailure); };

        // The SIGSEGV handler will fail to run due to the bogus
        // stack, the signal disposition will be reset, and the program will be
        // killed.
        if (sigaction(SIGSEGV, &sa, 0)) {
          exit(kExitTestFailure);
        }
        if (sigaction(SIGUSR1, &sa, 0)) {
          exit(kExitTestFailure);
        }

        raise_with_stack(SIGUSR1, 0x0);
      }(),
      testing::KilledBySignal(SIGSEGV), "");
}

}  // namespace
