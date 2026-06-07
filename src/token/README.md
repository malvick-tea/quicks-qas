# module: token

**Responsibility:** define `qas_token`, the data type the lexer produces and the
parser consumes, plus `qas_token_kind_name` for diagnostics/tests.

**Public interface:** `token/token.h` (`qas_token_kind`, `qas_token`,
`qas_token_kind_name`).

**Design notes:**
- A *pure data type*: depends only on the C fixed-width integer/boolean headers,
  not on `source` or `diag`. This avoids a dependency cycle, since both the lexer
  and the parser share it.
- Tokens do not copy text: a token records `offset`/`length` into the source, so
  the lexeme is `source->data[offset .. offset+length)`.
- The lexer does not classify identifiers (mnemonic vs register vs symbol) — that
  is the parser's job; all are `QAS_TOKEN_IDENTIFIER` here.

**Dependencies:** none (beyond the C standard headers).
