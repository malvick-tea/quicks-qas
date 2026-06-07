/*
 * qas — encoder: the generic encoding engine.
 *
 * Given a form selected from the instruction table and the actual operands, this
 * emits the instruction's bytes in the exact field order of Intel SDM Vol 2 §2.1,
 * Figure 2-1: [mandatory prefix] [operand-size 0x66] [REX] [0x0F] opcode
 * [ModR/M] [SIB] [displacement] [immediate]. The hard parts — REX construction
 * (§2.2.1), and the ModR/M+SIB addressing forms with their RSP/R12 SIB-escape and
 * RBP/R13 displacement rules (§2.1.5, Tables 2-2/2-3) — live here once and are
 * exercised byte-exact by the tests.
 */
#include "encoder/encoder.h"

#include <stdint.h>
#include <string.h>

#include "encoder/internal/insn_table.h"
#include "reg/reg.h"

/* The encoded ModR/M + SIB + displacement of a register-or-memory operand, plus
   the REX extension bits it contributes and any symbolic-displacement fixup. */
typedef struct rm_encoding {
    uint8_t        modrm;       /* mod and r/m fields; reg field left 0.          */
    bool           has_sib;
    uint8_t        sib;
    uint8_t        disp_len;    /* 0, 1, or 4 bytes.                              */
    int64_t        disp;        /* displacement value (when not symbolic).        */
    bool           rex_x;
    bool           rex_b;
    bool           disp_symbol;
    size_t         sym_off;
    size_t         sym_len;
    qas_fixup_kind disp_fixkind;
    int64_t        disp_addend;
} rm_encoding;

static bool fits_int8(int64_t s)  { return s >= -128 && s <= 127; }
static bool fits_int32(int64_t s) { return s >= INT32_MIN && s <= INT32_MAX; }

static uint8_t scale_to_bits(uint8_t scale)
{
    switch (scale) {
    case 2:  return 1;
    case 4:  return 2;
    case 8:  return 3;
    default: return 0; /* scale 1 (and the default) */
    }
}

/* Emit one byte into the instruction buffer; fails only on the architectural
   15-byte limit (Intel SDM Vol 2 §2.3.11), which our subset never reaches. */
static qas_status put(qas_encoded *out, uint8_t b)
{
    if (out->len >= QAS_INSN_MAX_LEN) {
        return QAS_ERR_ENCODE;
    }
    out->bytes[out->len++] = b;
    return QAS_OK;
}

/*
 * Encode a register operand as a direct (mod=11) ModR/M r/m. `rex_required` /
 * `forbid_rex` accumulate the byte-register constraints from this register.
 */
static void encode_rm_register(const qas_reg *r, rm_encoding *rm,
                               bool *need_rex, bool *forbid_rex)
{
    rm->modrm   = (uint8_t)(0xC0 | qas_reg_low3(r)); /* mod=11, r/m=reg          */
    rm->has_sib = false;
    rm->disp_len = 0;
    rm->rex_b   = qas_reg_ext(r) != 0;
    rm->rex_x   = false;
    *need_rex   = *need_rex || r->rex_required;
    *forbid_rex = *forbid_rex || r->high_byte;
}

/*
 * Encode a memory operand into ModR/M + SIB + displacement per Intel SDM Vol 2
 * §2.1.5 (Tables 2-2/2-3). `imm_tail` is the number of immediate bytes that will
 * follow the displacement, needed to compute the RIP-relative addend (the CPU's
 * RIP is the address of the *next* instruction). Returns QAS_ERR_ENCODE on an
 * out-of-range displacement.
 */
