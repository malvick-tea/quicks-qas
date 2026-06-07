# module: encoder

**Responsibility:** translate one parsed instruction into x86-64 machine code
bytes plus *fixups* (fields that need a symbol address resolved later). Encoding
is table-driven (ADR-0011): a cited instruction table maps mnemonic + operand
pattern to an opcode and an encoding form; one generic engine emits the field
sequence of Intel SDM Vol 2 §2.1.

**Public interface:** `encoder/encoder.h` (`qas_encoded`, `qas_fixup`,
`qas_fixup_kind`, `qas_encode`, `QAS_INSN_MAX_LEN`).

**Internal layout (ADR-0008):**
- `internal/insn_table.h/.c` — the form type, the instruction table (regular
  families expanded by cited macros), and form selection (`qas_insn_select`).
- `encoder.c` — the engine: prefixes, REX, opcode, ModR/M, SIB, displacement,
  immediate.

**Design notes:**
- Fixups (`PC32`/`ABS32S`/`ABS64`) keep the encoder independent of the ELF and
  symbol-table layers; the orchestrator resolves them locally or turns them into
  relocations. RIP-relative and rel32 fixups carry the correct addend (the CPU's
  RIP is the *next* instruction).
- The engine implements the addressing corner cases once: REX construction
  (§2.2.1), the RSP/R12 SIB escape and RBP/R13 displacement rule (§2.1.5), and the
  illegal "high-byte register with REX" check (§2.2.1.2).
- Form selection takes the first matching row, and rows are ordered shortest/most
  specific first, so e.g. the `add r/m, imm8` (0x83) form is preferred.

**Coverage now:** mov/lea, the eight arithmetic/logic ops, test, the F6/F7 unary
group, inc/dec, shl/shr/sar, push/pop, call/jmp/ret and the Jcc set, and the
no-operand/misc instructions (nop, syscall, leave, int3, hlt, cqo/cdq, …). New
instructions are added as table rows.

**Dependencies:** `status`, `source`, `diag`, `ast`, `reg`.
