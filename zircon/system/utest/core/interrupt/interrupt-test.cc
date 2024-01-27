// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/guest.h>
#include <lib/zx/interrupt.h>
#include <lib/zx/port.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <lib/zx/vcpu.h>
#include <lib/zx/vmar.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/iommu.h>
#include <zircon/syscalls/object.h>

#include <array>
#include <thread>

#include <fbl/algorithm.h>
#include <zxtest/zxtest.h>

#include "fixture.h"

namespace {

constexpr zx::time kSignaledTimeStamp1(12345);
constexpr zx::time kSignaledTimeStamp2(67890);
constexpr uint32_t kKey = 789;

// Use an alias so we use a different test case name.
using InterruptTest = RootResourceFixture;

// This is not really a function, but an entry point for a thread that has
// a tiny stack and no other setup. It's not really entered with the C ABI
// as such.  Rather, it's entered with the first argument register set to
// zx_handle_t and with the SP at the very top of the allocated stack.
// It's defined in pure assembly so that there are no issues with
// compiler-generated code's assumptions about the proper ABI setup,
// instrumentation, etc.
extern "C" void ThreadEntry(uintptr_t arg1, uintptr_t arg2);
// if (zx_interrupt_wait(static_cast<zx_handle_t>(arg1), nullptr) == ZX_OK) {
//   zx_thread_exit();
// }
// ASSERT(false);
__asm__(
    ".pushsection .text.ThreadEntry,\"ax\",%progbits\n"
    ".balign 4\n"
    ".type ThreadEntry,%function\n"
    "ThreadEntry:\n"
#ifdef __aarch64__
    "  mov x1, xzr\n"           // Load nullptr into argument register.
    "  bl zx_interrupt_wait\n"  // Call.
    "  cbz w0, exit\n"          // Exit if returned ZX_OK.
    "  brk #0\n"                // Else crash.
    "exit:\n"
    "  bl zx_thread_exit\n"
    "  brk #0\n"  // Crash if we didn't exit.
#elif defined(__riscv)
    "  mv a1, zero\n"             // nullptr in second argument register.
    "  call zx_interrupt_wait\n"  // Call.
    "  beqz a0, 0f\n"             // Exit if returned ZX_OK.
    "  unimp\n"                   // Else crash.
    "0:\n"
    "  call zx_thread_exit\n"
    "  unimp\n"  // Crash if we didn't exit.
#elif defined(__x86_64__)
    "  xor %edx, %edx\n"          // Load nullptr into argument register.
    "  call zx_interrupt_wait\n"  // Call.
    "  testl %eax, %eax\n"        // If returned ZX_OK...
    "  jz exit\n"                 // ...exit.
    "  ud2\n"                     // Else crash.
    "exit:\n"
    "  call zx_thread_exit\n"
    "  ud2\n"  // Crash if we didn't exit.
#else
#error "what machine?"
#endif
    ".size ThreadEntry, . - ThreadEntry\n"
    ".popsection");

// Tests to bind interrupt to a non-bindable port
TEST_F(InterruptTest, NonBindablePort) {
  zx::interrupt interrupt;
  zx::port port;

  ASSERT_OK(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt));
  // Incorrectly pass 0 for options instead of ZX_PORT_BIND_TO_INTERRUPT
  ASSERT_OK(zx::port::create(0, &port));

  ASSERT_EQ(interrupt.bind(port, kKey, 0), ZX_ERR_WRONG_TYPE);
}

// Tests that an interrupt that is in the TRIGGERED state will send the IRQ out on a port
// if it is bound to that port.
TEST_F(InterruptTest, BindTriggeredIrqToPort) {
  zx::interrupt interrupt;
  zx::port port;
  zx_port_packet_t out;

  ASSERT_OK(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt));
  ASSERT_OK(zx::port::create(ZX_PORT_BIND_TO_INTERRUPT, &port));

  // Trigger the IRQ.
  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));

  // Bind to a Port.
  ASSERT_OK(interrupt.bind(port, kKey, 0));

  // See if the packet is delivered.
  ASSERT_OK(port.wait(zx::time::infinite(), &out));
  ASSERT_EQ(out.interrupt.timestamp, kSignaledTimeStamp1.get());
}

