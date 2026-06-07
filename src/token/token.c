/*
 * qas — lexical tokens: implementation of qas_token_kind_name.
 *
 * As with qas_status_str, the switch lists every kind explicitly so that adding
 * a token kind without naming it here is caught by -Wswitch. A trailing return
 * keeps the function total for out-of-range values.
 */
#include "token/token.h"

const char *qas_token_kind_name(qas_token_kind kind)
{
    switch (kind) {
    case QAS_TOKEN_EOF:        return "eof";
    case QAS_TOKEN_NEWLINE:    return "newline";
    case QAS_TOKEN_IDENTIFIER: return "identifier";
    case QAS_TOKEN_DIRECTIVE:  return "directive";
    case QAS_TOKEN_INTEGER:    return "integer";
    case QAS_TOKEN_STRING:     return "string";
    case QAS_TOKEN_COMMA:      return "comma";
    case QAS_TOKEN_LBRACKET:   return "lbracket";
    case QAS_TOKEN_RBRACKET:   return "rbracket";
    case QAS_TOKEN_PLUS:       return "plus";
    case QAS_TOKEN_MINUS:      return "minus";
    case QAS_TOKEN_STAR:       return "star";
    case QAS_TOKEN_COLON:      return "colon";
    case QAS_TOKEN_ERROR:      return "error";
    }
    return "token";
}
