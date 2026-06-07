/*
 * qas — parser (token stream -> statement/operand AST)
 *
 * Responsibility
 * Group the lexer's tokens into the typed statements of ast.h: labels,
 * directives with their arguments, and instructions with their operands
 * (registers, immediates, and memory references `[base + index*scale + disp]`).
 * Syntax errors are reported as diagnostics and the parser recovers to the next
 * line, so one run surfaces many problems (error-handling.md), rather than
 * stopping at the first.
 *
 * Memory model
 *   The list of statements is a heap array the caller disposes. The variable-
 *   length parts referenced from a statement (directive argument arrays) live in
 *   the caller-supplied arena, and identifier/symbol text is referenced as spans
 *   into the source (never copied). Therefore the source and the arena must
 *   outlive any use of the parsed statements.
 *
 * Pipeline position: source -> lexer -> [parser] -> encoder -> elf
 * (Quicks-Meta roadmap, Phase 2; qas docs/design.md.)
 */
#ifndef QAS_PARSER_PARSER_H
#define QAS_PARSER_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#include "arena/arena.h"
#include "ast/ast.h"
#include "diag/diag.h"
#include "lexer/lexer.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/*
 * A growable list of parsed statements. The qas_stmt values are stored by value;
 * their internal pointers (directive args) point into the parser's arena, and
 * their name spans point into the source. Dispose frees only the array.
 */
typedef struct qas_stmt_list {
    qas_stmt *items;
    size_t    count;
    size_t    capacity;
} qas_stmt_list;

/*
 * Parser state. Treat as private. Borrows the source, the diagnostics sink, and
 * the arena; all three must outlive the parser and the statements it produces.
 */
typedef struct qas_parser {
    const qas_source *src;
    qas_diag_sink    *diags;
    qas_arena        *arena;
    qas_lexer         lexer;
    qas_token         cur;      /* The next token to consume.                    */
    qas_token         next;     /* One-token lookahead (for `name:` vs `name`).  */
    size_t            last_end; /* End offset of the most recently consumed token*/
    bool              had_error;/* A syntax error was reported.                  */
    qas_status        fatal;    /* OOM or a lexer failure that stops parsing.    */
} qas_parser;

/*
 * Initialize a parser over `src`, reporting to `diags`, using `arena` for the
 * AST's variable-length parts. All three pointers must be non-NULL and outlive
 * the parser. Primes the one-token lookahead.
 */
void qas_parser_init(qas_parser *parser, const qas_source *src,
                     qas_diag_sink *diags, qas_arena *arena);

/*
 * Parse the entire source into *out (initialized by this call; dispose with
 * qas_stmt_list_dispose). Returns QAS_OK even when there were syntax errors —
 * those are diagnostics; check qas_parser_had_error or the sink's error count.
 * Returns QAS_ERR_OUT_OF_MEMORY (or another fatal status) only on resource
 * failure, in which case *out still holds whatever was parsed so far.
 */
qas_status qas_parser_parse(qas_parser *parser, qas_stmt_list *out);

/* True if any syntax error was reported during parsing. */
bool qas_parser_had_error(const qas_parser *parser);

/* Initialize an empty statement list. */
void qas_stmt_list_init(qas_stmt_list *list);

/* Free the statement array (not the arena-held parts). Safe on an empty list. */
void qas_stmt_list_dispose(qas_stmt_list *list);

#endif /* QAS_PARSER_PARSER_H */
