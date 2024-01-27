// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Parses ELF files per the ELF specification in ulib/musl/include/elf.h
use {
    bitflags::bitflags,
    fuchsia_zircon as zx,
    num_derive::FromPrimitive,
    num_traits::cast::FromPrimitive,
    static_assertions::assert_eq_size,
    std::fmt,
    std::mem,
    thiserror::Error,
    zerocopy::{AsBytes, FromBytes, FromZeroes},
};

/// Possible errors that can occur during ELF parsing.
#[derive(Error, Debug)]
pub enum ElfParseError {
    #[error("Failed to read ELF from VMO: {}", _0)]
    ReadError(zx::Status),
    #[error("Parse error: {}", _0)]
    ParseError(&'static str),
    #[error("Invalid ELF file header: {}", _0)]
    InvalidFileHeader(&'static str),
    #[error("Invalid ELF program header: {}", _0)]
    InvalidProgramHeader(&'static str),
    #[error("Multiple ELF program headers of type {} present", _0)]
    MultipleHeaders(SegmentType),
}

impl ElfParseError {
    /// Returns an appropriate zx::Status code for the given error.
    pub fn as_zx_status(&self) -> zx::Status {
        match self {
            ElfParseError::ReadError(s) => *s,
            // Not a great status to return for an invalid ELF but there's no great fit, and this
            // matches elf_load.
            ElfParseError::ParseError(_)
            | ElfParseError::InvalidFileHeader(_)
            | ElfParseError::InvalidProgramHeader(_) => zx::Status::NOT_FOUND,
            ElfParseError::MultipleHeaders(_) => zx::Status::NOT_FOUND,
        }
    }
}

trait Validate {
    fn validate(&self) -> Result<(), ElfParseError>;
}

/// ELF identity header.
#[derive(FromZeroes, FromBytes, AsBytes, Debug, Eq, PartialEq, Default, Clone, Copy)]
#[repr(C)]
pub struct ElfIdent {
    /// e_ident[EI_MAG0:EI_MAG3]
    pub magic: [u8; 4],
    /// e_ident[EI_CLASS]
    pub class: u8,
    /// e_ident[EI_DATA]
    pub data: u8,
    /// e_ident[EI_VERSION]
    pub version: u8,
    /// e_ident[EI_OSABI]
    pub osabi: u8,
    /// e_ident[EI_ABIVERSION]
    pub abiversion: u8,
    /// e_ident[EI_PAD]
    pub pad: [u8; 7],
}

#[allow(unused)]
const EI_NIDENT: usize = 16;
assert_eq_size!(ElfIdent, [u8; EI_NIDENT]);

/// ELF class, from EI_CLASS.
#[derive(FromPrimitive, Eq, PartialEq)]
#[repr(u8)]
pub enum ElfClass {
    /// ELFCLASSNONE
    Unknown = 0,
    /// ELFCLASS32
    Elf32 = 1,
    /// ELFCLASS64
    Elf64 = 2,
}

/// ELF data encoding, from EI_DATA.
#[derive(FromPrimitive, Eq, PartialEq)]
#[repr(u8)]
pub enum ElfDataEncoding {
    /// ELFDATANONE
    Unknown = 0,
    /// ELFDATA2LSB
    LittleEndian = 1,
    /// ELFDATA2MSB
    BigEndian = 2,
}

/// ELF version, from EI_VERSION.
#[derive(FromPrimitive, Eq, PartialEq)]
#[repr(u8)]
pub enum ElfVersion {
    /// EV_NONE
    Unknown = 0,
    /// EV_CURRENT
    Current = 1,
}

impl ElfIdent {
    pub fn class(&self) -> Result<ElfClass, u8> {
        ElfClass::from_u8(self.class).ok_or(self.class)
    }

    pub fn data(&self) -> Result<ElfDataEncoding, u8> {
        ElfDataEncoding::from_u8(self.data).ok_or(self.data)
    }

    pub fn version(&self) -> Result<ElfVersion, u8> {
        ElfVersion::from_u8(self.version).ok_or(self.version)
    }
}

#[derive(FromZeroes, FromBytes, AsBytes, Debug, Eq, PartialEq, Default, Clone, Copy)]
#[repr(C)]
pub struct Elf64FileHeader {
    pub ident: ElfIdent,
    pub elf_type: u16,
    pub machine: u16,
    pub version: u32,
    pub entry: usize,
    pub phoff: usize,
    pub shoff: usize,
    pub flags: u32,
    pub ehsize: u16,
    pub phentsize: u16,
    pub phnum: u16,
    pub shentsize: u16,
    pub shnum: u16,
    pub shstrndx: u16,
}

#[derive(FromPrimitive, Copy, Clone, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum ElfType {
    /// ET_NONE
    Unknown = 0,
    /// ET_REL
    Relocatable = 1,
    /// ET_EXEC
    Executable = 2,
    /// ET_DYN
    SharedObject = 3,
    /// ET_CORE
    Core = 4,
}

#[derive(FromPrimitive, Copy, Clone, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum ElfArchitecture {
    /// EM_NONE
    Unknown = 0,
    /// EM_386
    I386 = 3,
    /// EM_ARM
    ARM = 40,
    /// EM_X86_64
    X86_64 = 62,
    /// EM_AARCH64
    AARCH64 = 183,
    /// EM_RISCV
    RISCV = 243,
}

pub const ELF_MAGIC: [u8; 4] = *b"\x7fELF";

#[cfg(target_endian = "little")]
pub const NATIVE_ENCODING: ElfDataEncoding = ElfDataEncoding::LittleEndian;
#[cfg(target_endian = "big")]
pub const NATIVE_ENCODING: ElfDataEncoding = ElfDataEncoding::BigEndian;

#[cfg(target_arch = "x86_64")]
pub const CURRENT_ARCH: ElfArchitecture = ElfArchitecture::X86_64;
#[cfg(target_arch = "aarch64")]
pub const CURRENT_ARCH: ElfArchitecture = ElfArchitecture::AARCH64;
#[cfg(target_arch = "riscv64")]
pub const CURRENT_ARCH: ElfArchitecture = ElfArchitecture::RISCV;

impl Elf64FileHeader {
    pub fn elf_type(&self) -> Result<ElfType, u16> {
        ElfType::from_u16(self.elf_type).ok_or(self.elf_type)
    }