static qas_status encode_rm_memory(const qas_source *src, const qas_stmt *insn,
                                   qas_diag_sink *diags, const qas_mem *m,
                                   uint8_t imm_tail, rm_encoding *rm)
{
    memset(rm, 0, sizeof *rm);

    /* RIP-relative: mod=00, r/m=101, always disp32 (Intel SDM Vol 2 §2.2.1.6). */
    if (m->base != NULL && m->base->reg_class == QAS_REG_CLASS_IP) {
        rm->modrm    = 0x05; /* mod=00, r/m=101 */
        rm->disp_len = 4;
        if (m->disp_is_symbol) {
            rm->disp_symbol  = true;
            rm->sym_off      = m->sym_off;
            rm->sym_len      = m->sym_len;
            rm->disp_fixkind = QAS_FIXUP_PC32;
            /* PC32 = S + A - P, P = the disp field; the CPU adds disp to the next
               instruction's address, so A = addend - (4 + imm_tail). */
            rm->disp_addend  = m->disp - (int64_t)(4 + imm_tail);
        } else {
            if (!fits_int32(m->disp)) {
                (void)qas_diag_emit(diags, QAS_DIAG_ERROR, src, insn->offset,
                                    insn->length, "RIP-relative displacement out of range");
                return QAS_ERR_ENCODE;
            }
            rm->disp = m->disp;
        }
        return QAS_OK;
    }

    const qas_reg *base  = m->base;
    const qas_reg *index = m->index;
    bool has_base  = base != NULL;
    bool has_index = index != NULL;
    uint8_t scale_bits = scale_to_bits(m->scale ? m->scale : 1);

    /* Decide mod and displacement length. */
    uint8_t mod;
    if (m->disp_is_symbol) {
        mod = 2; rm->disp_len = 4; /* absolute symbol => disp32 relocation */
    } else if (!has_base) {
        mod = 0; rm->disp_len = 4; /* [disp32] / [index*scale + disp32] */
    } else {
        bool base_is_bp = (qas_reg_low3(base) == 5); /* rbp/r13: no mod=00 form */
        if (m->disp == 0 && !base_is_bp) {
            mod = 0; rm->disp_len = 0;
        } else if (fits_int8(m->disp)) {
            mod = 1; rm->disp_len = 1;
        } else if (fits_int32(m->disp)) {
            mod = 2; rm->disp_len = 4;
        } else {
            (void)qas_diag_emit(diags, QAS_DIAG_ERROR, src, insn->offset,
                                insn->length, "displacement out of range");
            return QAS_ERR_ENCODE;
        }
    }

    bool need_sib = has_index || (has_base && qas_reg_low3(base) == 4);

    if (!has_base && !has_index) {
        /* Absolute [disp32]: ModR/M r/m=100 + SIB base=101,index=100, mod=00. */
        rm->modrm   = 0x04;
        rm->has_sib = true;
        rm->sib     = (uint8_t)((0 << 6) | (4 << 3) | 5);
        rm->disp_len = 4;
    } else if (need_sib) {
        rm->modrm   = (uint8_t)((mod << 6) | 0x04); /* r/m=100 selects SIB */
        rm->has_sib = true;
        uint8_t sib_index = has_index ? qas_reg_low3(index) : 4; /* 100 = none */
        uint8_t sib_base  = has_base ? qas_reg_low3(base) : 5;   /* 101 = none */
        rm->sib     = (uint8_t)((scale_bits << 6) | (sib_index << 3) | sib_base);
        rm->rex_x   = has_index && qas_reg_ext(index) != 0;
        rm->rex_b   = has_base && qas_reg_ext(base) != 0;
        if (!has_base) {
            /* index-only: SIB base=101 with mod=00 means disp32, no base. */
            rm->disp_len = 4;
        }
    } else {
        /* Plain base register in r/m. */
        rm->modrm = (uint8_t)((mod << 6) | qas_reg_low3(base));
        rm->rex_b = qas_reg_ext(base) != 0;
    }

    if (m->disp_is_symbol) {
        rm->disp_symbol  = true;
        rm->sym_off      = m->sym_off;
        rm->sym_len      = m->sym_len;
        rm->disp_fixkind = QAS_FIXUP_ABS32S;
        rm->disp_addend  = m->disp;
    } else {
        rm->disp = m->disp;
    }
    return QAS_OK;
}

/* Immediate width in bytes for an IMMK at the given operand size. */
static uint8_t imm_width(qas_immk k, uint8_t op_size)
{
    switch (k) {
    case IMMK_NONE:  return 0;
    case IMMK_IB:    return 1;
    case IMMK_IW:    return 2;
    case IMMK_ID:    return 4;
    case IMMK_IV:    return (op_size == 16) ? 2 : 4;
    case IMMK_IOV:   return (op_size == 16) ? 2 : (op_size == 32) ? 4 : 8;
    case IMMK_REL8:  return 1;
    case IMMK_REL32: return 4;
    }
    return 0;
}

/* Find the single immediate/relative operand of an instruction (the only one of
   kind IMM), or NULL. Used by the accumulator/push-imm/branch encodings. */
static const qas_operand *find_imm_operand(const qas_stmt *insn)
{
    for (size_t i = 0; i < insn->operand_count; ++i) {
        if (insn->operands[i].kind == QAS_OPERAND_IMM) {
            return &insn->operands[i];
        }
    }
    return NULL;
}

