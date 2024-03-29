# Helper library to create pprof-compatible protobuf files

This crate provides an implementation for some common tasks that are usually
needed to generate files in
[pprof's format](https://github.com/google/pprof/tree/main/proto):

* the `module_map` crate contains helpers to build the memory map of the
  profiled process (to populate the `mapping` field of the `Profile`) and map
  each address to the corresponding `Mapping` (to populate the `mapping_id`
  field of a `Location`).
* the `string_table` crate contains helpers to deduplicate strings, generate
  string indices, and populate the `string_table` field of the `Profile`
  accordingly.

In addition, this crate also re-exports all the protobuf type definitions
generated by prost.
