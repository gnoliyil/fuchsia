// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_LD_REMOTE_LOAD_MODULE_H_
#define LIB_LD_REMOTE_LOAD_MODULE_H_

#include <lib/elfldltl/load.h>
#include <lib/elfldltl/loadinfo-mapped-memory.h>
#include <lib/elfldltl/loadinfo-mutable-memory.h>
#include <lib/elfldltl/mapped-vmo-file.h>
#include <lib/elfldltl/memory.h>
#include <lib/elfldltl/relocation.h>
#include <lib/elfldltl/resolve.h>
#include <lib/elfldltl/segment-with-vmo.h>
#include <lib/elfldltl/soname.h>
#include <lib/fit/result.h>
#include <lib/ld/load-module.h>
#include <lib/ld/load.h>

#include <algorithm>
#include <vector>

namespace ld {

// RemoteLoadModule is the LoadModule type used in the remote dynamic linker.
using RemoteLoadModuleBase =
    LoadModule<elfldltl::Elf<>, elfldltl::StdContainer<std::vector>::Container,
               LoadModuleInline::kYes, LoadModuleRelocInfo::kYes, elfldltl::SegmentWithVmo::NoCopy>;

template <class Elf = elfldltl::Elf<>>
class RemoteLoadModule : public RemoteLoadModuleBase {
 public:
  using typename RemoteLoadModuleBase::Phdr;
  using typename RemoteLoadModuleBase::size_type;
  using typename RemoteLoadModuleBase::Soname;
  using Ehdr = typename Elf::Ehdr;
  using TlsDescGot = typename Elf::TlsDescGot;
  using List = std::vector<RemoteLoadModule>;
  using LoadInfo =
      elfldltl::LoadInfo<Elf, elfldltl::StdContainer<std::vector>::Container,
                         elfldltl::PhdrLoadPolicy::kBasic, elfldltl::SegmentWithVmo::NoCopy>;
  using MetadataMemory = elfldltl::LoadInfoMappedMemory<LoadInfo, elfldltl::MappedVmoFile>;
  using Loader = elfldltl::AlignedRemoteVmarLoader;

  // Information from decoding the main executable, specifically.
  struct ExecInfo {
    size_type relative_entry = 0;         // The file-relative entry point address.
    std::optional<size_type> stack_size;  // Requested initial stack size.
  };

  // The decode result of a single module, whether it's an executable or dependency.
  struct DecodeResult {
    std::vector<Soname> needed;  // Names of each DT_NEEDED entry for the module.
    // This information is only relevant for the main executable, and is copied
    // into the DecodeModulesResult that is returned to the caller.
    ExecInfo exec_info;
  };

  // The result returned to the caller after all modules have been decoded.
  struct DecodeModulesResult {
    List modules;        // The list of all decoded modules.
    ExecInfo main_exec;  // Decoded information for the main executable.
  };

  RemoteLoadModule() = default;

  RemoteLoadModule(RemoteLoadModule&&) = default;

  explicit RemoteLoadModule(const Soname& name) : RemoteLoadModuleBase{name} {}

