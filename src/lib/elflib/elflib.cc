// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/elflib/elflib.h"

#include <zircon/assert.h>

#include <algorithm>
#include <string>

namespace elflib {
namespace {

// NT_GNU_BUILD_ID identifier.
constexpr uint64_t kNoteGnuBuildId = 3;

// Pull a null-terminated string out of an array of bytes at an offset. Returns
// empty string if there is no null terminator.
std::string GetNullTerminatedStringAt(const uint8_t* data, size_t data_length,
                                      size_t offset) {
  size_t check = offset;

  while (check < data_length && data[check]) {
    check++;
  }

  if (check >= data_length) {
    return std::string();
  }

  const char* start = reinterpret_cast<const char*>(data) + offset;

  return std::string(start);
}

// Given a name, a symbol table (sized array of Elf64_Sym), and an accessor for
// a corresponding string table, find the symbol with the given name.
const Elf64_Sym* GetSymbolFromTable(
    const std::string& name, const std::pair<const Elf64_Sym*, size_t>& symtab,
    std::function<std::optional<std::string>(uint64_t)> get_string) {
  if (!symtab.first) {
    return nullptr;
  }

  const Elf64_Sym* symbols = symtab.first;
  const Elf64_Sym* end = symtab.first + symtab.second;

  for (auto symbol = symbols; symbol <= end; symbol++) {
    auto got_name = get_string(symbol->st_name);

    if (got_name && *got_name == name) {
      return symbol;
    }
  }

  return nullptr;
}

std::optional<std::map<std::string, Elf64_Sym>> SymtabToMap(
    const std::pair<const Elf64_Sym*, size_t>& symtab,
    const ElfLib::MemoryRegion& strtab) {
  auto [symtab_ptr, symtab_size] = symtab;
  if (!symtab_ptr)
    return std::nullopt;

  std::map<std::string, Elf64_Sym> out;

  const Elf64_Sym* symbols = symtab_ptr;
  const Elf64_Sym* end = symtab_ptr + symtab_size;
  for (auto symbol = symbols; symbol != end; symbol++) {
    auto sym_name =
        GetNullTerminatedStringAt(strtab.ptr, strtab.size, symbol->st_name);
    out[sym_name] = *symbol;
  }

  return out;
}

}  // namespace

// Proxy object for whatever address space we're exploring.
class ElfLib::MemoryAccessor {
 public:
  virtual ~MemoryAccessor() = default;

