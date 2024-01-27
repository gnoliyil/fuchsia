# The zither "zircon.ifs" Backend

The zither zircon.ifs backend is responsible for generating "zircon.ifs", the
text ABI specification of libzircon.so, derived from the FIDL syscall
specifications of library `zx`.

## Output layout

One "zircon.ifs" is generated.

## GN integration

`${fidl_target}_zither.zircon_ifs` generates the one file, which is intended to
be accessed via `zither_golden_files()` in order to be checked in as source.

## Bindings

Any declaration type not mentioned below is ignored.

### Syscalls

Each syscall contributes two .ifs entries:
```
- { Name: _zx_foo_bar, Type: Func }
- { Name: zx_foo_bar, Type: Func, Weak: true }
```




