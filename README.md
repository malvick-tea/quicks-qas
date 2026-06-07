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
**Phase 2, in progress.** Landed so far: the front-of-pipe foundation —

| Module | Role |
| --- | --- |
| `src/status` | Project-wide result type (`qas_status`). |
| `src/source` | Owns input bytes; maps offsets to (line, column); line index. |
| `src/diag`   | Accumulates errors/warnings with source spans; prints with a caret. |
| `src/token`  | The token data type (pure, dependency-free). |
| `src/lexer`  | Hand-written Intel-syntax tokenizer (locale-independent ASCII). |
| `src/app`    | CLI; currently offers `--dump-tokens`. |

Next: parser → instruction encoder (Intel SDM Vol 2) → ELF64 writer (ADR-0004).

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
Suites: `status`, `source`, `token`, `lexer`. The lexer suite asserts byte-exact
lexemes and exact integer values, the precision an assembler requires.

## Run
```sh
qas --dump-tokens examples/demo.s
```
Prints the token stream; malformed input is reported with `file:line:col` and a
caret, and the exit code is non-zero (0 ok, 1 errors, 2 usage).
