# Netstack

Netstack is a userspace TCP/IP network stack and interfaces with zircon
network drivers.
Netstack serves as a back-end for fdio socket API.

```
     +-----------+           +-----------+
     | FIDL app  |           | POSIX app |
     +--+--------+           +-----+-----+
        |                          |
        |                          |
        |                          |
        |                          |
        |                          |
        |                          |
        |   +----------------------v---------+
        |   |         BSD socket API         |
        |   |  (//sdk/lib/fdio)              |
        |   +---------+----------------------+
        |             |
     +--v-------------v----------------------+
     |            netstack                   |
     | (//src/connectivity/network/netstack) |
     +----------------+----------------------+
                      |
     +----------------v-----------------+
     |         Ethernet driver          |
     | (//zircon/system/udev/ethernet)  |
     +----------------------------------+
```

## Benchmarking

To run the benchmarks and obtain profiling data:

```shell
    fx test -o netstack-bench-gotests -- --test.run - --test.bench - \
      && ffx test run --output-directory out/profile fuchsia-pkg://fuchsia.com/netstack-bench-gotests#meta/netstack-bench-gotests.cm \
      && fx go tool pprof -http=:8080 out/profile/artifact-0/custom-0/netstack_bench_profile
```

Example output (astro):

    goos: fuchsia
    goarch: arm64
    pkg: netstack/bench
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=2048-4                  190443              6513 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=1024-4                  247176              4831 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=512-4                   326691              3842 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=256-4                   364168              3426 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=128-4                   394536              3282 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=64-4                    405102              3218 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=32-4                    423908              2967 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=16-4                    424617              2931 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=8-4                     386036              2935 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=4-4                     404241              2990 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=2-4                     371358              2887 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadDisabled/len(payload)=1-4                     431478              2889 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=2048-4                   461614              2766 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=1024-4                   417296              2924 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=512-4                    474790              2814 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=256-4                    453231              2832 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=128-4                    472670              2835 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=64-4                     469988              2894 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=32-4                     490627              2614 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=16-4                     485992              2753 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=8-4                      492573              2798 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=4-4                      487836              2923 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=2-4                      434959              2658 ns/op             112 B/op          3 allocs/op
    BenchmarkWritePacket/checksumOffloadEnabled/len(payload)=1-4                      490470              3010 ns/op             112 B/op          3 allocs/op

## Cobalt Stats

Netstack uses [Sampler] to forward [Inspect] diagnostics data to the [Cobalt]
telemetry system. See the [Sampler configuration] for how Inspect counters map
to Cobalt metrics and how often Inspect is polled; and the [Cobalt configuration]
for the reports defined for each metric.

[Sampler]: https://fuchsia.dev/fuchsia-src/development/diagnostics/analytics/sampler
[Inspect]: ./INSPECT.md
[Cobalt]: https://fuchsia.googlesource.com/cobalt/+/refs/heads/main/README.md
[Sampler configuration]: /src/diagnostics/config/sampler/netstack.json
[Cobalt configuration]: https://fuchsia.googlesource.com/cobalt-registry/+/refs/heads/main/fuchsia/networking/metrics.yaml