    pub fn machine(&self) -> Result<ElfArchitecture, u16> {
        ElfArchitecture::from_u16(self.machine).ok_or(self.machine)
    }
}

impl Validate for Elf64FileHeader {
    fn validate(&self) -> Result<(), ElfParseError> {
        if self.ident.magic != ELF_MAGIC {
            return Err(ElfParseError::InvalidFileHeader("Invalid ELF magic"));
        }
        if self.ident.class() != Ok(ElfClass::Elf64) {
            return Err(ElfParseError::InvalidFileHeader("Invalid ELF class"));
        }
        if self.ident.data() != Ok(NATIVE_ENCODING) {
            return Err(ElfParseError::InvalidFileHeader("Invalid ELF data encoding"));
        }
        if self.ident.version() != Ok(ElfVersion::Current) {
            return Err(ElfParseError::InvalidFileHeader("Invalid ELF version"));
        }
        if self.phentsize as usize != mem::size_of::<Elf64ProgramHeader>() {
            return Err(ElfParseError::InvalidFileHeader("Invalid ELF program header size"));
        }
        if self.phnum == std::u16::MAX {
            return Err(ElfParseError::InvalidFileHeader(
                "2^16 or more ELF program headers is unsupported",
            ));
        }
        if self.machine() != Ok(CURRENT_ARCH) {
            return Err(ElfParseError::InvalidFileHeader("Invalid ELF architecture"));
        }
        if self.elf_type() != Ok(ElfType::SharedObject)
            && self.elf_type() != Ok(ElfType::Executable)
        {
            return Err(ElfParseError::InvalidFileHeader(
                "Invalid or unsupported ELF type, only ET_DYN is supported",
            ));
        }
        Ok(())
    }
}

#[derive(FromZeroes, FromBytes, AsBytes, Debug, Eq, PartialEq, Default, Clone, Copy)]
#[repr(C)]
pub struct Elf64ProgramHeader {
    pub segment_type: u32,
    pub flags: u32,
    pub offset: usize,
    pub vaddr: usize,
    pub paddr: usize,
    pub filesz: u64,
    pub memsz: u64,
    pub align: u64,
}

#[derive(FromPrimitive, Debug, Eq, PartialEq)]
#[repr(u64)]
pub enum Elf64DynTag {
    Null = 0,
    Needed = 1,
    Pltrelsz = 2,
    Pltgot = 3,
    Hash = 4,
    Strtab = 5,
    Symtab = 6,
    Rela = 7,
    Relasz = 8,
    Relaent = 9,
    Strsz = 10,
    Syment = 11,
    Init = 12,
    Fini = 13,
    Soname = 14,
    Rpath = 15,
    Symbolic = 16,
    Rel = 17,
    Relsz = 18,
    Relent = 19,
    Pltrel = 20,
    Debug = 21,
    Textrel = 22,
    Jmprel = 23,
    BindNow = 24,
    InitArray = 25,
    FiniArray = 26,
    InitArraysz = 27,
    FiniArraysz = 28,
    Runpath = 29,
    Flags = 30,
    PreinitArray = 32,
    PreinitArraysz = 33,
    Loos = 0x6000000D,
    Hios = 0x6ffff000,
    Loproc = 0x70000000,
    Hiproc = 0x7fffffff,
}

#[derive(AsBytes, Copy, Clone, FromBytes, FromZeroes, Default, Debug, Eq, PartialEq)]
#[repr(C)]
pub struct Elf64Dyn {
    pub tag: u64,
    pub value: u64,
}

impl Elf64Dyn {
    pub fn tag(&self) -> Result<Elf64DynTag, u64> {
        Elf64DynTag::from_u64(self.tag).ok_or(self.tag)
    }
}

pub type Elf64Addr = u64;
pub type Elf64Half = u16;
pub type Elf64Word = u32;
pub type Elf64Xword = u64;

#[repr(C)]
#[derive(Debug, Default, Copy, Clone, AsBytes, FromBytes, FromZeroes)]
pub struct elf64_sym {
    pub st_name: Elf64Word,
    pub st_info: u8,
    pub st_other: u8,
    pub st_shndx: Elf64Half,
    pub st_value: Elf64Addr,
    pub st_size: Elf64Xword,
}
pub type Elf64Sym = elf64_sym;

#[derive(FromPrimitive, Copy, Clone, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum SegmentType {
    /// PT_NULL
    Unused = 0,
    /// PT_LOAD
    Load = 1,
    /// PT_DYNAMIC
    Dynamic = 2,
    /// PT_INTERP
    Interp = 3,
    /// PT_GNU_STACK
    GnuStack = 0x6474e551,
}

