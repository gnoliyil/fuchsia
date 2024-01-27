// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sockscripter.h"

#include <arpa/inet.h>
#include <getopt.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iomanip>
#include <optional>

#if PACKET_SOCKETS
#include <netpacket/packet.h>
#endif

#include "addr.h"
#include "log.h"
#include "src/lib/fxl/strings/string_number_conversions.h"
#include "util.h"

#define PRINT_SOCK_OPT_VAL(level, name) \
  LOG(INFO) << #level "=" << (level) << " " << #name << "=" << name

#define SET_SOCK_OPT_VAL(level, name, val)                                     \
  {                                                                            \
    if (api_->setsockopt(sockfd_, (level), (name), &(val), sizeof(val)) < 0) { \
      LOG(ERROR) << "Failed to set socket option " #level ":" #name "-"        \
                 << "[" << errno << "]" << strerror(errno);                    \
      return false;                                                            \
    }                                                                          \
    LOG(INFO) << "Set " #level ":" #name " = " << (int)(val);                  \
    return true;                                                               \
  }

#define LOG_SOCK_OPT_VAL(level, name, type_val)                             \
  {                                                                         \
    type_val opt = 0;                                                       \
    socklen_t opt_len = sizeof(opt);                                        \
    if (api_->getsockopt(sockfd_, (level), (name), &(opt), &opt_len) < 0) { \
      LOG(ERROR) << "Error-Getting " #level ":" #name "-"                   \
                 << "[" << errno << "]" << strerror(errno);                 \
      return false;                                                         \
    }                                                                       \
    LOG(INFO) << #level ":" #name " is set to " << (int)opt;                \
    return true;                                                            \
  }

bool TestRepeatCfg::Parse(const std::string& cmd) {
  command = "";
  repeat_count = 1;
  delay_ms = 0;
  if (cmd[0] != '{') {
    return false;
  }

  auto cmd_e = cmd.find_first_of('}');
  if (cmd_e == std::string::npos) {
    return false;
  }
  command = cmd.substr(1, cmd_e - 1);

  auto n_t_str = cmd.substr(cmd_e + 1);
  if (n_t_str.empty()) {
    return true;
  }

  auto n_num_str_i = n_t_str.find("N=");
  if (n_num_str_i != std::string::npos) {
    auto n_num_str = n_t_str.substr(n_num_str_i + 2);
    if (!str2int(n_num_str, &repeat_count)) {
      LOG(ERROR) << "Error parsing '" << cmd << "': in N=<n>, <n> is not a number!";
      return false;
    }
  }
  auto t_num_str_i = n_t_str.find("T=");
  if (t_num_str_i != std::string::npos) {
    auto t_num_str = n_t_str.substr(t_num_str_i + 2);
    if (!str2int(t_num_str, &delay_ms)) {
      LOG(ERROR) << "Error parsing '" << cmd << "': in T=<t>, <t> is not a number!";
    }
  }
  return true;
}

struct SocketType {
  const char* name;
  int domain;
  int type;
  std::optional<int> proto;
} socket_types[] = {{"udp", AF_INET, SOCK_DGRAM, IPPROTO_UDP},
                    {"udp6", AF_INET6, SOCK_DGRAM, IPPROTO_UDP},
                    {"icmp", AF_INET, SOCK_DGRAM, IPPROTO_ICMP},
                    {"icmp6", AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6},
                    {"tcp", AF_INET, SOCK_STREAM, IPPROTO_TCP},
                    {"tcp6", AF_INET6, SOCK_STREAM, IPPROTO_TCP},
                    {"raw", AF_INET, SOCK_RAW, std::nullopt},
                    {"raw6", AF_INET6, SOCK_RAW, std::nullopt},
#if PACKET_SOCKETS
                    {"packet", AF_PACKET, SOCK_DGRAM, 0},
                    {"packet-raw", AF_PACKET, SOCK_RAW, 0}
#endif
};