  // Initialize the module from the provided VMO, representing either the
  // binary or shared library to be loaded.  Create the data structures that
  // make make the VMO readable, and scan and decode its phdrs to set and
  // return relevant information about the module to make it ready for
  // relocation and loading.  Return a `DecodeResult` containing information
  // about this module's dependencies.  In error cases, the error_value() is
  // the return value from the Diagnostics object.
  template <class Diagnostics>
  fit::result<bool, DecodeResult> Decode(Diagnostics& diag, zx::vmo vmo, uint32_t modid) {
    if (auto result = InitMappedVmo(diag, std::move(vmo)); result.is_error()) [[unlikely]] {
      return result.take_error();
    }

    // Get direct pointers to the file header and the program headers inside
    // the mapped file image.
    auto headers = elfldltl::LoadHeadersFromFile<elfldltl::Elf<>>(
        diag, mapped_vmo_, elfldltl::NoArrayFromFile<Phdr>{});
    if (!headers) [[unlikely]] {
      // TODO(mcgrathr): LoadHeadersFromFile doesn't propagate Diagnostics
      // return value on failure.
      return fit::error{true};
    }

    // Decode phdrs to fill LoadInfo, BuildId, etc.
    auto& [ehdr_owner, phdrs_owner] = *headers;
    const Ehdr& ehdr = ehdr_owner;
    const cpp20::span<const Phdr> phdrs = phdrs_owner;
    std::optional<elfldltl::ElfNote> build_id;
    auto result =
        DecodeModulePhdrs(diag, phdrs, load_info().GetPhdrObserver(Loader::page_size()),
                          elfldltl::PhdrFileNoteObserver(
                              elfldltl::Elf<>{}, mapped_vmo_, elfldltl::NoArrayFromFile<Phdr>{},
                              elfldltl::ObserveBuildIdNote(build_id, true)));
    if (!result) [[unlikely]] {
      // DecodeModulePhdrs only fails if Diagnostics said to give up.
      return fit::error{false};
    }

    auto [dyn_phdr, tls_phdr, stack_size] = *result;

    // After successfully decoding the phdrs, we may now instantiate the module
    // and set its fields.
    EmplaceModule(name(), modid);

    module().symbols_visible = true;

    if (build_id) {
      module().build_id = build_id->desc;
    }

    // Fix up segments to be compatible with AlignedRemoteVmarLoader.
    if (!elfldltl::SegmentWithVmo::AlignSegments(diag, load_info(), vmo_.borrow(),
                                                 Loader::page_size())) {
      // AlignSegments only fails if Diagnostics said to give up.
      return fit::error{false};
    }

    auto memory = metadata_memory();
    SetModulePhdrs(module(), ehdr, load_info(), memory);

    auto needed = DecodeDynamic(diag, dyn_phdr);
    if (!needed) {
      // TODO(mcgrathr): DecodeDynamic doesn't propagate Diagnostics
      // return value on failure.
      return fit::error{true};
    }

    return fit::ok(DecodeResult{
        .needed = *std::move(needed),
        .exec_info = {.relative_entry = ehdr.entry, .stack_size = stack_size},
    });
  }

  // Decode dynamic sections and store the metadata collected from observers.
  // A vector of each DT_NEEDED entry string name is returned.
  template <class Diagnostics>
  std::optional<std::vector<Soname>> DecodeDynamic(
      Diagnostics& diag, const std::optional<typename Elf::Phdr>& dyn_phdr) {
    static const constexpr std::string_view kCollectionError = "Failed to push value to container.";
    using NeededObserver = elfldltl::DynamicValueCollectionObserver<
        Elf, elfldltl::ElfDynTag::kNeeded,
        elfldltl::StdContainer<std::vector>::Container<size_type>, kCollectionError>;

    // It is not guaranteed that the symbol table has been scanned before every
    // DT_NEEDED entry, so create an observer to collect their offsets so that
    // the string name can be accessed after `DecodeModuleDynamic` returns.
    elfldltl::StdContainer<std::vector>::Container<size_type> needed_strtab_offsets;

    auto memory = metadata_memory();
    auto result =
        DecodeModuleDynamic(module(), diag, memory, dyn_phdr, NeededObserver(needed_strtab_offsets),
                            elfldltl::DynamicRelocationInfoObserver(reloc_info()));
    if (result.empty()) [[unlikely]] {
      return std::nullopt;
    }

    // Now that the symbol table has been decoded, we may access the names of
    // each offset entry.
    auto ok = true;
    std::vector<Soname> needed_names;
    needed_names.reserve(needed_strtab_offsets.size());
    std::transform(needed_strtab_offsets.begin(), needed_strtab_offsets.end(),
                   std::back_inserter(needed_names), [this, &diag, &ok](size_type offset) {
                     std::string_view name = symbol_info().string(offset);
                     if (name.empty()) {
                       ok = diag.FormatError("Invalid offset ", offset, " in DT_NEEDED entry");
                     }
                     return Soname{name};
                   });
    if (!ok) [[unlikely]] {
      return std::nullopt;
    }

    return needed_names;
  }

