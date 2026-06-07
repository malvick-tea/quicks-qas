/*
 * qas — lexical tokens
 *
 * Responsibility
 * Define the token data type produced by the lexer and consumed by the parser.
 * This module is deliberately a *pure data type*: it depends only on the C
 * standard fixed-width integer/boolean headers, not on the source or diagnostics
 * modules. Keeping it dependency-free means both the lexer and parser can share
 * it without creating a dependency cycle.
 *
 * Syntax note
 *   qas uses Intel assembler syntax (Quicks-Meta ADR-0005), so the punctuation
 *   set reflects Intel memory-operand notation: `[ base + index*scale + disp ]`,
 *   operands separated by commas, labels ended by a colon.
 *
 * What the lexer does NOT decide
 *   The lexer is intentionally "dumb": it does not know which identifiers are
 *   mnemonics (e.g. `mov`) versus registers (e.g. `rax`) versus symbol names.
 *   That classification is the parser's job. Both are simply IDENTIFIER tokens
 *   here. This separation keeps each stage simple and independently testable.
 */
#ifndef QAS_TOKEN_TOKEN_H
#define QAS_TOKEN_TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum qas_token_kind {
    QAS_TOKEN_EOF = 0,    /* End of input. length == 0.                          */
    QAS_TOKEN_NEWLINE,    /* End of a statement ('\n'). Significant in assembly. */
    QAS_TOKEN_IDENTIFIER, /* Mnemonic, register, or symbol name.                 */
    QAS_TOKEN_DIRECTIVE,  /* '.' immediately followed by an identifier, e.g. .text */
    QAS_TOKEN_INTEGER,    /* Numeric literal; value in `int_value`.              */
    QAS_TOKEN_STRING,     /* "..." literal; span includes the quotes.           */
    QAS_TOKEN_COMMA,      /* ,                                                   */
    QAS_TOKEN_LBRACKET,   /* [                                                   */
    QAS_TOKEN_RBRACKET,   /* ]                                                   */
    QAS_TOKEN_PLUS,       /* +                                                   */
    QAS_TOKEN_MINUS,      /* -                                                   */
    QAS_TOKEN_STAR,       /* *  (the index*scale multiply in memory operands)    */
    QAS_TOKEN_COLON,      /* :  (label terminator)                              */
    QAS_TOKEN_ERROR       /* Malformed input; a diagnostic was already emitted.  */
} qas_token_kind;

/*
 * A single token.
 *
 * `offset`/`length` describe the token's exact byte span in its source, so the
 * lexeme text is source->data[offset .. offset+length) — we never copy the text,
 * which keeps tokens small and cheap. `line`/`column` are the 1-based location of
 * the first character, cached for fast diagnostics without a lookup.
 *
 * `int_value`/`int_overflow` are meaningful only when kind == QAS_TOKEN_INTEGER.
 */
typedef struct qas_token {
    qas_token_kind kind;
    size_t         offset;
    size_t         length;
    uint32_t       line;
    uint32_t       column;

    uint64_t       int_value;     /* Parsed value of an integer literal.          */
    bool           int_overflow;  /* True if the literal did not fit in uint64_t.  */
} qas_token;

/*
 * Stable, lowercase name of a token kind, for diagnostics and tests
 * (e.g. "identifier", "comma", "eof"). Total: returns "token" for any
 * unrecognized value.
 */
const char *qas_token_kind_name(qas_token_kind kind);

#endif /* QAS_TOKEN_TOKEN_H */
