# module: ast

**Responsibility:** define the typed representation the parser produces and the
encoder consumes — a statement (`qas_stmt`: label, directive, or instruction) and
the operands of an instruction (`qas_operand`: register, immediate, or memory).
Like `token`, it is a data type shared by two stages, kept separate so the
encoder depends on the *shape* of parsed input without depending on the parser.

**Public interface:** `ast/ast.h` (`qas_operand_kind`, `qas_imm`, `qas_mem`,
`qas_operand`, `qas_stmt_kind`, `qas_dir_arg`, `qas_stmt`, `QAS_MAX_OPERANDS`,
`qas_operand_kind_name`, `qas_stmt_kind_name`).

**Design notes:**
- Identifier/symbol text (label, mnemonic, directive name, operand symbols) is
  stored as `(offset, length)` spans into the source, never copied — the source
  outlives the AST (same convention as `token`).
- Integer literals/displacements hold the raw 64-bit value with any sign already
  applied in two's complement; range-checking to an operand width is the encoder's
  job (Intel SDM Vol 2 immediate/displacement sizes).
- A memory operand models `[base + index*scale + disp]` with optional components,
  a RIP-relative base (class `QAS_REG_CLASS_IP`), an optional symbol displacement
  (a relocation), and an optional `byte/word/dword/qword` size hint.

**Dependencies:** `reg`.