  // Decode the main executable VMO and all its dependencies. The `get_dep_vmo`
  // callback is used to retrieve the VMO for each DT_NEEDED entry; it takes a
  // `string_view` and should return a `zx::vmo`.
  template <class Diagnostics, typename GetDepVmo>
  static std::optional<DecodeModulesResult> DecodeModules(Diagnostics& diag,
                                                          zx::vmo main_executable_vmo,
                                                          GetDepVmo&& get_dep_vmo) {
    // Decode the main executable first and save its decoded information to
    // include in the result returned to the caller.
    RemoteLoadModule exec{abi::Abi<>::kExecutableName};
    auto exec_decode_result = exec.Decode(diag, std::move(main_executable_vmo), 0);
    if (exec_decode_result.is_error()) [[unlikely]] {
      return std::nullopt;
    }

    // The main executable will always be the first entry of the modules list.
    List modules = DecodeDeps(diag, std::move(exec), exec_decode_result->needed,
                              std::forward<GetDepVmo>(get_dep_vmo));
    if (modules.empty()) [[unlikely]] {
      return std::nullopt;
    }

    return DecodeModulesResult{
        .modules = std::move(modules),
        .main_exec = exec_decode_result->exec_info,
    };
  }

  // Initialize the the loader and allocate the address region for the module,
  // updating the module's runtime addr fields on success.
  template <class Diagnostics>
  bool Allocate(Diagnostics& diag, const zx::vmar& vmar) {
    loader_ = Loader{vmar};
    if (!loader_.Allocate(diag, load_info())) {
      return false;
    }
    SetModuleVaddrBounds(module(), load_info(), loader_.load_bias());
    return true;
  }

  template <class Diagnostics>
  static bool AllocateModules(Diagnostics& diag, List& modules, zx::unowned_vmar vmar) {
    auto allocate = [&diag, &vmar](auto& module) { return module.Allocate(diag, *vmar); };
    return OnModules(modules, allocate);
  }

  template <class Diagnostics>
  bool Relocate(Diagnostics& diag, const List& modules) {
    auto mutable_memory = elfldltl::LoadInfoMutableMemory{
        diag, load_info(), elfldltl::SegmentWithVmo::GetMutableMemory<LoadInfo>{vmo_.borrow()}};
    if (!mutable_memory.Init()) {
      return false;
    }
    if (!elfldltl::RelocateRelative(diag, mutable_memory, reloc_info(), load_bias())) {
      return false;
    }
    auto tlsdesc_resolver = [&diag](auto&&... args) {
      diag.FormatError("TODO(fxbug.dev/128502): remote TLSDESC not implemented yet");
      return TlsDescGot{};
    };
    auto resolver = elfldltl::MakeSymbolResolver(*this, modules, diag, tlsdesc_resolver);
    return elfldltl::RelocateSymbolic(mutable_memory, diag, reloc_info(), symbol_info(),
                                      load_bias(), resolver);
  }

  template <class Diagnostics>
  static bool RelocateModules(Diagnostics& diag, List& modules) {
    auto relocate = [&](auto& module) { return module.Relocate(diag, modules); };
    return OnModules(modules, relocate);
  }

  // Load the module into its allocated vaddr region.
  template <class Diagnostics>
  bool Load(Diagnostics& diag) {
    return loader_.Load(diag, load_info(), vmo_.borrow());
  }

  template <class Diagnostics>
  static bool LoadModules(Diagnostics& diag, List& modules) {
    auto load = [&diag](auto& module) { return module.Load(diag); };
    return OnModules(modules, load);
  }

  // This must be the last method called with the loader. Direct the loader to
  // preserve the load image before it is garbage collected.
  void Commit() { std::move(loader_).Commit(); }

