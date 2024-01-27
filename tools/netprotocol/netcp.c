// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <lib/netboot/netboot.h>
#include <libgen.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <tftp/tftp.h>

#include "netprotocol.h"

#define TFTP_BUF_SZ 2048

typedef struct {
  int fd;
  size_t size;
} file_info_t;

typedef struct {
  int socket;
  bool connected;
  uint32_t previous_timeout_ms;
  struct sockaddr_in6 target_addr;
} transport_info_t;

static const char* appname;

static ssize_t file_open_read(const char* filename, uint8_t session_timeout_secs,
                              void* file_cookie) {
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    return TFTP_ERR_IO;
  }
  file_info_t* file_info = file_cookie;
  file_info->fd = fd;
  struct stat st;
  if (fstat(file_info->fd, &st) < 0) {
    close(fd);
    return TFTP_ERR_IO;
  }
  file_info->size = st.st_size;
  return st.st_size;
}

static tftp_status file_open_write(const char* filename, size_t size, uint8_t session_timeout_secs,
                                   void* file_cookie) {
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (fd < 0) {
    return TFTP_ERR_IO;
  }
  file_info_t* file_info = file_cookie;
  file_info->fd = fd;
  file_info->size = size;
  return TFTP_NO_ERROR;
}

static tftp_status file_read(void* data, size_t* length, off_t offset, void* file_cookie) {
  int fd = ((file_info_t*)file_cookie)->fd;
  ssize_t n = pread(fd, data, *length, offset);
  if (n < 0) {
    return TFTP_ERR_IO;
  }
  *length = n;
  return TFTP_NO_ERROR;
}

static tftp_status file_write(const void* data, size_t* length, off_t offset, void* file_cookie) {
  int fd = ((file_info_t*)file_cookie)->fd;
  ssize_t n = pwrite(fd, data, *length, offset);
  if (n < 0) {
    return TFTP_ERR_IO;
  }
  *length = n;
  return TFTP_NO_ERROR;
}

static void file_close(void* file_cookie) { close(((file_info_t*)file_cookie)->fd); }

// Longest time we will wait for a send operation to succeed
#define MAX_SEND_TIME_MS 1000

static tftp_status transport_send(void* data, size_t len, void* transport_cookie) {
  transport_info_t* transport_info = transport_cookie;
  ssize_t send_result;
  struct pollfd poll_fds = {.fd = transport_info->socket, .events = POLLOUT};
  do {
    int poll_result = poll(&poll_fds, 1, MAX_SEND_TIME_MS);
    if (poll_result <= 0) {
      // We'll treat a timeout as an IO error and not a TFTP_ERR_TIMED_OUT,
      // since the latter is a timeout waiting for a response from the server.
      return TFTP_ERR_IO;
    }
    if (!transport_info->connected) {
      transport_info->target_addr.sin6_port = htons(NETBOOT_TFTP_INCOMING_PORT);
      send_result = sendto(transport_info->socket, data, len, 0,
                           (struct sockaddr*)&transport_info->target_addr,
                           sizeof(transport_info->target_addr));
    } else {
      send_result = send(transport_info->socket, data, len, 0);
    }
  } while ((send_result < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) ||
                                 (errno == ENOBUFS && sched_yield() == 0)));

  if (send_result < 0) {
    return TFTP_ERR_IO;
  }
  return TFTP_NO_ERROR;
}

static int transport_recv(void* data, size_t len, bool block, void* transport_cookie) {
  transport_info_t* transport_info = transport_cookie;
  int flags = fcntl(transport_info->socket, F_GETFL, 0);
  if (flags < 0) {
    return TFTP_ERR_IO;
  }
  if (block) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |= O_NONBLOCK;
  }
  if (fcntl(transport_info->socket, F_SETFL, flags)) {
    return TFTP_ERR_IO;
  }
  ssize_t recv_result;
  struct sockaddr_in6 connection_addr;
  socklen_t addr_len = sizeof(connection_addr);
  if (!transport_info->connected) {
    recv_result = recvfrom(transport_info->socket, data, len, 0, (struct sockaddr*)&connection_addr,
                           &addr_len);
  } else {
    recv_result = recv(transport_info->socket, data, len, 0);
  }
  if (recv_result < 0) {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
      return TFTP_ERR_TIMED_OUT;
    }
    return TFTP_ERR_INTERNAL;
  }
  if (!transport_info->connected) {
    if (connect(transport_info->socket, (struct sockaddr*)&connection_addr,
                sizeof(connection_addr)) < 0) {
      return TFTP_ERR_IO;
    }
    memcpy(&transport_info->target_addr, &connection_addr, sizeof(transport_info->target_addr));
    transport_info->connected = true;
  }
  return recv_result;
}

