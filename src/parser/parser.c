/*
 * qas — parser: implementation.
 *
 * A hand-written recursive-descent parser with one token of lookahead. The
 * lookahead exists for exactly one ambiguity: an identifier at the start of a
 * statement is a label when followed by ':' and a mnemonic otherwise. Each call
 * to parse one statement either produces a statement or, on a syntax error,
 * reports a diagnostic and skips to the next line so parsing continues.
 *
 * The grammar implemented (Intel syntax, ADR-0005):
 *   statement   := label | directive | instruction
 *   label       := IDENT ':'
 *   directive   := DIRECTIVE (arg (',' arg)*)?
 *   arg         := STRING | signed-int | IDENT
 *   instruction := IDENT (operand (',' operand)*)?
 *   operand     := register | immediate | memory | size-prefixed-memory
 *   memory      := ['byte'|'word'|'dword'|'qword' ['ptr']] '[' mem-expr ']'
 *   mem-expr    := term (('+'|'-') term)*
 *   term        := register | register '*' int | int '*' register | int | IDENT
 */
#include "parser/parser.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "buf/buf.h"
#include "reg/reg.h"

/* Lexeme pointer for a token (into the borrowed source). */
static const char *lexeme(const qas_parser *p, const qas_token *t)
{
    return p->src->data + t->offset;
}

/* ASCII case-insensitive compare of a token's lexeme to a lowercase keyword.
   Locale-independent, matching the lexer's classification policy. */
static bool tok_ieq(const qas_parser *p, const qas_token *t, const char *kw)
{
    const char *s = lexeme(p, t);
    for (size_t i = 0; i < t->length; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (kw[i] == '\0' || c != kw[i]) {
            return false;
        }
    }
    return kw[t->length] == '\0';
}

/* Operand-size keyword (NASM-style) in bits, or 0 if not one. */
static uint8_t size_keyword_bits(const qas_parser *p, const qas_token *t)
{
    if (tok_ieq(p, t, "byte"))  return 8;
    if (tok_ieq(p, t, "word"))  return 16;
    if (tok_ieq(p, t, "dword")) return 32;
    if (tok_ieq(p, t, "qword")) return 64;
    return 0;
}

/* Consume the current token, loading the next into the lookahead. */
static void advance(qas_parser *p)
{
    p->last_end = p->cur.offset + p->cur.length;
    p->cur = p->next;
    qas_status st = qas_lexer_next(&p->lexer, &p->next);
    if (st != QAS_OK) {
        p->fatal = st; /* OOM while lexing; parse loop will stop. */
    }
}

static bool is_kind(const qas_parser *p, qas_token_kind k) { return p->cur.kind == k; }

/* Report a syntax error at the current token and mark the parser errored. */
static void error_here(qas_parser *p, const char *fmt, ...)
{
    p->had_error = true;
    va_list args;
    va_start(args, fmt);
    qas_status st = qas_diag_emitv(p->diags, QAS_DIAG_ERROR, p->src, p->cur.offset,
                                   (p->cur.length > 0) ? p->cur.length : 1, fmt, args);
    va_end(args);
    if (st != QAS_OK) {
        p->fatal = st;
    }
}

/* Skip to the end of the current line so parsing can resume on the next one. */
static void recover_to_eol(qas_parser *p)
{
    while (!is_kind(p, QAS_TOKEN_NEWLINE) && !is_kind(p, QAS_TOKEN_EOF) &&
           p->fatal == QAS_OK) {
        advance(p);
    }
    if (is_kind(p, QAS_TOKEN_NEWLINE)) {
        advance(p);
    }
}

/*
 * Parse an optionally signed integer literal into *out (sign applied as two's
 * complement). Returns true on success; on failure reports an error and returns
 * false. Consumes the sign and the integer token.
 */
