# module: lexer

**Responsibility:** turn the bytes of a `qas_source` into a stream of
`qas_token`s (one per `qas_lexer_next` call), reporting malformed input as
diagnostics instead of aborting.

**Public interface:** `lexer/lexer.h` (`qas_lexer`, `qas_lexer_init`,
`qas_lexer_next`, `qas_lexer_had_error`).

**Grammar implemented now** (Intel syntax, ADR-0005): identifiers, `.`-directives,
integers (decimal and `0x`/`0b`/`0o` prefixes; a leading `0` is decimal per NASM
convention), `"..."` strings (raw span kept, escapes decoded later), the
punctuation `, [ ] + - * :`, `;` line comments, and significant newlines.

**Design notes:**
- Hand-written single-pass scanner: no dependencies, fast, good error messages.
- Character classification is explicit ASCII (not `<ctype.h>`), so tokenization
  is locale-independent and reproducible.
- End-of-input is detected by position, not by the NUL sentinel, so embedded NUL
  bytes lex as ordinary unexpected characters.
- Integer overflow past 64 bits is detected exactly and reported.

**Dependencies:** `status`, `source`, `diag`, `token`.
