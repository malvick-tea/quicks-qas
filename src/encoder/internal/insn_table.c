/*
 * qas — encoder: the instruction table and form selection.
 *
 * Each row is transcribed from the Intel SDM Vol 2 opcode reference. The regular
 * families (the eight ADD/OR/ADC/SBB/AND/SUB/XOR/CMP arithmetic ops, the F6/F7
 * unary group, the shift group, and the Jcc set) are expanded by macros so the
 * arithmetic relationship between their opcodes is written once and cannot drift
 * (coding-standard §3; ADR-0011). Forms for a mnemonic are ordered most-specific
 * /shortest first, and selection takes the first match — so, e.g., `add r/m, imm8`
 * (the 0x83 sign-extended form) is preferred over the longer 0x81 form.
 */
#include "encoder/internal/insn_table.h"

#include <stdint.h>

/* The eight basic arithmetic/logic instructions share an encoding pattern: the
   group number G (0..7) selects the opcode block (0x00|0x08|... for the r/m,r
   forms; 0x80/0x81/0x83 /G for the immediate forms) — Intel SDM Vol 2, the ADD
   reference plus "Table B-13"-style regularity across the group. */
#define ARITH_FORMS(MN, G) \
    { .mnemonic = MN, .op = {OPS_RM8, OPS_R8},    .op_count = 2, .enc = ENC_MR, .opcode = (uint8_t)(0x00 + (G) * 8) }, \
    { .mnemonic = MN, .op = {OPS_RMV, OPS_RV},    .op_count = 2, .enc = ENC_MR, .opcode = (uint8_t)(0x01 + (G) * 8), .size_variant = true }, \
    { .mnemonic = MN, .op = {OPS_R8, OPS_RM8},    .op_count = 2, .enc = ENC_RM, .opcode = (uint8_t)(0x02 + (G) * 8) }, \
    { .mnemonic = MN, .op = {OPS_RV, OPS_RMV},    .op_count = 2, .enc = ENC_RM, .opcode = (uint8_t)(0x03 + (G) * 8), .size_variant = true }, \
    { .mnemonic = MN, .op = {OPS_RMV, OPS_IMM8S}, .op_count = 2, .enc = ENC_MI, .opcode = 0x83, .ext = (G), .size_variant = true, .imm = IMMK_IB }, \
    { .mnemonic = MN, .op = {OPS_AL, OPS_IMM8},   .op_count = 2, .enc = ENC_I,  .opcode = (uint8_t)(0x04 + (G) * 8), .imm = IMMK_IB }, \
    { .mnemonic = MN, .op = {OPS_rAX, OPS_IMMV},  .op_count = 2, .enc = ENC_I,  .opcode = (uint8_t)(0x05 + (G) * 8), .size_variant = true, .imm = IMMK_IV }, \
    { .mnemonic = MN, .op = {OPS_RM8, OPS_IMM8},  .op_count = 2, .enc = ENC_MI, .opcode = 0x80, .ext = (G), .imm = IMMK_IB }, \
    { .mnemonic = MN, .op = {OPS_RMV, OPS_IMMV},  .op_count = 2, .enc = ENC_MI, .opcode = 0x81, .ext = (G), .size_variant = true, .imm = IMMK_IV }

/* F6/F7 /digit unary group (NEG/NOT/MUL/IMUL1/DIV/IDIV). */
#define UNARY_FORMS(MN, N) \
    { .mnemonic = MN, .op = {OPS_RM8}, .op_count = 1, .enc = ENC_M, .opcode = 0xF6, .ext = (N) }, \
    { .mnemonic = MN, .op = {OPS_RMV}, .op_count = 1, .enc = ENC_M, .opcode = 0xF7, .ext = (N), .size_variant = true }

/* FE/FF /digit INC/DEC group (the 0x40+ short forms are invalid in 64-bit mode). */
#define INCDEC_FORMS(MN, N) \
    { .mnemonic = MN, .op = {OPS_RM8}, .op_count = 1, .enc = ENC_M, .opcode = 0xFE, .ext = (N) }, \
    { .mnemonic = MN, .op = {OPS_RMV}, .op_count = 1, .enc = ENC_M, .opcode = 0xFF, .ext = (N), .size_variant = true }

