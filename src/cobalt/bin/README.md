# Cobalt

[TOC]

## Summary

Cobalt is a system for collecting metrics from Fuchsia devices, sending
metric observations to servers running in the cloud, aggregating and analyzing
the collected observations and generating useful reports and visualizations.
Cobalt emphasizes the preservation of user privacy while collecting
high-quality, useful analytics.

## Concepts

* **Customer** and **Project**. Cobalt is a multi-tenant system. We partition
the system using the notions of customer and project. For example
**Fuchsia** is a customer and **Ledger** is a project.
* **Metric**: A variable to be measured, e.g. the number of times that a Fuchsia
device was booted.
* **Observation**: A single measurement of a metric, e.g.
“This Fuchsia device has booted.” Observations are the units of data
transmitted from the client running Fuchsia to the Cobalt servers.
* **Encoding**: A built-in algorithm for encoding Observations. Some of Cobalt's
encodings implement special privacy-preserving algorithms.

## FIDL Protocol

Fuchsia code uses Cobalt through its FIDL protocol.
See [`//sdk/fidl/fuchsia.cobalt/cobalt.fidl`](/sdk/fidl/fuchsia.cobalt/cobalt.fidl).

### Cobalt Test App

The Cobalt test app `cobalt_testapp.cc` serves as an example usage of the Cobalt
FIDL service.

### Building the test app:

For example:

```sh
fx set x64
fx full-build
```

### Running the test app

Start Fuchsia. For example:

```sh
fx ffx emu start --net tap --console
```

From within Fuchsia:

```sh
system/test/cobalt_testapp
```

 or try

```sh
system/test/cobalt_testapp --verbose=3
```