bitflags! {
    pub struct SegmentFlags: u32 {
        const EXECUTE = 0b0001;
        const WRITE   = 0b0010;
        const READ    = 0b0100;
    }
}

impl fmt::Display for SegmentType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SegmentType::Unused => write!(f, "PT_NULL"),
            SegmentType::Load => write!(f, "PT_LOAD"),
            SegmentType::Dynamic => write!(f, "PT_DYNAMIC"),
            SegmentType::Interp => write!(f, "PT_INTERP"),
            SegmentType::GnuStack => write!(f, "PT_GNU_STACK"),
        }
    }
}

impl Elf64ProgramHeader {
    pub fn segment_type(&self) -> Result<SegmentType, u32> {
        SegmentType::from_u32(self.segment_type).ok_or(self.segment_type)
    }

    pub fn flags(&self) -> SegmentFlags {
        // Ignore bits that don't correspond to one of the flags included in SegmentFlags
        SegmentFlags::from_bits_truncate(self.flags)
    }
}

impl Validate for [Elf64ProgramHeader] {
    fn validate(&self) -> Result<(), ElfParseError> {
        let page_size = zx::system_get_page_size() as usize;
        let mut vaddr_high: usize = 0;
        for hdr in self {
            if hdr.filesz > hdr.memsz {
                return Err(ElfParseError::InvalidProgramHeader("filesz > memsz"));
            }

            match hdr.segment_type() {
                Ok(SegmentType::Load) => {
                    // Virtual addresses for PT_LOAD segments should not overlap.
                    if hdr.vaddr < vaddr_high {
                        return Err(ElfParseError::InvalidProgramHeader(
                            "Overlap in virtual addresses",
                        ));
                    }
                    vaddr_high = hdr.vaddr + hdr.memsz as usize;

                    // Segment alignment should be a multiple of the system page size.
                    if hdr.align % page_size as u64 != 0 {
                        return Err(ElfParseError::InvalidProgramHeader(
                            "Alignment must be multiple of the system page size",
                        ));
                    }

                    // Virtual addresses should be at the same page offset as their offset in the
                    // file.
                    if (hdr.vaddr % hdr.align as usize) != (hdr.offset % hdr.align as usize) {
                        return Err(ElfParseError::InvalidProgramHeader(
                            "Virtual address and offset in file are not at same offset in page",
                        ));
                    }
                }
                Ok(SegmentType::GnuStack) => {
                    if hdr.flags().contains(SegmentFlags::EXECUTE) {
                        return Err(ElfParseError::InvalidProgramHeader(
                            "Fuchsia does not support executable stacks",
                        ));
                    }
                }
                // No specific validation to perform for these.
                Ok(SegmentType::Unused) | Ok(SegmentType::Interp) | Ok(SegmentType::Dynamic) => {}
                // Ignore segment types that we don't care about.
                Err(_) => {}
            }
        }
        Ok(())
    }
}

