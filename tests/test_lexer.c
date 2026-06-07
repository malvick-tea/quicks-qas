/*
 * Tests for the lexer: token kinds, exact lexemes, integer values across bases,
 * overflow detection, and the error paths (unexpected character, malformed
 * number, unterminated string). These assert byte-exact lexemes and exact
 * values, which is the level of precision an assembler demands.
 */
#include "qtest.h"

#include "diag/diag.h"
#include "lexer/lexer.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

#define MAX_TOK 64

typedef struct lexed {
    qas_source    src;
    qas_diag_sink diags;
    qas_token     toks[MAX_TOK];
    size_t        n;
    bool          had_error;
} lexed;

/* Lex `text` completely, collecting tokens (including the final EOF). */
static void lex_all(lexed *L, const char *text)
{
    L->n = 0;
    QTEST_CHECK_EQ_INT(
        qas_source_from_memory("test", text, strlen(text), &L->src), QAS_OK,
        "from_memory");
    qas_diag_sink_init(&L->diags);

    qas_lexer lx;
    qas_lexer_init(&lx, &L->src, &L->diags);
    for (;;) {
        qas_token t;
        qas_status st = qas_lexer_next(&lx, &t);
        QTEST_CHECK_EQ_INT(st, QAS_OK, "lexer_next status");
        if (st != QAS_OK) {
            break;
        }
        if (L->n < MAX_TOK) {
            L->toks[L->n++] = t;
        }
        if (t.kind == QAS_TOKEN_EOF) {
            break;
        }
    }
    L->had_error = qas_lexer_had_error(&lx);
}

static void lexed_dispose(lexed *L)
{
    qas_diag_sink_dispose(&L->diags);
    qas_source_dispose(&L->src);
}

/* Convenience: pointer to token i's lexeme bytes. */
static const char *lexeme(const lexed *L, size_t i)
{
    return L->src.data + L->toks[i].offset;
}

static void check_kind(const lexed *L, size_t i, qas_token_kind kind, const char *msg)
{
    if (i >= L->n) {
        QTEST_CHECK(0, msg);
        return;
    }
    QTEST_CHECK_EQ_INT(L->toks[i].kind, kind, msg);
}


static void test_instruction(void)
{
    lexed L;
    lex_all(&L, "mov rax, 0x10");

    QTEST_CHECK_EQ_UINT(L.n, 5u, "token count");
    check_kind(&L, 0, QAS_TOKEN_IDENTIFIER, "0 mov kind");
    QTEST_CHECK_SPAN(lexeme(&L, 0), L.toks[0].length, "mov", "0 mov text");
    check_kind(&L, 1, QAS_TOKEN_IDENTIFIER, "1 rax kind");
    QTEST_CHECK_SPAN(lexeme(&L, 1), L.toks[1].length, "rax", "1 rax text");
    check_kind(&L, 2, QAS_TOKEN_COMMA, "2 comma");
    check_kind(&L, 3, QAS_TOKEN_INTEGER, "3 int kind");
    QTEST_CHECK_EQ_UINT(L.toks[3].int_value, 16u, "3 int value");
    check_kind(&L, 4, QAS_TOKEN_EOF, "4 eof");

    /* The first token's location must be line 1, column 1. */
    QTEST_CHECK_EQ_UINT(L.toks[0].line, 1u, "mov line");
    QTEST_CHECK_EQ_UINT(L.toks[0].column, 1u, "mov column");
    QTEST_CHECK_TRUE(!L.had_error);
    QTEST_CHECK_EQ_UINT(qas_diag_count(&L.diags), 0u, "no diagnostics");

    lexed_dispose(&L);
}

static void test_memory_operand(void)
{
    lexed L;
    lex_all(&L, "add rbx, [rbp + rcx*8 - 4]");

    QTEST_CHECK_EQ_UINT(L.n, 13u, "token count");
    check_kind(&L, 0, QAS_TOKEN_IDENTIFIER, "add");
    check_kind(&L, 1, QAS_TOKEN_IDENTIFIER, "rbx");
    check_kind(&L, 2, QAS_TOKEN_COMMA, "comma");
    check_kind(&L, 3, QAS_TOKEN_LBRACKET, "[");
    check_kind(&L, 4, QAS_TOKEN_IDENTIFIER, "rbp");
    check_kind(&L, 5, QAS_TOKEN_PLUS, "+");
    check_kind(&L, 6, QAS_TOKEN_IDENTIFIER, "rcx");
    check_kind(&L, 7, QAS_TOKEN_STAR, "*");
    check_kind(&L, 8, QAS_TOKEN_INTEGER, "8");
    QTEST_CHECK_EQ_UINT(L.toks[8].int_value, 8u, "scale value");
    check_kind(&L, 9, QAS_TOKEN_MINUS, "-");
    check_kind(&L, 10, QAS_TOKEN_INTEGER, "4");
    QTEST_CHECK_EQ_UINT(L.toks[10].int_value, 4u, "disp value");
    check_kind(&L, 11, QAS_TOKEN_RBRACKET, "]");
    check_kind(&L, 12, QAS_TOKEN_EOF, "eof");
    QTEST_CHECK_TRUE(!L.had_error);

    lexed_dispose(&L);
}

