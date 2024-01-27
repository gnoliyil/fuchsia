// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <elf-search.h>
#include <elf.h>
#include <inttypes.h>
#include <lib/elfldltl/constants.h>
#include <lib/elfldltl/container.h>
#include <lib/elfldltl/diagnostics.h>
#include <lib/elfldltl/load.h>
#include <lib/elfldltl/machine.h>
#include <lib/elfldltl/vmar-loader.h>
#include <lib/elfldltl/vmo.h>
#include <lib/elfldltl/zircon.h>
#include <lib/fit/defer.h>
#include <lib/zx/job.h>
#include <lib/zx/port.h>
#include <lib/zx/process.h>
#include <lib/zx/time.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <stdio.h>
#include <stdlib.h>
#include <zircon/status.h>
#include <zircon/syscalls/port.h>

#include <algorithm>
#include <iterator>
#include <string>

#include <fbl/vector.h>
#include <test-utils/test-utils.h>
#include <zxtest/zxtest.h>

namespace {

void WriteHeaders(const cpp20::span<const Elf64_Phdr>& phdrs, const zx::vmo& vmo) {
  const Elf64_Ehdr ehdr = {
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc99-designator"
#endif
    .e_ident =
        {
            [EI_MAG0] = ELFMAG0,
            [EI_MAG1] = ELFMAG1,
            [EI_MAG2] = ELFMAG2,
            [EI_MAG3] = ELFMAG3,
            [EI_CLASS] = ELFCLASS64,
            [EI_DATA] = ELFDATA2LSB,
            [EI_VERSION] = EV_CURRENT,
            [EI_OSABI] = ELFOSABI_NONE,
        },
#if defined(__clang__)
#pragma GCC diagnostic pop
#endif
    .e_type = ET_DYN,
    .e_machine = static_cast<uint16_t>(elfldltl::ElfMachine::kNative),
    .e_version = EV_CURRENT,
    .e_entry = 0,
    .e_phoff = sizeof(Elf64_Ehdr),
    .e_shoff = 0,
    .e_flags = 0,
    .e_ehsize = sizeof(Elf64_Ehdr),
    .e_phentsize = sizeof(Elf64_Phdr),
    .e_phnum = static_cast<Elf64_Half>(phdrs.size()),
    .e_shentsize = 0,
    .e_shnum = 0,
    .e_shstrndx = 0,
  };
  EXPECT_OK(vmo.write(&ehdr, 0, sizeof(ehdr)));
  EXPECT_OK(vmo.write(phdrs.data(), sizeof(ehdr), sizeof(Elf64_Phdr) * phdrs.size()));
}

// TODO(jakehehrlich): Switch all uses of uint8_t to std::byte once libc++ lands.

void WriteBuildID(cpp20::span<const uint8_t> build_id, const zx::vmo& vmo, uint64_t note_offset) {
  uint8_t buf[64];
  const Elf64_Nhdr nhdr = {
      .n_namesz = sizeof(ELF_NOTE_GNU),
      .n_descsz = static_cast<Elf64_Word>(build_id.size()),
      .n_type = NT_GNU_BUILD_ID,
  };
  ASSERT_GT(sizeof(buf), sizeof(nhdr) + sizeof(ELF_NOTE_GNU) + build_id.size());
  uint64_t note_size = 0;
  memcpy(buf + note_size, &nhdr, sizeof(nhdr));
  note_size += sizeof(nhdr);
  memcpy(buf + note_size, ELF_NOTE_GNU, sizeof(ELF_NOTE_GNU));
  note_size += sizeof(ELF_NOTE_GNU);
  memcpy(buf + note_size, build_id.data(), build_id.size());
  note_size += build_id.size();
  EXPECT_OK(vmo.write(buf, note_offset, note_size));
}

struct Module {
  std::string_view name;
  cpp20::span<const Elf64_Phdr> phdrs;
  cpp20::span<const uint8_t> build_id;
  zx::vmo vmo;
};

void MakeELF(Module* mod) {
  size_t size = 0;
  for (const auto& phdr : mod->phdrs) {
    size = std::max(size, phdr.p_offset + phdr.p_filesz);
  }
  ASSERT_OK(zx::vmo::create(size, 0, &mod->vmo));
  EXPECT_OK(mod->vmo.set_property(ZX_PROP_NAME, mod->name.data(), mod->name.size()));
  EXPECT_OK(mod->vmo.replace_as_executable(zx::resource(), &mod->vmo));
  ASSERT_NO_FATAL_FAILURE(WriteHeaders(mod->phdrs, mod->vmo));
  for (const auto& phdr : mod->phdrs) {
    if (phdr.p_type == PT_NOTE) {
      ASSERT_NO_FATAL_FAILURE(WriteBuildID(mod->build_id, mod->vmo, phdr.p_offset));
    }
  }
}

constexpr Elf64_Phdr MakePhdr(uint32_t type, uint64_t size, uint64_t offset, uint64_t addr,
                              uint32_t flags, uint32_t align) {
  return Elf64_Phdr{
      .p_type = type,
      .p_flags = flags,
      .p_offset = offset,
      .p_vaddr = addr,
      .p_paddr = addr,
      .p_filesz = size,
      .p_memsz = size,
      .p_align = align,
  };
}

void GetKoid(const zx::vmo& obj, zx_koid_t* out) {
  zx_info_handle_basic_t info;
  ASSERT_OK(obj.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));
  *out = info.koid;
}