pub struct Elf64Headers {
    file_header: Box<Elf64FileHeader>,
    // Use a Box<[_]> instead of a Vec<> to communicate/enforce that the slice is not mutated after
    // construction.
    program_headers: Option<Box<[Elf64ProgramHeader]>>,
    // Section headers are not parsed currently since they aren't needed for the current use case,
    // but could be added if needed.
}

impl Elf64Headers {
    pub fn from_vmo(vmo: &zx::Vmo) -> Result<Elf64Headers, ElfParseError> {
        // Read and parse the ELF file header from the VMO.
        let mut file_header = Box::<Elf64FileHeader>::default();
        vmo.read(file_header.as_bytes_mut(), 0).map_err(|s| ElfParseError::ReadError(s))?;
        file_header.validate()?;

        // Read and parse the ELF program headers from the VMO. Also support the degenerate case
        // where there are no program headers, which is valid ELF but probably not useful outside
        // tests.
        let mut program_headers = None;
        if file_header.phnum > 0 {
            let mut phdrs = vec![Elf64ProgramHeader::default(); file_header.phnum as usize];
            vmo.read(phdrs.as_bytes_mut(), file_header.phoff as u64)
                .map_err(|s| ElfParseError::ReadError(s))?;
            phdrs.validate()?;
            program_headers = Some(phdrs.into_boxed_slice());
        }

        Ok(Elf64Headers { file_header, program_headers })
    }

    pub fn file_header(&self) -> &Elf64FileHeader {
        &*self.file_header
    }

    pub fn program_headers(&self) -> &[Elf64ProgramHeader] {
        match &self.program_headers {
            Some(boxed_slice) => &*boxed_slice,
            None => &[],
        }
    }

    /// Returns an iterator that yields all program headers of the given type.
    pub fn program_headers_with_type(
        &self,
        stype: SegmentType,
    ) -> impl Iterator<Item = &Elf64ProgramHeader> {
        self.program_headers().iter().filter(move |x| match x.segment_type() {
            Ok(t) => t == stype,
            _ => false,
        })
    }

    /// Returns 0 or 1 headers of the given type, or Err([ElfParseError::MultipleHeaders]) if more
    /// than 1 such header is present.
    pub fn program_header_with_type(
        &self,
        stype: SegmentType,
    ) -> Result<Option<&Elf64ProgramHeader>, ElfParseError> {
        let mut headers = self.program_headers_with_type(stype);
        let header = headers.next();
        if headers.next().is_some() {
            return Err(ElfParseError::MultipleHeaders(stype));
        }
        return Ok(header);
    }

