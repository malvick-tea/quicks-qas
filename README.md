# qas — the Quicks assembler

`qas` turns x86-64 assembly (Intel syntax) into ELF64 relocatable objects. It is
the first tool of the **Quicks** toolchain and the first seed dependency we
retire (it replaces `nasm` / GNU `as`).

> Cross-cutting conventions, the roadmap, and the rationale for every decision
> live in the **`Quicks-Meta`** repo (`../Quicks-Meta`). Especially:
> [coding standard](../Quicks-Meta/docs/standards/coding-standard.md),
> [ADR-0005 (Intel syntax)](../Quicks-Meta/docs/adr/0005-assembler-intel-syntax.md),
> [ADR-0004 (ELF64)](../Quicks-Meta/docs/adr/0004-object-format-elf64.md).

## Status
**Phase 2: assembling.** The full pipeline works — `qas` reads an Intel-syntax
source and writes an ELF64 relocatable object.

| Module | Role |
| --- | --- |
| `src/status` | Project-wide result type (`qas_status`). |
| `src/source` | Owns input bytes; maps offsets to (line, column); line index. |
| `src/diag`   | Accumulates errors/warnings with source spans; prints with a caret. |
| `src/token`  | The token data type (pure, dependency-free). |
| `src/lexer`  | Hand-written Intel-syntax tokenizer (locale-independent ASCII). |
| `src/reg`    | x86-64 register table (encoding, REX split, byte-register rules). |
| `src/buf`    | Growable little-endian byte buffer with in-place patching. |
| `src/arena`  | Region allocator for the parser's AST nodes. |
| `src/ast`    | Typed statements and operands shared by parser and encoder. |
| `src/parser` | Recursive-descent parser (labels, directives, instructions). |
| `src/encoder`| Table-driven instruction encoder (Intel SDM Vol 2; ADR-0011). |
| `src/elf`    | From-scratch ELF64 `ET_REL` writer (ADR-0004; x86-64 psABI). |
| `src/asm`    | The driver: sections, symbols, label layout, fixup resolution. |
| `src/app`    | CLI: assemble (`-o`), or `--dump-tokens`. |

Next: broaden the instruction table and directive set as the kernel/tools need
them; differentially check emitted objects during bootstrap, then feed `qld`.

## Architecture
Per [ADR-0008](../Quicks-Meta/docs/adr/0008-directory-architecture-rules.md):
folder = module with one public header; `src/` is the include root, so modules
reference each other as `"module/header.h"`. Each module has its own README.

## Build (seed toolchain)
Needs a C11 compiler and CMake (see `../Quicks-Meta/scripts/bootstrap-seed.sh`).

```sh
# Visual Studio (MSVC) — what this repo is currently built/tested with:
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug

# Or any single-config generator with a GCC/Clang seed:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

Builds clean under `/W4 /WX /permissive-` (MSVC) and is written to be clean under
`-Wall -Wextra -Wpedantic -Werror` (GCC/Clang).

## Test
```sh
ctest --test-dir build -C Debug --output-on-failure
```
Suites: `status`, `source`, `token`, `lexer`, `reg`, `buf`, `elf`, `arena`,
`parser`, `encoder`, `asm`. They assert byte-exact output against the Intel SDM /
ELF psABI — instruction encodings, object layout, symbol ordering, and
relocations — the precision an assembler requires.

## Run
```sh
qas examples/demo.s -o demo.o      # assemble to an ELF64 object
qas --dump-tokens examples/demo.s  # or just inspect the token stream
```
Malformed input is reported with `file:line:col` and a caret; the exit code is
0 (ok), 1 (errors), or 2 (usage). With `-o` omitted, the object is written next to
the input with a `.o` extension.
