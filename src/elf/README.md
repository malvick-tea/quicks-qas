# module: elf

**Responsibility:** build and serialize an ELF64 relocatable object (`ET_REL`)
for x86-64 — the container `qas` emits and `qld` will read. Implemented from
scratch (no libelf), which ADR-0004 establishes is within the from-scratch rule.

**Public interface:** `elf/elf.h` — the ELF64 types/constants (`Elf64_Ehdr`,
`Elf64_Shdr`, `Elf64_Sym`, `Elf64_Rela`, `SHT_*`, `SHF_*`, `STB_*`, `STT_*`,
`R_X86_64_*`) plus the builder (`qas_elf_builder`, `qas_elf_builder_init`,
`_add_section`, `_append`, `_reserve_bss`, `_section_size`, `_add_symbol`,
`_add_rela`, `_finish`, `_dispose`).

**Internal layout (ADR-0008):**
- `internal/model.h` — the concrete section/symbol/relocation records, opaque to
  callers (the public struct holds only pointers to them).
- `elf.c` — the builder: mutable accumulation and ownership.
- `internal/serialize.c` — the one-shot translation to ELF bytes.

**What it produces:** `.text`/`.data`/`.rodata`/`.bss`-style caller sections plus
synthesized `.symtab`, `.strtab`, `.shstrtab`, and one `.rela.<sec>` per section
with relocations. Locals are ordered before globals with `.symtab`'s `sh_info`
set to the first global (gABI requirement); relocations use RELA with the symbol
handle remapped to its final symbol-table index.

**Design notes:**
- The builder takes sections/symbols/relocations in any order via stable handles
  and resolves all cross-references (`sh_link`, `sh_info`, `st_shndx`, `r_info`,
  file offsets) once at `finish`; the serializer never mutates the builder.
- All output is little-endian (`ELFDATA2LSB`) written field-by-field via `buf`,
  so it is independent of host byte order and free of struct padding.
- `_Static_assert`s pin the on-disk struct sizes (64/64/24/24) at compile time.

**Authorities:** System V gABI / TIS ELF Specification; System V x86-64 psABI
(`EM_X86_64`, RELA, relocation types). See Quicks-Meta docs/abi/object-format-notes.md.

**Dependencies:** `status`, `buf`.