    /// Creates an instance of Elf64Headers from in-memory representations of the ELF headers.
    #[cfg(test)]
    pub fn new_for_test(
        file_header: &Elf64FileHeader,
        program_headers: Option<&[Elf64ProgramHeader]>,
    ) -> Self {
        Self {
            file_header: Box::new(*file_header),
            program_headers: program_headers.map(|headers| headers.into()),
        }
    }
}

pub struct Elf64DynSection {
    dyn_entries: Box<[Elf64Dyn]>,
}
impl Elf64DynSection {
    pub fn from_vmo(vmo: &zx::Vmo) -> Result<Elf64DynSection, ElfParseError> {
        let headers = Elf64Headers::from_vmo(vmo)?;
        const ENTRY_SIZE: usize = std::mem::size_of::<Elf64Dyn>();
        if let Some(dynamic_header) = headers.program_header_with_type(SegmentType::Dynamic)? {
            let dyn_entries_size = dynamic_header.filesz as usize / ENTRY_SIZE;
            let mut entries = vec![Elf64Dyn::default(); dyn_entries_size];
            vmo.read(entries.as_bytes_mut(), dynamic_header.offset as u64)
                .map_err(|s| ElfParseError::ReadError(s))?;
            let dyn_entries = entries.into_boxed_slice();
            return Ok(Elf64DynSection { dyn_entries });
        }
        Err(ElfParseError::ParseError("Dynamic header not found"))
    }

    pub fn dynamic_entries(&self) -> &[Elf64Dyn] {
        &*self.dyn_entries
    }

    pub fn dynamic_entry_with_tag(&self, tag: Elf64DynTag) -> Option<&Elf64Dyn> {
        self.dynamic_entries().iter().find(move |x| match x.tag() {
            Ok(t) => t == tag,
            _ => false,
        })
    }
}

#[cfg(test)]
mod tests {
    use {super::*, anyhow::Error, assert_matches::assert_matches, fdio, std::fs::File};

    // These are specially crafted files that just contain a valid ELF64 file header but
    // nothing else.
    static HEADER_DATA_X86_64: &'static [u8] =
        include_bytes!("../test-utils/elf_x86-64_file-header.bin");
    static HEADER_DATA_AARCH64: &'static [u8] =
        include_bytes!("../test-utils/elf_aarch64_file-header.bin");
    #[cfg(target_arch = "x86_64")]
    static HEADER_DATA: &'static [u8] = HEADER_DATA_X86_64;
    #[cfg(target_arch = "aarch64")]
    static HEADER_DATA: &'static [u8] = HEADER_DATA_AARCH64;
    #[cfg(target_arch = "x86_64")]
    static HEADER_DATA_WRONG_ARCH: &'static [u8] = HEADER_DATA_AARCH64;
    #[cfg(target_arch = "aarch64")]
    static HEADER_DATA_WRONG_ARCH: &'static [u8] = HEADER_DATA_X86_64;

    #[test]
    fn test_parse_file_header() -> Result<(), Error> {
        let vmo = zx::Vmo::create(HEADER_DATA.len() as u64)?;
        vmo.write(&HEADER_DATA, 0)?;

        let headers = Elf64Headers::from_vmo(&vmo)?;
        assert_eq!(
            headers.file_header(),
            &Elf64FileHeader {
                ident: ElfIdent {
                    magic: ELF_MAGIC,
                    class: ElfClass::Elf64 as u8,
                    data: ElfDataEncoding::LittleEndian as u8,
                    version: ElfVersion::Current as u8,
                    osabi: 0,
                    abiversion: 0,
                    pad: [0; 7],
                },
                elf_type: ElfType::SharedObject as u16,
                machine: CURRENT_ARCH as u16,
                version: 1,
                entry: 0x10000,
                phoff: 0,
                shoff: 0,
                flags: 0,
                ehsize: mem::size_of::<Elf64FileHeader>() as u16,
                phentsize: mem::size_of::<Elf64ProgramHeader>() as u16,
                phnum: 0,
                shentsize: 0,
                shnum: 0,
                shstrndx: 0,
            }
        );
        assert_eq!(headers.program_headers().len(), 0);
        Ok(())
    }

