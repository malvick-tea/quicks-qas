/*
 * qas — ELF writer: internal builder model.
 *
 * The concrete element records the builder accumulates. These are private to the
 * elf module (ADR-0008 information hiding): the public header forward-declares
 * `struct qas_elf_section` / `struct qas_elf_symbol` and stores only pointers to
 * them, so callers can stack-allocate a qas_elf_builder yet cannot reach inside a
 * record. Only elf.c and elf/internal/serialize.c include this header.
 */
#ifndef QAS_ELF_INTERNAL_MODEL_H
#define QAS_ELF_INTERNAL_MODEL_H

#include <stdint.h>

#include "buf/buf.h"
#include "elf/elf.h"

/* One relocation, as supplied by the caller (symbol is a builder handle). */
typedef struct qas_elf_reloc {
    uint64_t     offset;  /* Section-relative site of the field to patch.        */
    uint32_t     symbol;  /* Builder symbol handle (remapped to symtab index).  */
    Elf64_Word   type;    /* R_X86_64_*.                                         */
    Elf64_Sxword addend;  /* Explicit addend.                                   */
} qas_elf_reloc;

/* One output section plus the relocations that target it. */
typedef struct qas_elf_section {
    char        *name;        /* Owned copy of the section name.                 */
    Elf64_Word   type;        /* SHT_PROGBITS or SHT_NOBITS.                     */
    Elf64_Xword  flags;       /* SHF_*.                                          */
    Elf64_Xword  addralign;   /* Required alignment (>=1).                       */

    qas_buf      data;        /* Contents for PROGBITS (empty for NOBITS).       */
    uint64_t     bss_size;    /* Logical size for NOBITS.                        */

    qas_elf_reloc *relocs;    /* Owned array of relocations targeting this section*/
    size_t         reloc_count;
    size_t         reloc_cap;
} qas_elf_section;

/* One symbol as supplied by the caller. */
typedef struct qas_elf_symbol {
    char          *name;       /* Owned copy ("" allowed).                       */
    unsigned char  bind;       /* STB_*.                                         */
    unsigned char  type;       /* STT_*.                                         */
    qas_elf_symref ref;        /* How st_shndx is determined.                    */
    uint32_t       section;    /* Output-section handle when ref == SECTION.      */
    uint64_t       value;      /* st_value.                                      */
    uint64_t       size;       /* st_size.                                       */
} qas_elf_symbol;

#endif /* QAS_ELF_INTERNAL_MODEL_H */
