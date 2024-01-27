// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! # `pprof`-related utilities
//!
//! This crate provides a `build_profile` function that can be used to turn
//! profiling information gathered by `memory_sampler` into a
//! `pprof`-compatible profile.
//!
//! # Examples
//!
//! ```
//! use pprof;
//!
//! let profile = pprof::build_profile(module_map, live_allocations, dead_allocations);
//! ```
pub use profile_rust_proto::perfetto::third_party::perftools::profiles as pproto;

use std::collections::{BTreeMap, HashMap};
use std::ops::Bound::{Excluded, Unbounded};

use fidl_fuchsia_memory_sampler::ModuleMap;
use itertools::Itertools;

use crate::profile_builder::{DeadAllocation, LiveAllocation};

/// Pprof interns most strings: it keeps a table of known strings and replaces
/// most occurrences of strings in its data by their index. This structure
/// implements that behavior.
struct StringTable {
    strings: HashMap<String, i64>,
    table: Vec<String>,
}

impl StringTable {
    pub fn new() -> StringTable {
        let mut st = StringTable { strings: HashMap::new(), table: vec![] };
        st.intern("");
        st
    }
    /// If a string is already known to the table, return its index. Otherwise,
    /// insert it and return its newly created index.
    pub fn intern(&mut self, string: &str) -> i64 {
        *self.strings.entry(string.to_string()).or_insert_with(|| {
            let index = self.table.len();
            self.table.push(string.to_string());
            index as i64
        })
    }
    fn finalize(self) -> Vec<String> {
        self.table
    }
}

/// Helper structure to match an address to a memory range.
struct Mapping {
    ranges: BTreeMap<u64, (u64, u64)>, // memory_limit -> (memory_start, mapping_id)
}

impl Mapping {
    /// Produce a collection of `pprof`'s `Mapping` for use in a
    /// profile, and an index to map an address to the corresponding
    /// mapping.
    pub fn build<'a>(
        modules: impl Iterator<Item = &'a ModuleMap>,
        st: &mut StringTable,
    ) -> (Mapping, Vec<pproto::Mapping>) {
        let (ranges, table) = modules
            // Distribute the build_id on each segment.
            .flat_map(|ModuleMap { executable_segments, build_id, .. }| {
                let build_id = build_id.as_ref().map_or(0, |id| st.intern(&hex::encode(id)));
                executable_segments.iter().flatten().zip(std::iter::repeat(build_id))
            })
            // Use the index of the collection as a unique id.
            .enumerate()
            .filter_map(|(id, (segment, build_id))| {
                // ID 0 is reserved and cannot be used in the mapping.
                let id = id as u64 + 1;
                let memory_start = segment.start_address?;
                let memory_limit = memory_start + segment.size?;
                let file_offset = segment.relative_address?;
                let range = (memory_limit, (memory_start, id));
                let mapping = pproto::Mapping {
                    id,
                    memory_start,
                    memory_limit,
                    file_offset,
                    build_id,
                    ..Default::default()
                };
                Some((range, mapping))
            })
            .unzip();
        (Mapping { ranges }, table)
    }
    /// Resolve an address to the corresponding mapping_id, or return
    /// 0 if not found.
    pub fn resolve(&self, address: u64) -> u64 {
        if let Some((memory_limit, (memory_start, mapping_id))) =
            self.ranges.range((Excluded(address), Unbounded)).next()
        {
            assert!(address < *memory_limit);
            if *memory_start <= address {
                return *mapping_id;
            }
        }
        0 // Not found
    }
}