static int transport_timeout_set(uint32_t timeout_ms, void* transport_cookie) {
  transport_info_t* transport_info = transport_cookie;
  if (transport_info->previous_timeout_ms != timeout_ms && timeout_ms > 0) {
    transport_info->previous_timeout_ms = timeout_ms;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = 1000 * (timeout_ms - 1000 * tv.tv_sec);
    return setsockopt(transport_info->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  return 0;
}

static int transfer_file(bool push, int s, struct sockaddr_in6* addr, const char* dst,
                         const char* src) {
  // Initialize session
  tftp_session* session = NULL;
  size_t session_data_sz = tftp_sizeof_session();
  void* session_data = calloc(session_data_sz, 1);
  if (session_data == NULL) {
    fprintf(stderr, "%s: unable to allocate tftp session memory\n", appname);
    return 1;
  }
  if (tftp_init(&session, session_data, session_data_sz) != TFTP_NO_ERROR) {
    fprintf(stderr, "%s: unable to initiate tftp session\n", appname);
    free(session_data);
    return 1;
  }

  // Initialize file interface
  file_info_t file_info;
  tftp_file_interface file_ifc = {file_open_read, file_open_write, file_read, file_write,
                                  file_close};
  tftp_session_set_file_interface(session, &file_ifc);

  // Initialize transport interface
  transport_info_t transport_info;
  transport_info.previous_timeout_ms = 0;
  transport_info.socket = s;
  transport_info.connected = false;
  memcpy(&transport_info.target_addr, addr, sizeof(transport_info.target_addr));
  tftp_transport_interface transport_ifc = {transport_send, transport_recv, transport_timeout_set};
  tftp_session_set_transport_interface(session, &transport_ifc);

  // Set our preferred transport options
  tftp_set_options(session, &tftp_block_size, NULL, &tftp_window_size);

  // Prepare buffers
  char err_msg[128];
  tftp_request_opts opts = {0};
  opts.inbuf = malloc(TFTP_BUF_SZ);
  opts.inbuf_sz = TFTP_BUF_SZ;
  opts.outbuf = malloc(TFTP_BUF_SZ);
  opts.outbuf_sz = TFTP_BUF_SZ;
  opts.err_msg = err_msg;
  opts.err_msg_sz = sizeof(err_msg);

  tftp_status status;
  if (push) {
    status = tftp_push_file(session, &transport_info, &file_info, src, dst, &opts);
  } else {
    status = tftp_pull_file(session, &transport_info, &file_info, dst, src, &opts);
  }

  free(session_data);
  free(opts.inbuf);
  free(opts.outbuf);

  if (status < 0) {
    fprintf(stderr, "%s: %s (status = %d)\n", appname, opts.err_msg, (int)status);
    return 1;
  }

  fprintf(stderr, "wrote %zu bytes\n", file_info.size);

  return 0;
}

static void usage(void) {
  fprintf(stderr, "usage: %s [options] [hostname:]src [hostname:]dst\n", appname);
  netboot_usage(true);
}

int main(int argc, char** argv) {
  appname = argv[0];

  int index = netboot_handle_getopt(argc, argv);
  if (index < 0) {
    usage();
    return -1;
  }
  argv += index;
  argc -= index;

  if (argc != 2) {
    usage();
    return -1;
  }

  const char* src = argv[0];
  const char* dst = argv[1];

  int push = -1;
  char* pos;
  const char* hostname;
  if ((pos = strpbrk(src, ":")) != 0) {
    push = 0;
    hostname = src;
    pos[0] = 0;
    src = pos + 1;
  }
  if ((pos = strpbrk(dst, ":")) != 0) {
    if (push == 0) {
      fprintf(stderr, "%s: only one of src or dst can have a hostname\n", appname);
      return -1;
    }
    push = 1;
    hostname = dst;
    pos[0] = 0;
    dst = pos + 1;
  }
  if (push == -1) {
    fprintf(stderr, "%s: either src or dst needs a hostname\n", appname);
    return -1;
  }

  int s;
  struct sockaddr_in6 server_addr;
  if ((s = netboot_open(hostname, NULL, &server_addr, false)) < 0) {
    if (errno == ETIMEDOUT) {
      fprintf(stderr, "%s: lookup of %s timed out\n", appname, hostname);
    } else {
      fprintf(stderr, "%s: failed to connect to %s: %d\n", appname, hostname, errno);
    }
    return -1;
  }

  int ret = transfer_file(push, s, &server_addr, dst, src);
  close(s);
  return ret;
}
