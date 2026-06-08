/*
 * qas — assembler driver: directive handling (implementation).
 *
 * One entry point, qas_asm_apply_directive, dispatches on the directive name to a
 * small handler. The dispatch is a chain of name comparisons (not the table-driven
 * style the *encoder* uses): directives are a handful of distinct behaviors, not a
 * uniform field-layout problem, so a clear handler per directive is the readable
 * choice here. Each handler validates its arguments at the boundary and reports
 * problems as located diagnostics.
 */
#include "asm/internal/directive.h"

#include <stdint.h>
#include <string.h>

#include "asm/internal/strdec.h"

/* True if the statement's directive name equals the NUL-terminated literal `lit`
   (which includes the leading '.'); case-sensitive, matching our lowercase house
   spelling of directives. */
static bool dir_is(const qas_asm_unit *u, const qas_stmt *st, const char *lit)
{
    size_t n = strlen(lit);
    return st->name_len == n &&
           memcmp(u->src->data + st->name_off, lit, n) == 0;
}

/* Convenience: emit an error diagnostic located at the whole statement. */
static qas_status err_at_stmt(qas_asm_unit *u, const qas_stmt *st, const char *msg)
{
    return qas_diag_emit(u->diags, QAS_DIAG_ERROR, u->src, st->offset,
                         st->length, "%s", msg);
}

/* Reject any arguments (for the no-argument section directives). */
static qas_status require_no_args(qas_asm_unit *u, const qas_stmt *st)
{
    if (st->arg_count != 0) {
        return err_at_stmt(u, st, "this directive takes no arguments");
    }
    return QAS_OK;
}

/* The current section must hold file bytes for data/code emission; .bss may not.
   Returns true if OK; otherwise emits a diagnostic and returns false. */
static bool require_progbits(qas_asm_unit *u, const qas_stmt *st)
{
    qas_asm_section *sec = qas_asm_unit_current(u);
    if (sec != NULL && sec->type == SHT_NOBITS) {
        (void)err_at_stmt(u, st,
                          "cannot place data in a NOBITS section (.bss); only "
                          ".zero/.skip/.space and labels are allowed there");
        return false;
    }
    return true;
}

/* Does `v` fit a `width`-byte field as either a signed or an unsigned value? */
static bool fits_width(uint64_t v, unsigned width)
{
    if (width >= 8) {
        return true;
    }
    unsigned bits = width * 8u;
    uint64_t umax = (1ULL << bits) - 1ULL;          /* widest unsigned. */
    if (v <= umax) {
        return true;
    }
    int64_t s    = (int64_t)v;                       /* two's-complement view. */
    int64_t smin = -(int64_t)(1ULL << (bits - 1));
    return s >= smin && s < 0;                        /* negative values in range. */
}

/* Emit the low `width` bytes of `v` in little-endian order to the current
   section. The caller has range-checked `v`. */
