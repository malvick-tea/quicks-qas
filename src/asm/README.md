# module: asm

**Responsibility:** the assembler *driver* — turn one source into a complete ELF64
relocatable object. It is the orchestrator that ties the pipeline together: parse →
lay out sections and labels → encode instructions → resolve intra-section
references → emit relocations → serialize the object. It performs no I/O (the CLI
owns reading/writing files), so the whole assembler is unit-testable in memory.

**Public interface:** `asm/asm.h` (`qas_assemble`). Status-vs-diagnostic contract:
`qas_assemble` returns `QAS_OK` whether or not the *source* was valid; the caller
checks `*out_image` (non-NULL only on a clean assembly) and the diagnostics sink. A
non-OK status is reserved for fatal conditions (out of memory, bad argument).

**Passes:**
1. **Parse** — source → statement list (lexer + parser modules).
2. **Layout & encode** — walk statements; directives switch the current section,
   labels are defined at the current offset, instructions are encoded to bytes, and
   every symbolic reference is recorded as a fixup in *section* coordinates. Every
   encoding in our subset is fixed-size, so one forward pass places all labels and
   forward references are completed in pass 3.
3. **Resolve & emit** — resolve each fixup locally or as a relocation, then build
   and serialize the ELF object.

**Resolution policy (the crux):** a PC-relative reference to a **local** label in
the **same** section has a link-invariant distance, so the `rel32` is patched in
place and no relocation is produced (the classic local-branch resolution). Every
other reference becomes a relocation the linker completes (System V x86-64 psABI):
absolute references (value depends on link-time placement), references to **global**
symbols (may be defined elsewhere/interposed), **cross-section** references, and
references to **undefined** symbols. Kind → type: `PC32 → R_X86_64_PC32`,
`ABS32S → R_X86_64_32S`, `ABS32 → R_X86_64_32`, `ABS64 → R_X86_64_64`.

**Internal layout (ADR-0008):**
- `internal/section.h/.c` — output sections and a name-interning registry.
- `internal/symtab.h/.c` — symbols interned by an FNV-1a open-addressing hash.
- `internal/fixups.h/.c` — the pending-fixup list collected during emission.
- `internal/strdec.h/.c` — decode `"..."` string literals (escapes) for `.ascii`/etc.
- `internal/unit.h/.c` — the assembly-state aggregate plus emit/align/label helpers.
- `internal/directive.h/.c` — directive dispatch and handlers.
- `asm.c` — the three-pass driver.

**Directives:** sections `.text/.data/.rodata/.bss` (and `.section "name"[,"flags"]`);
symbols `.globl`/`.global`, `.set`/`.equ`; data `.byte`, `.word`/`.short`/`.value`,
`.long`/`.int`, `.quad`, `.ascii`, `.asciz`/`.string`, `.zero`/`.skip`/`.space`;
alignment `.align`/`.balign`, `.p2align`. A `.long`/`.quad` of a symbol relocates.
Unknown directives are errors, not ignored noise.

**Dependencies:** `status`, `source`, `diag`, `ast`, `parser`, `arena`, `reg`,
`encoder`, `buf`, `elf`.
