# ELF Loading & Dynamic Linking Template Library (`elfldltl`)

This library provides a toolkit of C++ template APIs for implementing ELF
dynamic linking and loading functionality.  It uses C++ namespace `elfldltl`.

The library API supports either 32-bit or 64-bit ELF with either little-endian
or big-endian byte order, via template parameters.  Thus it is suitable for
cross-tools without assumptions about host machine or operating system details,
as well as for ELF target build environments of various sorts.

## Format layouts and constants

The library provides the ELF format layouts and constants in a modern C++ API
using canonical style.  It does not intend to be an exhaustive API for all of
the ELF format.  It provides the full range of data structures and constants
used in dynamic linking and loading, and some others that are of use in related
tools.  It does not provide full details needed for compilers, linkers,
debuggers, etc.  In some instances it defines all constants of a particular
type, while in others it defines only the subset actually used or supported by
the toolkit.  It provides details from the formal ELF specification and for de
facto standard GNU and LLVM extensions on the same footing.  The constants and
data structures largely mimic the spellings used in the ELF specification,
which come from the traditional `<elf.h>` C API.  However, these definitions
use C++ scoping and canonical identifier styles that do not overlap with it.

The generic ELF format constants are defined in
[`<lib/elfldltl/constants.h>`](include/lib/elfldltl/constants.h).  These are
mostly `enum class` types that apply to all format variants and machines.
Types selecting format variant and machine also include convenient `kNative`
aliases for the native platform being compiled for.

ELF data layouts are defined in
[`<lib/elfldltl/layout.h>`](include/lib/elfldltl/layout.h).  These are provided
as C++ struct types within the `elfldltl::Elf` template class, which is
parameterized by class (32-bit vs 64-bit) and data format (byte order) with
default template arguments matching the native platform being compiled for.

### Field accessors

ELF format data types are defined using a simple set of generic wrapper
template types for defining fields, defined in
[`<lib/elfldltl/field.h>`](include/lib/elfldltl/field.h).  This API can also be
used directly for other data structures in ELF sections or in memory for which
doing access across bit widths and/or byte orders may be useful.

The wrapper types used for fields have the same exact ABI layout as a specified
underlying integer type.  Their API behavior as C++ objects is roughly like a
normal field of a given integer or `enum` type, in that they are implicitly
convertible to and from that type and can be compared to it for equality.  In
contexts where the C++ type is flexible, these implicit conversions may not be
sufficient and these objects do not fully emulate an integer or `enum` type.
So they also provide direct accessor methods via `field.get()` or `field()`
(i.e. calling the field as a function of no arguments).

## Note parser

[`<lib/elfldltl/note.h>`](include/lib/elfldltl/note.h) provides a parser for
the ELF note format found in `PT_NOTE` segments or `SHT_NOTE` sections.  It
provides a convenient container-view / iterator API across a note segment in
memory.  The "elements" of the virtual container provide easy access to the
note name (vendor) and description (payload) bytes using `string_view` and
`span` style types, and a field accessor for the 32-bit type field.  The note
format is the same across 32-bit and 64-bit ELF files, so the note parser's
template classes are actually parameterized only by the byte order.  But the
canonical access to the API is via the `elfldltl::Elf` template class for the
specific format variant, which has the `Note` and `NoteSegment` types.

## Introspection

[`<lib/elfldltl/self.h>`](include/lib/elfldltl/self.h) provides accessors for a
program or shared library to refer to its own ELF data structures at runtime
using link-time references.  Both 32-bit and 64-bit formats are supported
independent of the native pointer size, as is dynamic selection between the two
in case the 32-bit format is used to size-optimize 64-bit binaries.  These APIs
are useful for implementing static PIE self-relocation and similar cases.

## Relocation

[`<lib/elfldltl/relocation.h>`](include/lib/elfldltl/relocation.h) provides
decoders for the relocation metadata formats.  This provides straightforward
iterable C++ container views for the various kinds of relocation records.

[`<lib/elfldltl/machine.h>`](include/lib/elfldltl/machine.h) provides a uniform
API for the machine-specific format constants needed for dynamic relocation and
TLS layout via a template class parameterized by machine.  Each specialization
provides a consistent API for the constants whose values (and presence) vary by
machine.  Only the constants related to machines and relocation types supported
by the toolkit are provided.

## Symbol table

[`<lib/elfldltl/symbol.h>`](include/lib/elfldltl/symbol.h) provides access to
the dynamic symbol table of an ELF file.  It provides C++ container views for
the symbol table and the hash tables.  It handles symbol name string hashing
and hash table lookup.

## Dynamic Section

[`<lib/elfldltl/dynamic.h>`](include/lib/elfldltl/dynamic.h) provides a
framework for examining the `PT_DYNAMIC` metadata in a single pass using a
mix-and-match variety of observer objects looking for different kinds of
entries.  The toolkit provides observer object types for some common tasks.
Custom observer objects can be implemented easily.

## "Remotable" pointers

['<lib/elfldltl/abi-ptr.h>`](include/lib/elfldltl/abi-ptr.h) and
['<lib/elfldltl/abi-span.h>`](include/lib/elfldltl/abi-span.h) provide an
abstraction for pointer types and `std::span`-style types.  The purpose of
these is to define data structure layouts that can be populated from outside
the address space where they will be used.  This is used to implement the
"passive ABI" concept that enables out-of-process dynamic linking to be
indistinguishable from in-process dynamic linking for application code.  See
[`//sdk/lib/ld`](/sdk/lib/ld) for a more thorough explanation.

A subset of the class APIs representing information of use in this kind of ABI
are defined using `elfldltl::AbiPtr` and related template types.  These classes
take an extra "traits" template parameter that's passed along to the
`elfldltl::AbiPtr` template to facilitate duplicating the data structure from
one address space and pointer encoding to another.  There is a default that
boils down to using normal pointer and integer types under the hood, so this
can generally be ignored by users of these classes.  When these classes are
instantiated using different traits as part of the "remoting" process, those
class objects can't really be used with all the methods supported by the
default instantiations--they're really just for the mechanics of remoting.

**TODO(fxbug.dev/121817):** Remoting is not implemented yet.
