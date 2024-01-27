# zither

`zither` is a FIDL backend for generating simple language bindings for
'data layouts', by which we mean bindings representing memory formats (e.g.,
syscall or ZBI data types) that mirror the format proper (either directly
with a C-style representation or with a small, provided transformation to get
there). zither in turn has its own (sub)backends (see below).

## Supported backends

Here we document the supported zither backends and their particularities.
Consider a FIDL library by the name of `${id1}.${id2}.....${idn}`:

### C
* One header `<lib/${id1}/${id2}/.../${idn}/${idn}/c/${filename}.h>` is
generated per original FIDL source file, containing the bindings for the
declarations defined there.
* Type translation from FIDL to C is as follows:
  - `{u,}int{8,16,32,64}`map to `{u,}int{8,16,32,64}_t`, respectively;
  - `bool` maps to `bool`;
  - unbounded `string`s are only permitted in constants and map to string
  literals;
  - TODO(fxbug.dev/51002): Document more as we go.
* Type aliases are naturally converted into `typedef`s.
* A constant declaration yields a preprocessor variable of name
`UpperSnakeCase(${id1}_${id2}_..._${idn}_${declname})`;
* An enum or bits declaration yields a typedef of the underlying primitive
type with a name of `LowerSnakeCase(${id1}_${id2}_..._${idn}_${declname}_t)`;
  - a member yields a preprocessor variable of name
  `UpperSnakeCase(${id1}_${id2}_..._${idn}_${parent_name}_${member_name})`
* A struct declaration yields a typedef of the given struct as
`LowerSnakeCase(${id1}_${id2}_..._${idn}_${declname})_t`, with an
obvious mapping of members.

### Assembly
* One header `<lib/${id1}/${id2}/.../${idn}/${idn}/asm/${filename}.h>` is
generated per original FIDL source file, containing the bindings for the
declarations defined there.
* A constant declaration yields a preprocessor variable of name
`UpperSnakeCase(${id1}_${id2}_..._${idn}_${declname})`;
* An enum member yields a preprocessor variable of name
  `UpperSnakeCase(${id1}_${id2}_..._${idn}_${parent_name}_${member_name})`
* A bits member yields two preprocessor variables:
  - `UpperSnakeCase(${id1}_${id2}_..._${idn}_${parent_name}_${member_name})`
  - `UpperSnakeCase(${id1}_${id2}_..._${idn}_${parent_name}_${member_name})_SHIFT`
* A struct declaration yields a preprocessor variable
  `UpperSnakeCase(${id1}_${id2}_..._${idn}_${declname})_SIZEOF` (giving the
  size of the struct), while each member yields a
  `UpperSnakeCase(${id1}_${id2}_..._${idn}_${declname})` giving its offset.

### Go
* The generated (nested) package name is `${id1}/.../${idn}`, which is also
written to a text file `pkg_name.txt`.
* One file `${id1}/${id2}/.../${idn}/${idn}/${filename}.go` is generated
per original FIDL source file, containing the bindings for the declarations
defined there.
* Type translation from FIDL to C is as follows:
  - `bool`, `string` and `{u,}int{8,16,32,64}`map identically into Go's type
  system;
  - unbounded `string`s are only permitted in constants and map to string
  literals;
  - TODO(fxbug.dev/51002): Document more as we go.
* Type aliases are converted into naturally into go's notion of a type alias.
* A constant declaration yields a constant of name
`UpperCamelCase(${declname})`;
* An enum or bits declaration yields a new type declaration of the underlying
primitive type with a name of `UpperCamelCase(${declname})`;
  - a member yields a constant of name
  `UpperCamelCase(${parent_name}${declname}`)
* A struct declaration yields a struct of name `UpperCamelCase(${declname})`,
with an obvious mapping of members.

### Rust
* One crate `zither-${id1}-${id2}-...-{idn}-${idn}` is generated: it contains
a `${filename}.rs` for each original FIDL source file and a crate root `lib.rs`
* Type translation from FIDL to Rust is as follows:
  - `{u,}int{8,16,32,64}` map to `{u,i}{8,16,32,64}`, respectively;
  - `bool` maps to `bool`;
  - unbounded `string`s are only permitted in constants and map to static
  `&str` literals;
* Type aliases are naturally converted into `pub type $name = $type;`
statements.
* A constant yields an exported `const` declaration of the name
`UpperSnakeCase(${declname})`;
* An enum declaration is naturally translated to an exported
`#[repr(${subtype}]` `enum` with `UpperCamelCase(${member_name})` member names;
* A bits declaration yields a conventional `bitflags!` instantiation on a
`struct` of name `UpperCamelCase(${member_name})` with
`const` members of name `UpperSnakeCase(${member_name})`;
* A struct declaration yields an exported `#[repr(C)]` `struct` declaration of
name `UpperCamelCase(${declname})` with exported members of name
`LowerSnakeCase(${member_name})`.

TODO(fxbug.dev/91102): Also C++

## Testing
zither's testing strategy is three-fold.

### Build-time golden tests
Executing golden tests at build-time is significantly more convenient than at
runtime: ninja executes them with maximal parallelism, testing is a
side-effect of building and not something that requires shell script glue
around complex packaging of golden and generated files (which will quickly
grow to be quite numerous), and the updating of goldens will amount to running
a build-generated `cp` command (as opposed to something involving more
cross-referencing and copy-pasting). See `zither_golden_test()` in `BUILD.gn`
for more detail.

It is worth noting that this form of testing should not increase build times in
any meaningful way: whether diffing happens at build-time or runtime, the build
would dedicate the same amount of time to `fidlc` and `zither` invocations - and,
further, build-time diffing is imperceptibly quick.

### Compilation integration tests
Testing that code was emitted as expected can only be part of the strategy; one
needs to further verify that the generated code is actually valid and compiles.
This is covered by another form of build-time testing that aims to do exactly
that for each of the supported backends.

### Basic unit-testing
Finally, pieces of the generation logic of sufficient modularity and complexity
will be conventionally unit-tested. This in particular includes the translation
from FIDL's IR to another of zither's.