  virtual const uint8_t* GetMemory(uint64_t mapped_address,
                                   size_t mapped_size) = 0;
};

ElfLib::ElfLib(std::unique_ptr<MemoryAccessor>&& memory,
               ElfLib::AddressMode address_mode)
    : address_mode_(address_mode), memory_(std::move(memory)) {}

ElfLib::~ElfLib() = default;

std::unique_ptr<ElfLib> ElfLib::Create(std::unique_ptr<MemoryAccessor>&& memory,
                                       ElfLib::AddressMode address_mode) {
  std::unique_ptr<ElfLib> out =
      std::make_unique<ElfLib>(std::move(memory), address_mode);

  auto header = reinterpret_cast<const Elf64_Ehdr*>(
      out->memory_->GetMemory(0, sizeof(Elf64_Ehdr)));

  if (!header) {
    return std::unique_ptr<ElfLib>();
  }

  out->header_ = *header;

  // Header magic should be correct.
  if (!std::equal(ElfMagic, ElfMagic + 4, out->header_.e_ident)) {
    return std::unique_ptr<ElfLib>();
  }

  // We only support 64-bit binaries.
  if (out->header_.e_ident[EI_CLASS] != ELFCLASS64) {
    return std::unique_ptr<ElfLib>();
  }

  const uint32_t kOne = 1;

  // Endianness of the file has to match the endianness of the host. To do the
  // endianness check, we snip the first byte off of a 4-byte word. If it
  // contains the LSB (a value of 1) we are on a little-endian machine.
  if (out->header_.e_ident[EI_DATA] == ELFDATA2MSB &&
      *reinterpret_cast<const char*>(&kOne)) {
    return std::unique_ptr<ElfLib>();
  }

  // Version field has only had one correct value for most of the life of the
  // spec.
  if (out->header_.e_ident[EI_VERSION] != EV_CURRENT) {
    return std::unique_ptr<ElfLib>();
  }

  if (out->header_.e_version != EV_CURRENT) {
    return std::unique_ptr<ElfLib>();
  }

  // We'll skip EI_OSABI and EI_ABIVERSION as well as e_machine and e_type. In
  // either case any valid value should be fine. We just don't screen for
  // invalid values.

  // We don't support non-standard section header sizes. Stripped binaries that
  // don't have sections sometimes zero out the shentsize, so we can ignore it
  // if we have no sections.
  if (out->header_.e_shnum > 0 &&
      out->header_.e_shentsize != sizeof(Elf64_Shdr)) {
    return std::unique_ptr<ElfLib>();
  }

  // We don't support non-standard program header sizes.
  if (out->header_.e_phentsize != sizeof(Elf64_Phdr)) {
    return std::unique_ptr<ElfLib>();
  }

  return out;
}

std::unique_ptr<ElfLib> ElfLib::Create(FILE* fp, ElfLib::Ownership owned) {
  class FileAccessor : public MemoryAccessor {
   public:
    FileAccessor(FILE* fp, bool take_ownership)
        : file_(fp), take_ownership_(take_ownership) {}

    ~FileAccessor() {
      if (file_ && take_ownership_) {
        fclose(file_);
      }
    }

    const uint8_t* GetMemory(uint64_t offset, size_t size) override {
      if (!file_) {
        return nullptr;
      }

      auto& ret = data_[std::make_pair(offset, size)];
      if (ret.size() == size) {
        return ret.data();
      }

      ret.resize(size);

      fseek(file_, offset, SEEK_SET);
      if (fread(ret.data(), 1, size, file_) != size) {
        return nullptr;
      }

      return ret.data();
    }

   private:
    FILE* file_ = nullptr;
    bool take_ownership_;

    // Elflib treats our read functions like they're random-access, so we cache
    // the results of reads. We could end up with overlaps in this map, though
    // in practice we don't in today's code.
    std::map<std::pair<uint64_t, size_t>, std::vector<uint8_t>> data_;
  };

  return Create(std::make_unique<FileAccessor>(
                    fp, owned == ElfLib::Ownership::kTakeOwnership),
                AddressMode::kFile);
}

std::unique_ptr<ElfLib> ElfLib::Create(const uint8_t* mem, size_t size) {
  class DataAccessor : public MemoryAccessor {
   public:
    DataAccessor(const uint8_t* mem, size_t size) : mem_(mem), size_(size) {}

    const uint8_t* GetMemory(uint64_t offset, size_t size) override {
      if (size + offset > size_) {
        return nullptr;
      }

      return mem_ + offset;
    }

   private:
    const uint8_t* mem_;
    size_t size_;
  };

  return Create(std::make_unique<DataAccessor>(mem, size), AddressMode::kFile);
}

std::unique_ptr<ElfLib> ElfLib::Create(
    std::function<bool(uint64_t, std::vector<uint8_t>*)> fetch,
    ElfLib::AddressMode address_mode) {
  class CallbackAccessor : public MemoryAccessor {
   public:
    CallbackAccessor(std::function<bool(uint64_t, std::vector<uint8_t>*)> fetch)
        : fetch_(fetch) {}

    const uint8_t* GetMemory(uint64_t offset, size_t size) override {
#ifndef NDEBUG
      auto iter = data_.upper_bound(offset);

      if (iter != data_.begin()) {
        --iter;
      }

      if (iter != data_.end() && iter->first <= offset &&
          iter->first + iter->second.size() > offset) {
        ZX_DEBUG_ASSERT(iter->first == offset && iter->second.size() == size);
      }
#endif  // NDEBUG
      for (const auto& range : data_[offset]) {
        if (range.size() >= size) {
          return range.data();
        }
      }

      auto& vec = data_[offset].emplace_back(size, 0);

      if (!fetch_(offset, &vec)) {
        return nullptr;
      }

      return vec.data();
    }

   private:
    std::function<bool(uint64_t, std::vector<uint8_t>*)> fetch_;
    std::map<uint64_t, std::vector<std::vector<uint8_t>>> data_;
  };

  return Create(std::make_unique<CallbackAccessor>(std::move(fetch)),
                address_mode);
}

std::unique_ptr<ElfLib> ElfLib::Create(const std::string& path) {
  return Create(fopen(path.c_str(), "r"), ElfLib::Ownership::kTakeOwnership);
}

bool ElfLib::SetDebugData(std::unique_ptr<ElfLib> debug) {
  if (debug_) {
    return false;
  }

  if (debug->debug_) {
    return false;
  }

  if (header_.e_shnum > 0) {
    return false;
  }

  debug_ = std::move(debug);

  debug_->LoadSectionNames();
  section_names_ = debug_->section_names_;
  sections_ = debug_->sections_;

  LoadProgramHeaders();
  std::map<size_t, size_t> bounds;
  for (size_t i = 0; i < segments_.size(); i++) {
    if (segments_[i].p_type != PT_LOAD) {
      continue;
    }

    bounds[segments_[i].p_vaddr] = i;
  }

  for (auto& section : sections_) {
    if (section.sh_type != SHT_NOBITS) {
      // When we encounter an SHT_NULL section and we have debug data, we'll
      // consult the debug data for that section.
      section.sh_type = SHT_NULL;
      continue;
    }

    // Find the first segment starting at or before this section by finding the
    // first segment starting at or after this section, and backing up one
    // entry if necessary.
    auto found = bounds.lower_bound(section.sh_addr);
    if (found == bounds.end()) {
      continue;
    }
    if (found->first != section.sh_addr) {
      if (found == bounds.begin()) {
        continue;
      }

      --found;
    }

    auto& segment = segments_[found->second];

    if (segment.p_vaddr + segment.p_memsz <= section.sh_addr) {
      continue;
    }

    section.sh_offset = segment.p_offset + (section.sh_addr - segment.p_vaddr);
    section.sh_type = SHT_PROGBITS;
  }

  return true;
}

const Elf64_Shdr* ElfLib::GetSectionHeader(size_t section) {
  // Processes may not map the section headers at all, so we don't look for
  // section headers unless we're in file mode.
  if (address_mode_ == AddressMode::kFile && sections_.empty()) {
    auto sections = reinterpret_cast<const Elf64_Shdr*>(memory_->GetMemory(
        header_.e_shoff, sizeof(Elf64_Shdr) * header_.e_shnum));

    if (!sections) {
      return nullptr;
    }

    std::copy(sections, sections + header_.e_shnum,
              std::back_inserter(sections_));
  }

  if (section >= sections_.size()) {
    return nullptr;
  }

  return &sections_[section];
}

bool ElfLib::LoadProgramHeaders() {
  if (!segments_.empty()) {
    return true;
  }

  auto segments = reinterpret_cast<const Elf64_Phdr*>(memory_->GetMemory(
      header_.e_phoff, sizeof(Elf64_Phdr) * header_.e_phnum));

  if (!segments) {
    return false;
  }

  std::copy(segments, segments + header_.e_phnum,
            std::back_inserter(segments_));
  return true;
}

const std::vector<Elf64_Phdr>& ElfLib::GetSegmentHeaders() {
  LoadProgramHeaders();
  return segments_;
}

ElfLib::MemoryRegion ElfLib::GetSegmentData(size_t segment) {
  LoadProgramHeaders();

  if (segment > segments_.size()) {
    return {};
  }

  const Elf64_Phdr* header = &segments_[segment];
  ElfLib::MemoryRegion result;

  if (address_mode_ == AddressMode::kFile) {
    result.ptr = memory_->GetMemory(header->p_offset, header->p_filesz);
    result.size = header->p_filesz;
  } else {
    result.ptr = memory_->GetMemory(header->p_vaddr, header->p_memsz);
    result.size = header->p_memsz;
  }

  return result;
}

std::optional<std::vector<uint8_t>> ElfLib::GetNote(const std::string& name,
                                                    uint64_t type) {
  LoadProgramHeaders();

  for (size_t idx = 0; idx < segments_.size(); idx++) {
    if (segments_[idx].p_type != PT_NOTE) {
      continue;
    }

    auto data = GetSegmentData(idx);

    const Elf64_Nhdr* header;
    size_t namesz_padded;
    size_t descsz_padded;

    for (const uint8_t* pos = data.ptr; pos < data.ptr + data.size;
         pos += sizeof(Elf64_Nhdr) + namesz_padded + descsz_padded) {
      header = reinterpret_cast<const Elf64_Nhdr*>(pos);
      namesz_padded = (header->n_namesz + 3) & ~3UL;
      descsz_padded = (header->n_descsz + 3) & ~3UL;

      if (header->n_type != type) {
        continue;
      }

      auto name_data = pos + sizeof(Elf64_Nhdr);
      std::string entry_name(reinterpret_cast<const char*>(name_data),
                             header->n_namesz - 1);

      if (entry_name == name) {
        auto desc_data = name_data + namesz_padded;

        return std::vector(desc_data, desc_data + header->n_descsz);
      }
    }
  }

  return std::nullopt;
}

std::string ElfLib::GetGNUBuildID() {
  auto note = GetNote("GNU", kNoteGnuBuildId);
  if (!note) {
    return std::string();
  }

  std::string ret;

  for (const auto& byte : *note) {
    char buf[3];
    snprintf(buf, 3, "%02x", byte);
    ret += buf;
  }

  return ret;
}

ElfLib::MemoryRegion ElfLib::GetSectionData(size_t section) {
  const Elf64_Shdr* header = GetSectionHeader(section);

  if (!header) {
    return {};
  }

  if (header->sh_type == SHT_NULL) {
    if (debug_) {
      return debug_->GetSectionData(section);
    }

    return {};
  }

  if (address_mode_ == AddressMode::kFile && header->sh_type == SHT_NOBITS) {
    return {};
  }

  ElfLib::MemoryRegion result;
  result.size = header->sh_size;

  if (address_mode_ == AddressMode::kFile) {
    result.ptr = memory_->GetMemory(header->sh_offset, header->sh_size);
  } else {
    result.ptr = memory_->GetMemory(header->sh_addr, header->sh_size);
  }

  return result;
}

bool ElfLib::LoadSectionNames() {
  if (section_names_.size() != 0) {
    return true;
  }

  auto section_name_data = GetSectionData(header_.e_shstrndx);

  if (!section_name_data.ptr) {
    return false;
  }

  size_t idx = 0;
  // We know sections_ is populated from the GetSectionData above
  for (const auto& section : sections_) {
    auto name = GetNullTerminatedStringAt(
        section_name_data.ptr, section_name_data.size, section.sh_name);
    section_names_[name] = idx;

    idx++;
  }

  return true;
}

ElfLib::MemoryRegion ElfLib::GetSectionData(const std::string& name) {
  if (!LoadSectionNames()) {
    return {};
  }

  const auto& iter = section_names_.find(name);

  if (iter == section_names_.end()) {
    return {};
  }

  return GetSectionData(iter->second);
}

bool ElfLib::LoadDynamicSymbols() {
  if (did_load_dynamic_symbols_) {
    return true;
  }

  did_load_dynamic_symbols_ = true;

  LoadProgramHeaders();

  for (size_t idx = 0; idx < segments_.size(); idx++) {
    if (segments_[idx].p_type != PT_DYNAMIC) {
      continue;
    }

    auto data = GetSegmentData(idx);

    if (!data.ptr) {
      return false;
    }

    const Elf64_Dyn* start = reinterpret_cast<const Elf64_Dyn*>(data.ptr);
    const Elf64_Dyn* end = start + (data.size / sizeof(Elf64_Dyn));

    for (auto dyn = start; dyn != end; dyn++) {
      if (dyn->d_tag == DT_STRTAB) {
        if (dynstr_.offset) {
          Warn("Multiple DT_STRTAB entries found.");
          continue;
        }

        dynstr_.offset = dyn->d_un.d_ptr;
      } else if (dyn->d_tag == DT_SYMTAB) {
        if (dynsym_.offset) {
          Warn("Multiple DT_SYMTAB entries found.");
          continue;
        }

        dynsym_.offset = dyn->d_un.d_ptr;
      } else if (dyn->d_tag == DT_STRSZ) {
        if (dynstr_.size) {
          Warn("Multiple DT_STRSZ entries found.");
          continue;
        }

        dynstr_.size = dyn->d_un.d_val;
      } else if (dyn->d_tag == DT_HASH) {
        // A note: The old DT_HASH style of hash table is considered legacy on
        // Fuchsia. Technically a binary could provide both styles of hash
        // table and we can produce a sane result in that case, so this code
        // ignores DT_HASH.
        Warn("Old style DT_HASH table found.");
      } else if (dyn->d_tag == DT_GNU_HASH) {
        if (dynsym_.size) {
          Warn("Multiple DT_GNU_HASH entries found.");
          continue;
        }
        auto addr = dyn->d_un.d_ptr;

        // Our elf header doesn't provide the DT_GNU_HASH header structure.
        struct Header {
          uint32_t nbuckets;
          uint32_t symoffset;
          uint32_t bloom_size;
          uint32_t bloom_shift;
        } header;

        static_assert(sizeof(Header) == 16);

        auto data = memory_->GetMemory(addr, sizeof(header));

        if (!data) {
          continue;
        }

        header = *reinterpret_cast<const Header*>(data);

        addr += sizeof(header);
        addr += 8 * header.bloom_size;

        size_t bucket_bytes = 4 * header.nbuckets;
        auto bucket_data = memory_->GetMemory(addr, bucket_bytes);

        if (!bucket_data) {
          continue;
        }

        const uint32_t* buckets =
            reinterpret_cast<const uint32_t*>(bucket_data);
        uint32_t max_bucket =
            *std::max_element(buckets, buckets + header.nbuckets);

        if (max_bucket < header.symoffset) {
          dynsym_.size = max_bucket;
          continue;
        }

        addr += bucket_bytes;
        addr += (max_bucket - header.symoffset) * 4;

        for (uint32_t nsyms = max_bucket + 1;; nsyms++, addr += 4) {
          auto chain_entry_data = memory_->GetMemory(addr, 4);

          if (!chain_entry_data) {
            break;
          }

          uint32_t chain_entry =
              *reinterpret_cast<const uint32_t*>(chain_entry_data);

          if (chain_entry & 1) {
            dynsym_.size = nsyms;
            break;
          }
        }
      } else if (dyn->d_tag == DT_PLTREL) {
        dynamic_plt_use_rela_ = dyn->d_un.d_val == DT_RELA;
      }
    }

    return true;
  }

  return false;
}

// A PltEntryBuffer is a PLT region - a sequence of PLT entries - with a mark
// somewhere in it.  The mark indicates the position of the next element to be
// read or written.
class ElfLib::PltEntryBuffer {
 public:
  PltEntryBuffer(ElfLib* lib)
      : ptr_(nullptr), end_(nullptr), start_(nullptr), size_(0), lib_(lib) {}
  virtual void SetRegion(const uint8_t* ptr, size_t size) {
    size_ = size;
    end_ = ptr + size;
    start_ = ptr + (IgnoredEntryCount() * EntrySize());
    ptr_ = start_;
  }

