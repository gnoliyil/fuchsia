# Tracing the netstack

_TODO(https://fxbug.dev/102863): Improve ergonomics of enabling tracing on the_
_system netstack._

You can enable tracing for the netstack by swapping out the component in the
netstack package included in your build for the one that has tracing enabled.
You can do this by replacing the deps for the
"//src/connectivity/network:netstack2" rule:

```diff
 fuchsia_package("netstack2") {
   package_name = "netstack"
-  deps = [ "netstack:component" ]
+  deps = [ "netstack:component-with-tracing" ]
 }
```

Ensure that your `fx set` includes `--with-base //bundles/tools`, otherwise
Netstack will fail to connect to `trace_manager` at initialization time.

Make sure to rebuild and reboot your emulator or OTA/flash your device to
ensure the system picks up the changes.

## How to run a trace

You can use the `ffx trace` subcommand (see [docs][ffx-trace]) to run a trace on
a running Fuchsia system (e.g. an emulator, smart display, or NUC) that is
connected to your development host machine. For example:

```
ffx trace start --categories net,kernel:sched --duration 10
```

This will record a trace of all categories in the `kernel:sched` and `net`
categories. (`kernel:sched` includes detailed scheduler information, and `net`
includes all events emitted by the netstack.) You might want to record this
trace while doing something that exercises the netstack on the Fuchsia device;
for example, running iperf3.

## How to view the trace results

When you run `ffx trace`, it will produce a trace file that is viewable in
[Perfetto](https://ui.perfetto.dev/).

[ffx-trace]: https://fuchsia.dev/fuchsia-src/development/sdk/ffx/record-traces
