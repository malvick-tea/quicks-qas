/*
 * Tests for the token module: stable kind names and a total fallback.
 */
#include "qtest.h"

#include "token/token.h"

static void check_name(qas_token_kind kind, const char *expected)
{
    const char *got = qas_token_kind_name(kind);
    QTEST_CHECK_SPAN(got, strlen(got), expected, expected);
}

int main(void)
{
    check_name(QAS_TOKEN_EOF, "eof");
    check_name(QAS_TOKEN_NEWLINE, "newline");
    check_name(QAS_TOKEN_IDENTIFIER, "identifier");
    check_name(QAS_TOKEN_DIRECTIVE, "directive");
    check_name(QAS_TOKEN_INTEGER, "integer");
    check_name(QAS_TOKEN_STRING, "string");
    check_name(QAS_TOKEN_COMMA, "comma");
    check_name(QAS_TOKEN_LBRACKET, "lbracket");
    check_name(QAS_TOKEN_RBRACKET, "rbracket");
    check_name(QAS_TOKEN_PLUS, "plus");
    check_name(QAS_TOKEN_MINUS, "minus");
    check_name(QAS_TOKEN_STAR, "star");
    check_name(QAS_TOKEN_COLON, "colon");
    check_name(QAS_TOKEN_ERROR, "error");

    /* Out-of-range value must still return a non-NULL sentinel. */
    const char *fallback = qas_token_kind_name((qas_token_kind)9999);
    QTEST_CHECK_SPAN(fallback, strlen(fallback), "token", "fallback name");

    return qtest_report("token");
}