  // The number of bogus PLT entries at the beginning of the buffer.
  virtual int IgnoredEntryCount() const = 0;

  // The number of the element at this position.
  virtual uint32_t MarkIndex() const = 0;

  // Ensures that the PLT entry at the current mark is valid.
  virtual bool VerifyAtMark() const = 0;

  // Returns a value indicating whether the mark is currently in the region.
  bool MarkInBound() const {
    return ptr_ >= start_ && ptr_ + EntrySize() <= end_;
  }

  // Move the mark to the next position.  Also increments the index by one.
  void IncrementMark() { ptr_ += EntrySize(); }

  // The size of a PLT entry in this buffer.
  virtual size_t EntrySize() const = 0;

 protected:
  const uint8_t* ptr_;
  const uint8_t* end_;
  const uint8_t* start_;
  size_t size_;
  ElfLib* lib_;
};

class PltEntryBufferX86 : public ElfLib::PltEntryBuffer {
 public:
  explicit PltEntryBufferX86(ElfLib* lib) : PltEntryBuffer(lib) {}
  virtual uint32_t MarkIndex() const override { return GetPltPtr()->index; }
  virtual bool VerifyAtMark() const override {
    if (GetPltPtr()->push_opcode != 0x68) {
      lib_->Warn("Push OpCode not found where expected in PLT.");
      return false;
    }
    return true;
  }

