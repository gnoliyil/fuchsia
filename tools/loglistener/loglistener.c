// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define _POSIX_C_SOURCE 200809L

// for SO_REUSEPORT
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <lib/netboot/netboot.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef _DARWIN_C_SOURCE
#define REUSEPORT SO_REUSEPORT
#else
#define REUSEPORT SO_REUSEADDR
#endif

static const char* appname;
static const char* nodename = "*";

int main(int argc, char** argv) {
  struct sockaddr_in6 addr;
  char tmp[INET6_ADDRSTRLEN];
  int s, n = 1;
  uint32_t last_seqno = 0;

  // Make stdout line buffered.
  setvbuf(stdout, NULL, _IOLBF, 0);

  appname = argv[0];

  if ((argc > 1) && (argv[1][0])) {
    nodename = argv[1];
  } else {
    char* envname = getenv("ZIRCON_NODENAME");
    if (envname) {
      nodename = envname;
    }
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(33337);

  s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) {
    fprintf(stderr, "%s: cannot create socket %d\n", appname, s);
    return -1;
  }
  setsockopt(s, SOL_SOCKET, REUSEPORT, &n, sizeof(n));
  if (bind(s, (void*)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, "%s: cannot bind to [%s]%d: %d: %s\n", appname,
            inet_ntop(AF_INET6, &addr.sin6_addr, tmp, sizeof(tmp)), ntohs(addr.sin6_port), errno,
            strerror(errno));
    if (errno == 98) {
      fprintf(stderr,
              "%s: another process is already using udp port %d. "
              "Try running `lsof -i udp:%d`.\n",
              appname, ntohs(addr.sin6_port), ntohs(addr.sin6_port));
    }
    return -1;
  }

  fprintf(stderr, "%s: listening on [%s]%d for device %s\n", appname,
          inet_ntop(AF_INET6, &addr.sin6_addr, tmp, sizeof(tmp)), ntohs(addr.sin6_port), nodename);
  for (;;) {
    struct sockaddr_in6 ra;
    socklen_t rlen;
    char buf[4096 + 1];
    netboot_debuglog_packet_t* pkt = (void*)buf;
    rlen = sizeof(ra);
    ssize_t r = recvfrom(s, buf, 4096, 0, (void*)&ra, &rlen);
    if (r < 0) {
      fprintf(stderr, "%s: socket read error %zd\n", appname, r);
      break;
    }
    if (r < 8)
      continue;
    if ((ra.sin6_addr.s6_addr[0] != 0xFE) || (ra.sin6_addr.s6_addr[1] != 0x80)) {
      fprintf(stderr, "ignoring non-link-local message\n");
      continue;
    }
    if (pkt->magic != NETBOOT_DEBUGLOG_MAGIC)
      continue;
    if (strncmp(nodename, "*", 1) && strncmp(pkt->nodename, nodename, sizeof(pkt->nodename)))
      continue;
    if (pkt->seqno != last_seqno) {
      buf[r] = 0;
      printf("%s", pkt->data);
      last_seqno = pkt->seqno;
    }
    sendto(s, buf, 8, 0, (struct sockaddr*)&ra, rlen);
  }

  return 0;
}
