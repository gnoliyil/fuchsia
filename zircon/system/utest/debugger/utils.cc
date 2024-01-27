// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "utils.h"

#include <elf.h>
#include <inttypes.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/debug.h>
#include <zircon/syscalls/object.h>
#include <zircon/syscalls/port.h>

#include <fbl/algorithm.h>
#include <test-utils/test-utils.h>
#include <zxtest/zxtest.h>

// argv[0]
const char* g_program_path;

uint64_t extract_pc_reg(const zx_thread_state_general_regs_t* regs) {
#if defined(__x86_64__)
  return regs->rip;
#elif defined(__aarch64__) || defined(__riscv)
  return regs->pc;
#else
#error "what machine?"
#endif
}

uint64_t extract_sp_reg(const zx_thread_state_general_regs_t* regs) {
#if defined(__x86_64__)
  return regs->rsp;
#elif defined(__aarch64__) || defined(__riscv)
  return regs->sp;
#else
#error "what machine?"
#endif
}

static __NO_RETURN void fatal(const char* msg, ...) {
  fprintf(stderr, "%s: encountered fatal error:\n", g_program_path);
  va_list args;
  va_start(args, msg);
  vfprintf(stderr, msg, args);
  va_end(args);
  exit(1);
}

uint32_t get_uint32(char* buf) {
  uint32_t value = 0;
  memcpy(&value, buf, sizeof(value));
  return value;
}

uint64_t get_uint64(char* buf) {
  uint64_t value = 0;
  memcpy(&value, buf, sizeof(value));
  return value;
}

void set_uint64(char* buf, uint64_t value) { memcpy(buf, &value, sizeof(value)); }

uint32_t get_uint32_property(zx_handle_t handle, uint32_t prop) {
  uint32_t value;
  zx_status_t status = zx_object_get_property(handle, prop, &value, sizeof(value));
  if (status != ZX_OK)
    tu_fatal("zx_object_get_property failed", status);
  return value;
}

void send_request(zx_handle_t handle, const request_message_t& rqst) {
  printf("sending request %d on handle %u\n", rqst.type, handle);
  zx_status_t status = zx_channel_write(handle, 0, &rqst, sizeof(rqst), NULL, 0);
  ZX_ASSERT(status == ZX_OK);
}

void send_simple_request(zx_handle_t handle, request_t type) {
  printf("sending request %d on handle %u\n", type, handle);
  zx_status_t status = zx_channel_write(handle, 0, &type, sizeof(type), NULL, 0);
  ZX_ASSERT(status == ZX_OK);
}

void send_response(zx_handle_t handle, const response_message_t& resp) {
  printf("sending response %d on handle %u\n", resp.type, handle);
  zx_status_t status = zx_channel_write(handle, 0, &resp, sizeof(resp), NULL, 0);
  ZX_ASSERT(status == ZX_OK);
}

void send_response_with_handle(zx_handle_t handle, const response_message_t& resp,
                               zx_handle_t resp_handle) {
  printf("sending response %d on handle %u\n", resp.type, handle);
  zx_status_t status = zx_channel_write(handle, 0, &resp, sizeof(resp), &resp_handle, 1);
  ZX_ASSERT(status == ZX_OK);
}

void send_simple_response(zx_handle_t handle, response_t type) {
  printf("sending response %d on handle %u\n", type, handle);
  zx_status_t status = zx_channel_write(handle, 0, &type, sizeof(type), NULL, 0);
  ZX_ASSERT(status == ZX_OK);
}

void recv_request(zx_handle_t handle, request_message_t* rqst) {
  printf("waiting for request on handle %u\n", handle);

  ZX_ASSERT_MSG(tu_channel_wait_readable(handle), "peer closed while trying to read message");

  uint32_t num_bytes = sizeof(*rqst);
  zx_status_t status = zx_channel_read(handle, 0, rqst, nullptr, num_bytes, 0, &num_bytes, nullptr);
  ZX_ASSERT(status == ZX_OK);
  ZX_ASSERT_MSG(num_bytes <= sizeof(*rqst), "unexpected request size");
}

void recv_response(zx_handle_t handle, response_message_t* resp) {
  printf("waiting for response on handle %u\n", handle);

  ZX_ASSERT_MSG(tu_channel_wait_readable(handle), "peer closed while trying to read message");

  uint32_t num_bytes = sizeof(*resp);
  zx_handle_t resp_handle = ZX_HANDLE_INVALID;
  uint32_t num_handles = 1;
  zx_status_t status = zx_channel_read(handle, 0, resp, &resp_handle, num_bytes, num_handles,
                                       &num_bytes, &num_handles);
  ZX_ASSERT(status == ZX_OK);
  ZX_ASSERT_MSG(num_bytes <= sizeof(*resp), "unexpected response size");
  if (num_handles > 0) {
    ZX_ASSERT(num_handles == 1);
    ZX_ASSERT(resp_handle != ZX_HANDLE_INVALID);
    resp->handle = resp_handle;
  }
}