static bool parse_signed_int(qas_parser *p, uint64_t *out)
{
    int sign = 1;
    if (is_kind(p, QAS_TOKEN_MINUS)) { sign = -1; advance(p); }
    else if (is_kind(p, QAS_TOKEN_PLUS)) { advance(p); }

    if (!is_kind(p, QAS_TOKEN_INTEGER)) {
        error_here(p, "expected an integer");
        return false;
    }
    uint64_t v = p->cur.int_value;
    if (p->cur.int_overflow) {
        error_here(p, "integer literal out of range");
        /* keep going with the clamped value so we can report more errors */
    }
    advance(p);
    *out = (sign < 0) ? (uint64_t)(0u - v) : v; /* two's-complement negate */
    return true;
}

/* End of a statement: a newline or EOF. */
static bool at_stmt_end(const qas_parser *p)
{
    return p->cur.kind == QAS_TOKEN_NEWLINE || p->cur.kind == QAS_TOKEN_EOF;
}

/* Memory-operand building. */

/* Set the index register + scale, validating the scale and the rsp restriction. */
static bool mem_set_index(qas_parser *p, qas_mem *m, const qas_reg *r, uint8_t scale)
{
    if (r->reg_class != QAS_REG_CLASS_GPR) {
        error_here(p, "only a general-purpose register may be a memory index");
        return false;
    }
    /* rsp/r12-with-encoding-4 in the index slot means "no index" in SIB, so rsp
       cannot be an index register (Intel SDM Vol 2 §2.1.5, Table 2-3). */
    if (qas_reg_low3(r) == 4 && qas_reg_ext(r) == 0) {
        error_here(p, "rsp cannot be used as an index register");
        return false;
    }
    if (scale != 1 && scale != 2 && scale != 4 && scale != 8) {
        error_here(p, "scale must be 1, 2, 4, or 8");
        return false;
    }
    if (m->index != NULL) {
        error_here(p, "memory operand has more than one index register");
        return false;
    }
    m->index = r;
    m->scale = scale;
    return true;
}

/* Place a plain register (no scale) as base, or as index*1 if base is taken. */
static bool mem_set_plain_reg(qas_parser *p, qas_mem *m, const qas_reg *r)
{
    if (r->reg_class == QAS_REG_CLASS_IP) { /* rip: only as a lone base */
        if (m->base != NULL || m->index != NULL) {
            error_here(p, "rip cannot be combined with other registers");
            return false;
        }
        m->base = r;
        return true;
    }
    if (m->base != NULL && m->base->reg_class == QAS_REG_CLASS_IP) {
        error_here(p, "rip cannot be combined with other registers");
        return false;
    }
    if (m->base == NULL) {
        m->base = r;
        return true;
    }
    return mem_set_index(p, m, r, 1); /* second plain register => index, scale 1 */
}

/* Parse one term of a memory expression, applying `sign` (±1) to integers. */
static bool parse_mem_term(qas_parser *p, qas_mem *m, int sign)
{
    if (is_kind(p, QAS_TOKEN_IDENTIFIER)) {
        const qas_reg *r = NULL;
        if (qas_reg_lookup(lexeme(p, &p->cur), p->cur.length, &r)) {
            if (sign < 0) {
                error_here(p, "a register cannot be subtracted in a memory operand");
                return false;
            }
            advance(p);
            if (is_kind(p, QAS_TOKEN_STAR)) { /* reg * scale */
                advance(p);
                uint64_t scale;
                if (!parse_signed_int(p, &scale)) {
                    return false;
                }
                return mem_set_index(p, m, r, (uint8_t)scale);
            }
            return mem_set_plain_reg(p, m, r);
        }
        /* A symbol displacement (e.g. [rip + foo]). */
        if (sign < 0) {
            error_here(p, "a symbol cannot be negated in a memory operand");
            return false;
        }
        if (m->disp_is_symbol) {
            error_here(p, "memory operand has more than one symbol");
            return false;
        }
        m->disp_is_symbol = true;
        m->has_disp       = true;
        m->sym_off        = p->cur.offset;
        m->sym_len        = p->cur.length;
        advance(p);
        return true;
    }

    if (is_kind(p, QAS_TOKEN_INTEGER)) {
        /* Could be `int * reg` (scale-first index) or a plain displacement. */
        if (p->next.kind == QAS_TOKEN_STAR) {
            uint64_t scale = p->cur.int_value;
            if (sign < 0) {
                error_here(p, "a scaled index cannot be subtracted");
                return false;
            }
            advance(p); /* int */
            advance(p); /* '*' */
            if (!is_kind(p, QAS_TOKEN_IDENTIFIER)) {
                error_here(p, "expected an index register after '*'");
                return false;
            }
            const qas_reg *r = NULL;
            if (!qas_reg_lookup(lexeme(p, &p->cur), p->cur.length, &r)) {
                error_here(p, "expected a register after '*'");
                return false;
            }
            advance(p);
            return mem_set_index(p, m, r, (uint8_t)scale);
        }
        uint64_t v = p->cur.int_value;
        advance(p);
        m->has_disp = true;
        m->disp += (sign < 0) ? -(int64_t)v : (int64_t)v;
        return true;
    }

    error_here(p, "expected a register, number, or symbol in a memory operand");
    return false;
}

