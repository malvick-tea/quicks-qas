# module: source

**Responsibility:** own a unit of input (a file or an in-memory buffer) and
translate byte offsets into 1-based (line, column) locations and source-line text
for diagnostics.

**Public interface:** `source/source.h` (`qas_source`, `qas_source_load_file`,
`qas_source_from_memory`, `qas_source_dispose`, `qas_source_location`,
`qas_source_line_text`).

**Design notes:**
- The buffer carries a guaranteed NUL sentinel one byte past the content, so
  look-ahead never needs an end-of-buffer special case.
- A line-start index is built once at load time; offset → (line, column) is then
  an O(log n) binary search.
- File I/O uses the portable ISO C library (a seed dependency, ADR-0009); files
  are read in binary mode so byte offsets and line endings are exact.

**Dependencies:** `status`.