// Tests Interrupts bound to a port
TEST_F(InterruptTest, BindPort) {
  zx::interrupt interrupt;
  zx::port port;
  zx_port_packet_t out;

  ASSERT_OK(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt));
  ASSERT_OK(zx::port::create(ZX_PORT_BIND_TO_INTERRUPT, &port));

  // Test port binding
  ASSERT_OK(interrupt.bind(port, kKey, 0));
  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));
  ASSERT_OK(port.wait(zx::time::infinite(), &out));
  ASSERT_EQ(out.interrupt.timestamp, kSignaledTimeStamp1.get());

  // Triggering 2nd time, ACKing it causes port packet to be delivered
  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));
  ASSERT_OK(interrupt.ack());
  ASSERT_OK(port.wait(zx::time::infinite(), &out));
  ASSERT_EQ(out.interrupt.timestamp, kSignaledTimeStamp1.get());
  ASSERT_EQ(out.key, kKey);
  ASSERT_EQ(out.type, ZX_PKT_TYPE_INTERRUPT);
  ASSERT_OK(out.status);
  ASSERT_OK(interrupt.ack());

  // Triggering it twice
  // the 2nd timestamp is recorded and upon ACK another packet is queued
  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));
  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp2));
  ASSERT_OK(port.wait(zx::time::infinite(), &out));
  ASSERT_EQ(out.interrupt.timestamp, kSignaledTimeStamp1.get());
  ASSERT_OK(interrupt.ack());
  ASSERT_OK(port.wait(zx::time::infinite(), &out));
  ASSERT_EQ(out.interrupt.timestamp, kSignaledTimeStamp2.get());

  // Try to destroy now, expecting to return error telling packet
  // has been read but the interrupt has not been re-armed
  ASSERT_EQ(interrupt.destroy(), ZX_ERR_NOT_FOUND);
  ASSERT_EQ(interrupt.ack(), ZX_ERR_CANCELED);
  ASSERT_EQ(interrupt.trigger(0, kSignaledTimeStamp1), ZX_ERR_CANCELED);
}

// Tests Interrupt Unbind
TEST_F(InterruptTest, UnBindPort) {
  zx::interrupt interrupt;
  ASSERT_OK(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt));
  zx::port port;
  ASSERT_OK(zx::port::create(ZX_PORT_BIND_TO_INTERRUPT, &port));

  // Test port binding
  ASSERT_OK(interrupt.bind(port, kKey, ZX_INTERRUPT_BIND));
  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));
  zx_port_packet_t out;
  ASSERT_OK(port.wait(zx::time::infinite(), &out));
  ASSERT_EQ(out.interrupt.timestamp, kSignaledTimeStamp1.get());

  // Ubind port, and test the unbind-trigger-port_wait sequence. The interrupt packet
  // should not be delivered from port_wait, since the trigger happened after the Unbind.
  // not receive the interrupt packet. But test some invalid use cases of unbind first.
  ASSERT_STATUS(interrupt.bind(port, 0, 2), ZX_ERR_INVALID_ARGS);
  zx::port port2;
  ASSERT_OK(zx::port::create(ZX_PORT_BIND_TO_INTERRUPT, &port2));
  ASSERT_STATUS(interrupt.bind(port2, 0, ZX_INTERRUPT_UNBIND), ZX_ERR_NOT_FOUND);
  ASSERT_OK(interrupt.bind(port, 0, ZX_INTERRUPT_UNBIND));
  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));
  ASSERT_STATUS(port.wait(zx::deadline_after(zx::msec(10)), &out), ZX_ERR_TIMED_OUT);

  // Bind again, and test the trigger-unbind-port_wait sequence. Interrupt packet should
  // be removed from the port at unbind, so there should be no interrupt packets to read here.
  ASSERT_OK(interrupt.bind(port, kKey, ZX_INTERRUPT_BIND));
  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));
  ASSERT_OK(interrupt.bind(port, 0, ZX_INTERRUPT_UNBIND));
  ASSERT_STATUS(port.wait(zx::deadline_after(zx::msec(10)), &out), ZX_ERR_TIMED_OUT);
  ASSERT_EQ(out.interrupt.timestamp, kSignaledTimeStamp1.get());

  // Finally test the case of an UNBIND after the interrupt dispatcher object has been
  // destroyed.
  ASSERT_OK(interrupt.bind(port, kKey, ZX_INTERRUPT_BIND));
  // destroy the interrupt and try unbind. For the destroy, we expect ZX_ERR_CANCELED,
  // since the packet has been read but the interrupt hasn't been re-armed.
  ASSERT_STATUS(interrupt.destroy(), ZX_ERR_NOT_FOUND);
  ASSERT_STATUS(interrupt.bind(port, 0, ZX_INTERRUPT_UNBIND), ZX_ERR_CANCELED);
}

