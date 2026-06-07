# module: diag

**Responsibility:** collect human-facing diagnostics (note/warning/error), each
anchored to a span of a `qas_source`, and render them as `file:line:col:
severity: message` with the offending source line and a caret underline.

**Public interface:** `diag/diag.h` (`qas_diag_severity`, `qas_diag`,
`qas_diag_sink`, `qas_diag_sink_init/dispose`, `qas_diag_emit[v]`,
`qas_diag_count`, `qas_diag_severity_count`, `qas_diag_severity_str`,
`qas_diag_sink_print`).

**Design notes:**
- Diagnostics are separate from `qas_status` so a stage can keep going after a
  recoverable error and report several problems per run (error-handling.md).
- Messages are formatted with the two-pass vsnprintf idiom (measure, then fill);
  `va_copy` is used because a `va_list` cannot be reused after consumption.
- The sink owns its message strings; it borrows (does not own) the `qas_source`.

**Dependencies:** `status`, `source`.
