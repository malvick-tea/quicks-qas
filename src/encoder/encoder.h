/*
 * qas — instruction encoder (x86-64 machine-code emitter)
 *
 * Responsibility
 * Translate one parsed instruction (a qas_stmt of kind QAS_STMT_INSTRUCTION) into
 * the exact bytes the CPU decodes, plus any *fixups* (places that need a symbol's
 * address filled in later). Encoding is table-driven per ADR-0011: a cited
 * instruction table maps (mnemonic, operand pattern) to an opcode and an encoding
 * form, and one generic engine emits the field sequence of Intel SDM Vol 2 §2.1 —
 * legacy/operand-size prefixes, REX (§2.2.1), opcode, ModR/M and SIB (§2.1.5),
 * displacement, and immediate.
 *
 * Why fixups instead of relocations here
 *   The encoder is kept independent of the ELF layer and of the symbol table: a
 *   symbolic operand becomes a qas_fixup that names the symbol (as a source span)
 *   and the field to patch. The orchestrator decides whether to resolve it
 *   locally (a same-section PC-relative reference whose target is known) or to
 *   emit an ELF relocation, translating the fixup kind to an R_X86_64_* type.
 *
 * One instruction is at most 15 bytes (Intel SDM Vol 2 §2.3.11, instruction
 * length limit), so a fixed buffer suffices and there is no allocation here.
 */
#ifndef QAS_ENCODER_ENCODER_H
#define QAS_ENCODER_ENCODER_H

#include <stdint.h>

#include "ast/ast.h"
#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"

/* The architectural maximum length of an x86-64 instruction. */
#define QAS_INSN_MAX_LEN 15

/*
 * What kind of value a fixup needs and how it is computed. These map one-to-one
 * onto x86-64 psABI relocations, but are named in the encoder's own terms so the
 * encoder need not include the ELF header.
 */
typedef enum qas_fixup_kind {
    QAS_FIXUP_PC32 = 0, /* 4-byte PC-relative (S + A - P): rel32 / RIP-relative.  */
    QAS_FIXUP_ABS32S,   /* 4-byte absolute, sign-extended (S + A).               */
    QAS_FIXUP_ABS64     /* 8-byte absolute (S + A).                              */
} qas_fixup_kind;

/*
 * A field within the emitted bytes whose value depends on a symbol. `offset` is
 * the byte position of the field within this instruction; `size` its width (4 or
 * 8). The symbol is the source span [sym_off, sym_off+sym_len). `addend` is the
 * constant added in the relocation's computation (e.g. -4 for a rel32 branch).
 */
typedef struct qas_fixup {
    uint8_t        offset;
    uint8_t        size;
    qas_fixup_kind kind;
    size_t         sym_off;
    size_t         sym_len;
    int64_t        addend;
} qas_fixup;

/* The result of encoding one instruction: the bytes plus up to two fixups (a
   memory displacement and an immediate can each be symbolic). */
typedef struct qas_encoded {
    uint8_t   bytes[QAS_INSN_MAX_LEN];
    uint8_t   len;
    qas_fixup fixups[2];
    uint8_t   fixup_count;
} qas_encoded;

/*
 * Encode the instruction statement `insn` into *out. Returns QAS_OK on success.
 * On an unrecognized mnemonic, an operand pattern with no encoding, an illegal
 * register combination (e.g. a high-byte register that would require a REX
 * prefix), or an out-of-range immediate/displacement, it emits a diagnostic at
 * the offending location and returns QAS_ERR_ENCODE. `src` provides the bytes for
 * resolving operand symbol/mnemonic spans; `diags` collects error messages.
 */
qas_status qas_encode(const qas_source *src, const qas_stmt *insn,
                      qas_diag_sink *diags, qas_encoded *out);

#endif /* QAS_ENCODER_ENCODER_H */