/* Shift/rotate group: by 1 (D0/D1), by CL (D2/D3), by imm8 (C0/C1), /digit = N. */
#define SHIFT_FORMS(MN, N) \
    { .mnemonic = MN, .op = {OPS_RM8, OPS_ONE},  .op_count = 2, .enc = ENC_M1, .opcode = 0xD0, .ext = (N) }, \
    { .mnemonic = MN, .op = {OPS_RMV, OPS_ONE},  .op_count = 2, .enc = ENC_M1, .opcode = 0xD1, .ext = (N), .size_variant = true }, \
    { .mnemonic = MN, .op = {OPS_RM8, OPS_CL},   .op_count = 2, .enc = ENC_MC, .opcode = 0xD2, .ext = (N) }, \
    { .mnemonic = MN, .op = {OPS_RMV, OPS_CL},   .op_count = 2, .enc = ENC_MC, .opcode = 0xD3, .ext = (N), .size_variant = true }, \
    { .mnemonic = MN, .op = {OPS_RM8, OPS_IMM8}, .op_count = 2, .enc = ENC_MI, .opcode = 0xC0, .ext = (N), .imm = IMMK_IB }, \
    { .mnemonic = MN, .op = {OPS_RMV, OPS_IMM8}, .op_count = 2, .enc = ENC_MI, .opcode = 0xC1, .ext = (N), .size_variant = true, .imm = IMMK_IB }

/* Jcc: short (0x70+cc) and near (0x0F 0x80+cc). Short first so a literal in range
   uses the compact form; a symbol target only matches the near (rel32) form. */
#define JCC_FORMS(MN, CC) \
    { .mnemonic = MN, .op = {OPS_REL8},  .op_count = 1, .enc = ENC_D, .opcode = (uint8_t)(0x70 + (CC)), .imm = IMMK_REL8 }, \
    { .mnemonic = MN, .op = {OPS_REL32}, .op_count = 1, .enc = ENC_D, .opcode = (uint8_t)(0x80 + (CC)), .two_byte = true, .imm = IMMK_REL32 }

