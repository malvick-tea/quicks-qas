/*
 * qas — assembler driver: output sections (internal).
 *
 * Responsibility
 * Model the output sections an assembly unit accumulates (.text, .data, .rodata,
 * .bss, and any named `.section`) and provide a registry that interns them by
 * name. Each section owns its emitted bytes (for SHT_PROGBITS) or just a logical
 * size (for SHT_NOBITS / .bss), plus the section attributes that become the ELF
 * section header's sh_type / sh_flags / sh_addralign (System V gABI, "Sections").
 *
 * This is a thin, policy-free container: it does not decide a section's flags from
 * its name (that is the directive handler's job) and it does not know about
 * symbols or relocations. It simply stores bytes and sizes and hands back stable
 * indices. The ELF section header values it carries are cited where they are set
 * (the directive handler and asm.c). Private to the asm module (ADR-0008).
 */
#ifndef QAS_ASM_INTERNAL_SECTION_H
#define QAS_ASM_INTERNAL_SECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buf/buf.h"
#include "elf/elf.h"
#include "status/status.h"

/*
 * One output section.
 *
 * For SHT_PROGBITS, `data` holds the emitted bytes and is the authority for the
 * section's size. For SHT_NOBITS (.bss), `data` stays empty and `bss_size` is the
 * logical size (the gABI says a NOBITS section "occupies no space in the file but
 * otherwise resembles SHT_PROGBITS"). `addralign` is the strongest alignment any
 * directive has requested for this section (a power of two, >= 1); the linker must
 * honor it, so an `.align` both pads the contents *and* raises this value.
 */
typedef struct qas_asm_section {
    char        *name;       /* Owned copy of the section name (e.g. ".text").     */
    Elf64_Word   type;       /* SHT_PROGBITS or SHT_NOBITS.                        */
    Elf64_Xword  flags;      /* SHF_* (ALLOC/WRITE/EXECINSTR).                     */
    Elf64_Xword  addralign;  /* Required alignment, >= 1.                          */
    qas_buf      data;       /* Emitted bytes (PROGBITS); empty for NOBITS.        */
    uint64_t     bss_size;   /* Logical size for NOBITS; 0 for PROGBITS.           */
} qas_asm_section;

/*
 * A set of output sections, kept in first-seen creation order so the emitted
 * object's section order is deterministic. Indices into `items` are stable for the
 * life of the set (it only grows), so symbols and fixups can refer to a section by
 * index.
 */
typedef struct qas_asm_section_set {
    qas_asm_section *items;
    size_t           count;
    size_t           capacity;
} qas_asm_section_set;

/* Initialize an empty set. Always succeeds. */
void qas_asm_section_set_init(qas_asm_section_set *set);

/* Free every section (names and byte buffers) and reset to empty. */
void qas_asm_section_set_dispose(qas_asm_section_set *set);

/*
 * Find a section by name. Returns true and writes its index to *out_index if
 * present; returns false otherwise. `name` is a NUL-terminated string.
 */
bool qas_asm_section_set_find(const qas_asm_section_set *set, const char *name,
                              uint32_t *out_index);

/*
 * Add a new section with the given attributes; `name` is copied. *out_index
 * receives its stable index. The caller must have checked it does not already
 * exist (use find first). Returns QAS_OK or QAS_ERR_OUT_OF_MEMORY /
 * QAS_ERR_INVALID_ARGUMENT.
 */
qas_status qas_asm_section_set_add(qas_asm_section_set *set, const char *name,
                                   Elf64_Word type, Elf64_Xword flags,
                                   Elf64_Xword addralign, uint32_t *out_index);

/* Borrow the section at `index` (NULL if out of range). */
qas_asm_section *qas_asm_section_set_at(qas_asm_section_set *set, uint32_t index);

/*
 * The current size of a section — the offset the next emitted byte would take.
 * This is data.len for PROGBITS and bss_size for NOBITS, so it is the right value
 * for a label defined at the current point regardless of section type.
 */
uint64_t qas_asm_section_size(const qas_asm_section *section);

#endif /* QAS_ASM_INTERNAL_SECTION_H */
