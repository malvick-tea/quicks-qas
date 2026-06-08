/*
 * qas — assembler driver: string-literal decoding (implementation).
 *
 * A small state-free scanner over the raw token bytes. Because the lexer captured
 * the literal verbatim, multi-character escapes (\NNN octal, \xHH hex) are decoded
 * here from the raw bytes, consuming as many digits as the escape allows — the
 * lexer treated only the single byte after a backslash specially, so the digits of
 * a numeric escape are ordinary bytes in the span we now interpret.
 */
#include "asm/internal/strdec.h"

#include <stdbool.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

qas_status qas_asm_decode_string(const qas_source *src, size_t off, size_t len,
                                 qas_diag_sink *diags, qas_buf *out)
{
    if (src == NULL || out == NULL || len < 2) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    const char *s = src->data + off;
    if (s[0] != '"' || s[len - 1] != '"') {
        return QAS_ERR_INVALID_ARGUMENT;
    }

    /* Scan the interior [1, len-1), i.e. between the quotes. */
    size_t i = 1;
    const size_t end = len - 1;
    while (i < end) {
        char c = s[i];
        if (c != '\\') {
            qas_status st = qas_buf_append_u8(out, (uint8_t)c);
            if (st != QAS_OK) {
                return st;
            }
            i += 1;
            continue;
        }

        /* An escape. There is always at least one byte after the backslash,
           because the lexer guaranteed the closing quote is not the escaped
           character (lexer.c, lexer_lex_string). */
        i += 1; /* consume '\\' */
        char e = s[i];
        uint8_t value;
        bool emit = true;
        switch (e) {
        case 'n': value = 0x0A; i += 1; break;
        case 't': value = 0x09; i += 1; break;
        case 'r': value = 0x0D; i += 1; break;
        case 'f': value = 0x0C; i += 1; break;
        case 'b': value = 0x08; i += 1; break;
        case 'a': value = 0x07; i += 1; break;
        case 'v': value = 0x0B; i += 1; break;
        case 'e': value = 0x1B; i += 1; break; /* ESC (GNU extension). */
        case '\\': value = 0x5C; i += 1; break;
        case '"':  value = 0x22; i += 1; break;
        case '\'': value = 0x27; i += 1; break;
        case 'x':
        case 'X': {
            i += 1; /* consume 'x' */
            int hv = (i < end) ? hex_value(s[i]) : -1;
            if (hv < 0) {
                (void)qas_diag_emit(diags, QAS_DIAG_ERROR, src, off + i, 1,
                                    "\\x escape needs at least one hex digit");
                value = 0;
                emit = false;
                break;
            }
            unsigned v = 0;
            while (i < end && hex_value(s[i]) >= 0) {
                v = (v << 4) | (unsigned)hex_value(s[i]); /* low 8 bits kept below */
                i += 1;
            }
            value = (uint8_t)(v & 0xFFu);
            break;
        }
        default:
            if (e >= '0' && e <= '7') {
                unsigned v = 0;
                int digits = 0;
                while (i < end && s[i] >= '0' && s[i] <= '7' && digits < 3) {
                    v = (v * 8u) + (unsigned)(s[i] - '0');
                    i += 1;
                    digits += 1;
                }
                value = (uint8_t)(v & 0xFFu);
            } else {
                (void)qas_diag_emit(diags, QAS_DIAG_ERROR, src, off + i - 1, 2,
                                    "unknown string escape '\\%c'", e);
                /* Best-effort: take the character literally and keep going. */
                value = (uint8_t)e;
                i += 1;
            }
            break;
        }

        if (emit) {
            qas_status st = qas_buf_append_u8(out, value);
            if (st != QAS_OK) {
                return st;
            }
        }
    }
    return QAS_OK;
}