using SockScripterHandler = bool (SockScripter::*)(char*);
const struct Command {
  // Command name as used on the commandline.
  const char* name;
  // Argument description, nullptr if the command doesn't have an argument.
  const char* opt_arg_descr;
  // String displayed in the help text.
  const char* help_str;
  SockScripterHandler handler;
} kCommands[] = {
    {"close", nullptr, "close socket", &SockScripter::Close},
    {"bind", "<bind-ip>:<bind-port>", "bind to the given local address", &SockScripter::Bind},
    {"shutdown", "<how>", "shutdown socket for rd and/or wr", &SockScripter::Shutdown},
    {"bound", nullptr, "log bound-to-address", &SockScripter::LogBoundToAddress},
    {"log-peername", nullptr, "log peer name", &SockScripter::LogPeerAddress},
    {"connect", "<connect-ip>:<connect-port>", "connect to the given remote address",
     &SockScripter::Connect},
    {"disconnect", nullptr, "disconnect a connected socket", &SockScripter::Disconnect},
    {"set-broadcast", "{0|1}", "set SO_BROADCAST flag", &SockScripter::SetBroadcast},
    {"log-broadcast", nullptr, "log SO_BROADCAST option flag", &SockScripter::LogBroadcast},
    {"set-bindtodevice", "<if-name-string>", "set SO_BINDTODEVICE", &SockScripter::SetBindToDevice},
    {"log-bindtodevice", nullptr, "log SO_BINDTODEVICE option value",
     &SockScripter::LogBindToDevice},
    {"set-reuseaddr", "{0|1}", "set SO_REUSEADDR flag", &SockScripter::SetReuseaddr},
    {"log-reuseaddr", nullptr, "log SO_REUSEADDR option value", &SockScripter::LogReuseaddr},
    {"set-reuseport", "{0|1}", "set SO_REUSEPORT flag", &SockScripter::SetReuseport},
    {"log-reuseport", nullptr, "log SO_REUSEPORT option value", &SockScripter::LogReuseport},
    {"set-unicast-ttl", "<ttl-for-IP_TTL>", "set TTL for V4 unicast packets",
     &SockScripter::SetIpUnicastTTL},
    {"log-unicast-ttl", nullptr, "log IP_TTL option value", &SockScripter::LogIpUnicastTTL},
    {"set-unicast-hops", "<ttl-for-IPV6_UNICAST_HOPS>", "set hops for V6 unicast packets",
     &SockScripter::SetIpUnicastHops},
    {"log-unicast-hops", nullptr, "log IPV6_UNICAST_HOPS option value",
     &SockScripter::LogIpUnicastHops},
    {"set-mcast-ttl", "<ttl-for-IP_MULTICAST-TTL>", "set TTL for V4 mcast packets",
     &SockScripter::SetIpMcastTTL},
    {"log-mcast-ttl", nullptr, "log IP_MULTICAST_TTL option value", &SockScripter::LogIpMcastTTL},
    {"set-mcast-loop4", "{1|0}", "set IP_MULTICAST_LOOP flag", &SockScripter::SetIpMcastLoop4},
    {"log-mcast-loop4", nullptr, "log IP_MULTICAST_LOOP option value",
     &SockScripter::LogIpMcastLoop4},
    {"set-mcast-hops", "<ttl-for-IPV6_MULTICAST_HOPS>", "set hops for V6 mcast packets",
     &SockScripter::SetIpMcastHops},
    {"log-mcast-hops", nullptr, "log IPV6_MULTICAST_HOPS option value",
     &SockScripter::LogIpMcastHops},
    {"set-mcast-loop6", "{1|0}", "set IPV6_MULTICAST_LOOP flag", &SockScripter::SetIpMcastLoop6},
    {"log-mcast-loop6", nullptr, "log IPV6_MULTICAST_LOOP option value",
     &SockScripter::LogIpMcastLoop6},
    {"set-mcast-if4", "{<local-intf-Addr>}", "set IP_MULTICAST_IF for IPv4 mcast",
     &SockScripter::SetIpMcastIf4},
    {"log-mcast-if4", nullptr, "log IP_MULTICAST_IF option value", &SockScripter::LogIpMcastIf4},
    {"set-mcast-if6", "<local-intf-id>", "set IP_MULTICAST_IF6 for IPv6 mcast",
     &SockScripter::SetIpMcastIf6},
    {"log-mcast-if6", nullptr, "log IP_MULTICAST_IF6 option value", &SockScripter::LogIpMcastIf6},

    {"set-ipv6-only", "{0|1}", "set IPV6_V6ONLY flag", &SockScripter::SetIpV6Only},
    {"log-ipv6-only", nullptr, "log IPV6_V6ONLY option value", &SockScripter::LogIpV6Only},

    {"join4", "<mcast-ip>-<local-intf-Addr>",
     "join IPv4 mcast group (IP_ADD_MEMBERSHIP) on local interface", &SockScripter::Join4},
    {"drop4", "<mcast-ip>-<local-intf-Addr>",
     "drop IPv4 mcast group (IP_DROP_MEMBERSHIP) on local interface", &SockScripter::Drop4},
    {"block4", "<mcast-ip>-<source-ip>-<local-intf-ip>",
     "block IPv4 packets from source sent to mcast group on local interface (IP_BLOCK_SOURCE)",
     &SockScripter::Block4},
    {"join6", "<mcast-ip>-<local-intf-id>",
     "join IPv6 mcast group (IPV6_ADD_MEMBERSHIP/IPV6_JOIN_GROUP) on local interface",
     &SockScripter::Join6},
    {"drop6", "<mcast-ip>-<local-intf-id>",
     "drop IPv6 mcast group (IPV6_DROP_MEMBERSHIP/IPV6_LEAVE_GROUP) on local interface",
     &SockScripter::Drop6},
    {"listen", "<backlog>", "listen on TCP socket with accept backlog length",
     &SockScripter::Listen},
    {"accept", nullptr, "accept on TCP socket", &SockScripter::Accept},
    {"close-listener", nullptr, "close listening TCP socket", &SockScripter::CloseListener},
    {"sendto", "<send-to-ip>:<send-to-port>", "sendto(send_buf, ip, port)", &SockScripter::SendTo},
    {"send", nullptr, "send send_buf on a connected socket", &SockScripter::Send},
    {"recvfrom", nullptr, "recvfrom()", &SockScripter::RecvFrom},
    {"recvfrom-ping", nullptr, "recvfrom() and ping the packet back to the sender",
     &SockScripter::RecvFromPing},
    {"recv", nullptr, "recv() on a connected TCP socket", &SockScripter::Recv},
    {"recv-ping", nullptr,
     "recv() and ping back the packet on a connected TCP "
     "socket",
     &SockScripter::RecvPing},
    {"set-send-buf-hex", "\"xx xx ..\" ", "set send-buffer with hex values",
     &SockScripter::SetSendBufHex},
    {"set-send-buf-text", "\"<string>\" ", "set send-buffer with text chars",
     &SockScripter::SetSendBufText},
    {"sleep", "<sleep-secs>", "sleeps", &SockScripter::Sleep},
#if PACKET_SOCKETS
    {"packet-bind", "<protocol>:<if-name-string>",
     "bind a packet socket to the given protocol and interface", &SockScripter::PacketBind},
#endif
};

void print_socket_types() {
  for (const auto& socket_type : socket_types) {
    std::cout << socket_type.name << " ";
  }
  std::cout << std::endl;
}

bool check_socket_type_has_proto(const char* stype) {
  for (const auto& socket_type : socket_types) {
    if (strcmp(stype, socket_type.name) == 0) {
      return socket_type.proto.has_value();
    }
  }
  return false;
}

int print_commands_list() {
  for (const auto& cmd : kCommands) {
    std::cout << cmd.name << " ";
  }
  std::cout << std::endl;
  return 0;
}

int check_command_has_args(const char* command) {
  for (const auto& cmd : kCommands) {
    if (strcmp(command, cmd.name) == 0) {
      if (cmd.opt_arg_descr) {
        return 0;
      }
      break;
    }
  }
  return 1;
}