  virtual size_t EntrySize() const override { return sizeof(PltEntryX86); }

  virtual int IgnoredEntryCount() const override { return 1; }

 private:
  // A PLT entry consists of 3 x86 instructions: a jump using a 6-byte
  // encoding, a push of one 32 bit value on to the stack, and another jump,
  // this one using a 5-byte encoding.
  //
  // We don't care about either of the jumps, but we want the value that is
  // pushed as it is the index into the relocation table which will tell us
  // what symbol this entry is for.
  struct PltEntryX86 {
    char first_jump[6];
    char push_opcode;
    uint32_t index;
    char second_jump[5];
  } __attribute__((packed, aligned(1)));

  static_assert(sizeof(PltEntryX86) == 16);

  const PltEntryX86* GetPltPtr() const {
    return reinterpret_cast<const PltEntryX86*>(ptr_);
  }
};

class PltEntryBufferArm : public ElfLib::PltEntryBuffer {
 public:
  explicit PltEntryBufferArm(ElfLib* lib) : PltEntryBuffer(lib) {}
  virtual uint32_t MarkIndex() const override {
    return (RawOffset() - rela_base_) / sizeof(uint64_t);
  }
  virtual bool VerifyAtMark() const override {
    // Prefix for adrp is 1??10000.
    if ((GetPltPtr()->adrp & 0x9F000000) != 0x90000000) {
      lib_->Warn("adrp OpCode not found where expected in PLT.");
      return false;
    }
    // Prefix for ldr immediate is 1?11100101
    if ((GetPltPtr()->ldr & 0xBFC00000) != 0xB9400000) {
      lib_->Warn("ldr OpCode not found where expected in PLT.");
      return false;
    }
    // Prefix for 64-bit add immediate is 10010001
    if ((GetPltPtr()->add & 0xFF800000) != 0x91000000) {
      lib_->Warn("add OpCode not found where expected in PLT.");
      return false;
    }
    // Prefix for br is 1101_0110_0001_1111_0000_00??_???0_0000
    if ((GetPltPtr()->br & 0xFFFFFC1F) != 0xD61F0000) {
      lib_->Warn("br OpCode not found where expected in PLT.");
      return false;
    }
    return true;
  }

