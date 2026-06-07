/*
 * qas — ELF64 relocatable-object writer
 *
 * Responsibility
 * Build and serialize an ELF64 relocatable object (`ET_REL`) for x86-64: the
 * container `qas` emits and `qld` will consume. The module offers a *builder*
 * that accumulates output sections, a symbol table, and relocations, then
 * serializes a complete, valid object image to a byte buffer. We implement ELF
 * from scratch — no libelf — which ADR-0004 establishes is within the
 * "from scratch" rule because a format is a specification, not a third-party tool.
 *
 * Authorities (verify field values against these)
 *   - System V gABI / TIS ELF Specification — generic ELF: the header, section
 *     headers, symbol table, string tables, and the local-before-global symbol
 *     ordering rule with `sh_info`.
 *   - System V ABI, AMD64 Architecture Processor Supplement (the x86-64 psABI) —
 *     `EM_X86_64`, the RELA relocation form, and the R_X86_64_* relocation types.
 *   The subset we commit to is recorded in Quicks-Meta docs/abi/object-format-notes.md.
 *
 * Why a builder rather than raw struct writes
 *   The on-disk order, the synthesized `.symtab`/`.strtab`/`.shstrtab`/`.rela.*`
 *   sections, the symbol reordering, and the cross-references (`sh_link`,
 *   `sh_info`, `r_info`'s symbol index, `st_shndx`) are interdependent. The
 *   builder lets callers add sections/symbols/relocations in any order using
 *   stable handles, and resolves all of that once at `finish` — so the encoder
 *   and orchestrator never compute a file offset by hand.
 *
 * Endianness/layout: all multi-byte fields are written little-endian by the
 * serializer (via the buf module), independent of host byte order; we never
 * memcpy a struct to disk, so compiler struct padding can never leak in.
 */
#ifndef QAS_ELF_ELF_H
#define QAS_ELF_ELF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "status/status.h"

/*
 * The ELF64 base data types (System V gABI, "Data Representation"). We use the
 * spec's own names so the structs read exactly like the standard's tables and so
 * a future reader (qld) shares the vocabulary.
 */
typedef uint64_t Elf64_Addr;   /* Program address.                              */
typedef uint64_t Elf64_Off;    /* File offset.                                  */
typedef uint16_t Elf64_Half;   /* Medium integer.                               */
typedef uint32_t Elf64_Word;   /* Integer.                                      */
typedef int32_t  Elf64_Sword;  /* Signed integer.                               */
typedef uint64_t Elf64_Xword;  /* Large integer.                                */
typedef int64_t  Elf64_Sxword; /* Signed large integer.                         */

/* e_ident[] indices (System V gABI, "ELF Identification"). */
enum {
    EI_MAG0 = 0, EI_MAG1 = 1, EI_MAG2 = 2, EI_MAG3 = 3,
    EI_CLASS = 4, EI_DATA = 5, EI_VERSION = 6, EI_OSABI = 7,
    EI_ABIVERSION = 8, EI_PAD = 9, EI_NIDENT = 16
};

/* e_ident magic and selectors. */
enum {
    ELFMAG0 = 0x7f, ELFMAG1 = 'E', ELFMAG2 = 'L', ELFMAG3 = 'F',
    ELFCLASS64 = 2,     /* 64-bit objects.                                       */
    ELFDATA2LSB = 1,    /* Two's-complement, little-endian (x86-64).             */
    ELFOSABI_SYSV = 0   /* System V ABI; the psABI's default for x86-64.         */
};

/* e_version / EV_* and e_type / ET_*. */
enum { EV_NONE = 0, EV_CURRENT = 1 };
enum { ET_NONE = 0, ET_REL = 1, ET_EXEC = 2, ET_DYN = 3 };

/* e_machine (psABI: x86-64 is 62). */
enum { EM_X86_64 = 62 };

/* Special section indices (System V gABI, "Sections"). */
enum {
    SHN_UNDEF = 0,        /* Undefined / absent reference.                       */
    SHN_ABS = 0xfff1,     /* Absolute value, not affected by relocation.         */
    SHN_COMMON = 0xfff2   /* Common (tentative) symbol.                          */
};

/* sh_type values (the subset we emit). */
enum {
    SHT_NULL = 0,      /* Inactive header.                                       */
    SHT_PROGBITS = 1,  /* Program-defined bytes (.text/.data/.rodata).           */
    SHT_SYMTAB = 2,    /* Symbol table.                                          */
    SHT_STRTAB = 3,    /* String table.                                          */
    SHT_RELA = 4,      /* Relocations with explicit addends.                     */
    SHT_NOBITS = 8     /* Occupies no file space (.bss).                         */
};

/* sh_flags bits. */
enum {
    SHF_WRITE = 0x1,      /* Writable at run time.                               */
    SHF_ALLOC = 0x2,      /* Occupies memory in the loaded image.                */
    SHF_EXECINSTR = 0x4   /* Executable machine instructions.                    */
};

/* Symbol binding (high nibble of st_info). */
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
/* Symbol type (low nibble of st_info). */
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4 };
/* Reserved symbol-table index 0. */
enum { STN_UNDEF = 0 };

