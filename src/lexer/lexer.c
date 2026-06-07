/*
 * qas — lexer: implementation.
 *
 * See lexer.h for the grammar and design intent. The lexer is a hand-written
 * single-pass scanner: it is simpler, faster, and easier to give good error
 * messages than a table generator, and it has no external dependencies — fitting
 * the project's "from scratch" rule.
 *
 * Cursor model
 *   `pos` is the byte offset of the next character to read. The source buffer is
 *   NUL-terminated one past `size` (see source.h), but we never rely on that NUL
 *   to detect end-of-input; we always compare `pos` against `size`. This makes
 *   embedded NUL bytes in a file lex as ordinary (unexpected) characters rather
 *   than silently ending the stream.
 */
#include "lexer/lexer.h"

#include <stdarg.h>
#include <stdint.h>

/* ASCII character classification. */
/* We do NOT use <ctype.h>: its results depend on the current locale, whereas */
/* assembly tokenization must be deterministic and locale-independent so that */
/* the same source always produces the same tokens (and ultimately the same */
/* bytes). These tests are pure ASCII by construction. */

static bool is_digit(char c)       { return c >= '0' && c <= '9'; }
static bool is_alpha(char c)       { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_ident_start(char c) { return is_alpha(c) || c == '_'; }
static bool is_ident_cont(char c)  { return is_ident_start(c) || is_digit(c); }

/*
 * Value of digit `c` in the given base (2, 8, 10, or 16), or -1 if `c` is not a
 * valid digit for that base. Letters are accepted case-insensitively for hex.
 */
static int digit_value(char c, int base)
{
    int v;
    if (c >= '0' && c <= '9') {
        v = c - '0';
    } else if (c >= 'a' && c <= 'z') {
        v = 10 + (c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        v = 10 + (c - 'A');
    } else {
        return -1;
    }
    return (v < base) ? v : -1;
}

/* Cursor primitives. */

/* The current byte, or '\0' at end of input. */
static char lexer_peek(const qas_lexer *lx)
{
    return (lx->pos < lx->src->size) ? lx->src->data[lx->pos] : '\0';
}

/* The byte after the current one, or '\0' if there is none. */
static char lexer_peek2(const qas_lexer *lx)
{
    return (lx->pos + 1 < lx->src->size) ? lx->src->data[lx->pos + 1] : '\0';
}

/*
 * Consume and return the current byte, advancing the line/column counters.
 * A newline moves to column 1 of the next line; any other byte advances the
 * column by one (a tab counts as a single column — see source.h note). At end
 * of input this is a no-op returning '\0'.
 */
static char lexer_advance(qas_lexer *lx)
{
    if (lx->pos >= lx->src->size) {
        return '\0';
    }
    char c = lx->src->data[lx->pos++];
    if (c == '\n') {
        lx->line++;
        lx->column = 1;
    } else {
        lx->column++;
    }
    return c;
}

/* Token construction + error reporting helpers. */

/* Fill the common fields of a token and clear the integer payload. */
static void token_set(qas_token *t, qas_token_kind kind, size_t offset,
                      size_t length, uint32_t line, uint32_t column)
{
    t->kind         = kind;
    t->offset       = offset;
    t->length       = length;
    t->line         = line;
    t->column       = column;
    t->int_value    = 0;
    t->int_overflow = false;
}

/*
 * Produce a QAS_TOKEN_ERROR token spanning [offset, offset+length) and emit an
 * error diagnostic describing it. Returns the diagnostic emit status (QAS_OK, or
 * QAS_ERR_OUT_OF_MEMORY if the message could not be allocated), which the caller
 * propagates. Sets the lexer's had_error flag.
 */
static qas_status lexer_error(qas_lexer *lx, qas_token *out, size_t offset,
                              size_t length, uint32_t line, uint32_t column,
                              const char *fmt, ...)
{
    token_set(out, QAS_TOKEN_ERROR, offset, length, line, column);
    lx->had_error = true;

    va_list args;
    va_start(args, fmt);
    qas_status st = qas_diag_emitv(lx->diags, QAS_DIAG_ERROR, lx->src, offset,
                                   (length > 0) ? length : 1, fmt, args);
    va_end(args);
    return st;
}

/* Per-kind lexers. */

static qas_status lexer_lex_identifier(qas_lexer *lx, qas_token *out)
{
    size_t   start = lx->pos;
    uint32_t line  = lx->line;
    uint32_t col   = lx->column;
    while (is_ident_cont(lexer_peek(lx))) {
        lexer_advance(lx);
    }
    token_set(out, QAS_TOKEN_IDENTIFIER, start, lx->pos - start, line, col);
    return QAS_OK;
}

static qas_status lexer_lex_directive(qas_lexer *lx, qas_token *out)
{
    size_t   start = lx->pos;
    uint32_t line  = lx->line;
    uint32_t col   = lx->column;
    lexer_advance(lx); /* consume '.'; the caller guaranteed an identifier follows */
    while (is_ident_cont(lexer_peek(lx))) {
        lexer_advance(lx);
    }
    token_set(out, QAS_TOKEN_DIRECTIVE, start, lx->pos - start, line, col);
    return QAS_OK;
}

/*
 * Lex an integer literal. Supports decimal and the prefixed forms 0x/0X (hex),
 * 0b/0B (binary), 0o/0O (octal). A leading 0 without a base letter is decimal
 * (NASM convention), avoiding the C surprise where "010" means 8.
 *
 * Overflow is detected exactly: before each accumulation we check whether
 * value*base + digit would exceed UINT64_MAX, using the division form of the
 * test (which cannot itself overflow). An out-of-range literal is reported as an
 * error but still yields an INTEGER token (with int_overflow set) so lexing can
 * continue and report further problems.
 */
static qas_status lexer_lex_number(qas_lexer *lx, qas_token *out)
{
    size_t   start = lx->pos;
    uint32_t line  = lx->line;
    uint32_t col   = lx->column;

    int  base       = 10;
    bool has_prefix = false;
    if (lexer_peek(lx) == '0') {
        char n = lexer_peek2(lx);
        if (n == 'x' || n == 'X') { base = 16; has_prefix = true; }
        else if (n == 'b' || n == 'B') { base = 2;  has_prefix = true; }
        else if (n == 'o' || n == 'O') { base = 8;  has_prefix = true; }
    }
    if (has_prefix) {
        lexer_advance(lx); /* '0' */
        lexer_advance(lx); /* base letter */
    }

    uint64_t value       = 0;
    bool     overflow    = false;
    size_t   digit_count = 0;
    for (;;) {
        int dv = digit_value(lexer_peek(lx), base);
        if (dv < 0) {
            break;
        }
        lexer_advance(lx);
        digit_count += 1;

        if (value > (UINT64_MAX - (uint64_t)dv) / (uint64_t)base) {
            overflow = true; /* Keep consuming digits to span the whole literal. */
        } else {
            value = value * (uint64_t)base + (uint64_t)dv;
        }
    }

    /*
     * Malformed if a base prefix had no digits (e.g. "0x"), or if the literal is
     * immediately followed by an identifier character — which means a stray digit
     * for the base ("0b12") or a letter glued to a number ("123abc"). We swallow
     * the trailing identifier run so the scanner makes progress.
     */
    bool malformed = (has_prefix && digit_count == 0) || is_ident_cont(lexer_peek(lx));
    if (malformed) {
        while (is_ident_cont(lexer_peek(lx))) {
            lexer_advance(lx);
        }
        return lexer_error(lx, out, start, lx->pos - start, line, col,
                           "malformed numeric literal");
    }

    token_set(out, QAS_TOKEN_INTEGER, start, lx->pos - start, line, col);
    out->int_value    = overflow ? UINT64_MAX : value;
    out->int_overflow = overflow;
    if (overflow) {
        lx->had_error = true;
        return qas_diag_emit(lx->diags, QAS_DIAG_ERROR, lx->src, start,
                             lx->pos - start,
                             "integer literal out of range (exceeds 64 bits)");
    }
    return QAS_OK;
}

/*
 * Lex a "..." string. The raw span (including the surrounding quotes and any
 * backslash escapes) is captured verbatim; escape decoding is deferred to the
 * consumer (e.g. a .ascii directive), because the correct decoding can depend on
 * the directive. A newline or end-of-input before the closing quote is an
 * unterminated-string error.
 */
static qas_status lexer_lex_string(qas_lexer *lx, qas_token *out)
{
    size_t   start = lx->pos;
    uint32_t line  = lx->line;
    uint32_t col   = lx->column;
    lexer_advance(lx); /* opening quote */

    for (;;) {
        if (lx->pos >= lx->src->size || lexer_peek(lx) == '\n') {
            return lexer_error(lx, out, start, lx->pos - start, line, col,
                               "unterminated string literal");
        }
        char c = lexer_peek(lx);
        if (c == '"') {
            lexer_advance(lx); /* closing quote */
            break;
        }
        if (c == '\\') {
            lexer_advance(lx); /* backslash */
            if (lx->pos >= lx->src->size || lexer_peek(lx) == '\n') {
                return lexer_error(lx, out, start, lx->pos - start, line, col,
                                   "unterminated string literal after escape");
            }
            lexer_advance(lx); /* the escaped character (decoding deferred) */
            continue;
        }
        lexer_advance(lx);
    }

    token_set(out, QAS_TOKEN_STRING, start, lx->pos - start, line, col);
    return QAS_OK;
}

/* Public interface. */

void qas_lexer_init(qas_lexer *lexer, const qas_source *src, qas_diag_sink *diags)
{
    if (lexer == NULL) {
        return;
    }
    lexer->src       = src;
    lexer->diags     = diags;
    lexer->pos       = 0;
    lexer->line      = 1;
    lexer->column    = 1;
    lexer->had_error = false;
}

bool qas_lexer_had_error(const qas_lexer *lexer)
{
    return lexer != NULL && lexer->had_error;
}

qas_status qas_lexer_next(qas_lexer *lx, qas_token *out)
{
    if (lx == NULL || out == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }

    /* Skip inter-token whitespace and line comments, but NOT newlines: a newline
       is a significant token (end of statement). '\r' is treated as whitespace so
       CRLF inputs lex identically to LF inputs. */
    for (;;) {
        char c = lexer_peek(lx);
        if (c == ' ' || c == '\t' || c == '\r') {
            lexer_advance(lx);
            continue;
        }
        if (c == ';') { /* line comment to end of line (NASM/Intel convention) */
            while (lx->pos < lx->src->size && lexer_peek(lx) != '\n') {
                lexer_advance(lx);
            }
            continue;
        }
        break;
    }

    size_t   start = lx->pos;
    uint32_t line  = lx->line;
    uint32_t col   = lx->column;

    if (lx->pos >= lx->src->size) {
        token_set(out, QAS_TOKEN_EOF, start, 0, line, col);
        return QAS_OK;
    }

    char c = lexer_peek(lx);
    switch (c) {
    case '\n':
        lexer_advance(lx);
        token_set(out, QAS_TOKEN_NEWLINE, start, 1, line, col);
        return QAS_OK;
    case ',':
        lexer_advance(lx);
        token_set(out, QAS_TOKEN_COMMA, start, 1, line, col);
        return QAS_OK;
    case '[':
        lexer_advance(lx);
        token_set(out, QAS_TOKEN_LBRACKET, start, 1, line, col);
        return QAS_OK;
    case ']':
        lexer_advance(lx);
        token_set(out, QAS_TOKEN_RBRACKET, start, 1, line, col);
        return QAS_OK;
    case '+':
        lexer_advance(lx);
        token_set(out, QAS_TOKEN_PLUS, start, 1, line, col);
        return QAS_OK;
    case '-':
        lexer_advance(lx);
        token_set(out, QAS_TOKEN_MINUS, start, 1, line, col);
        return QAS_OK;
    case '*':
        lexer_advance(lx);
        token_set(out, QAS_TOKEN_STAR, start, 1, line, col);
        return QAS_OK;
    case ':':
        lexer_advance(lx);
        token_set(out, QAS_TOKEN_COLON, start, 1, line, col);
        return QAS_OK;
    case '"':
        return lexer_lex_string(lx, out);
    case '.':
        if (is_ident_start(lexer_peek2(lx))) {
            return lexer_lex_directive(lx, out);
        }
        lexer_advance(lx); /* a lone '.' is not a valid token */
        return lexer_error(lx, out, start, 1, line, col, "unexpected character '.'");
    default:
        if (is_digit(c)) {
            return lexer_lex_number(lx, out);
        }
        if (is_ident_start(c)) {
            return lexer_lex_identifier(lx, out);
        }
        /* Unknown byte: report it (printably if possible) and skip it. */
        lexer_advance(lx);
        return lexer_error(lx, out, start, 1, line, col,
                           "unexpected character '%c' (0x%02X)",
                           (c >= 32 && c < 127) ? c : '?',
                           (unsigned)(unsigned char)c);
    }
}