/// Build a `pprof`-compatible profile from a program address mapping
/// and its allocation/deallocation information.
pub fn build_profile<'a>(
    modules: impl Iterator<Item = &'a ModuleMap>,
    live_allocations: Vec<LiveAllocation>,
    dead_allocations: Vec<DeadAllocation>,
) -> pproto::Profile {
    let mut st = StringTable::new();
    let mut pprof = pproto::Profile {
        // This table describes the types of values contained in each sample.
        sample_type: vec![
            pproto::ValueType {
                r#type: st.intern("residual_allocated_objects"),
                unit: st.intern("count"),
            },
            pproto::ValueType {
                r#type: st.intern("residual_allocated_space"),
                unit: st.intern("bytes"),
            },
            pproto::ValueType { r#type: st.intern("allocated_objects"), unit: st.intern("count") },
            pproto::ValueType { r#type: st.intern("allocated_space"), unit: st.intern("bytes") },
            pproto::ValueType {
                r#type: st.intern("deallocated_objects"),
                unit: st.intern("count"),
            },
            pproto::ValueType { r#type: st.intern("deallocated_space"), unit: st.intern("bytes") },
        ],
        default_sample_type: 3, // Clients should default to showing the allocated space.
        ..pproto::Profile::default()
    };

    {
        let (mp, mappings) = Mapping::build(modules, &mut st);
        pprof.mapping = mappings;

        let live_locations = live_allocations
            .iter()
            .flat_map(|LiveAllocation { stack_trace: ast, .. }| ast.stack_frames.iter().flatten());
        let dead_locations = dead_allocations.iter().flat_map(
            |DeadAllocation { allocation_stack_trace: ast, .. }| ast.stack_frames.iter().flatten(),
        );
        let deallocations_locations = dead_allocations.iter().flat_map(
            |DeadAllocation { deallocation_stack_trace: dst, .. }| {
                dst.stack_frames.iter().flatten()
            },
        );
        let all_locations =
            live_locations.chain(dead_locations).chain(deallocations_locations).unique();

        pprof.location.extend(all_locations.map(|&address| pproto::Location {
            id: address,
            mapping_id: mp.resolve(address),
            address,
            ..Default::default()
        }));
    }

    // Live allocations
    {
        let samples =
            live_allocations.into_iter().filter_map(|LiveAllocation { size, stack_trace: ast }| {
                Some(pproto::Sample {
                    value: vec![1, size, 1, size, 0, 0],
                    location_id: ast.stack_frames?,
                    ..Default::default()
                })
            });
        pprof.sample.extend(samples);
    }
    // Dead allocations and deallocations
    {
        let samples = dead_allocations
            .into_iter()
            .filter_map(
                |DeadAllocation {
                     size,
                     allocation_stack_trace: ast,
                     deallocation_stack_trace: dst,
                 }| {
                    Some(vec![
                        // Dead allocation
                        pproto::Sample {
                            value: vec![0, 0, 1, size, 0, 0],
                            location_id: ast.stack_frames?,
                            ..Default::default()
                        },
                        // Deallocation
                        pproto::Sample {
                            value: vec![0, 0, 0, 0, 1, size],
                            location_id: dst.stack_frames?,
                            ..Default::default()
                        },
                    ])
                },
            )
            .flatten();
        pprof.sample.extend(samples);
    }

    pprof.string_table = st.finalize();
    pprof
}

#[cfg(test)]
mod test {
    use fidl_fuchsia_memory_sampler::{ExecutableSegment, ModuleMap, StackTrace};

    use super::build_profile;
    use crate::profile_builder::{DeadAllocation, LiveAllocation};

    mod string_table {
        use crate::pprof::StringTable;
        use std::iter::zip;

        /// `pprof` mandates that the string table starts with the empty
        /// string.
        #[fuchsia::test]
        fn test_string_table_starts_with_empty_string() {
            let result = StringTable::new().finalize();

            assert_eq!(vec![""], result);
        }

        /// Ensure that the string table knows to resolve the empty
        /// string.
        #[fuchsia::test]
        fn test_string_table_knows_empty_string() {
            let mut st = StringTable::new();

            assert_ne!(0, st.intern("word"));
            assert_eq!(0, st.intern(""));
        }

        /// Ensure the string table can intern unique words.
        #[fuchsia::test]
        fn test_string_table_interns_unique_words() {
            let mut st = StringTable::new();
            let words = vec!["Those", "are", "unique", "words"];
            let indices: Vec<i64> = words.iter().map(|w| st.intern(w)).collect();
            let result = st.finalize();

            for (index, word) in zip(indices, words.iter()) {
                assert_eq!(*word, result[index as usize]);
            }
        }

        /// Ensure the string table can intern repeated words, and reuses
        /// the same entries.
        #[fuchsia::test]
        fn test_string_table_interns_repeated_words() {
            let mut st = StringTable::new();
            let words = vec!["word", "word", "word", "word"];
            let indices: Vec<i64> = words.iter().map(|w| st.intern(w)).collect();
            let result = st.finalize();

            let repeated = indices[0];
            for (index, word) in zip(indices, words.iter()) {
                assert_eq!(*word, result[index as usize]);
                assert_eq!(repeated, index);
            }
        }
    }

    mod mapping {
        use crate::pprof::Mapping;
        use crate::pprof::StringTable;
        use fidl_fuchsia_memory_sampler::{ExecutableSegment, ModuleMap};

        #[fuchsia::test]
        fn test_mapping_can_resolve() {
            let mut st = StringTable::new();
            let modules = vec![
                // Describes a module that lives between addresses 0 and 100.
                ModuleMap {
                    executable_segments: Some(vec![ExecutableSegment {
                        start_address: Some(0),
                        size: Some(100),
                        relative_address: Some(0),
                        ..ExecutableSegment::EMPTY
                    }]),
                    build_id: Some(vec![0, 0, 0, 0]),
                    ..ModuleMap::EMPTY
                },
                // Describes a module that lives between addresses 1000 and 2000.
                ModuleMap {
                    executable_segments: Some(vec![
                        ExecutableSegment {
                            start_address: Some(1000),
                            size: Some(500),
                            relative_address: Some(0),
                            ..ExecutableSegment::EMPTY
                        },
                        ExecutableSegment {
                            start_address: Some(1500),
                            size: Some(500),
                            relative_address: Some(500),
                            ..ExecutableSegment::EMPTY
                        },
                    ]),
                    build_id: Some(vec![1, 0, 0, 0]),
                    ..ModuleMap::EMPTY
                },
            ];

            let (mapping, table) = Mapping::build(modules.iter(), &mut st);
            let strings = st.finalize();
            let id_to_mapping = {
                let mut im = vec![0; table.len() + 1];
                for (i, crate::pprof::pproto::Mapping { id, .. }) in table.iter().enumerate() {
                    im[*id as usize] = i;
                }
                im
            };
            let lookup_by_address =
                |address| &table[id_to_mapping[mapping.resolve(address) as usize]];

            {
                // Address 1 resolves to the first segment of the first module.
                let address = 1;
                let expected_module = &modules[0];
                let expected_segment = &expected_module.executable_segments.as_ref().unwrap()[0];

                let segment = lookup_by_address(address);
                {
                    let expected_build_id =
                        hex::encode(&expected_module.build_id.as_ref().unwrap()[..]);
                    let build_id = &strings[segment.build_id as usize];
                    assert_eq!(expected_build_id, *build_id);
                }
                {
                    let expected_start = expected_segment.start_address.unwrap();
                    let expected_end = expected_start + expected_segment.size.unwrap();
                    assert_eq!(expected_start, segment.memory_start);
                    assert_eq!(expected_end, segment.memory_limit);
                    assert_eq!(expected_segment.relative_address.unwrap(), segment.file_offset);
                }
            }

            {
                // Address 1000 resolves to the first segment of the second module
                let address = 1000;
                let expected_module = &modules[1];
                let expected_segment = &expected_module.executable_segments.as_ref().unwrap()[0];

                let segment = lookup_by_address(address);
                {
                    let expected_build_id =
                        hex::encode(&expected_module.build_id.as_ref().unwrap()[..]);
                    let build_id = &strings[segment.build_id as usize];
                    assert_eq!(expected_build_id, *build_id);
                }
                {
                    let expected_start = expected_segment.start_address.unwrap();
                    let expected_end = expected_start + expected_segment.size.unwrap();
                    assert_eq!(expected_start, segment.memory_start);
                    assert_eq!(expected_end, segment.memory_limit);
                    assert_eq!(expected_segment.relative_address.unwrap(), segment.file_offset);
                }
            }

            {
                // Address 1700 resolves to the second module
                let address = 1700;
                let expected_module = &modules[1];
                let expected_segment = &expected_module.executable_segments.as_ref().unwrap()[1];

                let segment = lookup_by_address(address);
                {
                    let expected_build_id =
                        hex::encode(&expected_module.build_id.as_ref().unwrap()[..]);
                    let build_id = &strings[segment.build_id as usize];
                    assert_eq!(expected_build_id, *build_id);
                }

                {
                    let expected_start = expected_segment.start_address.unwrap();
                    let expected_end = expected_start + expected_segment.size.unwrap();
                    assert_eq!(expected_start, segment.memory_start);
                    assert_eq!(expected_end, segment.memory_limit);
                    assert_eq!(expected_segment.relative_address.unwrap(), segment.file_offset);
                }
            }

            {
                // Address 500 is not found.
                let address_not_found = 500;
                assert_eq!(0, mapping.resolve(address_not_found));
            }
        }
    }

    #[fuchsia::test]
    fn test_build_profile() {
        let modules = vec![
            // Describes a module that lives between addresses 0 and 100.
            ModuleMap {
                executable_segments: Some(vec![ExecutableSegment {
                    start_address: Some(0),
                    size: Some(100),
                    relative_address: Some(0),
                    ..ExecutableSegment::EMPTY
                }]),
                build_id: Some(vec![0, 0, 0, 0]),
                ..ModuleMap::EMPTY
            },
            // Describes a module that lives between addresses 1000 and 2000.
            ModuleMap {
                executable_segments: Some(vec![
                    ExecutableSegment {
                        start_address: Some(1000),
                        size: Some(500),
                        relative_address: Some(0),
                        ..ExecutableSegment::EMPTY
                    },
                    ExecutableSegment {
                        start_address: Some(1500),
                        size: Some(500),
                        relative_address: Some(500),
                        ..ExecutableSegment::EMPTY
                    },
                ]),
                build_id: Some(vec![1, 0, 0, 0]),
                ..ModuleMap::EMPTY
            },
        ];

        let live_allocations = vec![LiveAllocation {
            size: 10,
            stack_trace: StackTrace { stack_frames: Some(vec![]), ..StackTrace::EMPTY },
        }];
        let dead_allocations = vec![DeadAllocation {
            size: 20,
            allocation_stack_trace: StackTrace { stack_frames: Some(vec![]), ..StackTrace::EMPTY },
            deallocation_stack_trace: StackTrace {
                stack_frames: Some(vec![]),
                ..StackTrace::EMPTY
            },
        }];

        let profile =
            build_profile(modules.iter(), live_allocations.clone(), dead_allocations.clone());

        // Check that the profile contains one mapping per segment.
        let segment_count: usize = modules
            .iter()
            .map(|ModuleMap { executable_segments, .. }| {
                executable_segments.as_ref().unwrap().len()
            })
            .sum();
        assert_eq!(segment_count, profile.mapping.len(), "Unexpected number of mappings.");

        // Check that the profile contains one sample per allocation
        // and one sample per deallocation. There is one allocation
        // per `live_allocations` and per `dead_allocations`, and one
        // deallocation per `dead_allocations`.
        let allocation_count = live_allocations.len() + 2 * dead_allocations.len();
        assert_eq!(allocation_count, profile.sample.len(), "Unexpected number of samples.");

        // Check that the number of sample value per sample matches
        // the sample_type.
        let sample_type_count = profile.sample_type.len();
        assert!(
            profile.sample.iter().all(|sample| sample.value.len() == sample_type_count),
            "Unexpected sample value count."
        );
    }
}