/*
 * x86-64 relocation types (psABI relocation table). The computation each one
 * performs (S = symbol value, A = addend, P = place being relocated) is cited at
 * the point of use; this is the subset object-format-notes.md commits to.
 */
enum {
    R_X86_64_NONE = 0,   /* No relocation.                                       */
    R_X86_64_64 = 1,     /* word64   S + A      (absolute 64-bit).               */
    R_X86_64_PC32 = 2,   /* word32   S + A - P  (PC-relative 32-bit).            */
    R_X86_64_PLT32 = 4,  /* word32   L + A - P  (== PC32 in a static no-PLT link)*/
    R_X86_64_32 = 10,    /* word32   S + A      (absolute, zero-extended).       */
    R_X86_64_32S = 11    /* word32   S + A      (absolute, sign-extended).       */
};

/* ELF header. Field order/sizes per the System V gABI "ELF Header" table. */
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;       /* Object file type (ET_REL for qas output).     */
    Elf64_Half    e_machine;    /* Architecture (EM_X86_64).                     */
    Elf64_Word    e_version;    /* Object file version (EV_CURRENT).             */
    Elf64_Addr    e_entry;      /* Entry point (0 for a relocatable object).     */
    Elf64_Off     e_phoff;      /* Program header table offset (0; none in REL). */
    Elf64_Off     e_shoff;      /* Section header table offset.                  */
    Elf64_Word    e_flags;      /* Processor flags (0 on x86-64).               */
    Elf64_Half    e_ehsize;     /* ELF header size in bytes (64).                */
    Elf64_Half    e_phentsize;  /* Program header entry size.                    */
    Elf64_Half    e_phnum;      /* Program header entry count (0).               */
    Elf64_Half    e_shentsize;  /* Section header entry size (64).               */
    Elf64_Half    e_shnum;      /* Section header entry count.                   */
    Elf64_Half    e_shstrndx;   /* Section index of the section-name string table*/
} Elf64_Ehdr;

/* Section header. */
typedef struct {
    Elf64_Word  sh_name;       /* Section name (offset into .shstrtab).          */
    Elf64_Word  sh_type;       /* SHT_*.                                         */
    Elf64_Xword sh_flags;      /* SHF_*.                                         */
    Elf64_Addr  sh_addr;       /* Load address (0 in a relocatable object).      */
    Elf64_Off   sh_offset;     /* Byte offset of contents in the file.           */
    Elf64_Xword sh_size;       /* Size in bytes (logical size for NOBITS).       */
    Elf64_Word  sh_link;       /* Link to another section (table-specific).       */
    Elf64_Word  sh_info;       /* Extra info (table-specific).                   */
    Elf64_Xword sh_addralign;  /* Required alignment of sh_offset/sh_addr.        */
    Elf64_Xword sh_entsize;    /* Entry size for tables (0 otherwise).           */
} Elf64_Shdr;

/* Symbol table entry. */
typedef struct {
    Elf64_Word    st_name;   /* Name (offset into the linked string table).      */
    unsigned char st_info;   /* Binding (high nibble) and type (low nibble).     */
    unsigned char st_other;  /* Visibility (0 = default).                        */
    Elf64_Half    st_shndx;  /* Section index, or SHN_UNDEF/SHN_ABS.             */
    Elf64_Addr    st_value;  /* Value (offset within its section for ET_REL).    */
    Elf64_Xword   st_size;   /* Size in bytes (0 if unknown).                    */
} Elf64_Sym;

/* Relocation with explicit addend (RELA; x86-64 uses RELA, not REL). */
typedef struct {
    Elf64_Addr   r_offset;  /* Offset of the field to relocate, within its section*/
    Elf64_Xword  r_info;    /* Symbol index (high 32) and type (low 32).         */
    Elf64_Sxword r_addend;  /* Constant addend used to compute the value.        */
} Elf64_Rela;

/*
 * Compile-time checks that our struct layouts match the spec's fixed sizes. If a
 * compiler ever inserted padding (it must not, given the natural field
 * alignment), this fails the build instead of silently corrupting output.
 */
_Static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr must be 64 bytes");
_Static_assert(sizeof(Elf64_Shdr) == 64, "Elf64_Shdr must be 64 bytes");
_Static_assert(sizeof(Elf64_Sym) == 24, "Elf64_Sym must be 24 bytes");
_Static_assert(sizeof(Elf64_Rela) == 24, "Elf64_Rela must be 24 bytes");

/* st_info / r_info field composition (System V gABI; psABI for r_info width). */
#define QAS_ELF64_ST_INFO(bind, type) ((unsigned char)(((bind) << 4) + ((type) & 0xf)))
#define QAS_ELF64_R_INFO(sym, type)   (((Elf64_Xword)(sym) << 32) | ((Elf64_Xword)((type) & 0xffffffffu)))

/*
 * Where a symbol is defined, controlling its st_shndx. Kept abstract so callers
 * use a real *output-section handle* (not a raw final index they cannot know
 * until finish) for defined symbols.
 */
