# component manager

Reviewed on: 2019-07-12

Component manager is the program which runs and manages v2 components. More
about what components are, what semantics they provide, and how to use them is
available [here](/docs/the-book/components/README.md).

## Building

Component manager should be included in all builds of Fuchsia, but if missing
can be added to builds by including `--with //src/sys/component_manager` to the
`fx set` invocation.

## Running

Component manager runs by default on all Fuchsia builds.

## Testing

Unit tests for component manager are available in the `component_manager_tests`
package.

```
$ fx test component_manager_tests
```

Integration tests are also available in the following packages:

- `storage_integration_test`
- `routing_integration_test`
- `elf_runner_test`
- `no_pkg_resolver_test`

## Source layout

The entrypoint is located in `src/main.rs`, and the core model implementation is
under `src/model/`. Unit tests are co-located with the code, with the exception
of `src/model/` which has unit tests in `src/model/tests/`. Integration tests
live in `tests/`.

## Development best practices

### `Arc<Realm>` and `fasync::spawn`

#### Problem

Many parts of the code need access to a `Realm`. Some of those are long-running asynchronous
operations, such as hosting a vfs directory with a closure. These operations are executed on the
global executor through `fasync::spawn`.

These closures should never capture an `Arc<Realm>`, as the closures lifetime is not bound to the `Realm`,
even though it is conceptually tied to the life of `Realm`. This can lead to memory leaks / reference cycles.

#### Solution

Use `WeakRealm`, which wraps a `Weak<Realm>` (weak pointer to `Realm`) along with the `AbsoluteMoniker` of
the realm, for good error-reporting when the `Realm` has been destroyed.

```
use crate::model::realm::{Realm, WeakRealm};

let realm: Arc<Realm> = model.look_up_realm(...)?;
let captured_realm: WeakRealm = realm.as_weak();
fasync::spawn(async move {
    let realm = match captured_realm.upgrade() {
        Ok(realm) => realm,
        Err(e) => {
            log::error!("failed to upgrade WeakRealm: {}", e);
            return;
        }
    };
    ...
});
```
