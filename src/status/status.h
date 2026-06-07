/*
 * qas — status codes (the project-wide result type for qas)
 *
 * Responsibility
 * Define the single status enum used by every fallible function in qas, plus a
 * function to render a status as a stable, human-readable string.
 *
 * Why a dedicated status type (and not, say, returning -1 / errno)?
 *   The project's error-handling policy (Quicks-Meta/docs/standards/error-handling.md)
 *   requires explicit, named result codes returned by value, with results passed
 *   through out-parameters. This keeps control flow legible and makes it impossible
 *   to confuse "a valid result that happens to be -1" with "an error".
 *
 * Invariants (relied upon across the codebase)
 *   - QAS_OK == 0, so both `if (status)` and `if (status != QAS_OK)` mean "failed".
 *   - No failure code is 0, and every code has a distinct value.
 *
 * This is the public interface of the `status` module (one public header per
 * module — see Quicks-Meta/docs/adr/0008-directory-architecture-rules.md).
 */
#ifndef QAS_STATUS_STATUS_H
#define QAS_STATUS_STATUS_H

/*
 * qas_status — the result of any operation that can fail.
 *
 * The numeric values are deliberately left implicit (0, 1, 2, …) except that
 * QAS_OK is pinned to 0; callers must compare against the named constants, never
 * against raw integers, so the exact values are not part of the contract.
 */
typedef enum qas_status {
    QAS_OK = 0,                 /* Success. Always zero (see invariants above).      */

    QAS_ERR_OUT_OF_MEMORY,      /* A memory allocation failed.                       */
    QAS_ERR_IO,                 /* An I/O operation failed (open/read/write).        */
    QAS_ERR_INVALID_ARGUMENT,   /* A precondition on an argument was violated (a
                                   programming error in the caller).                 */
    QAS_ERR_NOT_FOUND,          /* A requested entity (file, symbol, …) is absent.   */
    QAS_ERR_OVERFLOW,           /* A value did not fit the target representation.    */

    QAS_ERR_LEX,                /* Lexical error(s) were found; details were emitted
                                   as diagnostics (see the diag module).             */
    QAS_ERR_PARSE,             /* Syntax error(s); details emitted as diagnostics.   */
    QAS_ERR_ENCODE,            /* Instruction/operand could not be encoded.          */

    QAS_ERR_UNSUPPORTED         /* A well-formed request we do not implement yet.    */
} qas_status;

/*
 * qas_status_str — return a stable, human-readable name for a status.
 *
 * The returned pointer is to a static string literal: it is always valid, must
 * not be freed, and never changes. Intended for diagnostics and test output, not
 * for end-user error messages (those carry source locations via the diag module).
 *
 * Returns a generic "unknown status" string for any value not enumerated above,
 * so the function is total and safe even if the enum grows and a caller lags.
 */
const char *qas_status_str(qas_status status);

#endif /* QAS_STATUS_STATUS_H */