int usage(const char* name) {
  std::stringstream socket_types_str;
  for (const auto& socket_type : socket_types) {
    socket_types_str << "    " << socket_type.name << " ";
    if (!socket_type.proto.has_value()) {
      socket_types_str << "<protocol>";
    }
    socket_types_str << " : open new " << GetDomainName(socket_type.domain) << " "
                     << GetTypeName(socket_type.type);
    if (socket_type.proto.has_value()) {
      socket_types_str << " " << GetProtoName(socket_type.proto.value());
    }
    socket_types_str << " socket" << std::endl;
  }

  std::stringstream cmds_str;
  for (const auto& cmd : kCommands) {
    cmds_str << "    " << cmd.name << " " << (cmd.opt_arg_descr ? cmd.opt_arg_descr : "") << " : "
             << cmd.help_str << "\n";
  }

  fprintf(stderr,
          "\nUsage: %s [-h] [-s] [-p <proto>] [-c] [-a <cmd>] "
          " {socket-type} {socket-cmds}\n"
          "    -h : this help message\n"
          "    -s : prints available socket-types (for bash completion)\n"
          "    -p <proto> : returns 0 if the given socket-type has a parameter\n"
          "    -c : prints available commands (for bash completion)\n"
          "    -a <cmd> : returns 0 if the given command has a parameter\n"
          "    -C : continue after socket operation errors\n"
          "\n"
          "  socket-type is one of the following:\n%s\n"
          "  socket-cmd is one of the following:\n%s\n"
          "  <local-intf-id> is an integer prefixed by '%%', e.g. '%%1'\n"
          "  <local-intf-Addr> is of the format [<IP>][<ID>], e.g. '192.168.1.166',"
          "'192.168.1.166%%2', '%%2'\n"
          "  <backlog> is a signed integer, e.g. '100', '0', '-5'\n\n"
          "  A command can be repeated by wrapping it in the following structure:\n"
          "    {<cmd>}[N=<n>][T=<t>]\n"
          "        <n> is the number of repeats (default: n=1)\n"
          "        <t> is the delay in ms between commands (default: t=1000)\n\n"
          "  Examples:\n    ------\n"
          "    Joining the multicast group 224.0.0.120 on local-IP "
          "192.168.1.99, bind to 224.0.0.120:2000,\n"
          "    and receive a two packets.\n"
          "       %s udp join4 224.0.0.120-192.168.1.99 bind 224.0.0.120:2000 "
          "{recvfrom}N=2\n"
          "\n"
          "    Send two packets, 50ms apart, to the multicast group 224.0.0.120:2000 "
          "from the local interface 192.168.1.166\n"
          "      %s udp set-mcast-if4 192.168.1.166 {sendto}N=2T=50 224.0.0.120:2000\n"
          "\n\n",
          name, socket_types_str.str().c_str(), cmds_str.str().c_str(), name, name);
  return 99;
}

struct Escaped {
  explicit Escaped(const std::string_view contents) : contents(contents) {}
  std::string_view contents;
};

std::ostream& operator<<(std::ostream& out, const Escaped& logged) {
  for (unsigned char c : logged.contents) {
    if (std::isprint(c)) {
      out << c;
    } else {
      out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<uint16_t>(c);
    }
  }
  return out;
}

int SockScripter::Execute(int argc, char* const argv[]) {
  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, "+Chsp:ca:")) != -1) {
    switch (opt) {
      case 's':
        print_socket_types();
        return 0;
      case 'p':
        return check_socket_type_has_proto(optarg) ? 0 : 1;
      case 'c':
        return print_commands_list();
      case 'C':
        continue_after_error_ = true;
        break;
      case 'a':
        return check_command_has_args(optarg);
      case 'h':
      default:
        return usage(argv[0]);
    }
  }

  if (optind >= argc) {
    return usage(argv[0]);
  }

  {
    bool found = false;
    for (const auto& socket_type : socket_types) {
      if (strcmp(argv[optind], socket_type.name) == 0) {
        found = true;
        int proto;
        if (socket_type.proto.has_value()) {
          proto = socket_type.proto.value();
        } else {
          optind++;
          if (optind >= argc) {
            fprintf(stderr, "Error-Need protocol# for RAW socket!\n\n");
            return -1;
          }
          if (!str2int(argv[optind], &proto)) {
            fprintf(stderr, "Error-Invalid protocol# (%s) for RAW socket!\n\n", argv[optind]);
            return -1;
          }
        }
        if (!Open(socket_type.domain, socket_type.type, proto)) {
          return -1;
        }
        break;
      }
    }
    if (!found) {
      fprintf(stderr, "Error-first parameter (%s) needs to be socket type:", argv[optind]);
      print_socket_types();
      return -1;
    }
  }

  optind++;
  while (optind < argc) {
    TestRepeatCfg cfg;
    const char* cmd_arg;
    if (argv[optind][0] == '{') {
      if (!cfg.Parse(argv[optind])) {
        return -1;
      }
      cmd_arg = cfg.command.c_str();
    } else {
      cmd_arg = argv[optind];
    }

    {
      bool found = false;
      for (const auto& cmd : kCommands) {
        if (strcmp(cmd_arg, cmd.name) == 0) {
          found = true;
          optind++;
          char* arg = nullptr;
          if (cmd.opt_arg_descr) {
            if (optind < argc) {
              arg = argv[optind];
              optind++;
            } else {
              fprintf(stderr, "Missing argument %s for %s!\n\n", cmd.opt_arg_descr, cmd.name);
              return -1;
            }
          }
          for (int i = 0; i < cfg.repeat_count; i++) {
            auto handler = cmd.handler;
            if (!(this->*(handler))(arg) && !continue_after_error_) {
              return -1;
            }
            usleep(1000 * cfg.delay_ms);
          }
          break;
        }
      }
      if (!found) {
        fprintf(stderr, "Error: Cannot find command '%s'!\n\n", argv[optind]);
        return -1;
      }
    }
  }
  return 0;
}

