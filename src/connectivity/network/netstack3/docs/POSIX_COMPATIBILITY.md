# POSIX-like Compatibility

Fuchsia is [committed][Fuchsia RFC-0184] to supporting a POSIX-like interface
for the system netstack. Like the existing networking stack, Netstack3 will
do this by implementing the same [`fuchsia.posix.socket`] FIDL services.

## How does Netstack3 support a POSIX-like interface?

The Netstack3 ["bindings"][core and bindings] component (a.k.a. the `netstack3`
crate) provides the top-level implementation of the `fuchsia.posix.socket`
services to other Fuchsia components running on the system. The `netstack3`
bindings crate provides a direct implementation of some of these calls, like
`fuchsia.posix.socket.BaseDatagramSocket/GetInfo`, though most calls are
implemented by making calls into `core`.

This requires that `core` provide a call surface that exposes POSIX-like
semantics to bindings, including

- the notion of "unbound" sockets that exist but will not receive traffic,
- the ability to set options on sockets,
- emulation of POSIX `bind()` and `connect()` with address conflict detection,

and more.

Because the `core` surface is written in Rust and called into by other Rust
code, it can provide stronger guarantees than POSIX at compile time via type
constraints. The POSIX [`getpeername`] function, for example, can only be called
on connected sockets.

## Known incompatibilities with existing POSIX-like systems

Netstack3 aims to be POSIX compatible. It also implements a number of common
extensions to the POSIX specification, though it does not emulate or reproduce
the behavior of any particular POSIX-like system.

### `listen(int socket, int backlog)`

The [POSIX specification][POSIX listen] for the `listen` syscall requires that

> If listen() is called with a backlog argument value that is less than 0, the
> function behaves as if it had been called with a backlog argument value of 0.

Linux does not adhere to this requirement, and instead treats a backlog size
less than 0 as requesting the maximum. Netstack3 treats a backlog size of 0 or
less as requesting the minimum, and applies a minimum backlog size of 1.

### `SO_REUSEPORT`

Netstack3 supports the `SO_REUSEPORT` socket option present in Linux and
BSD-based systems. The behavior of sockets that have `SO_REUSEPORT` set is
similar on Netstack3 and Linux with regards to sending and receiving packets.
Where Linux allows setting a socket's `SO_REUSEPORT` flag at any point,
including after it has been bound, Netstack3 only allows setting `SO_REUSEPORT`
on a socket before it is bound to a local address.

### `SO_BINDTODEVICE`

Netstack3 supports setting the interface on which a socket will send and receive
packets using the Linux `SO_BINDTODEVICE` socket option. Unlike Linux, Netstack3
checks that the bound device does not conflict with other functionality that
controls the interface used for sending and receiving, including
- [`IPV6_MULTICAST_IF`] and [`IP_MULTICAST_IF`],
- for an IPv6 socket, the scope ID associated with its local or remote address,

### `SO_SNDBUF` and `SO_RCVBUF`

Netstack3 supports the [`SO_SNDBUF` and `SO_RCVBUF`][POSIX buffer sizes] socket
options for setting a socket's send and receive buffer sizes, respectively. On
Linux, the value provided when setting one of these options
[is doubled][Linux buffer sizes], so reading the value for the same option
returns a different value. Netstack3 handles setting one of these buffer sizes
by using the value to set the variable-size portion of a socket's buffer.
Reading the same buffer size value reports the sum of the variable and any
fixed-size portion of the socket's buffer.

Note that like on other platforms, the value applied when setting these options
is limited by system-defined minimums and maximums.

[Fuchsia RFC-0184]: /docs/contribute/governance/rfcs/0184_posix_compatibility_for_the_system_netstack
[`fuchsia.posix.socket`]: /sdk/fidl/fuchsia.posix.socket/socket.fidl
[core and bindings]: ./CORE_BINDINGS.md#core-and-bindings
[`getpeername`]: https://pubs.opengroup.org/onlinepubs/9699919799/functions/getpeername.html
[POSIX listen]: https://pubs.opengroup.org/onlinepubs/9699919799/functions/listen.html
[`IPV6_MULTICAST_IF`]: https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html
[`IP_MULTICAST_IF`]: https://man7.org/linux/man-pages/man7/ip.7.html
[POSIX buffer sizes]: https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tagtcjh_8
[Linux buffer sizes]: https://man7.org/linux/man-pages/man7/socket.7.html
