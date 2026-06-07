/*
 * qas — status codes: implementation of qas_status_str.
 *
 * The switch enumerates every status explicitly (no `default` that hides missing
 * cases): if a new status is added to the enum and not handled here, a
 * conforming compiler with -Wswitch/-Wswitch-enum will flag it, which is exactly
 * the early warning we want. A final `return` after the switch handles
 * out-of-range integer values cast to the enum type, keeping the function total
 * (see the header's contract).
 */
#include "status/status.h"

const char *qas_status_str(qas_status status)
{
    switch (status) {
    case QAS_OK:                   return "ok";
    case QAS_ERR_OUT_OF_MEMORY:    return "out of memory";
    case QAS_ERR_IO:               return "I/O error";
    case QAS_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QAS_ERR_NOT_FOUND:        return "not found";
    case QAS_ERR_OVERFLOW:         return "value overflow";
    case QAS_ERR_LEX:              return "lexical error";
    case QAS_ERR_PARSE:            return "syntax error";
    case QAS_ERR_ENCODE:           return "encoding error";
    case QAS_ERR_UNSUPPORTED:      return "unsupported";
    }

    /*
     * Reached only if `status` holds an integer outside the enumerated set
     * (e.g. a corrupted value). Returning a sentinel keeps the function safe.
     */
    return "unknown status";
}
