// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <net/if.h>
#include <unistd.h>

#include "sockscripter.h"

// Bind all api abstractions to real posix calls.
class PosixCalls : public ApiAbstraction {
 public:
  int socket(int domain, int type, int protocol) override {
    int s = ::socket(domain, type, protocol);
#if defined(SO_NOSIGPIPE)
    // Since we don't install signal handlers, prevent signal triggering on
    // send which can cause SIGPIPE to be raised on unconnected stream sockets.
    if (s >= 0) {
      constexpr bool kTrue = true;
      ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &kTrue, sizeof(kTrue));
    }
#endif
    return s;
  }

  int close(int fd) override { return ::close(fd); }

  int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) override {
    return ::setsockopt(fd, level, optname, optval, optlen);
  }

  int getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen) override {
    return ::getsockopt(fd, level, optname, optval, optlen);
  }

  int bind(int fd, const struct sockaddr* addr, socklen_t len) override {
    return ::bind(fd, addr, len);
  }

  int shutdown(int fd, int how) override { return ::shutdown(fd, how); }

  int connect(int fd, const struct sockaddr* addr, socklen_t len) override {
    return ::connect(fd, addr, len);
  }

  int accept(int fd, struct sockaddr* addr, socklen_t* len) override {
    return ::accept(fd, addr, len);
  }

  int listen(int fd, int backlog) override { return ::listen(fd, backlog); }

  ssize_t send(int fd, const void* buf, size_t len, int flags) override {
#if defined(MSG_NOSIGNAL)
    // Since we don't install signal handlers, prevent signal triggering on
    // send which can cause SIGPIPE to be raised on unconnected stream sockets.
    flags |= MSG_NOSIGNAL;
#endif
    return ::send(fd, buf, len, flags);
  }

  ssize_t sendto(int fd, const void* buf, size_t buflen, int flags, const struct sockaddr* addr,
                 socklen_t addrlen) override {
#if defined(MSG_NOSIGNAL)
    // Since we don't install signal handlers, prevent signal triggering on
    // sendto which can cause SIGPIPE to be raised on unconnected stream
    // sockets.
    flags |= MSG_NOSIGNAL;
#endif
    return ::sendto(fd, buf, buflen, flags, addr, addrlen);
  }

  ssize_t recv(int fd, void* buf, size_t len, int flags) override {
    return ::recv(fd, buf, len, flags);
  }

  ssize_t recvfrom(int fd, void* buf, size_t buflen, int flags, struct sockaddr* addr,
                   socklen_t* addrlen) override {
    return ::recvfrom(fd, buf, buflen, flags, addr, addrlen);
  }

  int getsockname(int fd, struct sockaddr* addr, socklen_t* len) override {
    return ::getsockname(fd, addr, len);
  }

  int getpeername(int fd, struct sockaddr* addr, socklen_t* len) override {
    return ::getpeername(fd, addr, len);
  }

  unsigned int if_nametoindex(const char* ifname) override { return ::if_nametoindex(ifname); }
};

int main(int argc, char* const argv[]) {
  PosixCalls calls;
  SockScripter scripter(&calls);
  return scripter.Execute(argc, argv);
}