static void test_directive_label_comment(void)
{
    lexed L;
    lex_all(&L, ".text\nstart:\n  ret ; trailing comment\n");

    QTEST_CHECK_EQ_UINT(L.n, 8u, "token count");
    check_kind(&L, 0, QAS_TOKEN_DIRECTIVE, "0 directive");
    QTEST_CHECK_SPAN(lexeme(&L, 0), L.toks[0].length, ".text", "0 .text text");
    check_kind(&L, 1, QAS_TOKEN_NEWLINE, "1 newline");
    check_kind(&L, 2, QAS_TOKEN_IDENTIFIER, "2 start");
    QTEST_CHECK_SPAN(lexeme(&L, 2), L.toks[2].length, "start", "2 start text");
    check_kind(&L, 3, QAS_TOKEN_COLON, "3 colon");
    check_kind(&L, 4, QAS_TOKEN_NEWLINE, "4 newline");
    check_kind(&L, 5, QAS_TOKEN_IDENTIFIER, "5 ret");
    check_kind(&L, 6, QAS_TOKEN_NEWLINE, "6 newline (comment skipped)");
    check_kind(&L, 7, QAS_TOKEN_EOF, "7 eof");

    /* 'start' is on line 2, column 1. */
    QTEST_CHECK_EQ_UINT(L.toks[2].line, 2u, "start line");
    QTEST_CHECK_EQ_UINT(L.toks[2].column, 1u, "start column");
    QTEST_CHECK_TRUE(!L.had_error);

    lexed_dispose(&L);
}

static void test_number_bases(void)
{
    lexed L;
    lex_all(&L, "123 0b1010 0o17 0xFF 0");

    QTEST_CHECK_EQ_UINT(L.n, 6u, "token count");
    QTEST_CHECK_EQ_UINT(L.toks[0].int_value, 123u, "decimal");
    QTEST_CHECK_EQ_UINT(L.toks[1].int_value, 10u, "binary");
    QTEST_CHECK_EQ_UINT(L.toks[2].int_value, 15u, "octal");
    QTEST_CHECK_EQ_UINT(L.toks[3].int_value, 255u, "hex");
    QTEST_CHECK_EQ_UINT(L.toks[4].int_value, 0u, "zero");
    for (size_t i = 0; i < 5; ++i) {
        check_kind(&L, i, QAS_TOKEN_INTEGER, "integer kind");
    }
    QTEST_CHECK_TRUE(!L.had_error);

    lexed_dispose(&L);
}

static void test_uint64_max_fits(void)
{
    lexed L;
    lex_all(&L, "0xFFFFFFFFFFFFFFFF"); /* exactly UINT64_MAX: must NOT overflow */
    check_kind(&L, 0, QAS_TOKEN_INTEGER, "kind");
    QTEST_CHECK_TRUE(!L.toks[0].int_overflow);
    QTEST_CHECK_EQ_UINT(L.toks[0].int_value, 0xFFFFFFFFFFFFFFFFull, "value");
    QTEST_CHECK_TRUE(!L.had_error);
    lexed_dispose(&L);
}

static void test_overflow(void)
{
    lexed L;
    lex_all(&L, "99999999999999999999999999"); /* > 2^64 */
    check_kind(&L, 0, QAS_TOKEN_INTEGER, "still integer kind");
    QTEST_CHECK_TRUE(L.toks[0].int_overflow);
    QTEST_CHECK_TRUE(L.had_error);
    QTEST_CHECK_TRUE(qas_diag_severity_count(&L.diags, QAS_DIAG_ERROR) >= 1);
    lexed_dispose(&L);
}

static void test_unexpected_char(void)
{
    lexed L;
    lex_all(&L, "@");
    QTEST_CHECK_EQ_UINT(L.n, 2u, "token count");
    check_kind(&L, 0, QAS_TOKEN_ERROR, "error token");
    check_kind(&L, 1, QAS_TOKEN_EOF, "eof");
    QTEST_CHECK_TRUE(L.had_error);
    QTEST_CHECK_EQ_UINT(qas_diag_severity_count(&L.diags, QAS_DIAG_ERROR), 1u,
                        "one error");
    lexed_dispose(&L);
}

static void test_malformed_number(void)
{
    lexed L;
    lex_all(&L, "123abc");
    check_kind(&L, 0, QAS_TOKEN_ERROR, "malformed -> error");
    /* The whole run is consumed as one error token so the scanner progresses. */
    QTEST_CHECK_SPAN(lexeme(&L, 0), L.toks[0].length, "123abc", "error span");
    QTEST_CHECK_TRUE(L.had_error);
    lexed_dispose(&L);
}

static void test_string_raw_and_unterminated(void)
{
    /* Raw span is captured verbatim, quotes and backslash included. */
    lexed L;
    lex_all(&L, "\"hi\\n\"");
    check_kind(&L, 0, QAS_TOKEN_STRING, "string kind");
    QTEST_CHECK_SPAN(lexeme(&L, 0), L.toks[0].length, "\"hi\\n\"", "raw span");
    QTEST_CHECK_TRUE(!L.had_error);
    lexed_dispose(&L);

    lexed U;
    lex_all(&U, "\"abc");
    check_kind(&U, 0, QAS_TOKEN_ERROR, "unterminated -> error");
    QTEST_CHECK_TRUE(U.had_error);
    lexed_dispose(&U);
}

int main(void)
{
    test_instruction();
    test_memory_operand();
    test_directive_label_comment();
    test_number_bases();
    test_uint64_max_fits();
    test_overflow();
    test_unexpected_char();
    test_malformed_number();
    test_string_raw_and_unterminated();
    return qtest_report("lexer");
}
