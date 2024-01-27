# Diagnostics Archivist

Reviewed on: 2020-11-16

Archivist collects component lifecycle events, inspect snapshots, and log streams on Fuchsia,
making them available over the `fuchsia.diagnostics/ArchiveAccessor` protocol.

## Building

This project is included in the `bringup` build product and most others as a result.

## Running

The production Archivist is "mounted" in the component topology in the [bootstrap] realm.

Tests run by `run-test-component` have an Archivist embedded in the test realm when either
`fuchsia.logger/LogSink` or `fuchsia.logger/Log` is requested.

Realms can run their own Archivist by running `archivist-for-embedding.cm` from the
`archivist-for-embedding` package. This has a number of sharp edges today and the Diagnostics team
recommends consulting with us in the process of writing a new integration.

## Testing

Unit tests are available in the `archivist-tests` package.

Integration tests for system logging are available in these packages:

* `archivist-integration-tests`
* `archivist-integration-tests-v2`
* `test-logs-from-crashes`

## Source layout

The entrypoint is located in `src/main.rs`, with the rest of the code living in
`src/*.rs` files. Unit tests are co-located with the code and integration tests
are located in the `tests/` directory.

Each data type the Archivist supports has a directory:

* `src/inspect`
* `src/lifecycle`
* `src/logs`

## Configuration

Archivist accepts a configuration file to allow product-specific configuration separately from its
package. The configuration file must be valid JSON with a single object at the top level. Its path
within Archivist's namespace is defined by the `--config-path` argument.

<!-- TODO(fxbug.dev/60812) link to the fuchsia.dev configuration docs -->

See [the generated docs for Archivist's serde-based config parser][config-docs] for details.

[bootstrap]: /src/sys/bootstrap/meta/bootstrap.cml
[config-docs]: https://fuchsia-docs.firebaseapp.com/rust/archivist_lib/configs/struct.Config.html
