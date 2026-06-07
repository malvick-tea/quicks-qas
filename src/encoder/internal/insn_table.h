/*
 * qas — encoder: the instruction table (internal).
 *
 * The data the encoder is driven by (ADR-0011): each row is one encodable form of
 * a mnemonic — its operand pattern, opcode, and an encoding descriptor saying how
 * operands map onto the instruction's fields. The generic engine in encoder.c
 * turns a matched row plus the actual operands into bytes. Adding an instruction
 * is adding a row, not writing code. Only insn_table.c and encoder.c include this.
 *
 * The notation mirrors the Intel SDM Vol 2 opcode reference:
 *   - operand specs use the SDM's size letters: 8 = byte, v = word/dword/qword by
 *     operand size, plus specific registers (AL/rAX/CL) for accumulator forms.
 *   - ENC_* names the operand-to-field mapping (/r forms, /digit forms, +rd, etc).
 *   - IMMK_* names the immediate/displacement-from-relative to emit.
 */
#ifndef QAS_ENCODER_INTERNAL_INSN_TABLE_H
#define QAS_ENCODER_INTERNAL_INSN_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "ast/ast.h"
#include "source/source.h"

/* Operand specifications a form's operand can require. "v" forms vary with the
   operand size (16/32/64) selected by the 0x66 prefix and REX.W. */
typedef enum qas_opspec {
    OPS_NONE = 0,
    OPS_R8, OPS_RV,        /* register: byte / word-dword-qword                  */
    OPS_RM8, OPS_RMV,      /* register-or-memory: byte / v                       */
    OPS_R64, OPS_RM64,     /* specific 64-bit register / r-m (push/pop/call/jmp) */
    OPS_M,                 /* memory of any size (for lea)                       */
    OPS_IMM8,             /* 8-bit immediate (zero/sign acceptable)             */
    OPS_IMM8S,            /* 8-bit immediate restricted to sign-extend range    */
    OPS_IMMV,            /* operand-size immediate (imm16/imm32; 32 sign-ext@64)*/
    OPS_IMM32,           /* fixed 32-bit immediate (push imm32)                 */
    OPS_IMM64,           /* full 64-bit immediate (mov r64, imm64)              */
    OPS_REL8, OPS_REL32, /* branch displacement                                 */
    OPS_AL, OPS_rAX,     /* accumulator forms (AL; AX/EAX/RAX by size)          */
    OPS_CL,              /* the CL register (variable shift count)              */
    OPS_ONE              /* the literal constant 1 (shift-by-one forms)         */
} qas_opspec;

/* How operands map to instruction fields. */
typedef enum qas_enc {
    ENC_ZO = 0, /* no operands.                                                  */
    ENC_O,      /* register encoded in the opcode (+rd).                         */
    ENC_OI,     /* register in opcode, then immediate.                           */
    ENC_MR,     /* ModR/M: r/m = op0, reg = op1.                                 */
    ENC_RM,     /* ModR/M: reg = op0, r/m = op1.                                 */
    ENC_MI,     /* ModR/M: r/m = op0, reg = /digit, then immediate.              */
    ENC_M,      /* ModR/M: r/m = op0, reg = /digit.                              */
    ENC_M1,     /* ModR/M: r/m = op0, reg = /digit; op1 is the constant 1.       */
    ENC_MC,     /* ModR/M: r/m = op0, reg = /digit; op1 is CL.                   */
    ENC_D,      /* relative displacement operand (branches).                     */
    ENC_I       /* immediate only, no ModR/M (accumulator/push-imm forms).       */
} qas_enc;

/* Which immediate/relative field to emit. */
typedef enum qas_immk {
    IMMK_NONE = 0,
    IMMK_IB,    /* 1-byte immediate.                                             */
    IMMK_IW,    /* 2-byte immediate.                                             */
    IMMK_ID,    /* 4-byte immediate.                                             */
    IMMK_IV,    /* 2 bytes if operand size 16 else 4 (sign-extended at 64).      */
    IMMK_IOV,   /* full operand-size immediate: 2/4/8 by operand size.           */
    IMMK_REL8,  /* 1-byte PC-relative.                                           */
    IMMK_REL32  /* 4-byte PC-relative.                                           */
} qas_immk;

/* One encodable form. See header comment. */
typedef struct qas_insn_form {
    const char *mnemonic;
    qas_opspec  op[QAS_MAX_OPERANDS];
    uint8_t     op_count;
    qas_enc     enc;
    uint8_t     opcode;
    uint8_t     ext;          /* /digit for ENC_M/ENC_MI/ENC_M1/ENC_MC.          */
    bool        two_byte;     /* prefix the opcode with 0x0F.                    */
    bool        size_variant; /* derive 0x66/REX.W from the operand size.        */
    bool        rex_w;        /* force REX.W (non-variant 64-bit forms).         */
    bool        pfx_66;       /* force 0x66 (non-variant 16-bit forms).          */
    uint8_t     mand_pfx;     /* mandatory prefix byte (0xF2/0xF3), or 0.        */
    qas_immk    imm;          /* immediate/relative to emit.                     */
} qas_insn_form;

/* The selected form plus the resolved operand size (0/8/16/32/64) the engine
   needs for prefixes and the size-dependent immediate widths. */
typedef struct qas_insn_match {
    const qas_insn_form *form;
    uint8_t              op_size;
} qas_insn_match;

/*
 * Choose the encodable form for `insn` (mnemonic + operands). Returns true and
 * fills *out on success; returns false if the mnemonic is unknown or no form's
 * operand pattern matches (the caller turns that into a diagnostic). `src`
 * resolves the mnemonic and any symbol spans needed for size decisions.
 */
bool qas_insn_select(const qas_source *src, const qas_stmt *insn,
                     qas_insn_match *out);

#endif /* QAS_ENCODER_INTERNAL_INSN_TABLE_H */