    #[test]
    fn test_parse_wrong_arch() -> Result<(), Error> {
        let vmo = zx::Vmo::create(HEADER_DATA_WRONG_ARCH.len() as u64)?;
        vmo.write(&HEADER_DATA, 0)?;

        match Elf64Headers::from_vmo(&vmo) {
            Err(ElfParseError::InvalidFileHeader(msg)) => {
                assert_eq!(msg, "Invalid ELF architecture");
            }
            _ => {}
        }
        Ok(())
    }

    #[test]
    fn test_parse_program_headers() -> Result<(), Error> {
        // Let's try to parse ourselves!
        // Ideally we'd use std::env::current_exe but that doesn't seem to be implemented (yet?)
        let file = File::open("/pkg/bin/process_builder_lib_test")?;
        let vmo = fdio::get_vmo_copy_from_file(&file)?;

        let headers = Elf64Headers::from_vmo(&vmo)?;
        assert!(headers.program_headers().len() > 0);
        assert!(headers.program_header_with_type(SegmentType::Interp)?.is_some());
        assert!(headers.program_headers_with_type(SegmentType::Dynamic).count() == 1);
        assert!(headers.program_headers_with_type(SegmentType::Load).count() > 1);
        Ok(())
    }

    #[test]
    fn test_parse_dynamic_section() -> Result<(), Error> {
        let file = File::open("/pkg/bin/process_builder_lib_test")?;
        let vmo = fdio::get_vmo_copy_from_file(&file)?;

        let dynamic_section = Elf64DynSection::from_vmo(&vmo)?;
        assert!(dynamic_section.dynamic_entries().len() > 0);
        Ok(())
    }

    #[test]
    fn test_parse_static_pie() -> Result<(), Error> {
        // Parse the statically linked PIE test binary.
        let file = File::open("/pkg/bin/static_pie_test_util")?;
        let vmo = fdio::get_vmo_copy_from_file(&file)?;

        // Should have no PT_INTERP header, but should have PT_DYNAMIC and 1+ PT_LOAD.
        let headers = Elf64Headers::from_vmo(&vmo)?;
        assert!(headers.program_headers().len() > 0);
        assert!(headers.program_header_with_type(SegmentType::Interp)?.is_none());
        assert!(headers.program_headers_with_type(SegmentType::Dynamic).count() == 1);
        assert!(headers.program_headers_with_type(SegmentType::Load).count() > 1);
        Ok(())
    }

    #[test]
    fn test_parse_program_header_bad_alignment() {
        let page_size = zx::system_get_page_size() as usize;
        let headers = [Elf64ProgramHeader {
            segment_type: SegmentType::Load as u32,
            flags: (SegmentFlags::READ | SegmentFlags::EXECUTE).bits,
            align: page_size as u64 + 1024,
            offset: 0x1000,
            vaddr: 0x1000,
            paddr: 0x1000,
            filesz: 0x1000,
            memsz: 0x1000,
        }];
        assert_matches!(
            headers.validate(),
            Err(ElfParseError::InvalidProgramHeader(
                "Alignment must be multiple of the system page size"
            ))
        );
    }

    #[test]
    fn test_parse_program_header_bad_offset_and_vaddr() {
        let page_size = zx::system_get_page_size() as usize;
        let headers = [Elf64ProgramHeader {
            segment_type: SegmentType::Load as u32,
            flags: (SegmentFlags::READ | SegmentFlags::EXECUTE).bits,
            align: page_size as u64,
            offset: 0x1001,
            vaddr: 0x1002,
            paddr: 0x1000,
            filesz: 0x1000,
            memsz: 0x1000,
        }];
        assert_matches!(
            headers.validate(),
            Err(ElfParseError::InvalidProgramHeader(
                "Virtual address and offset in file are not at same offset in page"
            ))
        );
    }
}