zx_status_t LoadElf(const zx::vmar& vmar, const zx::vmo& vmo, uintptr_t& base, uintptr_t& entry) {
  // Ignore any error details, but capture the zx_status_t of a SystemError.
  // Tell the toolkit code to keep going after a SystemError if possible.  No
  // other kinds of errors should be possible since those would indicate an
  // invalid ELF image.
  zx_status_t status = ZX_OK;
  auto report = [&status](auto&&... args) -> bool {
    auto check = [&status](auto arg) -> bool {
      if constexpr (std::is_same_v<decltype(arg), elfldltl::ZirconError>) {
        status = arg.status;
        return true;
      }
      return false;
    };
    return (check(args) || ...);
  };
  auto diag = elfldltl::Diagnostics(report, elfldltl::DiagnosticsPanicFlags());

  elfldltl::UnownedVmoFile file(vmo.borrow(), diag);
  elfldltl::LoadInfo<elfldltl::Elf<>, elfldltl::StdContainer<std::vector>::Container> load_info;
  elfldltl::RemoteVmarLoader loader{vmar};
  if (auto headers = elfldltl::LoadHeadersFromFile<elfldltl::Elf<>>(
          diag, file, elfldltl::NewArrayFromFile<elfldltl::Elf<>::Phdr>())) {
    auto& [ehdr, phdrs_result] = *headers;
    cpp20::span<const elfldltl::Elf<>::Phdr> phdrs = phdrs_result.get();
    if (elfldltl::DecodePhdrs(diag, phdrs, load_info.GetPhdrObserver(loader.page_size())) &&
        loader.Load(diag, load_info, vmo.borrow())) {
      ZX_ASSERT(status == ZX_OK);
      ZX_ASSERT(load_info.vaddr_start() == 0);
      base = loader.load_bias();
      entry = ehdr.entry + loader.load_bias();
      std::move(loader).Commit();
      return ZX_OK;
    }
  }

  ZX_ASSERT(status != ZX_OK);
  return status;
}

