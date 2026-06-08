/*
 * qas — assembler driver: the assembly unit (internal).
 *
 * Responsibility
 * Bundle the mutable state of one assembly run — the output sections, the symbol
 * table, the list of pending fixups, and which section is "current" — and offer
 * the small set of mutating operations the driver and the directive handler share:
 * select a section, emit bytes/zeros, align, define a label, and record a fixup.
 *
 * Why one aggregate (and not globals)
 *   The coding standard forbids global mutable state as a coordination mechanism
 *   (coding-standard §2/§5): state is owned by an explicit context passed around.
 *   qas_asm_unit is that context. The instruction path (asm.c) and the directive
 *   path (directive.c) both mutate the same unit through these functions, so the
 *   two stay consistent without either reaching into the other.
 *
 * Current-section model
 *   Assembly begins in `.text` (the GNU as default), so unit_init creates it and
 *   makes it current. Directives change the current section; everything emitted —
 *   instruction bytes, data directives, label definitions — lands in it. Private
 *   to the asm module (ADR-0008).
 */
#ifndef QAS_ASM_INTERNAL_UNIT_H
#define QAS_ASM_INTERNAL_UNIT_H

#include <stddef.h>
#include <stdint.h>

#include "asm/internal/fixups.h"
#include "asm/internal/section.h"
#include "asm/internal/symtab.h"
#include "diag/diag.h"
#include "elf/elf.h"
#include "source/source.h"
#include "status/status.h"

/* The whole mutable state of one assembly run. Treat fields as private; use the
   functions below. `current` indexes into `sections`. */
typedef struct qas_asm_unit {
    const qas_source    *src;     /* Borrowed input bytes (for symbol/string spans)*/
    qas_diag_sink       *diags;   /* Borrowed diagnostics sink.                    */
    qas_asm_section_set  sections;
    qas_asm_symtab       syms;
    qas_asm_fix_list     fixes;
    uint32_t             current; /* Index of the current output section.          */
} qas_asm_unit;

/*
 * Initialize a unit over `src`, reporting to `diags`, and create the initial
 * `.text` section as current. Returns QAS_OK or QAS_ERR_OUT_OF_MEMORY /
 * QAS_ERR_INVALID_ARGUMENT.
 */
qas_status qas_asm_unit_init(qas_asm_unit *unit, const qas_source *src,
                             qas_diag_sink *diags);

/* Free everything the unit owns. Safe on a zeroed unit. */
void qas_asm_unit_dispose(qas_asm_unit *unit);

/* Borrow the current section (never NULL after a successful init). */
qas_asm_section *qas_asm_unit_current(qas_asm_unit *unit);

/*
 * Make the section named `name` current, creating it with (type, flags,
 * addralign) if it does not exist yet. Re-entering an existing section keeps its
 * original attributes (GNU as semantics) and ignores the passed-in ones. Returns
 * QAS_OK or QAS_ERR_OUT_OF_MEMORY.
 */
qas_status qas_asm_unit_select_section(qas_asm_unit *unit, const char *name,
                                       Elf64_Word type, Elf64_Xword flags,
                                       Elf64_Xword addralign);

/* The offset the next emitted byte of the current section would take. */
uint64_t qas_asm_unit_here(qas_asm_unit *unit);

/*
 * Append `len` literal bytes to the current section. Precondition: the current
 * section is SHT_PROGBITS (the caller, which has the source location, must reject
 * emitting into a NOBITS/.bss section with a diagnostic first). Returns QAS_OK,
 * QAS_ERR_OUT_OF_MEMORY, or QAS_ERR_INVALID_ARGUMENT if called on a NOBITS section.
 */
qas_status qas_asm_unit_emit_bytes(qas_asm_unit *unit, const void *bytes, size_t len);

/*
 * Reserve `count` zero bytes in the current section: appended as zeros for
 * PROGBITS, or counted into the logical size for NOBITS (.bss). Works for both, so
 * it serves `.zero`/`.skip`/`.space` and alignment padding. Returns QAS_OK or
 * QAS_ERR_OUT_OF_MEMORY.
 */
qas_status qas_asm_unit_emit_zeros(qas_asm_unit *unit, uint64_t count);

/*
 * Align the current section to `alignment` bytes (a power of two): pad to the
 * boundary and raise the section's recorded alignment to at least `alignment` (so
 * the linker keeps the section aligned). `alignment` <= 1 is a no-op. Returns
 * QAS_OK or QAS_ERR_OUT_OF_MEMORY.
 */
qas_status qas_asm_unit_align(qas_asm_unit *unit, uint64_t alignment);

/*
 * Define a label named src->data[name_off .. name_off+name_len) at the current
 * location (current section, current offset). Reports a diagnostic and keeps the
 * first definition if the name is already defined. Returns QAS_OK or a fatal
 * allocation status.
 */
qas_status qas_asm_unit_define_label(qas_asm_unit *unit, size_t name_off,
                                     size_t name_len);

/*
 * Record a fixup at `field_offset` (offset within the current section) referencing
 * the symbol src->data[sym_off .. sym_off+sym_len), with the given kind and
 * addend. Returns QAS_OK or QAS_ERR_OUT_OF_MEMORY.
 */
qas_status qas_asm_unit_add_fix(qas_asm_unit *unit, qas_asm_fix_kind kind,
                                size_t sym_off, size_t sym_len, int64_t addend,
                                uint64_t field_offset);

#endif /* QAS_ASM_INTERNAL_UNIT_H */