  virtual size_t EntrySize() const override { return sizeof(PltEntryArm); }

  virtual int IgnoredEntryCount() const override { return 2; }

  virtual void SetRegion(const uint8_t* start, size_t size) override {
    // The offset of the first entry in the rela table.  There is really only
    // one PLT entry in the first 8 quadwords - the default / lookup entry. It
    // is not associated with a symbol, starts at offset 1, and points to the
    // base of the rela table. So, to find the base of the rela table, we set
    // ptr_ to be 1 offset from the start, read the base of the rela table out
    // of that PLT entry, and then immediately call SetRegion to set ptr_ to the
    // first PLT entry that is associated with a symbol.
    ptr_ = start + sizeof(uint32_t);
    rela_base_ = RawOffset() + sizeof(uint32_t);

    PltEntryBuffer::SetRegion(start, size);
  }

 private:
  // A PLT entry consists of 4 ARM instructions: an adrp that gives the page
  // containing the real location of the link target, a ldr giving the offset on
  // the page, an add to add the two, and a br that takes you to the loaded
  // target.
  //
  // We need the target at the offset of the LDR as it is the index into the
  // relocation table which will tell us what symbol this entry is for.
  struct PltEntryArm {
    uint32_t adrp;
    uint32_t ldr;
    uint32_t add;
    uint32_t br;
  } __attribute__((packed, aligned(1)));