/* Parse a bracketed memory operand into *op; `size_hint` is the byte/word/... bits. */
static bool parse_memory(qas_parser *p, qas_operand *op, uint8_t size_hint)
{
    op->kind     = QAS_OPERAND_MEM;
    op->mem.size = size_hint;
    advance(p); /* consume '[' */

    bool any = false;
    while (!is_kind(p, QAS_TOKEN_RBRACKET) && !at_stmt_end(p) && p->fatal == QAS_OK) {
        int sign = 1;
        if (is_kind(p, QAS_TOKEN_PLUS)) {
            advance(p);
        } else if (is_kind(p, QAS_TOKEN_MINUS)) {
            sign = -1;
            advance(p);
        }
        if (!parse_mem_term(p, &op->mem, sign)) {
            return false;
        }
        any = true;
        if (is_kind(p, QAS_TOKEN_PLUS) || is_kind(p, QAS_TOKEN_MINUS)) {
            continue;
        }
        break;
    }

    if (!is_kind(p, QAS_TOKEN_RBRACKET)) {
        error_here(p, "expected ']' to close the memory operand");
        return false;
    }
    advance(p); /* consume ']' */

    if (!any) {
        error_here(p, "empty memory operand");
        return false;
    }
    return true;
}

/* Parse one instruction operand into *op. */
static bool parse_operand(qas_parser *p, qas_operand *op)
{
    memset(op, 0, sizeof *op);
    op->offset = p->cur.offset;

    /* Size-prefixed memory: `qword [..]` or `qword ptr [..]`. */
    if (is_kind(p, QAS_TOKEN_IDENTIFIER)) {
        uint8_t bits = size_keyword_bits(p, &p->cur);
        if (bits != 0) {
            advance(p);
            if (is_kind(p, QAS_TOKEN_IDENTIFIER) && tok_ieq(p, &p->cur, "ptr")) {
                advance(p);
            }
            if (!is_kind(p, QAS_TOKEN_LBRACKET)) {
                error_here(p, "expected '[' after a size specifier");
                return false;
            }
            bool ok = parse_memory(p, op, bits);
            op->length = p->last_end - op->offset;
            return ok;
        }
    }

    if (is_kind(p, QAS_TOKEN_LBRACKET)) {
        bool ok = parse_memory(p, op, 0);
        op->length = p->last_end - op->offset;
        return ok;
    }

    if (is_kind(p, QAS_TOKEN_IDENTIFIER)) {
        const qas_reg *r = NULL;
        if (qas_reg_lookup(lexeme(p, &p->cur), p->cur.length, &r)) {
            op->kind = QAS_OPERAND_REG;
            op->reg  = r;
            advance(p);
            op->length = p->last_end - op->offset;
            return true;
        }
        /* Symbol immediate, with an optional integer addend (`foo + 8`). */
        op->kind        = QAS_OPERAND_IMM;
        op->imm.is_symbol = true;
        op->imm.sym_off = p->cur.offset;
        op->imm.sym_len = p->cur.length;
        advance(p);
        if (is_kind(p, QAS_TOKEN_PLUS) || is_kind(p, QAS_TOKEN_MINUS)) {
            uint64_t addend;
            if (!parse_signed_int(p, &addend)) {
                return false;
            }
            op->imm.value = addend;
        }
        op->length = p->last_end - op->offset;
        return true;
    }

    if (is_kind(p, QAS_TOKEN_INTEGER) || is_kind(p, QAS_TOKEN_MINUS) ||
        is_kind(p, QAS_TOKEN_PLUS)) {
        uint64_t v;
        if (!parse_signed_int(p, &v)) {
            return false;
        }
        op->kind      = QAS_OPERAND_IMM;
        op->imm.value = v;
        op->length    = p->last_end - op->offset;
        return true;
    }

    error_here(p, "expected an operand");
    return false;
}