static qas_status emit_int_datum(qas_asm_unit *u, uint64_t v, unsigned width)
{
    uint8_t bytes[8];
    for (unsigned i = 0; i < width; ++i) {
        bytes[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
    return qas_asm_unit_emit_bytes(u, bytes, width);
}

/*
 * Data directive (.byte/.word/.long/.quad): each argument is an integer that is
 * stored in `width` bytes, or — for width 4 and 8 only — a symbol, which becomes
 * an absolute relocation against that symbol.
 */
static qas_status data_ints(qas_asm_unit *u, const qas_stmt *st, unsigned width)
{
    if (!require_progbits(u, st)) {
        return QAS_OK;
    }
    if (st->arg_count == 0) {
        return err_at_stmt(u, st, "this directive needs at least one value");
    }
    for (size_t i = 0; i < st->arg_count; ++i) {
        const qas_dir_arg *a = &st->args[i];
        if (a->kind == QAS_DIR_ARG_INT) {
            if (!fits_width(a->int_value, width)) {
                qas_status st2 = qas_diag_emit(u->diags, QAS_DIAG_ERROR, u->src,
                                               a->off, a->len,
                                               "value does not fit in %u byte(s)",
                                               width);
                if (st2 != QAS_OK) {
                    return st2;
                }
                continue; /* report and skip; assembly will produce no object. */
            }
            qas_status st2 = emit_int_datum(u, a->int_value, width);
            if (st2 != QAS_OK) {
                return st2;
            }
        } else if (a->kind == QAS_DIR_ARG_SYMBOL) {
            if (width != 4 && width != 8) {
                qas_status st2 = qas_diag_emit(u->diags, QAS_DIAG_ERROR, u->src,
                                               a->off, a->len,
                                               "a symbol value needs a 4- or 8-byte "
                                               "directive (.long or .quad)");
                if (st2 != QAS_OK) {
                    return st2;
                }
                continue;
            }
            qas_asm_fix_kind kind = (width == 8) ? QAS_ASM_FIX_ABS64
                                                 : QAS_ASM_FIX_ABS32;
            qas_status st2 = qas_asm_unit_add_fix(u, kind, a->off, a->len, 0,
                                                  qas_asm_unit_here(u));
            if (st2 != QAS_OK) {
                return st2;
            }
            st2 = qas_asm_unit_emit_zeros(u, width); /* placeholder, patched by ld. */
            if (st2 != QAS_OK) {
                return st2;
            }
        } else {
            qas_status st2 = qas_diag_emit(u->diags, QAS_DIAG_ERROR, u->src,
                                           a->off, a->len,
                                           "expected a number or symbol");
            if (st2 != QAS_OK) {
                return st2;
            }
        }
    }
    return QAS_OK;
}

/* .ascii / .asciz / .string: emit each string's bytes; `terminate` adds a NUL. */
static qas_status data_strings(qas_asm_unit *u, const qas_stmt *st, bool terminate)
{
    if (!require_progbits(u, st)) {
        return QAS_OK;
    }
    if (st->arg_count == 0) {
        return err_at_stmt(u, st, "this directive needs at least one string");
    }
    for (size_t i = 0; i < st->arg_count; ++i) {
        const qas_dir_arg *a = &st->args[i];
        if (a->kind != QAS_DIR_ARG_STRING) {
            qas_status st2 = qas_diag_emit(u->diags, QAS_DIAG_ERROR, u->src,
                                           a->off, a->len, "expected a string literal");
            if (st2 != QAS_OK) {
                return st2;
            }
            continue;
        }
        /* Decode into a scratch buffer, then emit in one append. */
        qas_buf decoded;
        qas_buf_init(&decoded);
        qas_status st2 = qas_asm_decode_string(u->src, a->off, a->len, u->diags,
                                               &decoded);
        if (st2 == QAS_OK) {
            st2 = qas_asm_unit_emit_bytes(u, decoded.data, decoded.len);
        }
        if (st2 == QAS_OK && terminate) {
            st2 = qas_asm_unit_emit_zeros(u, 1);
        }
        qas_buf_dispose(&decoded);
        if (st2 != QAS_OK) {
            return st2;
        }
    }
    return QAS_OK;
}

/* .zero / .skip / .space: reserve N zero bytes (valid in PROGBITS and .bss). */
static qas_status data_zero(qas_asm_unit *u, const qas_stmt *st)
{
    if (st->arg_count != 1 || st->args[0].kind != QAS_DIR_ARG_INT) {
        return err_at_stmt(u, st, "expected a single byte count");
    }
    return qas_asm_unit_emit_zeros(u, st->args[0].int_value);
}

/* .align / .balign (alignment in bytes) and .p2align (alignment = 2^n). */
static qas_status do_align(qas_asm_unit *u, const qas_stmt *st, bool power_of_two_arg)
{
    if (st->arg_count != 1 || st->args[0].kind != QAS_DIR_ARG_INT) {
        return err_at_stmt(u, st, "expected a single alignment value");
    }
    uint64_t arg = st->args[0].int_value;
    uint64_t alignment;
    if (power_of_two_arg) {
        if (arg > 63u) {
            return err_at_stmt(u, st, ".p2align exponent is too large");
        }
        alignment = 1ULL << arg;
    } else {
        if (arg == 0u || (arg & (arg - 1u)) != 0u) {
            return err_at_stmt(u, st, "alignment must be a power of two");
        }
        alignment = arg;
    }
    return qas_asm_unit_align(u, alignment);
}

/* .globl / .global: mark each named symbol as having global binding. */
static qas_status do_globl(qas_asm_unit *u, const qas_stmt *st)
{
    if (st->arg_count == 0) {
        return err_at_stmt(u, st, "expected one or more symbol names");
    }
    for (size_t i = 0; i < st->arg_count; ++i) {
        const qas_dir_arg *a = &st->args[i];
        if (a->kind != QAS_DIR_ARG_SYMBOL) {
            qas_status st2 = qas_diag_emit(u->diags, QAS_DIAG_ERROR, u->src,
                                           a->off, a->len, "expected a symbol name");
            if (st2 != QAS_OK) {
                return st2;
            }
            continue;
        }
        uint32_t index;
        qas_status st2 = qas_asm_symtab_intern(&u->syms, u->src->data + a->off,
                                               a->len, &index);
        if (st2 != QAS_OK) {
            return st2;
        }
        qas_asm_symtab_at(&u->syms, index)->is_global = true;
    }
    return QAS_OK;
}

/* .set / .equ name, value: define `name` as an absolute (section-independent)
   symbol equal to the constant `value`. */
static qas_status do_set(qas_asm_unit *u, const qas_stmt *st)
{
    if (st->arg_count != 2 || st->args[0].kind != QAS_DIR_ARG_SYMBOL ||
        st->args[1].kind != QAS_DIR_ARG_INT) {
        return err_at_stmt(u, st, "expected: .set name, value");
    }
    const qas_dir_arg *name = &st->args[0];
    uint32_t index;
    qas_status st2 = qas_asm_symtab_intern(&u->syms, u->src->data + name->off,
                                           name->len, &index);
    if (st2 != QAS_OK) {
        return st2;
    }
    qas_asm_sym *sym = qas_asm_symtab_at(&u->syms, index);
    if (sym->defined && sym->where == QAS_ASM_SYM_SECTION) {
        return qas_diag_emit(u->diags, QAS_DIAG_ERROR, u->src, name->off, name->len,
                             "symbol '%.*s' is already defined as a label",
                             (int)name->len, u->src->data + name->off);
    }
    sym->defined = true;
    sym->where   = QAS_ASM_SYM_ABS;
    sym->value   = st->args[1].int_value;
    return QAS_OK;
}

/* .section "name"[, "flags"]: a string-named section (bare dotted names use the
   .text/.data/.rodata/.bss shortcuts, since a leading-dot name lexes as its own
   directive token). Flags string: 'a' alloc, 'w' write, 'x' execinstr. */
static qas_status do_section(qas_asm_unit *u, const qas_stmt *st)
{
    if (st->arg_count < 1 || st->arg_count > 2 ||
        st->args[0].kind != QAS_DIR_ARG_STRING) {
        return err_at_stmt(u, st, "expected: .section \"name\"[, \"flags\"]");
    }
    /* Decode the (quoted) name and optional flags into NUL-terminated scratch. */
    qas_buf name_buf;
    qas_buf_init(&name_buf);
    qas_status st2 = qas_asm_decode_string(u->src, st->args[0].off,
                                           st->args[0].len, u->diags, &name_buf);
    if (st2 == QAS_OK) {
        st2 = qas_buf_append_u8(&name_buf, 0);
    }
    Elf64_Xword flags = SHF_ALLOC;
    if (st2 == QAS_OK && st->arg_count == 2) {
        if (st->args[1].kind != QAS_DIR_ARG_STRING) {
            qas_buf_dispose(&name_buf);
            return err_at_stmt(u, st, "section flags must be a string");
        }
        qas_buf flag_buf;
        qas_buf_init(&flag_buf);
        st2 = qas_asm_decode_string(u->src, st->args[1].off, st->args[1].len,
                                    u->diags, &flag_buf);
        if (st2 == QAS_OK) {
            flags = 0;
            for (size_t i = 0; i < flag_buf.len; ++i) {
                switch (flag_buf.data[i]) {
                case 'a': flags |= SHF_ALLOC;     break;
                case 'w': flags |= SHF_WRITE;     break;
                case 'x': flags |= SHF_EXECINSTR; break;
                default:  break; /* ignore unknown flag letters. */
                }
            }
        }
        qas_buf_dispose(&flag_buf);
    }
    if (st2 == QAS_OK) {
        st2 = qas_asm_unit_select_section(u, (const char *)name_buf.data,
                                          SHT_PROGBITS, flags, 1u);
    }
    qas_buf_dispose(&name_buf);
    return st2;
}

qas_status qas_asm_apply_directive(qas_asm_unit *unit, const qas_stmt *st)
{
    if (unit == NULL || st == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }

    /* Sections (no-argument shortcuts). Attribute values: System V gABI section
       flags — code is ALLOC|EXECINSTR, writable data ALLOC|WRITE, read-only data
       ALLOC, and .bss is a NOBITS ALLOC|WRITE section. */
    if (dir_is(unit, st, ".text")) {
        qas_status st2 = require_no_args(unit, st);
        return (st2 != QAS_OK) ? st2
            : qas_asm_unit_select_section(unit, ".text", SHT_PROGBITS,
                                          SHF_ALLOC | SHF_EXECINSTR, 1u);
    }
    if (dir_is(unit, st, ".data")) {
        qas_status st2 = require_no_args(unit, st);
        return (st2 != QAS_OK) ? st2
            : qas_asm_unit_select_section(unit, ".data", SHT_PROGBITS,
                                          SHF_ALLOC | SHF_WRITE, 1u);
    }
    if (dir_is(unit, st, ".rodata")) {
        qas_status st2 = require_no_args(unit, st);
        return (st2 != QAS_OK) ? st2
            : qas_asm_unit_select_section(unit, ".rodata", SHT_PROGBITS,
                                          SHF_ALLOC, 1u);
    }
    if (dir_is(unit, st, ".bss")) {
        qas_status st2 = require_no_args(unit, st);
        return (st2 != QAS_OK) ? st2
            : qas_asm_unit_select_section(unit, ".bss", SHT_NOBITS,
                                          SHF_ALLOC | SHF_WRITE, 1u);
    }
    if (dir_is(unit, st, ".section")) {
        return do_section(unit, st);
    }

    /* Symbols. */
    if (dir_is(unit, st, ".globl") || dir_is(unit, st, ".global")) {
        return do_globl(unit, st);
    }
    if (dir_is(unit, st, ".set") || dir_is(unit, st, ".equ")) {
        return do_set(unit, st);
    }

    /* Data. */
    if (dir_is(unit, st, ".byte"))  return data_ints(unit, st, 1);
    if (dir_is(unit, st, ".word") || dir_is(unit, st, ".short") ||
        dir_is(unit, st, ".value")) return data_ints(unit, st, 2);
    if (dir_is(unit, st, ".long") || dir_is(unit, st, ".int"))
        return data_ints(unit, st, 4);
    if (dir_is(unit, st, ".quad"))  return data_ints(unit, st, 8);
    if (dir_is(unit, st, ".ascii")) return data_strings(unit, st, false);
    if (dir_is(unit, st, ".asciz") || dir_is(unit, st, ".string"))
        return data_strings(unit, st, true);
    if (dir_is(unit, st, ".zero") || dir_is(unit, st, ".skip") ||
        dir_is(unit, st, ".space")) return data_zero(unit, st);

    /* Alignment. */
    if (dir_is(unit, st, ".align") || dir_is(unit, st, ".balign"))
        return do_align(unit, st, false);
    if (dir_is(unit, st, ".p2align")) return do_align(unit, st, true);

    /* Anything else is a real error for our own sources. */
    return qas_diag_emit(unit->diags, QAS_DIAG_ERROR, unit->src, st->name_off,
                         st->name_len, "unknown directive '%.*s'",
                         (int)st->name_len, unit->src->data + st->name_off);
}
