/*
 * qas — assembler driver: symbol table (internal).
 *
 * Responsibility
 * Intern the symbols of one assembly unit (labels, .globl names, .equ constants,
 * and external references) so that every mention of a name maps to a single
 * record. Each record carries everything the resolve pass and the ELF symbol
 * writer need: whether the symbol is defined, where (a section + offset, or an
 * absolute value), its binding (local/global), and whether a relocation referred
 * to it (so we know it must appear in .symtab).
 *
 * Why a hash table here (but not for sections)
 *   A unit can define and reference thousands of symbols, and the driver interns
 *   on *every* label and *every* symbolic operand. Linear lookup would be O(n^2)
 *   over a large file. So names are indexed by an open-addressing hash keyed on a
 *   64-bit FNV-1a digest of the name bytes. (FNV-1a is a well-known public-domain
 *   hash — a house choice, not from a project spec; it is fast, simple, and has
 *   good dispersion for short identifier strings.)
 *
 * Identity/casing
 *   Symbol names are case-sensitive (unlike mnemonics and register names, which
 *   the parser/reg module fold). Two spans intern to the same record iff their
 *   bytes are equal. Private to the asm module (ADR-0008).
 */
#ifndef QAS_ASM_INTERNAL_SYMTAB_H
#define QAS_ASM_INTERNAL_SYMTAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "status/status.h"

/* How a defined symbol is anchored, mirroring the ELF st_shndx cases we use. */
typedef enum qas_asm_sym_where {
    QAS_ASM_SYM_UNDEF = 0, /* Not defined in this unit (external reference).       */
    QAS_ASM_SYM_SECTION,   /* Defined at an offset within an output section.        */
    QAS_ASM_SYM_ABS        /* Absolute value (.equ/.set), independent of any section*/
} qas_asm_sym_where;

/*
 * One interned symbol. `name` is an owned NUL-terminated copy. When `defined`:
 * `where` is SECTION (then `section` + `value` give the location) or ABS (then
 * `value` is the constant). `is_global` is set by `.globl`. `referenced` is set
 * when a relocation against this symbol is emitted, which forces it into .symtab
 * even if it is local. `emitted`/`elf_handle` cache the symbol's handle in the ELF
 * builder once it has been added (so it is added at most once).
 */
typedef struct qas_asm_sym {
    char             *name;
    bool              defined;
    qas_asm_sym_where where;
    uint32_t          section;
    uint64_t          value;
    bool              is_global;
    bool              referenced;
    bool              emitted;
    uint32_t          elf_handle;
} qas_asm_sym;

/*
 * The symbol table: stable record storage plus a hash index over it. `syms` only
 * grows, so a record's index is stable for the table's life; `buckets` stores
 * 1-based indices into `syms` (0 means empty) and is rehashed on growth.
 */
typedef struct qas_asm_symtab {
    qas_asm_sym *syms;
    size_t       count;
    size_t       capacity;

    uint32_t    *buckets;     /* 1-based index into syms, 0 = empty slot.          */
    size_t       bucket_count;/* Always a power of two (mask = bucket_count - 1).  */
} qas_asm_symtab;

/* Initialize an empty table. Always succeeds. */
void qas_asm_symtab_init(qas_asm_symtab *table);

/* Free every symbol name and both arrays; reset to empty. */
void qas_asm_symtab_dispose(qas_asm_symtab *table);

/*
 * Intern the name [name, name+len). On success *out_index is the stable record
 * index (an existing one if the name was seen before, otherwise a freshly created
 * UNDEF/local/unreferenced record whose name is copied). Returns QAS_OK or
 * QAS_ERR_OUT_OF_MEMORY / QAS_ERR_INVALID_ARGUMENT.
 *
 * Note: a successful intern may reallocate internal storage, so any qas_asm_sym
 * pointer obtained earlier from qas_asm_symtab_at is invalidated — re-fetch it
 * after interning.
 */
qas_status qas_asm_symtab_intern(qas_asm_symtab *table, const char *name,
                                 size_t len, uint32_t *out_index);

/* Borrow the record at `index` (NULL if out of range). Invalidated by intern. */
qas_asm_sym *qas_asm_symtab_at(qas_asm_symtab *table, uint32_t index);

#endif /* QAS_ASM_INTERNAL_SYMTAB_H */
