/*
 * qas — assembler driver: the pending-fixup list (internal).
 *
 * Responsibility
 * A growable list of *fixups* gathered while the driver emits a section's bytes.
 * A fixup is a place in some output section whose final value depends on a
 * symbol's address: a branch displacement, a RIP-relative reference, or a data
 * word holding a symbol address. The encoder hands back fixups in *instruction*
 * coordinates (offset within one instruction); the driver translates each to a
 * *section* coordinate (offset within the output section) and records it here.
 *
 * The resolve pass (asm.c) then walks this list once and, for each fixup, either
 * resolves it locally — a same-section PC-relative reference to a local label,
 * whose relative distance is known at assembly time — or turns it into an ELF
 * relocation for the linker (System V x86-64 psABI, the R_X86_64_* table).
 *
 * Why a driver-local fixup kind (not the encoder's qas_fixup_kind)
 *   The encoder only ever produces PC-relative and absolute-signed/64-bit fields
 *   (it never emits a data word), so its enum has three members. The driver also
 *   emits data directives (.long/.quad of a symbol), which need a 4-byte
 *   *zero-extended* absolute (R_X86_64_32). Keeping a slightly larger kind here
 *   lets one resolve pass cover both instruction and directive fixups.
 *
 * This is private to the asm module (ADR-0008 information hiding); only the asm
 * driver and its directive handler include it.
 */
#ifndef QAS_ASM_INTERNAL_FIXUPS_H
#define QAS_ASM_INTERNAL_FIXUPS_H

#include <stddef.h>
#include <stdint.h>

#include "status/status.h"

/*
 * What value a fixup needs and how the linker computes it. The computation is
 * written with the psABI's notation: S = the symbol's value, A = the addend, P =
 * the address of the field being patched ("place"). Each maps to exactly one
 * R_X86_64_* relocation type (System V ABI, AMD64 supplement, relocation table);
 * the mapping is made in asm.c at the point the relocation is emitted.
 */
typedef enum qas_asm_fix_kind {
    QAS_ASM_FIX_PC32 = 0, /* 4-byte, S + A - P  (rel32 / RIP-relative).            */
    QAS_ASM_FIX_ABS32S,   /* 4-byte, S + A, sign-extended (imm32 at 64-bit).       */
    QAS_ASM_FIX_ABS32,    /* 4-byte, S + A, zero-extended (.long of a symbol).     */
    QAS_ASM_FIX_ABS64     /* 8-byte, S + A  (movabs imm64 / .quad of a symbol).    */
} qas_asm_fix_kind;

/*
 * One pending fixup. `out_section`/`offset` locate the field in the *driver's*
 * output-section space (offset is section-relative, the value an ELF relocation's
 * r_offset takes). The referenced symbol is the source span [sym_off, sym_off +
 * sym_len) into the originating qas_source (resolved to a symbol record in the
 * resolve pass). `addend` is the constant the relocation adds — for PC-relative
 * fields it already includes the -(4 + immediate-tail) the CPU's next-instruction
 * RIP semantics require, exactly as the encoder computed it.
 */
typedef struct qas_asm_fix {
    uint32_t         out_section;
    uint64_t         offset;
    qas_asm_fix_kind kind;
    size_t           sym_off;
    size_t           sym_len;
    int64_t          addend;
} qas_asm_fix;

/* A growable array of fixups. A zero-initialized list is valid-empty. */
typedef struct qas_asm_fix_list {
    qas_asm_fix *items;
    size_t       count;
    size_t       capacity;
} qas_asm_fix_list;

/* Initialize an empty list. Always succeeds. */
void qas_asm_fix_list_init(qas_asm_fix_list *list);

/* Free the backing array and reset to empty. Safe on a zeroed list. */
void qas_asm_fix_list_dispose(qas_asm_fix_list *list);

/*
 * Append a fixup (copied by value). Returns QAS_OK or QAS_ERR_OUT_OF_MEMORY
 * (leaving the list unchanged on failure).
 */
qas_status qas_asm_fix_list_push(qas_asm_fix_list *list, const qas_asm_fix *fix);

#endif /* QAS_ASM_INTERNAL_FIXUPS_H */