static const qas_insn_form qas_insn_forms[] = {
    /* Data movement. reg,reg prefers the MR (store) form, matching common
       assembler output; mov r/m64,imm32 (REX.W C7 /0) beats the 10-byte imm64
       form when the value fits a sign-extended 32-bit field. */
    { .mnemonic = "mov", .op = {OPS_RM8, OPS_R8},    .op_count = 2, .enc = ENC_MR, .opcode = 0x88 },
    { .mnemonic = "mov", .op = {OPS_RMV, OPS_RV},    .op_count = 2, .enc = ENC_MR, .opcode = 0x89, .size_variant = true },
    { .mnemonic = "mov", .op = {OPS_R8, OPS_RM8},    .op_count = 2, .enc = ENC_RM, .opcode = 0x8A },
    { .mnemonic = "mov", .op = {OPS_RV, OPS_RMV},    .op_count = 2, .enc = ENC_RM, .opcode = 0x8B, .size_variant = true },
    { .mnemonic = "mov", .op = {OPS_RM64, OPS_IMM32},.op_count = 2, .enc = ENC_MI, .opcode = 0xC7, .ext = 0, .rex_w = true, .imm = IMMK_IV },
    { .mnemonic = "mov", .op = {OPS_R8, OPS_IMM8},   .op_count = 2, .enc = ENC_OI, .opcode = 0xB0, .imm = IMMK_IB },
    { .mnemonic = "mov", .op = {OPS_RV, OPS_IMM64},  .op_count = 2, .enc = ENC_OI, .opcode = 0xB8, .size_variant = true, .imm = IMMK_IOV },
    { .mnemonic = "mov", .op = {OPS_RM8, OPS_IMM8},  .op_count = 2, .enc = ENC_MI, .opcode = 0xC6, .ext = 0, .imm = IMMK_IB },
    { .mnemonic = "mov", .op = {OPS_RMV, OPS_IMMV},  .op_count = 2, .enc = ENC_MI, .opcode = 0xC7, .ext = 0, .size_variant = true, .imm = IMMK_IV },

    { .mnemonic = "lea", .op = {OPS_RV, OPS_M},      .op_count = 2, .enc = ENC_RM, .opcode = 0x8D, .size_variant = true },

    ARITH_FORMS("add", 0),
    ARITH_FORMS("or",  1),
    ARITH_FORMS("adc", 2),
    ARITH_FORMS("sbb", 3),
    ARITH_FORMS("and", 4),
    ARITH_FORMS("sub", 5),
    ARITH_FORMS("xor", 6),
    ARITH_FORMS("cmp", 7),

    /* test: MR forms and the accumulator/imm forms (no RM form exists). */
    { .mnemonic = "test", .op = {OPS_RM8, OPS_R8},   .op_count = 2, .enc = ENC_MR, .opcode = 0x84 },
    { .mnemonic = "test", .op = {OPS_RMV, OPS_RV},   .op_count = 2, .enc = ENC_MR, .opcode = 0x85, .size_variant = true },
    { .mnemonic = "test", .op = {OPS_AL, OPS_IMM8},  .op_count = 2, .enc = ENC_I,  .opcode = 0xA8, .imm = IMMK_IB },
    { .mnemonic = "test", .op = {OPS_rAX, OPS_IMMV}, .op_count = 2, .enc = ENC_I,  .opcode = 0xA9, .size_variant = true, .imm = IMMK_IV },
    { .mnemonic = "test", .op = {OPS_RM8, OPS_IMM8}, .op_count = 2, .enc = ENC_MI, .opcode = 0xF6, .ext = 0, .imm = IMMK_IB },
    { .mnemonic = "test", .op = {OPS_RMV, OPS_IMMV}, .op_count = 2, .enc = ENC_MI, .opcode = 0xF7, .ext = 0, .size_variant = true, .imm = IMMK_IV },

    UNARY_FORMS("not",  2),
    UNARY_FORMS("neg",  3),
    UNARY_FORMS("mul",  4),
    UNARY_FORMS("imul", 5),
    UNARY_FORMS("div",  6),
    UNARY_FORMS("idiv", 7),

    INCDEC_FORMS("inc", 0),
    INCDEC_FORMS("dec", 1),

    SHIFT_FORMS("shl", 4),
    SHIFT_FORMS("shr", 5),
    SHIFT_FORMS("sar", 7),

    /* Stack. push/pop default to 64-bit operand size in long mode (no REX.W). */
    { .mnemonic = "push", .op = {OPS_R64},   .op_count = 1, .enc = ENC_O, .opcode = 0x50 },
    { .mnemonic = "push", .op = {OPS_IMM8},  .op_count = 1, .enc = ENC_I, .opcode = 0x6A, .imm = IMMK_IB },
    { .mnemonic = "push", .op = {OPS_IMM32}, .op_count = 1, .enc = ENC_I, .opcode = 0x68, .imm = IMMK_ID },
    { .mnemonic = "push", .op = {OPS_RM64},  .op_count = 1, .enc = ENC_M, .opcode = 0xFF, .ext = 6 },
    { .mnemonic = "pop",  .op = {OPS_R64},   .op_count = 1, .enc = ENC_O, .opcode = 0x58 },
    { .mnemonic = "pop",  .op = {OPS_RM64},  .op_count = 1, .enc = ENC_M, .opcode = 0x8F, .ext = 0 },

    /* Control transfer. rel forms first; register/memory indirect via FF /digit. */
    { .mnemonic = "call", .op = {OPS_REL32}, .op_count = 1, .enc = ENC_D, .opcode = 0xE8, .imm = IMMK_REL32 },
    { .mnemonic = "call", .op = {OPS_RM64},  .op_count = 1, .enc = ENC_M, .opcode = 0xFF, .ext = 2 },
    { .mnemonic = "jmp",  .op = {OPS_REL8},  .op_count = 1, .enc = ENC_D, .opcode = 0xEB, .imm = IMMK_REL8 },
    { .mnemonic = "jmp",  .op = {OPS_REL32}, .op_count = 1, .enc = ENC_D, .opcode = 0xE9, .imm = IMMK_REL32 },
    { .mnemonic = "jmp",  .op = {OPS_RM64},  .op_count = 1, .enc = ENC_M, .opcode = 0xFF, .ext = 4 },

    JCC_FORMS("jo",  0x0), JCC_FORMS("jno", 0x1),
    JCC_FORMS("jb",  0x2), JCC_FORMS("jc",  0x2), JCC_FORMS("jnae", 0x2),
    JCC_FORMS("jae", 0x3), JCC_FORMS("jnb", 0x3), JCC_FORMS("jnc",  0x3),
    JCC_FORMS("je",  0x4), JCC_FORMS("jz",  0x4),
    JCC_FORMS("jne", 0x5), JCC_FORMS("jnz", 0x5),
    JCC_FORMS("jbe", 0x6), JCC_FORMS("jna", 0x6),
    JCC_FORMS("ja",  0x7), JCC_FORMS("jnbe",0x7),
    JCC_FORMS("js",  0x8), JCC_FORMS("jns", 0x9),
    JCC_FORMS("jp",  0xA), JCC_FORMS("jpe", 0xA),
    JCC_FORMS("jnp", 0xB), JCC_FORMS("jpo", 0xB),
    JCC_FORMS("jl",  0xC), JCC_FORMS("jnge",0xC),
    JCC_FORMS("jge", 0xD), JCC_FORMS("jnl", 0xD),
    JCC_FORMS("jle", 0xE), JCC_FORMS("jng", 0xE),
    JCC_FORMS("jg",  0xF), JCC_FORMS("jnle",0xF),

    /* No-operand and miscellaneous. */
    { .mnemonic = "ret",     .op_count = 0, .enc = ENC_ZO, .opcode = 0xC3 },
    { .mnemonic = "leave",   .op_count = 0, .enc = ENC_ZO, .opcode = 0xC9 },
    { .mnemonic = "nop",     .op_count = 0, .enc = ENC_ZO, .opcode = 0x90 },
    { .mnemonic = "hlt",     .op_count = 0, .enc = ENC_ZO, .opcode = 0xF4 },
    { .mnemonic = "int3",    .op_count = 0, .enc = ENC_ZO, .opcode = 0xCC },
    { .mnemonic = "cdq",     .op_count = 0, .enc = ENC_ZO, .opcode = 0x99 },
    { .mnemonic = "cqo",     .op_count = 0, .enc = ENC_ZO, .opcode = 0x99, .rex_w = true },
    { .mnemonic = "syscall", .op_count = 0, .enc = ENC_ZO, .opcode = 0x05, .two_byte = true },
    { .mnemonic = "cpuid",   .op_count = 0, .enc = ENC_ZO, .opcode = 0xA2, .two_byte = true },
    { .mnemonic = "ud2",     .op_count = 0, .enc = ENC_ZO, .opcode = 0x0B, .two_byte = true },
    { .mnemonic = "int",     .op = {OPS_IMM8}, .op_count = 1, .enc = ENC_I, .opcode = 0xCD, .imm = IMMK_IB },
};