bool SockScripter::Open(int domain, int type, int proto) {
  sockfd_ = api_->socket(domain, type, proto);
  if (sockfd_ < 0) {
    LOG(ERROR) << "Error-Opening " << GetDomainName(domain) << "-" << GetTypeName(type)
               << " socket "
               << "(proto:" << GetProtoName(proto) << ") "
               << "failed-[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "Opened " << GetDomainName(domain) << "-" << GetTypeName(type) << " socket "
            << "(proto:" << GetProtoName(proto) << ") fd:" << sockfd_;
  return true;
}

bool SockScripter::Close(char* arg) {
  if (sockfd_ < 0) {
    LOG(ERROR) << "No socket to close";
    return false;
  }

  if (auto err = api_->close(sockfd_); err != 0) {
    LOG(ERROR) << "Failed to close fd=" << sockfd_ << ": " << std::strerror(err);
    return false;
  }

  LOG(INFO) << "Closed socket-fd:" << sockfd_;
  sockfd_ = -1;

  if (tcp_listen_socket_fd_ >= 0) {
    sockfd_ = tcp_listen_socket_fd_;
    tcp_listen_socket_fd_ = -1;
  }
  return true;
}

bool SockScripter::CloseListener(char* arg) {
  if (tcp_listen_socket_fd_ < 0) {
    LOG(ERROR) << "No listening TCP socket to close";
    return false;
  }

  if (auto err = api_->close(tcp_listen_socket_fd_); err != 0) {
    LOG(ERROR) << "Failed to close fd=" << tcp_listen_socket_fd_ << ": " << std::strerror(err);
    return false;
  }
  LOG(INFO) << "Closed socket-fd:" << tcp_listen_socket_fd_;
  tcp_listen_socket_fd_ = -1;
  return true;
}

bool SockScripter::SetBroadcast(char* arg) {
  int flag;
  if (!getFlagInt(arg, &flag)) {
    LOG(ERROR) << "Error: Invalid broadcast flag='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(SOL_SOCKET, SO_BROADCAST, flag)
}

bool SockScripter::LogBroadcast(char* arg) { LOG_SOCK_OPT_VAL(SOL_SOCKET, SO_BROADCAST, int) }

bool SockScripter::SetReuseaddr(char* arg) {
  int flag;
  if (!getFlagInt(arg, &flag)) {
    LOG(ERROR) << "Error: Invalid reuseaddr flag='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(SOL_SOCKET, SO_REUSEADDR, flag)
}

bool SockScripter::SetReuseport(char* arg) {
  int flag;
  if (!getFlagInt(arg, &flag)) {
    LOG(ERROR) << "Error: Invalid reuseport flag='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(SOL_SOCKET, SO_REUSEPORT, flag)
}

bool SockScripter::LogReuseaddr(char* arg) { LOG_SOCK_OPT_VAL(SOL_SOCKET, SO_REUSEADDR, int) }

bool SockScripter::LogReuseport(char* arg) { LOG_SOCK_OPT_VAL(SOL_SOCKET, SO_REUSEPORT, int) }

bool SockScripter::SetIpUnicastTTL(char* arg) {
  int ttl;
  if (!str2int(arg, &ttl) || ttl < 0) {
    LOG(ERROR) << "Error: Invalid unicast TTL='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(IPPROTO_IP, IP_TTL, ttl)
}

bool SockScripter::SetIpUnicastHops(char* arg) {
  int hops;
  if (!str2int(arg, &hops) || hops < 0) {
    LOG(ERROR) << "Error: Invalid unicast hops='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(IPPROTO_IPV6, IPV6_UNICAST_HOPS, hops)
}

bool SockScripter::LogIpUnicastHops(char* arg) {
  LOG_SOCK_OPT_VAL(IPPROTO_IPV6, IPV6_UNICAST_HOPS, int)
}

bool SockScripter::SetIpMcastTTL(char* arg) {
  int ttl;
  if (!str2int(arg, &ttl) || ttl < 0) {
    LOG(ERROR) << "Error: Invalid mcast TTL='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(IPPROTO_IP, IP_MULTICAST_TTL, ttl)
}

bool SockScripter::LogIpMcastTTL(char* arg) { LOG_SOCK_OPT_VAL(IPPROTO_IP, IP_MULTICAST_TTL, int) }

bool SockScripter::LogIpUnicastTTL(char* arg) { LOG_SOCK_OPT_VAL(IPPROTO_IP, IP_TTL, int) }

bool SockScripter::SetIpMcastLoop4(char* arg) {
  int flag;
  if (!getFlagInt(arg, &flag)) {
    LOG(ERROR) << "Error: Invalid loop4 flag='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(IPPROTO_IP, IP_MULTICAST_LOOP, flag)
}

bool SockScripter::LogIpMcastLoop4(char* arg) {
  LOG_SOCK_OPT_VAL(IPPROTO_IP, IP_MULTICAST_LOOP, int)
}

bool SockScripter::SetIpMcastHops(char* arg) {
  int hops;
  if (!str2int(arg, &hops) || hops < 0) {
    LOG(ERROR) << "Error: Invalid mcast hops='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(IPPROTO_IPV6, IPV6_MULTICAST_HOPS, hops)
}

bool SockScripter::LogIpMcastHops(char* arg) {
  LOG_SOCK_OPT_VAL(IPPROTO_IPV6, IPV6_MULTICAST_HOPS, int)
}

bool SockScripter::SetIpV6Only(char* arg) {
  int flag;
  if (!getFlagInt(arg, &flag)) {
    LOG(ERROR) << "Error: Invalid ipv6-only flag='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(IPPROTO_IPV6, IPV6_V6ONLY, flag)
}

bool SockScripter::LogIpV6Only(char* arg) { LOG_SOCK_OPT_VAL(IPPROTO_IPV6, IPV6_V6ONLY, int) }

bool SockScripter::SetIpMcastLoop6(char* arg) {
  int flag;
  if (!getFlagInt(arg, &flag)) {
    LOG(ERROR) << "Error: Invalid loop6 flag='" << arg << "'!";
    return false;
  }
  SET_SOCK_OPT_VAL(IPPROTO_IPV6, IPV6_MULTICAST_LOOP, flag)
}

bool SockScripter::LogIpMcastLoop6(char* arg) {
  LOG_SOCK_OPT_VAL(IPPROTO_IPV6, IPV6_MULTICAST_LOOP, int)
}

bool SockScripter::SetBindToDevice(char* arg) {
#ifdef SO_BINDTODEVICE
  if (api_->setsockopt(sockfd_, SOL_SOCKET, SO_BINDTODEVICE, arg,
                       static_cast<socklen_t>(strlen(arg))) < 0) {
    LOG(ERROR) << "Error-Setting SO_BINDTODEVICE failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "Set SO_BINDTODEVICE to " << arg;
  return true;
#else
  LOG(ERROR) << "SO_BINDTODEVICE not defined in this platform";
  return false;
#endif
}

bool SockScripter::LogBindToDevice(char* arg) {
#ifdef SO_BINDTODEVICE
  char name[IFNAMSIZ] = {};
  socklen_t name_len = sizeof(name);
  if (api_->getsockopt(sockfd_, SOL_SOCKET, SO_BINDTODEVICE, name, &name_len) < 0) {
    LOG(ERROR) << "Error-Getting SO_BINDTODEVICE failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "SO_BINDTODEVICE is set to " << name;
  return true;
#else
  LOG(ERROR) << "SO_BINDTODEVICE not defined in this platform";
  return false;
#endif
}

bool SockScripter::SetIpMcastIf4(char* arg) {
  auto [if_addr, if_id] = ParseIpv4WithScope(arg);
  if (!if_addr.has_value() && !if_id.has_value()) {
    LOG(ERROR) << "Error-No IPv4 address or local interface id given for "
               << "IP_MULTICAST_IF";
    return false;
  }

  struct ip_mreqn mreq = {};
  // mreq.imr_multiaddr is not set
  if (if_addr.has_value()) {
    mreq.imr_address = if_addr.value();
  }
  if (if_id.has_value()) {
    mreq.imr_ifindex = if_id.value();
  }
  if (api_->setsockopt(sockfd_, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq)) < 0) {
    LOG(ERROR) << "Error-Setting IP_MULTICAST_IF failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  std::stringstream o;
  if (if_addr.has_value()) {
    char buf[INET_ADDRSTRLEN] = {};
    o << inet_ntop(AF_INET, &if_addr.value(), buf, sizeof(buf));
  }
  if (if_id.has_value()) {
    o << '%' << if_id.value();
  }
  LOG(INFO) << "Set IP_MULTICAST_IF to " << o.str();
  return true;
}

bool SockScripter::LogIpMcastIf4(char* arg) {
  struct in_addr addr;
  memset(&addr, 0, sizeof(addr));
  socklen_t addr_len = sizeof(addr);
  if (api_->getsockopt(sockfd_, IPPROTO_IP, IP_MULTICAST_IF, &addr, &addr_len) < 0) {
    LOG(ERROR) << "Error-Getting IP_MULTICAST_IF failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  char buf[INET_ADDRSTRLEN] = {};
  LOG(INFO) << "IP_MULTICAST_IF is set to " << inet_ntop(AF_INET, &addr, buf, sizeof(buf));
  return true;
}

bool SockScripter::SetIpMcastIf6(char* arg) {
  int id;
  if (!str2int(arg, &id) || id < 0) {
    LOG(ERROR) << "Error-Invalid local interface ID given for IPV6_MULTICAST_IF: " << arg;
    return false;
  }

  if (api_->setsockopt(sockfd_, IPPROTO_IPV6, IPV6_MULTICAST_IF, &id, sizeof(id)) < 0) {
    LOG(ERROR) << "Error-Setting IPV6_MULTICAST_IF to " << id << " failed-[" << errno << "]"
               << strerror(errno);
    return false;
  }
  LOG(INFO) << "Set IPV6_MULTICAST_IF to " << id;
  return true;
}

bool SockScripter::LogIpMcastIf6(char* arg) {
  int id = -1;
  socklen_t id_len = sizeof(id);
  if (api_->getsockopt(sockfd_, IPPROTO_IPV6, IPV6_MULTICAST_IF, &id, &id_len) < 0) {
    LOG(ERROR) << "Error-Getting IPV6_MULTICAST_IF failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "IPV6_MULTICAST_IF is set to " << id;
  return true;
}

bool SockScripter::LogBoundToAddress(char* arg) {
  sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);

  if (api_->getsockname(sockfd_, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
    LOG(ERROR) << "Error-Calling getsockname failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "Bound to " << Format(addr);
  return true;
}

bool SockScripter::LogPeerAddress(char* arg) {
  sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);

  if (api_->getpeername(sockfd_, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
    LOG(ERROR) << "Error-Calling getpeername failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "Connected to " << Format(addr);
  return true;
}

bool SockScripter::Bind(char* arg) {
  std::optional addr = Parse(arg);
  if (!addr.has_value()) {
    return false;
  }

  LOG(INFO) << "Bind(fd:" << sockfd_ << ") to " << Format(addr.value());

  socklen_t addr_len = sizeof(addr.value());

  if (api_->bind(sockfd_, reinterpret_cast<sockaddr*>(&addr.value()), addr_len) < 0) {
    LOG(ERROR) << "Error-Bind(fd:" << sockfd_ << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  return LogBoundToAddress(nullptr);
}

#if PACKET_SOCKETS
bool SockScripter::PacketBind(char* arg) {
  std::string argstr = arg;
  size_t col_pos = argstr.find_first_of(':');
  if (col_pos == std::string::npos) {
    LOG(ERROR) << "Error-Cannot parse packet-bind arg='" << argstr
               << "' for <protocol>:<ifname> - missing separating colon ':'!";
    return false;
  }

  std::string protocol_str = argstr.substr(0, col_pos);
  std::string ifname_str = argstr.substr(col_pos + 1);

  int protocol;
  if (!str2int(protocol_str, &protocol)) {
    LOG(ERROR) << "Error-Cannot parse protocol number='" << protocol_str << "'!";
    return false;
  }

  unsigned int if_index = api_->if_nametoindex(ifname_str.c_str());
  if (!if_index) {
    LOG(ERROR) << "Error-if_nametoindex(" << ifname_str << ") failed -"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }

  const struct sockaddr_ll sll = {
      .sll_family = AF_PACKET,
      .sll_protocol = htons(static_cast<uint16_t>(protocol)),
      .sll_ifindex = static_cast<int>(if_index),
  };

  LOG(INFO) << "PacketBind(fd:" << sockfd_ << ", protocol:" << protocol << ", if_index:" << if_index
            << ")";
  if (api_->bind(sockfd_, reinterpret_cast<const struct sockaddr*>(&sll), sizeof(sll)) < 0) {
    LOG(ERROR) << "Error-PacketBind(fd:" << sockfd_ << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }

  return true;
}
#endif

bool SockScripter::Shutdown(char* arg) {
  std::string howStr(arg);

  bool read = false;
  if (howStr.find("rd") != std::string::npos) {
    read = true;
  }
  bool write = false;
  if (howStr.find("wr") != std::string::npos) {
    write = true;
  }

  int how;
  if (read && write) {
    how = SHUT_RDWR;
    howStr = "SHUT_RDWR";
  } else if (read) {
    how = SHUT_RD;
    howStr = "SHUT_RD";
  } else if (write) {
    how = SHUT_WR;
    howStr = "SHUT_WR";
  } else {
    LOG(ERROR) << "Error-Cannot parse how='" << arg << "'";
    return false;
  }

  LOG(INFO) << "Shutdown(fd:" << sockfd_ << ", " << howStr << ")";

  if (api_->shutdown(sockfd_, how) < 0) {
    LOG(ERROR) << "Error-Shutdown(fd:" << sockfd_ << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  return LogBoundToAddress(nullptr);
}

bool SockScripter::Connect(char* arg) {
  std::optional addr = Parse(arg);
  if (!addr.has_value()) {
    return false;
  }

  LOG(INFO) << "Connect(fd:" << sockfd_ << ") to " << Format(addr.value());

  socklen_t addr_len = sizeof(addr.value());

  if (api_->connect(sockfd_, reinterpret_cast<sockaddr*>(&addr.value()), addr_len) < 0) {
    LOG(ERROR) << "Error-Connect(fd:" << sockfd_ << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  return LogBoundToAddress(nullptr);
}

bool SockScripter::Disconnect(char* arg) {
  LOG(INFO) << "Disconnect(fd:" << sockfd_ << ")";

  struct sockaddr_storage addr = {};
  addr.ss_family = AF_UNSPEC;
  if (api_->connect(sockfd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr.ss_family)) < 0) {
    LOG(ERROR) << "Error-Disconnect(fd:" << sockfd_ << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  return LogBoundToAddress(nullptr);
}

bool SockScripter::JoinOrDrop4(const char* func, char* arg, int optname, const char* optname_str) {
  if (arg == nullptr) {
    LOG(ERROR) << "Called " << func << " with arg == nullptr!";
    return false;
  }

  std::string saved_arg(arg);
  char* mcast_ip_str = strtok(arg, "-");
  char* ip_id_str = strtok(nullptr, "-");

  if (mcast_ip_str == nullptr || ip_id_str == nullptr) {
    LOG(ERROR) << "Error-" << func << " got arg='" << saved_arg << "', "
               << "needs to be <mcast-ip>-{<local-intf-ip>|<local-intf-id>}";
    return false;
  }

  std::optional mcast_addr = Parse(mcast_ip_str, std::nullopt);
  if (!mcast_addr.has_value() || mcast_addr.value().ss_family != AF_INET) {
    LOG(ERROR) << "Error-" << func << " got invalid mcast address='" << mcast_ip_str << "'!";
    return false;
  }

  auto [if_addr, if_id] = ParseIpv4WithScope(ip_id_str);
  if (!if_addr.has_value() && !if_id.has_value()) {
    LOG(ERROR) << "Error-" << func << " got invalid interface='" << ip_id_str << "'!";
    return false;
  }

  struct ip_mreqn mreq = {
      .imr_multiaddr = reinterpret_cast<sockaddr_in*>(&mcast_addr.value())->sin_addr,
  };
  if (if_addr.has_value()) {
    mreq.imr_address = if_addr.value();
  }
  if (if_id.has_value()) {
    mreq.imr_ifindex = if_id.value();
  }

  if (api_->setsockopt(sockfd_, IPPROTO_IP, optname, &mreq, sizeof(mreq)) < 0) {
    LOG(ERROR) << "Error-Setting " << optname_str << " failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  std::stringstream o;
  if (if_addr.has_value()) {
    char buf[INET_ADDRSTRLEN] = {};
    o << inet_ntop(AF_INET, &if_addr.value(), buf, sizeof(buf));
  }
  if (if_id.has_value()) {
    o << '%' << if_id.value();
  }
  LOG(INFO) << "Set " << optname_str << " for " << Format(mcast_addr.value()) << " on " << o.str()
            << ".";

  return true;
}

bool SockScripter::Join4(char* arg) {
  return JoinOrDrop4(__FUNCTION__, arg, IP_ADD_MEMBERSHIP, "IP_ADD_MEMBERSHIP");
}

bool SockScripter::Drop4(char* arg) {
  return JoinOrDrop4(__FUNCTION__, arg, IP_DROP_MEMBERSHIP, "IP_DROP_MEMBERSHIP");
}

bool SockScripter::Block4(char* arg) {
  std::string saved_arg(arg);
  char* mcast_ip_str = strtok(arg, "-");
  char* source_ip_str = strtok(nullptr, "-");
  char* if_ip_str = strtok(nullptr, "-");

  if (mcast_ip_str == nullptr || source_ip_str == nullptr || if_ip_str == nullptr) {
    LOG(ERROR) << "Error-Block4 got arg='" << saved_arg << "', "
               << "needs to be <mcast-ip>-<source-ip>-<local-intf-id>";
    return false;
  }

  std::optional mcast_addr = Parse(mcast_ip_str, std::nullopt);
  if (!mcast_addr.has_value() || mcast_addr.value().ss_family != AF_INET) {
    LOG(ERROR) << "Error-Block4 got invalid mcast address='" << mcast_ip_str << "'!";
    return false;
  }

  std::optional source_addr = Parse(source_ip_str, std::nullopt);
  if (!source_addr.has_value() || source_addr.value().ss_family != AF_INET) {
    LOG(ERROR) << "Error-Block4 got invalid source address='" << source_ip_str << "'!";
    return false;
  }

  std::optional if_addr = Parse(if_ip_str, std::nullopt);
  if (!if_addr.has_value() || if_addr.value().ss_family != AF_INET) {
    LOG(ERROR) << "Error-Block4 got invalid interface address='" << if_ip_str << "'!";
    return false;
  }

  struct ip_mreq_source mreq;
  mreq.imr_multiaddr = reinterpret_cast<sockaddr_in*>(&mcast_addr.value())->sin_addr;
  mreq.imr_interface = reinterpret_cast<sockaddr_in*>(&if_addr.value())->sin_addr;
  mreq.imr_sourceaddr = reinterpret_cast<sockaddr_in*>(&source_addr.value())->sin_addr;

  if (api_->setsockopt(sockfd_, IPPROTO_IP, IP_BLOCK_SOURCE, &mreq, sizeof(mreq)) < 0) {
    LOG(ERROR) << "Error-Setting IP_BLOCK_SOURCE failed-[" << errno << "]" << strerror(errno);
    return false;
  }

  LOG(INFO) << "Set IP_BLOCK_SOURCE for " << Format(mcast_addr.value()) << " source "
            << Format(source_addr.value()) << " iface " << Format(if_addr.value()) << ".";

  return true;
}

bool SockScripter::JoinOrDrop6(const char* func, char* arg, int optname, const char* optname_str) {
  if (arg == nullptr) {
    LOG(ERROR) << "Called " << func << " with arg == nullptr!";
    return false;
  }

  std::string saved_arg(arg);
  char* mcast_ip_str = strtok(arg, "-");
  char* id_str = strtok(nullptr, "-");
  if (mcast_ip_str == nullptr || id_str == nullptr) {
    LOG(ERROR) << "Error-" << func << " got arg='" << saved_arg << "'', "
               << "needs to be '<mcast-ip>-<local-intf-id>'";
    return false;
  }

  std::optional mcast_addr = Parse(mcast_ip_str, std::nullopt);
  if (!mcast_addr.has_value() || mcast_addr.value().ss_family != AF_INET6) {
    LOG(ERROR) << "Error-" << func << " got invalid mcast address='" << mcast_ip_str << "'!";
    return false;
  }

  int if_id;
  if (!str2int(id_str, &if_id) || if_id < 0) {
    LOG(ERROR) << "Error-" << func << " got invalid interface='" << id_str << "'!";
    return false;
  }

  struct ipv6_mreq mreq = {
      .ipv6mr_multiaddr = reinterpret_cast<sockaddr_in6*>(&mcast_addr.value())->sin6_addr,
      .ipv6mr_interface = static_cast<unsigned int>(if_id),
  };

  if (api_->setsockopt(sockfd_, IPPROTO_IPV6, optname, &mreq, sizeof(mreq)) < 0) {
    LOG(ERROR) << "Error-Setting " << optname_str << " failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "Set " << optname_str << " for " << Format(mcast_addr.value()) << " on " << if_id
            << ".";

  return true;
}

bool SockScripter::Join6(char* arg) {
  // Note that IPV6_JOIN_GROUP and IPV6_ADD_MEMBERSHIP map to the same optname integer code, so we
  // can pick either one. We go with IPV6_JOIN_GROUP due to wider support.
  return JoinOrDrop6(__FUNCTION__, arg, IPV6_JOIN_GROUP, "IPV6_JOIN_GROUP");
}

bool SockScripter::Drop6(char* arg) {
  return JoinOrDrop6(__FUNCTION__, arg, IPV6_LEAVE_GROUP, "IPV6_LEAVE_GROUP");
}

bool SockScripter::Listen(char* arg) {
  int backlog = 0;
  if (!str2int(arg, &backlog)) {
    LOG(ERROR) << "Error-listen got invalid backlog size=" << arg;
    return false;
  }
  if (api_->listen(sockfd_, backlog) < 0) {
    LOG(ERROR) << "Error-listen(fd:" << sockfd_ << ", " << backlog << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  return true;
}

bool SockScripter::Accept(char* arg) {
  sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);

  LOG(INFO) << "Accepting(fd:" << sockfd_ << ")...";

  int fd = api_->accept(sockfd_, reinterpret_cast<sockaddr*>(&addr), &addr_len);
  if (fd < 0) {
    LOG(ERROR) << "Error-Accept(fd:" << sockfd_ << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }

  LOG(INFO) << "  accepted(fd:" << sockfd_ << ") new connection "
            << "(fd:" << fd << ") from " << Format(addr);

  // Switch to the newly created connection, remember the previous one.
  tcp_listen_socket_fd_ = sockfd_;
  sockfd_ = fd;
  return true;
}

bool SockScripter::SendTo(char* arg) {
  std::optional addr = Parse(arg);
  if (!addr.has_value()) {
    return false;
  }

  auto snd_buf = snd_buf_gen_.GetSndStr();

  LOG(INFO) << "Sending [" << snd_buf.length() << "]='" << Escaped(snd_buf) << "' on fd:" << sockfd_
            << " to " << Format(addr.value());

  ssize_t sent = api_->sendto(sockfd_, snd_buf.c_str(), snd_buf.length(), snd_flags_,
                              reinterpret_cast<sockaddr*>(&addr.value()), sizeof(addr.value()));
  if (sent < 0) {
    LOG(ERROR) << "Error-sendto(fd:" << sockfd_ << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "Sent [" << sent << "] on fd:" << sockfd_;
  return true;
}

bool SockScripter::Send(char* arg) {
  auto snd_buf = snd_buf_gen_.GetSndStr();

  LOG(INFO) << "Sending [" << snd_buf.length() << "]='" << Escaped(snd_buf)
            << "' on fd:" << sockfd_;

  ssize_t sent = api_->send(sockfd_, snd_buf.c_str(), snd_buf.length(), snd_flags_);
  if (sent < 0) {
    LOG(ERROR) << "Error-send(fd:" << sockfd_ << ") failed-"
               << "[" << errno << "]" << strerror(errno);
    return false;
  }
  LOG(INFO) << "Sent [" << sent << "] on fd:" << sockfd_;
  return true;
}

bool SockScripter::RecvFromInternal(bool ping) {
  sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);

  LOG(INFO) << "RecvFrom(fd:" << sockfd_ << ")...";

  memset(recv_buf_, 0, sizeof(recv_buf_));
  ssize_t recvd = api_->recvfrom(sockfd_, recv_buf_, sizeof(recv_buf_) - 1, recv_flags_,
                                 reinterpret_cast<sockaddr*>(&addr), &addr_len);
  if (recvd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      LOG(INFO) << "  returned EAGAIN or EWOULDBLOCK!";
    } else {
      LOG(ERROR) << "  Error-recvfrom(fd:" << sockfd_ << ") failed-"
                 << "[" << errno << "]" << strerror(errno);
    }
    return false;
  }

  std::string_view received(&recv_buf_[0], recvd);
  LOG(INFO) << "  received(fd:" << sockfd_ << ") [" << recvd << "]'" << Escaped(received) << "' "
            << "from " << Format(addr);
  if (ping) {
    if (api_->sendto(sockfd_, recv_buf_, recvd, snd_flags_, reinterpret_cast<sockaddr*>(&addr),
                     addr_len) < 0) {
      LOG(ERROR) << "Error-sendto(fd:" << sockfd_ << ") failed-"
                 << "[" << errno << "]" << strerror(errno);
      return false;
    }
  }
  return true;
}

bool SockScripter::RecvFrom(char* arg) { return RecvFromInternal(false /* ping */); }

bool SockScripter::RecvFromPing(char* arg) { return RecvFromInternal(true /* ping */); }

int SockScripter::RecvInternal(bool ping) {
  LOG(INFO) << "Recv(fd:" << sockfd_ << ") ...";
  LogBoundToAddress(nullptr);
  LogPeerAddress(nullptr);

  memset(recv_buf_, 0, sizeof(recv_buf_));
  ssize_t recvd = api_->recv(sockfd_, recv_buf_, sizeof(recv_buf_) - 1, recv_flags_);
  if (recvd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      LOG(INFO) << "  returned EAGAIN or EWOULDBLOCK!";
    } else {
      LOG(ERROR) << "  Error-recv(fd:" << sockfd_ << ") failed-"
                 << "[" << errno << "]" << strerror(errno);
    }
    return false;
  }
  LOG(INFO) << "  received(fd:" << sockfd_ << ") [" << recvd << "]'" << recv_buf_ << "' ";
  if (ping) {
    if (api_->send(sockfd_, recv_buf_, recvd, recv_flags_) < 0) {
      LOG(ERROR) << "Error-send(fd:" << sockfd_ << ") failed-"
                 << "[" << errno << "]" << strerror(errno);
      return false;
    }
  }
  return true;
}

bool SockScripter::Recv(char* arg) { return RecvInternal(false /* ping */); }

bool SockScripter::RecvPing(char* arg) { return RecvInternal(true /* ping */); }

bool SockScripter::SetSendBufHex(char* arg) { return snd_buf_gen_.SetSendBufHex(arg); }

bool SockScripter::SetSendBufText(char* arg) { return snd_buf_gen_.SetSendBufText(arg); }

bool SockScripter::Sleep(char* arg) {
  int sleeptime;
  if (!str2int(arg, &sleeptime) || sleeptime < 0) {
    LOG(ERROR) << "Error: Invalid sleeptime='" << arg << "'!";
    return false;
  }
  LOG(INFO) << "Sleep for " << sleeptime << " secs...";
  if (sleeptime > 0) {
    sleep(sleeptime);
  }
  LOG(INFO) << "Wake up!";
  return true;
}

std::string SendBufferGenerator::GetSndStr() {
  switch (mode_) {
    case COUNTER_TEXT: {
      std::string ret_str = snd_str_;
      auto cnt_specifier = ret_str.find("%c");
      if (cnt_specifier != std::string::npos) {
        ret_str.replace(cnt_specifier, 2, std::to_string(counter_));
      }
      counter_++;
      return ret_str;
    }
    case STATIC_TEXT:
    default:
      return snd_str_;
  }
}

bool SendBufferGenerator::SetSendBufHex(const char* arg) {
  std::string_view str(arg, strlen(arg));
  std::stringstream ss;
  while (str.length()) {
    auto f = str.front();
    uint8_t v;
    // always allow any number of spaces or commas to happen
    if (f == ' ' || f == ',') {
      str = str.substr(1);
      continue;
    } else if (str.length() < 2 ||
               !fxl::StringToNumberWithError(str.substr(0, 2), &v, fxl::Base::k16)) {
      // trailing character at the end that we don't recognize or failed to parse number
      return false;
    }
    str = str.substr(2);
    ss.put(v);
  }
  snd_str_ = ss.str();
  return true;
}

bool SendBufferGenerator::SetSendBufText(const char* arg) {
  snd_str_ = arg;
  return true;
}
