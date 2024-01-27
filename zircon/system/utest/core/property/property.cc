// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/stream.h>
#include <lib/zx/vmo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>
#include <zircon/threads.h>

#include <zxtest/zxtest.h>

namespace {

void get_rights(zx_handle_t handle, zx_rights_t* rights) {
  zx_info_handle_basic_t info;
  ASSERT_EQ(zx_object_get_info(handle, ZX_INFO_HANDLE_BASIC, &info, sizeof(info), NULL, NULL),
            ZX_OK, "");
  *rights = info.rights;
}

void get_new_rights(zx_handle_t handle, zx_rights_t new_rights, zx_handle_t* new_handle) {
  ASSERT_EQ(zx_handle_duplicate(handle, new_rights, new_handle), ZX_OK, "");
}

// |object| must have ZX_RIGHT_{GET,SET}_PROPERTY.

void test_name_property(zx_handle_t object) {
  char set_name[ZX_MAX_NAME_LEN];
  char get_name[ZX_MAX_NAME_LEN];

  // name with extra garbage at the end
  memset(set_name, 'A', sizeof(set_name));
  set_name[1] = '\0';

  EXPECT_EQ(zx_object_set_property(object, ZX_PROP_NAME, set_name, sizeof(set_name)), ZX_OK, "");
  EXPECT_EQ(zx_object_get_property(object, ZX_PROP_NAME, get_name, sizeof(get_name)), ZX_OK, "");
  EXPECT_EQ(get_name[0], 'A', "");
  for (size_t i = 1; i < sizeof(get_name); i++) {
    EXPECT_EQ(get_name[i], '\0', "");
  }

  // empty name
  strcpy(set_name, "");
  EXPECT_EQ(zx_object_set_property(object, ZX_PROP_NAME, set_name, strlen(set_name)), ZX_OK, "");
  EXPECT_EQ(zx_object_get_property(object, ZX_PROP_NAME, get_name, sizeof(get_name)), ZX_OK, "");
  EXPECT_EQ(strcmp(get_name, set_name), 0, "");

  // largest possible name
  memset(set_name, 'x', sizeof(set_name) - 1);
  set_name[sizeof(set_name) - 1] = '\0';
  EXPECT_EQ(zx_object_set_property(object, ZX_PROP_NAME, set_name, strlen(set_name)), ZX_OK, "");
  EXPECT_EQ(zx_object_get_property(object, ZX_PROP_NAME, get_name, sizeof(get_name)), ZX_OK, "");
  EXPECT_EQ(strcmp(get_name, set_name), 0, "");

  // too large a name by 1
  memset(set_name, 'x', sizeof(set_name));
  EXPECT_EQ(zx_object_set_property(object, ZX_PROP_NAME, set_name, sizeof(set_name)), ZX_OK, "");

  zx_rights_t current_rights;
  ASSERT_NO_FATAL_FAILURE(get_rights(object, &current_rights));
  zx_rights_t cant_set_rights = current_rights &= ~ZX_RIGHT_SET_PROPERTY;
  zx_handle_t cant_set;
  ASSERT_NO_FATAL_FAILURE(get_new_rights(object, cant_set_rights, &cant_set));
  EXPECT_EQ(zx_object_set_property(cant_set, ZX_PROP_NAME, "", 0), ZX_ERR_ACCESS_DENIED, "");
  zx_handle_close(cant_set);
}

TEST(Property, JobName) {
  zx_handle_t testjob;
  zx_status_t s = zx_job_create(zx_job_default(), 0, &testjob);
  EXPECT_EQ(s, ZX_OK, "");

  test_name_property(testjob);

  zx_handle_close(testjob);
}

TEST(Property, ProcessName) {
  zx_handle_t self = zx_process_self();
  test_name_property(self);
}

TEST(Property, ThreadName) {
  zx_handle_t main_thread = thrd_get_zx_handle(thrd_current());
  printf("thread handle %d\n", main_thread);
  test_name_property(main_thread);
}

TEST(Property, VmoName) {
  zx_handle_t vmo;
  ASSERT_EQ(zx_vmo_create(16, 0u, &vmo), ZX_OK, "");
  printf("VMO handle %d\n", vmo);

  char name[ZX_MAX_NAME_LEN];
  memset(name, 'A', sizeof(name));

  // Name should start out empty.
  EXPECT_EQ(zx_object_get_property(vmo, ZX_PROP_NAME, name, sizeof(name)), ZX_OK, "");
  for (size_t i = 0; i < sizeof(name); i++) {
    EXPECT_EQ(name[i], '\0', "");
  }

  // Check the rest.
  test_name_property(vmo);
}

TEST(Property, SocketBuffer) {
  zx_handle_t sockets[2];
  ASSERT_EQ(zx_socket_create(0, &sockets[0], &sockets[1]), ZX_OK, "");

  // Check the buffer size after a write.
  uint8_t buf[8] = {};
  size_t actual;
  ASSERT_EQ(zx_socket_write(sockets[1], 0, buf, sizeof(buf), &actual), ZX_OK, "");
  EXPECT_EQ(actual, sizeof(buf), "");

  zx_info_socket_t info;

  memset(&info, 0, sizeof(info));
  ASSERT_EQ(zx_object_get_info(sockets[0], ZX_INFO_SOCKET, &info, sizeof(info), NULL, NULL), ZX_OK,
            "");
  EXPECT_EQ(info.options, 0u, "");
  EXPECT_GT(info.rx_buf_max, 0u, "");
  EXPECT_EQ(info.rx_buf_size, sizeof(buf), "");
  EXPECT_EQ(info.rx_buf_available, sizeof(buf), "");
  EXPECT_GT(info.tx_buf_max, 0u, "");
  EXPECT_EQ(info.tx_buf_size, 0u, "");

  memset(&info, 0, sizeof(info));
  ASSERT_EQ(zx_object_get_info(sockets[1], ZX_INFO_SOCKET, &info, sizeof(info), NULL, NULL), ZX_OK,
            "");
  EXPECT_EQ(info.options, 0u, "");
  EXPECT_GT(info.rx_buf_max, 0u, "");
  EXPECT_EQ(info.rx_buf_size, 0u, "");
  EXPECT_EQ(info.rx_buf_available, 0u, "");
  EXPECT_GT(info.tx_buf_max, 0u, "");
  EXPECT_EQ(info.tx_buf_size, sizeof(buf), "");

  // Check TX buf goes to zero on peer closed.
  zx_handle_close(sockets[0]);

  memset(&info, 0, sizeof(info));
  ASSERT_EQ(zx_object_get_info(sockets[1], ZX_INFO_SOCKET, &info, sizeof(info), NULL, NULL), ZX_OK,
            "");
  EXPECT_EQ(info.options, 0u, "");
  EXPECT_GT(info.rx_buf_max, 0u, "");
  EXPECT_EQ(info.rx_buf_size, 0u, "");
  EXPECT_EQ(info.rx_buf_available, 0u, "");
  EXPECT_EQ(info.tx_buf_max, 0u, "");
  EXPECT_EQ(info.tx_buf_size, 0u, "");

  zx_handle_close(sockets[1]);

  ASSERT_EQ(zx_socket_create(ZX_SOCKET_DATAGRAM, &sockets[0], &sockets[1]), ZX_OK, "");

  memset(&info, 0, sizeof(info));
  ASSERT_EQ(zx_object_get_info(sockets[0], ZX_INFO_SOCKET, &info, sizeof(info), NULL, NULL), ZX_OK,
            "");
  EXPECT_EQ(info.options, ZX_SOCKET_DATAGRAM, "");
  EXPECT_GT(info.rx_buf_max, 0u, "");
  EXPECT_EQ(info.rx_buf_size, 0u, "");
  EXPECT_EQ(info.rx_buf_available, 0u, "");
  EXPECT_GT(info.tx_buf_max, 0u, "");
  EXPECT_EQ(info.tx_buf_size, 0u, "");

  ASSERT_EQ(zx_socket_write(sockets[1], 0, buf, sizeof(buf), &actual), ZX_OK, "");
  EXPECT_EQ(actual, sizeof(buf), "");

  memset(&info, 0, sizeof(info));
  ASSERT_EQ(zx_object_get_info(sockets[0], ZX_INFO_SOCKET, &info, sizeof(info), NULL, NULL), ZX_OK,
            "");
  EXPECT_EQ(info.options, ZX_SOCKET_DATAGRAM, "");
  EXPECT_GT(info.rx_buf_max, 0u, "");
  EXPECT_EQ(info.rx_buf_size, 8u, "");
  EXPECT_EQ(info.rx_buf_available, 8u, "");
  EXPECT_GT(info.tx_buf_max, 0u, "");
  EXPECT_EQ(info.tx_buf_size, 0u, "");

  ASSERT_EQ(zx_socket_write(sockets[1], 0, buf, sizeof(buf) / 2, &actual), ZX_OK, "");
  EXPECT_EQ(actual, sizeof(buf) / 2, "");

  memset(&info, 0, sizeof(info));
  ASSERT_EQ(zx_object_get_info(sockets[0], ZX_INFO_SOCKET, &info, sizeof(info), NULL, NULL), ZX_OK,
            "");
  EXPECT_EQ(info.options, ZX_SOCKET_DATAGRAM, "");
  EXPECT_GT(info.rx_buf_max, 0u, "");
  EXPECT_EQ(info.rx_buf_size, 12u, "");
  EXPECT_EQ(info.rx_buf_available, 8u, "");
  EXPECT_GT(info.tx_buf_max, 0u, "");
  EXPECT_EQ(info.tx_buf_size, 0u, "");

  zx_handle_close_many(sockets, 2);
}

TEST(Property, NameWrongType) {
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(zx_system_get_page_size(), 0, &vmo));

  zx::stream stream;
  ASSERT_OK(zx::stream::create(0, vmo, 0, &stream));

  // Stream does not have a name property, so we cannot get/set it.
  char name[ZX_MAX_NAME_LEN];
  ASSERT_EQ(ZX_ERR_WRONG_TYPE, stream.get_property(ZX_PROP_NAME, name, sizeof(name)));
  ASSERT_EQ(ZX_ERR_WRONG_TYPE, stream.set_property(ZX_PROP_NAME, name, sizeof(name)));
  ASSERT_EQ(ZX_ERR_WRONG_TYPE, stream.get_property(ZX_PROP_NAME, name, sizeof(name)));

  // VMO has a stream property, so we can successfully get/set it.
  ASSERT_OK(vmo.get_property(ZX_PROP_NAME, name, sizeof(name)));
  ASSERT_OK(vmo.set_property(ZX_PROP_NAME, name, sizeof(name)));
  ASSERT_OK(vmo.get_property(ZX_PROP_NAME, name, sizeof(name)));
}

}  // namespace
