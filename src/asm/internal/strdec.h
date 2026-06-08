/*
 * qas — assembler driver: string-literal decoding (internal).
 *
 * Responsibility
 * Turn the raw span of a `"..."` string token (which, per the lexer, still
 * includes the surrounding quotes and undecoded backslash escapes) into the actual
 * bytes a `.ascii` / `.asciz` / `.string` directive should emit. The lexer
 * deliberately defers this: it only used the backslash to avoid ending the string
 * early at a `\"`, leaving the *meaning* of escapes to whoever consumes the string
 * (lexer.c, "Lex a string" note). For an assembler that consumer is here.
 *
 * Escape set (GNU as "Strings" conventions, the de-facto assembler standard):
 *   \n \t \r \f \b \a \v   the usual C control characters
 *   \\ \" \'                a literal backslash, double quote, single quote
 *   \e                      ESC (0x1B) — a common GNU extension
 *   \NNN                    1–3 octal digits  -> that byte value
 *   \xHH                    1+ hex digits     -> that byte value (low 8 bits)
 * Any other escape is reported as a diagnostic and the character is taken
 * literally so decoding can continue and surface more problems.
 *
 * Private to the asm module (ADR-0008).
 */
#ifndef QAS_ASM_INTERNAL_STRDEC_H
#define QAS_ASM_INTERNAL_STRDEC_H

#include <stddef.h>

#include "buf/buf.h"
#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"

/*
 * Decode the string literal whose raw bytes are src->data[off .. off+len) — the
 * span MUST be a complete token starting and ending with a double quote — and
 * append the decoded bytes to *out (the NUL terminator, if any, is the caller's
 * job: .asciz/.string add it, .ascii does not).
 *
 * Bad escapes are reported through `diags` at their exact source location; they do
 * not stop decoding (the project reports many errors per run). The return status
 * reflects only *fatal* conditions: QAS_OK on success (even if diagnostics were
 * emitted), QAS_ERR_OUT_OF_MEMORY if `out` could not grow, or
 * QAS_ERR_INVALID_ARGUMENT if the span is not a quoted string.
 */
qas_status qas_asm_decode_string(const qas_source *src, size_t off, size_t len,
                                 qas_diag_sink *diags, qas_buf *out);

#endif /* QAS_ASM_INTERNAL_STRDEC_H */