// TODO(jakehehrlich): Not all error cases are tested. Appropriate tests can be
// sussed out by looking at coverage results.
TEST(ElfSearchTest, ForEachModule) {
  // Define some dummy modules.
  constexpr Elf64_Phdr mod0_phdrs[] = {
      MakePhdr(PT_LOAD, 0x2000, 0, 0, PF_R, 0x1000), MakePhdr(PT_NOTE, 20, 0x1000, 0x1000, PF_R, 4),
      MakePhdr(PT_LOAD, 0x1000, 0x2000, 0x2000, PF_R | PF_W, 0x1000),
      MakePhdr(PT_LOAD, 0x1000, 0x3000, 0x3000, PF_R | PF_X, 0x1000)};
  constexpr uint8_t mod0_build_id[] = {0xde, 0xad, 0xbe, 0xef};
  constexpr Elf64_Phdr mod1_phdrs[] = {
      MakePhdr(PT_LOAD, 0x2000, 0x0000, 0x0000, PF_R, 0x1000),
      MakePhdr(PT_NOTE, 20, 0x1000, 0x1000, PF_R, 4),
      MakePhdr(PT_LOAD, 0x1000, 0x2000, 0x2000, PF_R | PF_X, 0x1000)};
  constexpr uint8_t mod1_build_id[] = {0xff, 0xff, 0xff, 0xff};
  constexpr Elf64_Phdr mod2_phdrs[] = {MakePhdr(PT_LOAD, 0x2000, 0x0000, 0x0000, PF_R, 0x1000),
                                       MakePhdr(PT_NOTE, 20, 0x1000, 0x1000, PF_R, 4)};
  constexpr uint8_t mod2_build_id[] = {0x00, 0x00, 0x00, 0x00};
  constexpr Elf64_Phdr mod3_phdrs[] = {MakePhdr(PT_LOAD, 0x2000, 0, 0, PF_R, 0x1000),
                                       MakePhdr(PT_NOTE, 20, 0x1000, 0x1000, PF_R, 4),
                                       MakePhdr(PT_DYNAMIC, 0x800, 0x1800, 0x1800, PF_R, 4)};
  constexpr uint8_t mod3_build_id[] = {0x12, 0x34, 0x56, 0x78};
  constexpr Elf64_Dyn mod3_dyns[] = {{DT_STRTAB, {0x1900}}, {DT_SONAME, {1}}, {DT_NULL, {}}};
  constexpr const char* mod3_soname = "soname";
  // mod4 has `-z noseparate-code`, i.e., multiple PT_LOAD segments live on the same page.
  constexpr Elf64_Phdr mod4_phdrs[] = {MakePhdr(PT_LOAD, 0x950, 0, 0, PF_R, 0x1000),
                                       MakePhdr(PT_LOAD, 0x2b0, 0x950, 0x1950, PF_R | PF_X, 0x1000),
                                       MakePhdr(PT_LOAD, 0x258, 0xc00, 0x2c00, PF_R | PF_W, 0x1000),
                                       MakePhdr(PT_NOTE, 20, 0x270, 0x270, PF_R, 4)};
  constexpr uint8_t mod4_build_id[] = {0x44, 0x33, 0x22, 0x11};
  Module mods[] = {
      {"mod0", mod0_phdrs, mod0_build_id, {}}, {"mod1", mod1_phdrs, mod1_build_id, {}},
      {"mod2", mod2_phdrs, mod2_build_id, {}}, {"mod3", mod3_phdrs, mod3_build_id, {}},
      {"mod4", mod4_phdrs, mod4_build_id, {}},
  };

  // Create the test process using the Launcher service, which has the proper clearance to spawn new
  // processes. This has the side effect of loading in the VDSO and dynamic linker, which are
  // explicitly ignored below.
  const char* file = "bin/elf-search-test-helper";
  const char* root_dir = getenv("TEST_ROOT_DIR");
  // When running as a component, TEST_ROOT_DIR is not set and should be "/pkg".
  if (!root_dir) {
    root_dir = "/pkg";
  }
  std::string helper = std::string(root_dir) + "/" + file;
  const char* argv[] = {helper.c_str()};
  springboard_t* sb =
      tu_launch_init(ZX_HANDLE_INVALID, "mod-test", 1, argv, 0, nullptr, 0, nullptr, nullptr);
  auto delete_sb = fit::defer([=] { tu_launch_abort(sb); });
  zx::vmar vmar{springboard_get_root_vmar_handle(sb)};

  uintptr_t base, entry;
  for (auto& mod : mods) {
    ASSERT_NO_FATAL_FAILURE(MakeELF(&mod));
    if (mod.name == "mod3") {
      // Handle mod3's string table.
      EXPECT_OK(mods[3].vmo.write(&mod3_dyns, 0x1800, sizeof(mod3_dyns)));
      EXPECT_OK(mods[3].vmo.write(mod3_soname, 0x1901, strlen(mod3_soname) + 1));
    }
    ASSERT_OK(LoadElf(vmar, mod.vmo, base, entry), "Unable to load extra ELF");
  }
  zx::process process;
  EXPECT_NE(ZX_HANDLE_INVALID,
            *process.reset_and_get_address() = springboard_get_process_handle(sb));
  auto cleanup = fit::defer([&]() { process.kill(); });

  // These modules appear in the list as they are the mimimum possible set of mappings that a
  // process can be spawned with using fuchsia.process.Launcher, which tu_launch_init relies on.
  const char* ignored_mods[] = {
      // The dynamic linker, a.k.a. ld.so.1 in packages.
      "libc.so",
      // The VDSO.
      "libzircon.so",
  };

  // Now loop though everything, checking module info along the way.
  uint32_t matchCount = 0, moduleCount = 0;
  zx_status_t status = elf_search::ForEachModule(process, [&](const elf_search::ModuleInfo& info) {
    for (const auto& mod : ignored_mods) {
      if (info.name == mod) {
        return;
      }
    }
    ++moduleCount;
    for (const auto& mod : mods) {
      if (mod.build_id.size() == info.build_id.size() &&
          std::equal(mod.build_id.begin(), mod.build_id.end(), info.build_id.begin())) {
        ++matchCount;
        char name[ZX_MAX_NAME_LEN];
        zx_koid_t vmo_koid = 0;
        ASSERT_NO_FATAL_FAILURE(GetKoid(mod.vmo, &vmo_koid));
        if (mod.name != "mod3") {
          snprintf(name, sizeof(name), "<VMO#%" PRIu64 "=%.*s>", vmo_koid,
                   static_cast<int>(mod.name.size()), mod.name.data());
        } else {
          snprintf(name, sizeof(name), "%s", mod3_soname);
        }
        EXPECT_STREQ(info.name, name);
        EXPECT_EQ(mod.phdrs.size(), info.phdrs.size(), "expected same number of phdrs");
      }
    }
    EXPECT_EQ(moduleCount, matchCount, "Build for module was not found.");
  });
  EXPECT_OK(status);
  EXPECT_EQ(moduleCount, std::size(mods), "Unexpected number of modules found.");
}

}  // namespace