void recv_simple_response(zx_handle_t handle, response_t expected_type) {
  response_message_t response;
  recv_response(handle, &response);
  printf("received message %d\n", response.type);
  ZX_ASSERT(response.type == expected_type);
}

void verify_inferior_running(zx_handle_t channel) {
  send_simple_request(channel, RQST_PING);
  recv_simple_response(channel, RESP_PONG);
}

void get_inferior_thread_handle(zx_handle_t channel, zx_handle_t* thread) {
  send_simple_request(channel, RQST_GET_THREAD_HANDLE);
  response_message_t response;
  recv_response(channel, &response);
  ASSERT_EQ(response.type, RESP_THREAD_HANDLE, "");
  ASSERT_NE(response.handle, ZX_HANDLE_INVALID, "");
  *thread = response.handle;
}

static int phdr_info_callback(dl_phdr_info* info, size_t size, void* argp) {
  dl_phdr_info* arg = reinterpret_cast<dl_phdr_info*>(argp);
  if (info->dlpi_addr == arg->dlpi_addr) {
    *arg = *info;
    return 1;
  }
  return 0;
}

// Fetch the [inclusive] range of the executable segment of the vdso.

void get_vdso_exec_range(uintptr_t* start, uintptr_t* end) {
  uintptr_t prop_vdso_base;
  zx_status_t status = zx_object_get_property(zx_process_self(), ZX_PROP_PROCESS_VDSO_BASE_ADDRESS,
                                              &prop_vdso_base, sizeof(prop_vdso_base));
  ASSERT_EQ(status, 0, "zx_object_get_property failed: %d", status);

  dl_phdr_info info;
  info.dlpi_addr = prop_vdso_base;
  int ret = dl_iterate_phdr(&phdr_info_callback, &info);
  ASSERT_EQ(ret, 1, "dl_iterate_phdr didn't see vDSO?");

  uintptr_t vdso_code_start = 0;
  size_t vdso_code_len = 0;
  for (unsigned i = 0; i < info.dlpi_phnum; ++i) {
    if (info.dlpi_phdr[i].p_type == PT_LOAD && (info.dlpi_phdr[i].p_flags & PF_X)) {
      vdso_code_start = info.dlpi_addr + info.dlpi_phdr[i].p_vaddr;
      vdso_code_len = info.dlpi_phdr[i].p_memsz;
      break;
    }
  }
  ASSERT_NE(vdso_code_start, 0u, "vDSO has no code segment?");
  ASSERT_NE(vdso_code_len, 0u, "vDSO has no code segment?");

  *start = vdso_code_start;
  *end = vdso_code_start + vdso_code_len - 1;
}

struct find_so_callback_arg_t {
  const char* so_name;
  dl_phdr_info* info;
};

static int find_so_callback(dl_phdr_info* info, size_t size, void* argp) {
  auto arg = reinterpret_cast<find_so_callback_arg_t*>(argp);
  if (strcmp(info->dlpi_name, arg->so_name) == 0) {
    *arg->info = *info;
    return 1;
  }
  return 0;
}

void find_so(const char* so_name, dl_phdr_info* info) {
  find_so_callback_arg_t callback_arg;
  callback_arg.so_name = so_name;
  callback_arg.info = info;

  int ret = dl_iterate_phdr(&find_so_callback, &callback_arg);
  if (ret != 1) {
    fatal("dl_iterate_phdr didn't find SO?\n");
  }

  uint64_t base_addr = info->dlpi_addr;
  auto ehdr = reinterpret_cast<const Elf64_Ehdr*>(base_addr);
  if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' ||
      ehdr->e_ident[3] != 'F') {
    fatal("Unexpected executable ELF header contents\n");
  }
}

zx_vaddr_t get_exec_load_addr() {
  dl_phdr_info info;
  // The executable doesn't have an SO name.
  find_so("", &info);
  return info.dlpi_addr;
}

zx_vaddr_t get_libc_load_addr() {
  dl_phdr_info info;
  find_so("libc.so", &info);
  return info.dlpi_addr;
}

void get_inferior_load_addrs(zx_handle_t channel, zx_vaddr_t* libc_load_addr,
                             zx_vaddr_t* exec_load_addr) {
  send_simple_request(channel, RQST_GET_LOAD_ADDRS);
  response_message_t response;
  recv_response(channel, &response);
  ASSERT_EQ(response.type, RESP_LOAD_ADDRS, "");
  *libc_load_addr = response.payload.load_addrs.libc_load_addr;
  *exec_load_addr = response.payload.load_addrs.exec_load_addr;
}

zx_vaddr_t get_libc_entry_point() {
  dl_phdr_info info;
  find_so("libc.so", &info);

  uint64_t base_addr = info.dlpi_addr;
  auto ehdr = reinterpret_cast<const Elf64_Ehdr*>(base_addr);
  return ehdr->e_entry;
}
