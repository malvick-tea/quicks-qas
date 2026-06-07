# module: app

**Responsibility:** the `qas` command-line entry point. The only place in the
tool that touches `argv` and stdout/stderr, so the library modules (`qas_core`)
stay free of I/O policy and remain unit-testable.

**Entry point:** `app/main.c` (`main`).

**Current behavior:** parses options and one input file; supports `--dump-tokens`
(scan and print the token stream + diagnostics), `--help`, `--version`. Full
assembly is not implemented yet and the CLI says so. Exit codes: `0` ok, `1`
input errors / I/O failure, `2` usage error.

**Dependencies:** `status`, `source`, `diag`, `token`, `lexer`.