// Tests support for virtual interrupts
TEST_F(InterruptTest, VirtualInterrupts) {
  zx::interrupt interrupt;
  zx::interrupt interrupt_cancelled;
  zx::time timestamp;

  ASSERT_EQ(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_SLOT_USER, &interrupt),
            ZX_ERR_INVALID_ARGS);
  ASSERT_OK(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt));
  ASSERT_OK(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt_cancelled));

  ASSERT_OK(interrupt_cancelled.destroy());
  ASSERT_EQ(interrupt_cancelled.trigger(0, kSignaledTimeStamp1), ZX_ERR_CANCELED);

  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));

  ASSERT_EQ(interrupt_cancelled.wait(&timestamp), ZX_ERR_CANCELED);
  ASSERT_OK(interrupt.wait(&timestamp));
  ASSERT_EQ(timestamp.get(), kSignaledTimeStamp1.get());

  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));
  ASSERT_OK(interrupt.wait(&timestamp));
}

bool WaitThread(const zx::thread& thread, uint32_t reason) {
  while (true) {
    zx_info_thread_t info;
    EXPECT_OK(thread.get_info(ZX_INFO_THREAD, &info, sizeof(info), nullptr, nullptr));
    if (info.state == reason) {
      return true;
    }
    zx::nanosleep(zx::deadline_after(zx::msec(1)));
  }
  return true;
}

// Tests interrupt thread after suspend/resume
TEST_F(InterruptTest, WaitThreadFunctionsAfterSuspendResume) {
  zx::interrupt interrupt;
  zx::thread thread;
  constexpr char name[] = "interrupt_test_thread";
  // preallocated stack to satisfy the thread we create
  static uint8_t stack[1024] __ALIGNED(16);

  ASSERT_OK(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt));

  // Create and start a thread which waits for an IRQ
  ASSERT_OK(zx::thread::create(*zx::process::self(), name, sizeof(name), 0, &thread));

  ASSERT_OK(
      thread.start(ThreadEntry, &stack[sizeof(stack)], static_cast<uintptr_t>(interrupt.get()), 0));

  // Wait till the thread is in blocked state
  ASSERT_TRUE(WaitThread(thread, ZX_THREAD_STATE_BLOCKED_INTERRUPT));

  // Suspend the thread, wait till it is suspended
  zx::suspend_token suspend_token;
  ASSERT_OK(thread.suspend(&suspend_token));
  ASSERT_TRUE(WaitThread(thread, ZX_THREAD_STATE_SUSPENDED));

  // Resume the thread, wait till it is back to being in blocked state
  suspend_token.reset();
  ASSERT_TRUE(WaitThread(thread, ZX_THREAD_STATE_BLOCKED_INTERRUPT));

  // Signal the interrupt and wait for the thread to exit.
  interrupt.trigger(0, zx::time());
  zx_signals_t observed;
  ASSERT_OK(thread.wait_one(ZX_TASK_TERMINATED, zx::time::infinite(), &observed));
}

// Tests support for null output timestamp
// NOTE: Absent the changes to interrupt.h also submitted in this CL, this test invokes undefined
//       behavior not detectable at runtime.
TEST_F(InterruptTest, NullOutputTimestamp) {
  zx::interrupt interrupt;

  ASSERT_OK(zx::interrupt::create(*root_resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt));

  ASSERT_OK(interrupt.trigger(0, kSignaledTimeStamp1));

  ASSERT_OK(interrupt.wait(nullptr));
}

}  // namespace
