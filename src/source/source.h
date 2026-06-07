/*
 * qas — source buffers and source locations
 *
 * Responsibility
 * Own the bytes of a unit of input (a file loaded from disk, or an in-memory
 * buffer used by tests) and translate a byte offset into a human 1-based
 * (line, column) location. Every later stage (lexer, parser, encoder) reports
 * problems in terms of byte offsets into a qas_source; the diagnostics module
 * turns those into "file:line:col" messages with a source-line excerpt.
 *
 * Design choices (and why)
 *   - The buffer is NUL-terminated one byte past `size`. A guaranteed sentinel
 *     lets the lexer peek one byte ahead at end-of-input without a special case
 *     on every read, which removes a whole class of off-by-one bugs.
 *   - A precomputed line-start index is built once at load time. Converting an
 *     arbitrary offset to (line, column) is then an O(log n) binary search
 *     instead of an O(n) rescan per diagnostic. This is the "earn the depth"
 *     trade-off: a little more code now for predictable behavior at scale.
 *   - The C standard library is used for file I/O here. That is permitted for
 *     host tools as a *seed* dependency, to be replaced by qlibc when the
 *     toolchain self-hosts (Quicks-Meta/docs/adr/0009-...).
 *
 * Standard: ISO/IEC 9899 (C11). Uses only the portable, POSIX-free subset.
 */
#ifndef QAS_SOURCE_SOURCE_H
#define QAS_SOURCE_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "status/status.h"

/*
 * qas_source — an owned unit of input.
 *
 * Ownership: a successfully initialized qas_source owns `name`, `data`, and
 * `line_starts`; release them with qas_source_dispose(). The struct may be
 * stack-allocated by the caller; only its internal buffers are heap-owned.
 */
typedef struct qas_source {
    char   *name;         /* Display name (e.g. file path), NUL-terminated. Owned. */
    char   *data;         /* Content bytes; data[size] == '\0' sentinel. Owned.    */
    size_t  size;         /* Number of content bytes (excludes the sentinel NUL).  */

    size_t *line_starts;  /* line_starts[i] = byte offset where line (i+1) begins. */
    size_t  line_count;   /* Number of lines; always >= 1 (an empty input is 1).   */
} qas_source;

/*
 * Initialize a source from an in-memory byte range, copying it.
 *
 *   name  : display name for diagnostics (copied; may be NULL -> "<memory>").
 *   bytes : pointer to `size` bytes (may be NULL only if size == 0).
 *   size  : number of bytes.
 *   out   : receives the initialized source on success (must be non-NULL).
 *
 * Returns QAS_OK, or QAS_ERR_INVALID_ARGUMENT / QAS_ERR_OUT_OF_MEMORY. On any
 * failure, *out is left untouched and nothing is allocated to it.
 */
qas_status qas_source_from_memory(const char *name, const char *bytes,
                                  size_t size, qas_source *out);

/*
 * Initialize a source by loading an entire file into memory (binary mode).
 *
 *   path : filesystem path (also used as the display name).
 *   out  : receives the initialized source on success (must be non-NULL).
 *
 * Returns QAS_OK, QAS_ERR_INVALID_ARGUMENT, QAS_ERR_IO (open/read failure), or
 * QAS_ERR_OUT_OF_MEMORY. The whole file is read up front because an assembler
 * needs random access to it for diagnostics and multi-pass processing.
 */
qas_status qas_source_load_file(const char *path, qas_source *out);

/*
 * Release all memory owned by *src and zero it. Safe to call on a zeroed
 * struct and idempotent (a disposed source is zeroed, so a second call is a
 * no-op). Passing NULL is a no-op.
 */
void qas_source_dispose(qas_source *src);

/*
 * Map a byte offset to a 1-based (line, column).
 *
 *   offset : byte offset; values > size are clamped to size (end-of-input), so
 *            an EOF token still yields a sensible location.
 *   out_line / out_column : receive the result (each must be non-NULL).
 *
 * Columns count bytes from the line start (a tab is one column). Returns QAS_OK
 * or QAS_ERR_INVALID_ARGUMENT.
 */
qas_status qas_source_location(const qas_source *src, size_t offset,
                               uint32_t *out_line, uint32_t *out_column);

/*
 * Return a pointer to the text of `line` (1-based) and its length via
 * *out_length, excluding the line terminator (handles "\n" and "\r\n"). The
 * pointer is into the source buffer (not owned by the caller, valid for the
 * lifetime of the source). Returns NULL if `line` is out of range.
 */
const char *qas_source_line_text(const qas_source *src, uint32_t line,
                                 size_t *out_length);

#endif /* QAS_SOURCE_SOURCE_H */