  static void CommitModules(List& modules) {
    std::for_each(modules.begin(), modules.end(), [](auto& module) { module.Commit(); });
  }

 private:
  template <typename T>
  static bool OnModules(List& modules, T&& callback) {
    return std::all_of(modules.begin(), modules.end(), std::forward<T>(callback));
  }

  // Decode every transitive dependency module, yielding list in load order
  // starting with the main executable.  On failure this returns an empty List.
  // Otherwise the returned List::front() is always just main_exec moved into
  // place but the list may be longer.  If the Diagnostics object said to keep
  // going after an error, the returned list may be partial and the individual
  // entries may be partially decoded.  They should not be presumed complete,
  // such as calling module(), unless no errors were reported via Diagnostics.
  template <class Diagnostics, typename GetDepVmo>
  static List DecodeDeps(Diagnostics& diag, RemoteLoadModule main_exec,
                         const std::vector<Soname>& main_exec_needed, GetDepVmo&& get_dep_vmo) {
    // The list grows with enqueued DT_NEEDED dependencies of earlier elements.
    List modules;
    auto enqueue_deps = [&modules](const std::vector<Soname>& needed) {
      for (const Soname& soname : needed) {
        if (std::find(modules.begin(), modules.end(), soname) == modules.end()) {
          modules.emplace_back(soname);
        }
      }
    };

    // Start the list with the main executable, which already has module ID 0.
    // Each module's ID will be the same as its index in the list.
    assert(main_exec.HasModule());
    assert(main_exec.module().symbolizer_modid == 0);
    modules.emplace_back(std::move(main_exec));

    // First enqueue the executable's direct dependencies.
    enqueue_deps(main_exec_needed);

    // Now iterate over the queue remaining after the main executable itself,
    // adding indirect dependencies onto the end of the queue until the loop
    // has reached them all.  The total number of iterations is not known until
    // the loop terminates, every transitive dependency having been decoded.
    for (size_t idx = 1; idx < modules.size(); ++idx) {
      fit::result<bool, DecodeResult> decode_result = fit::error{false};

      {
        // The EnqueueDeps call below will extend the List (vector) and make
        // this reference invalid, so make it go out of scope before then.
        RemoteLoadModule& mod = modules[idx];

        // Only the main executable should already be decoded before this loop.
        assert(!mod.HasModule());

        auto vmo = get_dep_vmo(mod.name());
        if (!vmo) [[unlikely]] {
          // If the dep is not found, report the missing dependency, and defer
          // to the diagnostics policy on whether to continue processing.
          if (!diag.MissingDependency(mod.name().str())) {
            return {};
          }
          continue;
        }

        decode_result = mod.Decode(diag, std::move(vmo),
                                   // List index becomes symbolizer module ID.
                                   static_cast<uint32_t>(idx));
      }

      if (decode_result.is_error()) [[unlikely]] {
        if (decode_result.error_value()) {
          // Keep going to decode others, leaving this one undecoded.
          continue;
        }
        return {};
      }

      enqueue_deps(decode_result->needed);
    }

    return modules;
  }

  // Create and return a memory-adaptor object that serves as a wrapper
  // around this module's LoadInfo and MappedVmoFile. This is used to
  // translate vaddrs into file-relative offsets in order to read from the VMO.
  MetadataMemory metadata_memory() { return MetadataMemory{load_info(), mapped_vmo_}; }

  template <class Diagnostics>
  fit::result<bool> InitMappedVmo(Diagnostics& diag, zx::vmo vmo) {
    if (auto status = mapped_vmo_.Init(vmo.borrow()); status.is_error()) {
      return fit::error{diag.SystemError("cannot map VMO file for ", name(), " : ",
                                         elfldltl::ZirconError{status.status_value()})};
    }
    vmo_ = std::move(vmo);
    return fit::ok();
  }

  Loader loader_;
  elfldltl::MappedVmoFile mapped_vmo_;
  zx::vmo vmo_;
};

}  // namespace ld

#endif  // LIB_LD_REMOTE_LOAD_MODULE_H_
