# zbi

This library contains the definitions for the Zircon Boot Image (ZBI) format.

A Zircon Boot Image consists of a container header followed by boot items.
Each boot item has a header (zbi_header_t) and then a payload of
`zbi_header_t.length bytes`, which can be any size.  `The zbi_header_t.type`
field indicates how to interpret the payload.  Many types specify an additional
type-specific header that begins a variable-sized payload.
`zbi_header_t.length` does not include the `zbi_header_t` itself, but does
include any type-specific headers as part of the payload.  All fields in all
header formats are little-endian.

Padding bytes appear after each item as needed to align the payload size up to
a `ZBI_ALIGNMENT` (8-byte) boundary.  This padding is not reflected in the
`zbi_header_t.length` value.

A bootable ZBI can be booted by a Zircon-compatible boot loader. It contains one
`ZBI_TYPE_KERNEL_{ARCH}` boot item that must come first, followed by any number
of additional boot items whose number, types, and details much comport with that
kernel's expectations.

A partial ZBI cannot be booted, and is only used during the build process.  It
contains one or more boot items and can be combined with a kernel and other
ZBIs to make a bootable ZBI.

ZBI_TYPE_STORAGE_* types represent an image that might otherwise appear on some
block storage device, i.e. a RAM disk of some sort.  All zbi_header_t fields
have the same meanings for all these types.  The interpretation of the payload
(after possible decompression) is  indicated by the specific `zbi_header_t.type`
value.

**Note:** The ZBI_TYPE_STORAGE_* types are not a long-term stable ABI.
 - Items of these types are always packed for a specific version of the
   kernel and userland boot services, often in the same build that compiles
   the kernel.
 - These item types are **not** expected to be synthesized or
   examined by boot loaders.
 - New versions of the `zbi` tool will usually retain the ability to
   read old formats and non-default switches to write old formats, for
   diagnostic use.

## Maintenance & Evolution

### ABI Stability

The ZBI definitions form the contract between a bootloader and the kernel so
must remain ABI backwards-compatible. Failure to do so would result in undefined
behavior when devices update their kernel to the new ABI but their bootloader
still uses the old ABI (or vice-versa).

It is generally infeasible to update all bootloaders and kernels to a new ABI in
lockstep, not only due to the proliferation of boards running Fuchsia, but also
because many boards do not support firmware A/B/R without which it is impossible
to guarantee atomic updates to both the bootloader and kernel.

### API Stability

API stability is not required. These definitions are not exported to Fuchsia
application developers so will not break any out-of-tree builds if e.g. a
constant name is updated.

They are exported in the Firmware SDK for firmware developers, but firmware
does not expect to automatically roll new SDK builds; any uprev to a new SDK
is an intentional action that expects some porting work.