typedef enum qas_elf_symref {
    QAS_ELF_SYMREF_UNDEF = 0, /* External/undefined: st_shndx = SHN_UNDEF.       */
    QAS_ELF_SYMREF_SECTION,   /* Defined in the given output section.            */
    QAS_ELF_SYMREF_ABS        /* Absolute value: st_shndx = SHN_ABS.             */
} qas_elf_symref;

/*
 * Builder element types are defined in elf/internal/model.h and are opaque here:
 * the public struct holds only pointers to them, so callers can stack-allocate a
 * qas_elf_builder and use the API, but cannot reach into a section/symbol record
 * (information hiding per ADR-0008, without a heap-allocated PIMPL).
 */
struct qas_elf_section;
struct qas_elf_symbol;

/*
 * The builder. Treat the fields as private; use the functions below. A
 * zero-initialized builder is valid-empty, but call qas_elf_builder_init for
 * clarity. Owns every section's bytes, every symbol's name string, and the
 * relocation arrays until dispose or a successful finish.
 */
typedef struct qas_elf_builder {
    struct qas_elf_section *sections;
    size_t                  section_count;
    size_t                  section_cap;

    struct qas_elf_symbol  *symbols;
    size_t                  symbol_count;
    size_t                  symbol_cap;
} qas_elf_builder;

/* Initialize an empty builder. Always succeeds. */
void qas_elf_builder_init(qas_elf_builder *builder);

/* Free everything the builder owns and reset to empty. Safe on a zeroed builder. */
void qas_elf_builder_dispose(qas_elf_builder *builder);

/*
 * Add an output section (.text/.data/.rodata/.bss style). `name` is copied.
 * `type` is SHT_PROGBITS or SHT_NOBITS; `flags` are SHF_*; `addralign` is the
 * section's required alignment (a power of two, or 0/1 for none). On success
 * *out_section receives a stable handle (an index) used by the other calls.
 *
 * Returns QAS_OK, QAS_ERR_INVALID_ARGUMENT, or QAS_ERR_OUT_OF_MEMORY.
 */
qas_status qas_elf_builder_add_section(qas_elf_builder *builder, const char *name,
                                       Elf64_Word type, Elf64_Xword flags,
                                       Elf64_Xword addralign, uint32_t *out_section);

/*
 * Append `len` bytes to a PROGBITS section's contents. *out_offset (if non-NULL)
 * receives the section-relative offset at which the bytes start — the natural
 * value for a label defined here or a relocation site. Appending to a NOBITS
 * section is an error (use reserve_bss).
 */
qas_status qas_elf_builder_append(qas_elf_builder *builder, uint32_t section,
                                  const void *bytes, size_t len, uint64_t *out_offset);

/*
 * Grow a NOBITS (.bss) section's logical size by `bytes`, occupying no file
 * space. *out_offset (if non-NULL) receives the section-relative offset of the
 * reserved region. Calling this on a PROGBITS section is an error.
 */
qas_status qas_elf_builder_reserve_bss(qas_elf_builder *builder, uint32_t section,
                                       uint64_t bytes, uint64_t *out_offset);

/* Current size (next offset) of a section, or 0 if the handle is invalid. */
uint64_t qas_elf_builder_section_size(const qas_elf_builder *builder, uint32_t section);

/*
 * Add a symbol. `name` is copied (pass "" or NULL for an unnamed symbol). `bind`
 * is STB_*, `type` is STT_*. `ref` selects how the symbol is anchored: for
 * QAS_ELF_SYMREF_SECTION, `section` is an output-section handle and `value` is the
 * offset within it; for UNDEF/ABS, `section` is ignored. *out_symbol receives a
 * stable handle used by relocations. Symbols may be added in any order and in any
 * binding; finish reorders them so locals precede globals (gABI requirement).
 */
qas_status qas_elf_builder_add_symbol(qas_elf_builder *builder, const char *name,
                                      unsigned char bind, unsigned char type,
                                      qas_elf_symref ref, uint32_t section,
                                      uint64_t value, uint64_t size,
                                      uint32_t *out_symbol);

/*
 * Record a relocation applied within `section` at section-relative `offset`,
 * referencing the symbol handle `symbol` with relocation `reloc_type`
 * (R_X86_64_*) and explicit `addend`. The symbol must already have been added
 * (its handle valid now). A `.rela.<section>` is synthesized at finish.
 */
qas_status qas_elf_builder_add_rela(qas_elf_builder *builder, uint32_t section,
                                    uint64_t offset, uint32_t symbol,
                                    Elf64_Word reloc_type, Elf64_Sxword addend);

/*
 * Serialize the accumulated object into a freshly allocated ELF64 image.
 * *out_image receives the bytes (caller frees with free()), *out_size the length.
 * The builder is left intact (you may inspect or dispose it afterward).
 *
 * Returns QAS_OK, QAS_ERR_INVALID_ARGUMENT, or QAS_ERR_OUT_OF_MEMORY.
 */
qas_status qas_elf_builder_finish(qas_elf_builder *builder, uint8_t **out_image,
                                  size_t *out_size);

#endif /* QAS_ELF_ELF_H */
