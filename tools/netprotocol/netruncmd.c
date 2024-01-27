// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <lib/netboot/netboot.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netprotocol.h"

static const char* appname;

static void usage(void) {
  fprintf(stderr, "usage: %s [options] <hostname> <command>\n", appname);
  netboot_usage(false);
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

  if (argc < 2) {
    usage();
    return -1;
  }

  const char* hostname = argv[0];
  if (!strcmp(hostname, "-") || !strcmp(hostname, ":")) {
    hostname = "*";
  }

  char cmd[MAXSIZE];
  size_t cmd_len = 0;
  while (argc > 1) {
    size_t len = strlen(argv[1]);
    if (len > (MAXSIZE - cmd_len - 1)) {
      fprintf(stderr, "%s: command too long\n", appname);
      return -1;
    }
    memcpy(cmd + cmd_len, argv[1], len);
    cmd_len += len;
    cmd[cmd_len++] = ' ';
    argc--;
    argv++;
  }
  cmd[cmd_len - 1] = 0;

  int s;
  if ((s = netboot_open(hostname, NULL, NULL, true)) < 0) {
    if (errno == ETIMEDOUT) {
      fprintf(stderr, "%s: lookup timed out\n", appname);
    }
    return -1;
  }

  msg m;
  m.hdr.magic = NETBOOT_MAGIC;
  m.hdr.cookie = 0x11224455;
  m.hdr.cmd = NETBOOT_COMMAND_SHELL_CMD;
  m.hdr.arg = 0;
  memcpy(m.data, cmd, cmd_len);

  write(s, &m, sizeof(netboot_message_header_t) + cmd_len);
  close(s);

  return 0;
}