/* Append `width` little-endian bytes of `value` to the instruction. */
static qas_status put_le(qas_encoded *out, uint64_t value, uint8_t width)
{
    for (uint8_t i = 0; i < width; ++i) {
        qas_status st = put(out, (uint8_t)(value & 0xFFu));
        if (st != QAS_OK) {
            return st;
        }
        value >>= 8;
    }
    return QAS_OK;
}

qas_status qas_encode(const qas_source *src, const qas_stmt *insn,
                      qas_diag_sink *diags, qas_encoded *out)
{
    if (src == NULL || insn == NULL || out == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);

    qas_insn_match m;
    if (!qas_insn_select(src, insn, &m)) {
        (void)qas_diag_emit(diags, QAS_DIAG_ERROR, src, insn->offset, insn->length,
                            "no x86-64 encoding for this instruction/operand combination");
        return QAS_ERR_ENCODE;
    }
    const qas_insn_form *f = m.form;
    uint8_t op_size = m.op_size;

    /* Prefixes and REX.W: derived from the operand size for size-variant forms,
       taken from explicit flags otherwise (Intel SDM Vol 2 §2.2.1.2). */
    bool pfx_66 = f->size_variant ? (op_size == 16) : f->pfx_66;
    bool rex_w  = f->size_variant ? (op_size == 64) : f->rex_w;

    /* Immediate operand and the number of immediate bytes (the "tail" after any
       displacement, needed for the RIP-relative addend). */
    const qas_operand *imm_op = NULL;
    uint8_t imm_bytes = 0;
    if (f->imm != IMMK_NONE) {
        imm_bytes = imm_width(f->imm, op_size);
        imm_op = (f->enc == ENC_D) ? &insn->operands[0] : find_imm_operand(insn);
    }

    /* Build the opcode and (for ModR/M forms) the reg field, r/m encoding, and
       the REX.R/X/B + byte-register constraints. */
    uint8_t opcode = f->opcode;
    uint8_t reg3   = 0;
    bool    rex_r = false, need_rex = false, forbid_rex = false;
    bool    have_modrm = false;
    rm_encoding rm;
    memset(&rm, 0, sizeof rm);

    const qas_operand *rm_op  = NULL;
    const qas_operand *reg_op = NULL;

    switch (f->enc) {
    case ENC_ZO:
    case ENC_I:
    case ENC_D:
        break; /* no ModR/M, no opcode register */
    case ENC_O:
    case ENC_OI: {
        const qas_reg *r = insn->operands[0].reg;
        opcode = (uint8_t)(opcode + qas_reg_low3(r));
        rm.rex_b   = qas_reg_ext(r) != 0; /* +rd extension is REX.B */
        need_rex   = need_rex || r->rex_required;
        forbid_rex = forbid_rex || r->high_byte;
        break;
    }
    case ENC_MR:
        rm_op = &insn->operands[0];
        reg_op = &insn->operands[1];
        break;
    case ENC_RM:
        reg_op = &insn->operands[0];
        rm_op = &insn->operands[1];
        break;
    case ENC_MI:
    case ENC_M:
    case ENC_M1:
    case ENC_MC:
        rm_op = &insn->operands[0];
        reg3 = f->ext; /* /digit opcode extension */
        break;
    }

    if (reg_op != NULL) {
        reg3 = qas_reg_low3(reg_op->reg);
        rex_r = qas_reg_ext(reg_op->reg) != 0;
        need_rex = need_rex || reg_op->reg->rex_required;
        forbid_rex = forbid_rex || reg_op->reg->high_byte;
    }

    if (rm_op != NULL) {
        have_modrm = true;
        if (rm_op->kind == QAS_OPERAND_REG) {
            encode_rm_register(rm_op->reg, &rm, &need_rex, &forbid_rex);
        } else { /* memory */
            qas_status st = encode_rm_memory(src, insn, diags, &rm_op->mem,
                                             imm_bytes, &rm);
            if (st != QAS_OK) {
                return st;
            }
        }
    }

    /* Combine REX bits and validate the byte-register rules. */
    bool rex_b = rm.rex_b;
    bool rex_x = rm.rex_x;
    bool rex_needed = rex_w || rex_r || rex_x || rex_b || need_rex;
    if (rex_needed && forbid_rex) {
        (void)qas_diag_emit(diags, QAS_DIAG_ERROR, src, insn->offset, insn->length,
                            "cannot encode a high-byte register (ah/ch/dh/bh) with a "
                            "REX prefix; use spl/bpl/sil/dil or another register");
        return QAS_ERR_ENCODE;
    }

    /* Emit, in the order of Intel SDM Vol 2 Figure 2-1. */
    qas_status st;
    if (f->mand_pfx != 0) {
        if ((st = put(out, f->mand_pfx)) != QAS_OK) return st;
    }
    if (pfx_66) {
        if ((st = put(out, 0x66)) != QAS_OK) return st;
    }
    if (rex_needed) {
        uint8_t rex = (uint8_t)(0x40 | (rex_w << 3) | (rex_r << 2) |
                                (rex_x << 1) | (rex_b ? 1 : 0));
        if ((st = put(out, rex)) != QAS_OK) return st;
    }
    if (f->two_byte) {
        if ((st = put(out, 0x0F)) != QAS_OK) return st;
    }
    if ((st = put(out, opcode)) != QAS_OK) return st;

    if (have_modrm) {
        if ((st = put(out, (uint8_t)(rm.modrm | (reg3 << 3)))) != QAS_OK) return st;
        if (rm.has_sib) {
            if ((st = put(out, rm.sib)) != QAS_OK) return st;
        }
        if (rm.disp_len > 0) {
            if (rm.disp_symbol) {
                qas_fixup *fx = &out->fixups[out->fixup_count++];
                fx->offset  = out->len;
                fx->size    = rm.disp_len;
                fx->kind    = rm.disp_fixkind;
                fx->sym_off = rm.sym_off;
                fx->sym_len = rm.sym_len;
                fx->addend  = rm.disp_addend;
                if ((st = put_le(out, 0, rm.disp_len)) != QAS_OK) return st;
            } else {
                if ((st = put_le(out, (uint64_t)rm.disp, rm.disp_len)) != QAS_OK) return st;
            }
        }
    }

    /* Immediate or relative. */
    if (f->imm != IMMK_NONE && imm_op != NULL) {
        bool is_rel = (f->imm == IMMK_REL8 || f->imm == IMMK_REL32);
        if (imm_op->imm.is_symbol) {
            qas_fixup *fx = &out->fixups[out->fixup_count++];
            fx->offset  = out->len;
            fx->size    = imm_bytes;
            fx->sym_off = imm_op->imm.sym_off;
            fx->sym_len = imm_op->imm.sym_len;
            if (is_rel) {
                fx->kind   = QAS_FIXUP_PC32;
                fx->addend = (int64_t)imm_op->imm.value - (int64_t)imm_bytes;
            } else if (imm_bytes == 8) {
                fx->kind   = QAS_FIXUP_ABS64;
                fx->addend = (int64_t)imm_op->imm.value;
            } else if (imm_bytes == 4) {
                fx->kind   = QAS_FIXUP_ABS32S;
                fx->addend = (int64_t)imm_op->imm.value;
            } else {
                (void)qas_diag_emit(diags, QAS_DIAG_ERROR, src, imm_op->offset,
                                    imm_op->length,
                                    "a symbol cannot be used in a %u-bit immediate",
                                    (unsigned)imm_bytes * 8u);
                return QAS_ERR_ENCODE;
            }
            if ((st = put_le(out, 0, imm_bytes)) != QAS_OK) return st;
        } else {
            uint64_t v = imm_op->imm.value;
            /* Range check: a 32-bit immediate that is sign-extended to 64 bits
               must fit a signed 32-bit field. */
            bool sext32 = (f->imm == IMMK_IV && op_size == 64);
            bool ok = true;
            if (sext32) {
                ok = fits_int32((int64_t)v);
            } else if (imm_bytes == 1) {
                ok = fits_int8((int64_t)v) || v <= 0xFFu;
            } else if (imm_bytes == 2) {
                ok = ((int64_t)v >= -32768 && (int64_t)v <= 32767) || v <= 0xFFFFu;
            } else if (imm_bytes == 4) {
                ok = fits_int32((int64_t)v) || v <= 0xFFFFFFFFu;
            }
            if (!ok) {
                (void)qas_diag_emit(diags, QAS_DIAG_ERROR, src, imm_op->offset,
                                    imm_op->length, "immediate out of range for operand size");
                return QAS_ERR_ENCODE;
            }
            if ((st = put_le(out, v, imm_bytes)) != QAS_OK) return st;
        }
    }

    return QAS_OK;
}