/* Parse the operand list of an instruction whose mnemonic is already in *st. */
static bool parse_operand_list(qas_parser *p, qas_stmt *st)
{
    if (at_stmt_end(p)) {
        return true; /* no operands */
    }
    for (;;) {
        if (st->operand_count >= QAS_MAX_OPERANDS) {
            error_here(p, "too many operands (max %d)", QAS_MAX_OPERANDS);
            return false;
        }
        if (!parse_operand(p, &st->operands[st->operand_count])) {
            return false;
        }
        st->operand_count += 1;
        if (is_kind(p, QAS_TOKEN_COMMA)) {
            advance(p);
            continue;
        }
        break;
    }
    return true;
}

/* Parse a directive's argument list into the arena, storing it in *st. */
static bool parse_directive_args(qas_parser *p, qas_stmt *st)
{
    if (at_stmt_end(p)) {
        return true;
    }

    qas_buf tmp; /* temporary, packed array of qas_dir_arg */
    qas_buf_init(&tmp);
    bool ok = true;

    for (;;) {
        qas_dir_arg arg;
        memset(&arg, 0, sizeof arg);
        if (is_kind(p, QAS_TOKEN_STRING)) {
            arg.kind = QAS_DIR_ARG_STRING;
            arg.off  = p->cur.offset;
            arg.len  = p->cur.length;
            advance(p);
        } else if (is_kind(p, QAS_TOKEN_IDENTIFIER)) {
            arg.kind = QAS_DIR_ARG_SYMBOL;
            arg.off  = p->cur.offset;
            arg.len  = p->cur.length;
            advance(p);
        } else if (is_kind(p, QAS_TOKEN_INTEGER) || is_kind(p, QAS_TOKEN_MINUS) ||
                   is_kind(p, QAS_TOKEN_PLUS)) {
            arg.kind = QAS_DIR_ARG_INT;
            arg.off  = p->cur.offset;
            if (!parse_signed_int(p, &arg.int_value)) {
                ok = false;
                break;
            }
            arg.len = p->last_end - arg.off;
        } else {
            error_here(p, "expected a directive argument");
            ok = false;
            break;
        }

        if (qas_buf_append(&tmp, &arg, sizeof arg) != QAS_OK) {
            p->fatal = QAS_ERR_OUT_OF_MEMORY;
            ok = false;
            break;
        }
        if (is_kind(p, QAS_TOKEN_COMMA)) {
            advance(p);
            continue;
        }
        break;
    }

    if (ok && tmp.len > 0) {
        size_t count = tmp.len / sizeof(qas_dir_arg);
        void  *args = qas_arena_alloc_array(p->arena, count, sizeof(qas_dir_arg),
                                            _Alignof(qas_dir_arg));
        if (args == NULL) {
            p->fatal = QAS_ERR_OUT_OF_MEMORY;
            ok = false;
        } else {
            memcpy(args, tmp.data, tmp.len);
            st->args      = (const qas_dir_arg *)args;
            st->arg_count = count;
        }
    }

    qas_buf_dispose(&tmp);
    return ok;
}

/*
 * Parse one statement into *st. On success sets *produced = true. On a syntax
 * error, reports it, recovers to the next line, and sets *produced = false (the
 * return status stays QAS_OK — a syntax error is a diagnostic, not a fatal). A
 * non-OK return means a fatal resource failure that should stop parsing.
 */