  static_assert(sizeof(PltEntryArm) == 16);

  uint64_t RawOffset() const {
    // LDR with unsigned offset is encoded:
    // [size2 1 1 1 0 0 1 0 1 imm12 reg reg ]
    // The destination of the LDR is imm12 << size2
    uint32_t size = (0xC0000000 & GetPltPtr()->ldr) >> 30;
    uint32_t immediate_ldr = (0x3FFC00 & GetPltPtr()->ldr) >> 10;
    uint32_t offset = immediate_ldr << size;

    return offset;
  }

  const PltEntryArm* GetPltPtr() const {
    return reinterpret_cast<const PltEntryArm*>(ptr_);
  }

  uint64_t rela_base_;
};

std::map<std::string, uint64_t> ElfLib::GetPLTOffsets() {
  // We assume Fuchsia's defaults for each architecture. We could perhaps check
  // ELF_OSABI to firm up those assumptions. Fuchsia sets it to NONE.
  switch (header_.e_machine) {
    case EM_X86_64: {
      PltEntryBufferX86 pos(this);
      return GetPLTOffsetsCommon(pos);
    }
    case EM_AARCH64: {
      PltEntryBufferArm pos(this);
      return GetPLTOffsetsCommon(pos);
    }
    default:
      Warn("Architecture doesn't support GetPLTOffsets.");
      return {};
  }
}

std::map<std::string, uint64_t> ElfLib::GetPLTOffsetsCommon(
    PltEntryBuffer& buffer) {
  // We'd prefer if this works but we can get by without it, so we're not
  // checking the return value.
  LoadDynamicSymbols();

  if (!LoadSectionNames()) {
    return {};
  }

  if (!dynamic_plt_use_rela_) {
    Warn("Assuming Elf64_Rela PLT relocation format.");
  } else if (!*dynamic_plt_use_rela_) {
    Warn("Elf64_Rel style PLT Relocations unsupported.");
    return {};
  }

  auto plt_section = section_names_.find(".plt");

  if (plt_section == section_names_.end()) {
    return {};
  }

  auto plt_idx = plt_section->second;

  auto plt_shdr = GetSectionHeader(plt_idx);
  auto plt_memory = GetSectionData(plt_idx);

  if (!plt_shdr || !plt_memory.ptr) {
    return {};
  }

  auto plt_load_addr = plt_shdr->sh_addr;

  buffer.SetRegion(plt_memory.ptr, plt_memory.size);

  auto reloc_memory = GetSectionData(".rela.plt");

  if (!reloc_memory.ptr) {
    return {};
  }

  auto reloc = reinterpret_cast<const Elf64_Rela*>(reloc_memory.ptr);
  auto reloc_count = reloc_memory.size / sizeof(Elf64_Rela);

  ElfLib::MemoryRegion dynsym_mem = GetSectionData(".dynsym");

  if (!dynsym_mem.ptr) {
    return {};
  }

  auto symtab = reinterpret_cast<const Elf64_Sym*>(dynsym_mem.ptr);
  auto sym_count = dynsym_mem.size / sizeof(Elf64_Sym);

  ElfLib::MemoryRegion dynstr_mem = GetSectionData(".dynstr");

  if (!dynstr_mem.ptr) {
    return {};
  }

  uint64_t idx = buffer.IgnoredEntryCount();

  std::map<std::string, uint64_t> ret;

  for (; buffer.MarkInBound(); buffer.IncrementMark(), idx++) {
    if (!buffer.VerifyAtMark()) {
      continue;
    }

    uint32_t index = buffer.MarkIndex();
    if (index >= reloc_count) {
      Warn("PLT referenced reloc outside reloc table.");
      continue;
    }

    auto sym_idx = reloc[index].getSymbol();

    if (sym_idx >= sym_count) {
      Warn("PLT reloc referenced symbol outside symbol table.");
      continue;
    }

    auto name = GetNullTerminatedStringAt(dynstr_mem.ptr, dynstr_mem.size,
                                          symtab[sym_idx].st_name);

    if (!name.size()) {
      Warn("PLT symbol name could not be retrieved.");
      continue;
    }

    ret[name] = idx * buffer.EntrySize() + plt_load_addr;
  }

  return ret;
}

std::optional<std::string> ElfLib::GetDynamicString(size_t offset) {
  if (!LoadDynamicSymbols() || !dynstr_.IsValid()) {
    return std::nullopt;
  }

  auto data = memory_->GetMemory(*dynstr_.offset, *dynstr_.size);

  if (!data) {
    return std::nullopt;
  }

  return GetNullTerminatedStringAt(data, *dynstr_.size, offset);
}

std::optional<std::string> ElfLib::GetString(size_t offset) {
  auto string_data = GetSectionData(".strtab");

  if (!string_data.ptr) {
    return std::nullopt;
  }

  return GetNullTerminatedStringAt(string_data.ptr, string_data.size, offset);
}

std::pair<const Elf64_Sym*, size_t> ElfLib::GetSymtab() {
  ElfLib::MemoryRegion symtab = GetSectionData(".symtab");

  if (symtab.ptr) {
    const Elf64_Sym* symbols = reinterpret_cast<const Elf64_Sym*>(symtab.ptr);

    return std::make_pair(symbols, symtab.size / sizeof(Elf64_Sym));
  }

  return std::make_pair(nullptr, 0);
}

std::pair<const Elf64_Sym*, size_t> ElfLib::GetDynamicSymtab() {
  if (!LoadDynamicSymbols()) {
    return std::make_pair(nullptr, 0);
  }

  if (!dynsym_.IsValid()) {
    return std::make_pair(nullptr, 0);
  }

  auto memory =
      memory_->GetMemory(*dynsym_.offset, *dynsym_.size * sizeof(Elf64_Sym));

  return std::make_pair(reinterpret_cast<const Elf64_Sym*>(memory),
                        *dynsym_.size);
}

const Elf64_Sym* ElfLib::GetSymbol(const std::string& name) {
  return GetSymbolFromTable(name, GetSymtab(),
                            [this](uint64_t idx) { return GetString(idx); });
}

const Elf64_Sym* ElfLib::GetDynamicSymbol(const std::string& name) {
  return GetSymbolFromTable(name, GetDynamicSymtab(), [this](uint64_t idx) {
    return GetDynamicString(idx);
  });
}

std::optional<std::map<std::string, Elf64_Sym>> ElfLib::GetAllSymbols() {
  return SymtabToMap(GetSymtab(), GetSectionData(".strtab"));
}

std::optional<std::map<std::string, Elf64_Sym>> ElfLib::GetAllDynamicSymbols() {
  if (!LoadDynamicSymbols() || !dynstr_.IsValid()) {
    return std::nullopt;
  }

  return SymtabToMap(GetDynamicSymtab(),
                     ElfLib::MemoryRegion{.ptr = memory_->GetMemory(
                                              *dynstr_.offset, *dynstr_.size),
                                          .size = *dynstr_.size});
}

}  // namespace elflib
