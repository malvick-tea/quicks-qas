/*
 * qas — diagnostics
 *
 * Responsibility
 * Collect human-facing problems (errors, warnings, notes) discovered while
 * processing a source, each anchored to a span of that source, and render them
 * as "file:line:col: severity: message" with a source-line excerpt and a caret.
 *
 * Why diagnostics are separate from `qas_status`
 *   A status is for the *program* (control flow): "this call failed". A
 *   diagnostic is for the *human* (a message they can act on). Keeping them apart
 *   lets a stage keep going after a recoverable error and report *several*
 *   problems in one run, instead of dying on the first — the behavior users
 *   expect from an assembler/compiler. (See error-handling.md.)
 *
 * Ownership
 *   The sink owns the formatted message strings it stores and frees them on
 *   dispose. It does NOT own the qas_source a diagnostic points at; the source
 *   must outlive the sink.
 *
 * Standard: ISO/IEC 9899 (C11), portable subset (ADR-0009). Uses <stdarg.h> for
 * printf-style message formatting and <stdio.h> FILE for output.
 */
#ifndef QAS_DIAG_DIAG_H
#define QAS_DIAG_DIAG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "source/source.h"
#include "status/status.h"

/* Ordered by increasing seriousness; values are used to index counters. */
typedef enum qas_diag_severity {
    QAS_DIAG_NOTE = 0,
    QAS_DIAG_WARNING,
    QAS_DIAG_ERROR,
    QAS_DIAG_SEVERITY_COUNT /* Sentinel = number of severities (for counter array). */
} qas_diag_severity;

/* One recorded diagnostic. `message` is owned by the sink; `source` is borrowed. */
typedef struct qas_diag {
    qas_diag_severity  severity;
    const qas_source  *source;  /* Borrowed; must outlive the sink. May be NULL.  */
    size_t             offset;   /* Byte offset of the span start in `source`.     */
    size_t             length;   /* Span length in bytes (>= 1 for a real span).   */
    char              *message;  /* Owned, NUL-terminated, already formatted.      */
} qas_diag;

/*
 * A growable collection of diagnostics plus per-severity tallies. Treat the
 * fields as private; use the functions below.
 */
typedef struct qas_diag_sink {
    qas_diag *items;
    size_t    count;
    size_t    capacity;
    size_t    severity_counts[QAS_DIAG_SEVERITY_COUNT];
} qas_diag_sink;

/* Initialize an empty sink. Always succeeds. */
void qas_diag_sink_init(qas_diag_sink *sink);

/* Free all owned messages and the backing array; leaves the sink empty/zeroed. */
void qas_diag_sink_dispose(qas_diag_sink *sink);

/*
 * Record a diagnostic. The message is formatted printf-style into a sink-owned
 * buffer. `source` may be NULL for messages not tied to a span (e.g. CLI errors).
 *
 * Returns QAS_OK, or QAS_ERR_OUT_OF_MEMORY if the message or the array could not
 * be allocated. The variadic form forwards to the va_list form.
 */
qas_status qas_diag_emit(qas_diag_sink *sink, qas_diag_severity severity,
                         const qas_source *source, size_t offset, size_t length,
                         const char *fmt, ...);

qas_status qas_diag_emitv(qas_diag_sink *sink, qas_diag_severity severity,
                          const qas_source *source, size_t offset, size_t length,
                          const char *fmt, va_list args);

/* Total number of diagnostics recorded. */
size_t qas_diag_count(const qas_diag_sink *sink);

/* Number of diagnostics of a given severity (e.g. error count to set exit code). */
size_t qas_diag_severity_count(const qas_diag_sink *sink, qas_diag_severity severity);

/* Stable lowercase name of a severity ("error", "warning", "note"). */
const char *qas_diag_severity_str(qas_diag_severity severity);

/*
 * Print all diagnostics to `out` in source order of insertion, each as:
 *
 *     name:line:col: severity: message
 *         <the offending source line>
 *         <spaces/tabs>^~~~   (caret under the span)
 *
 * For diagnostics without a source, only the "severity: message" line is printed.
 */
void qas_diag_sink_print(const qas_diag_sink *sink, FILE *out);

#endif /* QAS_DIAG_DIAG_H */