static const size_t qas_insn_form_count =
    sizeof(qas_insn_forms) / sizeof(qas_insn_forms[0]);

/* Integer-range predicates over a value stored as raw 64-bit bits. */
static bool fits_int8(uint64_t v)  { int64_t s = (int64_t)v; return s >= -128 && s <= 127; }
static bool fits_byte(uint64_t v)  { int64_t s = (int64_t)v; return s >= -128 && s <= 255; }
static bool fits_int32(uint64_t v) { int64_t s = (int64_t)v; return s >= INT32_MIN && s <= INT32_MAX; }

/* ASCII case-insensitive compare of the mnemonic span to a lowercase name. */
static bool mnemonic_is(const qas_source *src, const qas_stmt *insn, const char *name)
{
    const char *s = src->data + insn->name_off;
    size_t      n = insn->name_len;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (name[i] == '\0' || c != name[i]) {
            return false;
        }
    }
    return name[n] == '\0';
}

/*
 * The word/dword/qword operand size implied by the operands (16/32/64), 0 if none
 * implies one, or 0xFF if two disagree (no encoding can satisfy that). Only 16/32/
 * 64 are "operand sizes" controlled by 0x66/REX.W; 8-bit forms have their own
 * opcodes and never consult this. Crucially, 8-bit operands are *excluded*, so a
 * fixed 8-bit count like CL in `shl rax, cl` does not falsely conflict with the
 * 64-bit destination (and 8-bit data operands are matched by their own specs).
 */
