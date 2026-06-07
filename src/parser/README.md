# module: parser

**Responsibility:** group the lexer's tokens into the typed statements of `ast` —
labels, directives with arguments, and instructions with operands (registers,
immediates, memory references `[base + index*scale + disp]`, optionally size-
prefixed). Syntax errors become diagnostics and the parser recovers to the next
line, so one run reports many problems (error-handling.md).

**Public interface:** `parser/parser.h` (`qas_parser`, `qas_stmt_list`,
`qas_parser_init`, `qas_parser_parse`, `qas_parser_had_error`,
`qas_stmt_list_init`, `qas_stmt_list_dispose`).

**Design notes:**
- Recursive descent with one token of lookahead, needed for the single ambiguity
  `name:` (label) vs `name …` (mnemonic).
- The statement list is a heap array; each statement's variable-length parts
  (directive argument arrays) are arena-allocated, and identifiers are source
  spans — so the source and arena must outlive the parsed statements.
- Memory operands resolve registers via the `reg` module and enforce the SIB
  rules at parse time (scale ∈ {1,2,4,8}; `rsp` cannot be an index; `rip` only as
  a lone base) — Intel SDM Vol 2 §2.1.5.
- Recovery: on error, emit a diagnostic and skip to end of line; parsing
  continues, and `qas_parser_parse` still returns the statements it did parse.

**Dependencies:** `status`, `source`, `diag`, `token`, `lexer`, `reg`, `arena`,
`ast`, `buf`.
