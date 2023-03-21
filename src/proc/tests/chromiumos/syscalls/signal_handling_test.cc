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

#include "src/proc/tests/chromiumos/syscalls/test_helper.h"

namespace {

uintptr_t stack_ptr;

constexpr int kRaisedSIGSEGV = 1;
constexpr int kExitTestFailure = 2;
constexpr int kExitTestSuccess = 3;

constexpr size_t kRedzoneSize = 128;

int setup_sigaltstack_at(uintptr_t base, size_t size) {
  stack_t ss = {
      .ss_sp = reinterpret_cast<void *>(base),
      .ss_flags = 0,
      .ss_size = size,
  };

  return sigaltstack(&ss, NULL);
}

// Helper function for setting up a sig altstack.
void *setup_sigaltstack(size_t size) {
  void *altstack =
      mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_STACK | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (altstack == MAP_FAILED) {
    exit(kExitTestFailure);
  }

  if (setup_sigaltstack_at(reinterpret_cast<uintptr_t>(altstack), size)) {
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

TEST(SignalHandlingDeathTest, SignalStackUnmappedDeliversSIGSEGV) {
  constexpr size_t kStackSize = 0x20000;
  void *temp_stack = mmap(NULL, kStackSize, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK, -1, 0);
  ASSERT_NE(MAP_FAILED, temp_stack);

  // Set the stack ptr to the end of the allocated region.
  uint64_t stack_addr = reinterpret_cast<uint64_t>(temp_stack) + kStackSize;

  auto test_func = [](uint64_t stack_addr) {
    // Set up a handler for SIGUSR1
    struct sigaction sa = {};
    sa.sa_handler = [](int) { exit(kExitTestSuccess); };

    if (sigaction(SIGUSR1, &sa, 0)) {
      exit(kExitTestFailure);
    }

    raise_with_stack(SIGUSR1, stack_addr);
  };

  EXPECT_EXIT(test_func(stack_addr), testing::ExitedWithCode(kExitTestSuccess), "");

  // Non-writable stack.
  ASSERT_EQ(0, mprotect(temp_stack, kStackSize, PROT_READ));

  EXPECT_EXIT(test_func(stack_addr), testing::KilledBySignal(SIGSEGV), "");

  // Non-readable stack.
  ASSERT_EQ(0, mprotect(temp_stack, kStackSize, PROT_NONE));
  EXPECT_EXIT(test_func(stack_addr), testing::KilledBySignal(SIGSEGV), "");

  ASSERT_EQ(0, munmap(temp_stack, kStackSize));
}

TEST(SignalHandlingDeathTest, SignalAltStackUnmappedDeliversSIGSEGV) {
  constexpr size_t kStackSize = 0x20000;
  void *temp_stack = mmap(NULL, kStackSize, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK, -1, 0);
  ASSERT_NE(MAP_FAILED, temp_stack);

  auto test_func = [&]() {
    if (setup_sigaltstack_at(reinterpret_cast<uintptr_t>(temp_stack), kStackSize)) {
      exit(kExitTestFailure);
    }

    struct sigaction sa = {};
    sa.sa_handler = [](int) { exit(kExitTestSuccess); };
    sa.sa_flags = SA_ONSTACK;

    if (sigaction(SIGUSR1, &sa, 0)) {
      exit(kExitTestFailure);
    }

    raise(SIGUSR1);
  };

  EXPECT_EXIT(test_func(), testing::ExitedWithCode(kExitTestSuccess), "");

  // Non-writable stack.
  ASSERT_EQ(0, mprotect(temp_stack, kStackSize, PROT_READ));
  EXPECT_EXIT(test_func(), testing::KilledBySignal(SIGSEGV), "");

  // Non-readable stack.
  ASSERT_EQ(0, mprotect(temp_stack, kStackSize, PROT_NONE));
  EXPECT_EXIT(test_func(), testing::KilledBySignal(SIGSEGV), "");
  ASSERT_EQ(0, munmap(temp_stack, kStackSize));
}

TEST(SignalHandlingDeathTest, SignalStackErrorsDeliversSIGSEGV) {
  std::vector<uint64_t> stack_addrs = {0x0, kRedzoneSize, kRedzoneSize + 1, UINT64_MAX,
                                       UINT64_MAX - 1};
  for (const auto stack_addr : stack_addrs) {
    EXPECT_EXIT(
        [stack_addr]() {
          // Set up a handler for SIGUSR1
          // Should not get called.
          struct sigaction sa = {};
          sa.sa_handler = [](int) { exit(kExitTestSuccess); };

          if (sigaction(SIGUSR1, &sa, 0)) {
            exit(kExitTestFailure);
          }

          raise_with_stack(SIGUSR1, stack_addr);
        }(),
        testing::KilledBySignal(SIGSEGV), "");
  }
}

constexpr size_t kSigAltStackMmapSize = 0x20000;

class SigaltstackDeathTest : public testing::Test {
 protected:
  void SetUp() override {
    // Map a chunk of memory for the sigaltstack, but place
    // the sigaltstack base in the middle of the mapping,
    // so that there is no risk of hitting unintended page faults.
    sigaltstack_mapping_size_ = kSigAltStackMmapSize;
    sigaltstack_mapping_ = mmap(NULL, sigaltstack_mapping_size_, PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK, -1, 0);
    ASSERT_NE(MAP_FAILED, sigaltstack_mapping_);
    sigaltstack_base_ =
        reinterpret_cast<uintptr_t>(sigaltstack_mapping_) + sigaltstack_mapping_size_ / 2;
    sigaltstack_size_ = sigaltstack_mapping_size_ / 2;
  }

  void TearDown() override { munmap(sigaltstack_mapping_, sigaltstack_mapping_size_); }

  // Points to the begginning of the mmaped region for the sigaltstack.
  void *sigaltstack_mapping_;
  size_t sigaltstack_mapping_size_;

  // Has a safe pointer to use in sigaltstack, such that there is
  // sigaltstack_size_ mapped memory to both sides.
  uintptr_t sigaltstack_base_;
  size_t sigaltstack_size_;
};

TEST_F(SigaltstackDeathTest, SigaltstackExceededThrowsSIGSEGV) {
  EXPECT_EXIT(([&]() {
                if (setup_sigaltstack_at(sigaltstack_base_, MINSIGSTKSZ)) {
                  exit(kExitTestFailure);
                }
                struct sigaction sa = {};
                sa.sa_handler = [](int) {};
                sa.sa_flags = SA_ONSTACK;

                if (sigaction(SIGUSR1, &sa, NULL)) {
                  exit(kExitTestFailure);
                }
                sa.sa_flags = 0;
                if (sigaction(SIGSEGV, &sa, NULL)) {
                  exit(kExitTestFailure);
                }

                uintptr_t raise_stack = sigaltstack_base_ + kRedzoneSize + 1;
                raise_with_stack(SIGUSR1, raise_stack);
                exit(kExitTestFailure);
              }()),
              testing::KilledBySignal(SIGSEGV), "");
}

TEST_F(SigaltstackDeathTest, MINSIGSTKSZIsEnough) {
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    ASSERT_EQ(0, setup_sigaltstack_at(sigaltstack_base_, MINSIGSTKSZ));
    struct sigaction sa = {};
    sa.sa_handler = [](int) {};  // empty handler.
    sa.sa_flags = SA_ONSTACK;

    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    sa = {};
    sa.sa_sigaction = [](int, siginfo_t *, void *) {};  // empty handler.
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;

    ASSERT_EQ(0, sigaction(SIGUSR2, &sa, NULL));
    raise(SIGUSR1);
    raise(SIGUSR2);
  });
  EXPECT_TRUE(helper.WaitForChildren());
}

TEST_F(SigaltstackDeathTest, NestedSignalsWork) {
  ForkHelper helper;

  helper.RunInForkedProcess([&] {
    ASSERT_EQ(0, setup_sigaltstack_at(sigaltstack_base_, sigaltstack_size_));
    struct sigaction sa = {};
    sa.sa_handler = [](int) { raise(SIGUSR2); };
    sa.sa_flags = SA_ONSTACK;

    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    sa = {};
    sa.sa_sigaction = [](int, siginfo_t *, void *) {};  // empty handler.
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;

    ASSERT_EQ(0, sigaction(SIGUSR2, &sa, NULL));
    raise(SIGUSR1);
  });
  EXPECT_TRUE(helper.WaitForChildren());
}

void *g_mmap_area;

TEST_F(SigaltstackDeathTest, SigaltstackSetupFailureIsNotResumed) {
  // Raise an exception with a non-writable main stack.
  // Handle the exception in the altstack by mprotecting the bad region.
  // If the sigusr1 handler is resumed, it should exit with kExitTestFailure.
  ForkHelper helper;

  g_mmap_area = mmap(NULL, 0x10000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  ASSERT_NE(MAP_FAILED, g_mmap_area);

  helper.RunInForkedProcess([&] {
    ASSERT_EQ(0, setup_sigaltstack_at(sigaltstack_base_, sigaltstack_size_));
    struct sigaction sa = {};
    sa.sa_handler = [](int) { exit(kExitTestFailure); };

    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    sa = {};
    sa.sa_handler = [](int) { mprotect(g_mmap_area, 0x10000, PROT_READ | PROT_WRITE); };
    sa.sa_flags = SA_ONSTACK;

    ASSERT_EQ(0, sigaction(SIGSEGV, &sa, NULL));
    raise_with_stack(SIGUSR1, reinterpret_cast<uintptr_t>(g_mmap_area) + 0x10000);
  });

  EXPECT_TRUE(helper.WaitForChildren());
  munmap(g_mmap_area, 0x10000);
}

TEST_F(SigaltstackDeathTest, SigaltstackSetupFailureCanUseMainStack) {
  // Raise an exception with a non-writable alt stack.
  // Handle the exception in the main stack.
  ForkHelper helper;
  ASSERT_EQ(0, mprotect(sigaltstack_mapping_, sigaltstack_mapping_size_, PROT_NONE));

  helper.RunInForkedProcess([&] {
    ASSERT_EQ(0, setup_sigaltstack_at(sigaltstack_base_, sigaltstack_size_));
    struct sigaction sa = {};
    sa.sa_handler = [](int) { exit(kExitTestFailure); };
    sa.sa_flags = SA_ONSTACK;

    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    // Handle Sigsegv on the main stack. Resume immediately.
    sa = {};
    sa.sa_handler = [](int) {};
    sa.sa_flags = 0;

    ASSERT_EQ(0, sigaction(SIGSEGV, &sa, NULL));
    raise(SIGUSR1);
  });

  EXPECT_TRUE(helper.WaitForChildren());
}

}  // namespace