static uint8_t compute_op_size(const qas_stmt *insn)
{
    uint8_t size = 0;
    for (size_t i = 0; i < insn->operand_count; ++i) {
        const qas_operand *op = &insn->operands[i];
        uint8_t s = 0;
        if (op->kind == QAS_OPERAND_REG) {
            s = op->reg->size_bits;
        } else if (op->kind == QAS_OPERAND_MEM && op->mem.size != 0) {
            s = op->mem.size;
        }
        if (s >= 16) {
            if (size == 0) {
                size = s;
            } else if (size != s) {
                return 0xFF;
            }
        }
    }
    return size;
}

static bool is_gpr(const qas_operand *op)
{
    return op->kind == QAS_OPERAND_REG && op->reg->reg_class == QAS_REG_CLASS_GPR;
}

/* Does operand `op` satisfy spec, given the resolved op_size? */
static bool match_spec(qas_opspec spec, const qas_operand *op, uint8_t op_size)
{
    switch (spec) {
    case OPS_NONE:
        return op->kind == QAS_OPERAND_NONE;
    case OPS_R8:
        return is_gpr(op) && op->reg->size_bits == 8;
    case OPS_RV:
        return is_gpr(op) && op->reg->size_bits == op_size && op_size >= 16;
    case OPS_R64:
        return is_gpr(op) && op->reg->size_bits == 64;
    case OPS_RM8:
        if (is_gpr(op)) return op->reg->size_bits == 8;
        return op->kind == QAS_OPERAND_MEM &&
               (op->mem.size == 8 || (op->mem.size == 0 && op_size == 8));
    case OPS_RMV:
        if (is_gpr(op)) return op->reg->size_bits == op_size && op_size >= 16;
        return op->kind == QAS_OPERAND_MEM &&
               ((op->mem.size >= 16 && op->mem.size == op_size) ||
                (op->mem.size == 0 && op_size >= 16));
    case OPS_RM64:
        if (is_gpr(op)) return op->reg->size_bits == 64;
        return op->kind == QAS_OPERAND_MEM &&
               (op->mem.size == 64 || op->mem.size == 0);
    case OPS_M:
        return op->kind == QAS_OPERAND_MEM;
    case OPS_IMM8:
        return op->kind == QAS_OPERAND_IMM && !op->imm.is_symbol && fits_byte(op->imm.value);
    case OPS_IMM8S:
        return op->kind == QAS_OPERAND_IMM && !op->imm.is_symbol && fits_int8(op->imm.value);
    case OPS_IMMV:
        return op->kind == QAS_OPERAND_IMM;
    case OPS_IMM32:
        return op->kind == QAS_OPERAND_IMM &&
               (op->imm.is_symbol || fits_int32(op->imm.value));
    case OPS_IMM64:
        return op->kind == QAS_OPERAND_IMM;
    case OPS_REL8:
        return op->kind == QAS_OPERAND_IMM && !op->imm.is_symbol && fits_int8(op->imm.value);
    case OPS_REL32:
        return op->kind == QAS_OPERAND_IMM;
    case OPS_AL:
        return op->kind == QAS_OPERAND_REG && op->reg->size_bits == 8 &&
               op->reg->encoding == 0 && !op->reg->high_byte;
    case OPS_rAX:
        return is_gpr(op) && op->reg->encoding == 0 &&
               op->reg->size_bits == op_size && op_size >= 16;
    case OPS_CL:
        return op->kind == QAS_OPERAND_REG && op->reg->size_bits == 8 &&
               op->reg->encoding == 1 && !op->reg->high_byte;
    case OPS_ONE:
        return op->kind == QAS_OPERAND_IMM && !op->imm.is_symbol && op->imm.value == 1;
    }
    return false;
}

bool qas_insn_select(const qas_source *src, const qas_stmt *insn,
                     qas_insn_match *out)
{
    uint8_t op_size = compute_op_size(insn);
    if (op_size == 0xFF) {
        return false; /* conflicting operand sizes */
    }

    for (size_t i = 0; i < qas_insn_form_count; ++i) {
        const qas_insn_form *f = &qas_insn_forms[i];
        if (f->op_count != insn->operand_count) {
            continue;
        }
        if (!mnemonic_is(src, insn, f->mnemonic)) {
            continue;
        }
        bool all = true;
        for (size_t k = 0; k < insn->operand_count; ++k) {
            if (!match_spec(f->op[k], &insn->operands[k], op_size)) {
                all = false;
                break;
            }
        }
        if (all) {
            out->form    = f;
            out->op_size = op_size;
            return true;
        }
    }
    return false;
}
