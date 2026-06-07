/*
 * qas — lexer (tokenizer for x86-64 Intel-syntax assembly)
 *
 * Responsibility
 * Turn the raw bytes of a qas_source into a stream of qas_tokens, one call at a
 * time, reporting malformed input as diagnostics rather than aborting. This is
 * the first stage of the assembler and the first concrete step toward replacing
 * the seed assembler (Quicks-Meta roadmap, Phase 2).
 *
 * Lexical grammar (the subset implemented now; all decisions documented in
 * lexer.c with their rationale)
 *   - Whitespace: spaces, tabs, and carriage returns separate tokens and are
 *     discarded. A newline ('\n') is NOT discarded — it is a NEWLINE token,
 *     because in assembly one statement ends at end of line.
 *   - Comments: ';' to end of line (NASM/Intel convention), discarded.
 *   - Identifiers: [A-Za-z_][A-Za-z0-9_]*  (mnemonics, registers, symbols alike;
 *     the lexer does not distinguish them — see token.h).
 *   - Directives: '.' immediately followed by an identifier, e.g. ".text".
 *   - Integers: decimal, or prefixed 0x/0X (hex), 0b/0B (binary), 0o/0O (octal).
 *     A leading 0 with no base letter is decimal (NASM convention), not octal.
 *   - Strings: "..." with backslash escapes; the raw span (quotes included) is
 *     kept and decoded later by whoever consumes it.
 *   - Punctuation: , [ ] + - * :  (Intel memory-operand and label notation).
 *
 * Character classification is done with explicit ASCII tests, NOT <ctype.h>,
 * so behavior is independent of the C locale and fully deterministic — important
 * for an assembler whose output must be reproducible byte-for-byte.
 */
#ifndef QAS_LEXER_LEXER_H
#define QAS_LEXER_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/*
 * Lexer state. Create with qas_lexer_init, then call qas_lexer_next repeatedly.
 * Treat the fields as private. The lexer borrows (does not own) the source and
 * the diagnostics sink; both must outlive the lexer.
 */
typedef struct qas_lexer {
    const qas_source *src;    /* Borrowed input.                                  */
    qas_diag_sink    *diags;  /* Borrowed sink for lexical-error messages.        */
    size_t            pos;    /* Current byte offset into src->data.              */
    uint32_t          line;   /* Current 1-based line.                            */
    uint32_t          column; /* Current 1-based column.                          */
    bool              had_error; /* Set if any QAS_TOKEN_ERROR has been produced.  */
} qas_lexer;

/*
 * Initialize a lexer to read `src`, sending diagnostics to `diags`.
 * Both pointers must be non-NULL and must outlive the lexer.
 */
void qas_lexer_init(qas_lexer *lexer, const qas_source *src, qas_diag_sink *diags);

/*
 * Produce the next token into *out.
 *
 * Returns:
 *   QAS_OK                  - a token was produced. On malformed input the token
 *                             kind is QAS_TOKEN_ERROR and a diagnostic was emitted
 *                             (check qas_lexer_had_error / the sink's error count).
 *   QAS_ERR_INVALID_ARGUMENT- lexer or out was NULL.
 *   QAS_ERR_OUT_OF_MEMORY   - a diagnostic could not be allocated.
 *
 * At end of input it repeatedly yields QAS_TOKEN_EOF (length 0), so callers can
 * loop "until EOF" safely.
 */
qas_status qas_lexer_next(qas_lexer *lexer, qas_token *out);

/* True if the lexer has produced at least one error token. */
bool qas_lexer_had_error(const qas_lexer *lexer);

#endif /* QAS_LEXER_LEXER_H */