static qas_status parse_statement(qas_parser *p, qas_stmt *st, bool *produced)
{
    *produced = false;
    memset(st, 0, sizeof *st);
    st->offset = p->cur.offset;
    st->line   = p->cur.line;

    bool ok;
    if (is_kind(p, QAS_TOKEN_IDENTIFIER) && p->next.kind == QAS_TOKEN_COLON) {
        st->kind     = QAS_STMT_LABEL;
        st->name_off = p->cur.offset;
        st->name_len = p->cur.length;
        advance(p); /* name */
        advance(p); /* ':' */
        ok = true;
    } else if (is_kind(p, QAS_TOKEN_DIRECTIVE)) {
        st->kind     = QAS_STMT_DIRECTIVE;
        st->name_off = p->cur.offset;
        st->name_len = p->cur.length;
        advance(p);
        ok = parse_directive_args(p, st);
        if (ok && !at_stmt_end(p)) {
            error_here(p, "unexpected token after directive arguments");
            ok = false;
        }
    } else if (is_kind(p, QAS_TOKEN_IDENTIFIER)) {
        st->kind     = QAS_STMT_INSTRUCTION;
        st->name_off = p->cur.offset;
        st->name_len = p->cur.length;
        advance(p);
        ok = parse_operand_list(p, st);
        if (ok && !at_stmt_end(p)) {
            error_here(p, "unexpected token after operands");
            ok = false;
        }
    } else {
        error_here(p, "expected a label, directive, or instruction");
        ok = false;
    }

    if (p->fatal != QAS_OK) {
        return p->fatal;
    }
    if (!ok) {
        recover_to_eol(p);
        return QAS_OK; /* recovered; produced stays false */
    }

    /* A well-formed label/directive/instruction ends the statement; consume the
       terminating newline if present so the next call starts cleanly. */
    if (is_kind(p, QAS_TOKEN_NEWLINE)) {
        advance(p);
    }
    st->length  = p->last_end - st->offset;
    *produced   = true;
    return QAS_OK;
}

/* Public interface. */

void qas_stmt_list_init(qas_stmt_list *list)
{
    if (list == NULL) {
        return;
    }
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void qas_stmt_list_dispose(qas_stmt_list *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    qas_stmt_list_init(list);
}

static qas_status stmt_list_push(qas_stmt_list *list, const qas_stmt *st)
{
    if (list->count >= list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 16u : list->capacity * 2u;
        qas_stmt *grown =
            (qas_stmt *)realloc(list->items, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return QAS_ERR_OUT_OF_MEMORY;
        }
        list->items    = grown;
        list->capacity = new_cap;
    }
    list->items[list->count++] = *st;
    return QAS_OK;
}

void qas_parser_init(qas_parser *parser, const qas_source *src,
                     qas_diag_sink *diags, qas_arena *arena)
{
    parser->src       = src;
    parser->diags     = diags;
    parser->arena     = arena;
    parser->last_end  = 0;
    parser->had_error = false;
    parser->fatal     = QAS_OK;
    qas_lexer_init(&parser->lexer, src, diags);

    /* Prime cur and the one-token lookahead. */
    qas_status s1 = qas_lexer_next(&parser->lexer, &parser->cur);
    qas_status s2 = qas_lexer_next(&parser->lexer, &parser->next);
    if (s1 != QAS_OK) {
        parser->fatal = s1;
    } else if (s2 != QAS_OK) {
        parser->fatal = s2;
    }
}

bool qas_parser_had_error(const qas_parser *parser)
{
    return parser != NULL && parser->had_error;
}

qas_status qas_parser_parse(qas_parser *p, qas_stmt_list *out)
{
    qas_stmt_list_init(out);
    if (p == NULL || out == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }

    for (;;) {
        while (is_kind(p, QAS_TOKEN_NEWLINE)) {
            advance(p);
        }
        if (p->fatal != QAS_OK) {
            return p->fatal;
        }
        if (is_kind(p, QAS_TOKEN_EOF)) {
            break;
        }

        qas_stmt st;
        bool     produced = false;
        qas_status r = parse_statement(p, &st, &produced);
        if (r != QAS_OK) {
            return r;
        }
        if (produced) {
            qas_status pr = stmt_list_push(out, &st);
            if (pr != QAS_OK) {
                return pr;
            }
        }
    }
    return QAS_OK;
}
