/*
 * Tests for the parser: statement classification (label/directive/instruction),
 * operand parsing (register/immediate/memory with base, scaled index, signed and
 * symbolic displacement, RIP-relative, size prefix), directive arguments, signed
 * and symbolic immediates, the SIB validity rules, and error recovery to the next
 * line. Spans are checked against the exact source text.
 */
#include "qtest.h"

#include "arena/arena.h"
#include "ast/ast.h"
#include "diag/diag.h"
#include "parser/parser.h"
#include "reg/reg.h"
#include "source/source.h"

typedef struct parsed {
    qas_source    src;
    qas_diag_sink diags;
    qas_arena     arena;
    qas_stmt_list list;
    bool          had_error;
} parsed;

static void parse_text(parsed *P, const char *text)
{
    QTEST_CHECK_EQ_INT(qas_source_from_memory("test", text, strlen(text), &P->src),
                       QAS_OK, "from_memory");
    qas_diag_sink_init(&P->diags);
    qas_arena_init(&P->arena);
    qas_parser p;
    qas_parser_init(&p, &P->src, &P->diags, &P->arena);
    QTEST_CHECK_EQ_INT(qas_parser_parse(&p, &P->list), QAS_OK, "parse status");
    P->had_error = qas_parser_had_error(&p);
}

static void parsed_dispose(parsed *P)
{
    qas_stmt_list_dispose(&P->list);
    qas_arena_dispose(&P->arena);
    qas_diag_sink_dispose(&P->diags);
    qas_source_dispose(&P->src);
}

/* True if span [off,len) of the source equals `expected`. */
static int span_eq(const parsed *P, size_t off, size_t len, const char *expected)
{
    size_t el = strlen(expected);
    return len == el && memcmp(P->src.data + off, expected, len) == 0;
}

static void test_instruction(void)
{
    parsed P;
    parse_text(&P, "mov rax, 0x10");
    QTEST_CHECK_EQ_UINT(P.list.count, 1u, "one stmt");
    const qas_stmt *s = &P.list.items[0];
    QTEST_CHECK_EQ_INT(s->kind, QAS_STMT_INSTRUCTION, "instruction");
    QTEST_CHECK_TRUE(span_eq(&P, s->name_off, s->name_len, "mov"));
    QTEST_CHECK_EQ_UINT(s->operand_count, 2u, "2 operands");
    QTEST_CHECK_EQ_INT(s->operands[0].kind, QAS_OPERAND_REG, "op0 reg");
    QTEST_CHECK_TRUE(s->operands[0].reg != NULL && s->operands[0].reg->encoding == 0);
    QTEST_CHECK_EQ_INT(s->operands[1].kind, QAS_OPERAND_IMM, "op1 imm");
    QTEST_CHECK_TRUE(!s->operands[1].imm.is_symbol);
    QTEST_CHECK_EQ_UINT(s->operands[1].imm.value, 16u, "imm 16");
    QTEST_CHECK_TRUE(!P.had_error);
    parsed_dispose(&P);
}

static void test_labels_and_no_operands(void)
{
    parsed P;
    parse_text(&P, "a: b: nop\nret\n");
    QTEST_CHECK_EQ_UINT(P.list.count, 4u, "a,b,nop,ret");
    QTEST_CHECK_EQ_INT(P.list.items[0].kind, QAS_STMT_LABEL, "label a");
    QTEST_CHECK_TRUE(span_eq(&P, P.list.items[0].name_off, P.list.items[0].name_len, "a"));
    QTEST_CHECK_EQ_INT(P.list.items[1].kind, QAS_STMT_LABEL, "label b");
    QTEST_CHECK_EQ_INT(P.list.items[2].kind, QAS_STMT_INSTRUCTION, "nop");
    QTEST_CHECK_EQ_UINT(P.list.items[2].operand_count, 0u, "nop no operands");
    QTEST_CHECK_EQ_INT(P.list.items[3].kind, QAS_STMT_INSTRUCTION, "ret");
    parsed_dispose(&P);
}

static void test_memory_operand(void)
{
    parsed P;
    parse_text(&P, "lea rax, [rbp + rcx*8 - 16]");
    const qas_stmt *s = &P.list.items[0];
    QTEST_CHECK_EQ_UINT(s->operand_count, 2u, "2 ops");
    const qas_operand *m = &s->operands[1];
    QTEST_CHECK_EQ_INT(m->kind, QAS_OPERAND_MEM, "mem");
    QTEST_CHECK_TRUE(m->mem.base != NULL && m->mem.base->encoding == 5);  /* rbp */
    QTEST_CHECK_TRUE(m->mem.index != NULL && m->mem.index->encoding == 1);/* rcx */
    QTEST_CHECK_EQ_UINT(m->mem.scale, 8u, "scale 8");
    QTEST_CHECK_TRUE(m->mem.has_disp);
    QTEST_CHECK_EQ_INT((int)m->mem.disp, -16, "disp -16");
    QTEST_CHECK_TRUE(!m->mem.disp_is_symbol);
    QTEST_CHECK_TRUE(!P.had_error);
    parsed_dispose(&P);
}

static void test_rip_relative_and_size_prefix(void)
{
    parsed P;
    parse_text(&P, "mov rax, [rip + foo]\nmov qword ptr [rax], 1\n");
    const qas_operand *m = &P.list.items[0].operands[1];
    QTEST_CHECK_EQ_INT(m->kind, QAS_OPERAND_MEM, "mem");
    QTEST_CHECK_TRUE(m->mem.base != NULL && m->mem.base->reg_class == QAS_REG_CLASS_IP);
    QTEST_CHECK_TRUE(m->mem.disp_is_symbol);
    QTEST_CHECK_TRUE(span_eq(&P, m->mem.sym_off, m->mem.sym_len, "foo"));

    const qas_operand *m2 = &P.list.items[1].operands[0];
    QTEST_CHECK_EQ_INT(m2->kind, QAS_OPERAND_MEM, "mem2");
    QTEST_CHECK_EQ_UINT(m2->mem.size, 64u, "qword size");
    QTEST_CHECK_TRUE(m2->mem.base != NULL && m2->mem.base->encoding == 0); /* rax */
    QTEST_CHECK_EQ_INT(P.list.items[1].operands[1].kind, QAS_OPERAND_IMM, "imm 1");
    QTEST_CHECK_TRUE(!P.had_error);
    parsed_dispose(&P);
}

static void test_directives(void)
{
    parsed P;
    parse_text(&P, ".global main\n.byte 1, 2, 3\n.ascii \"hi\"\n");
    QTEST_CHECK_EQ_UINT(P.list.count, 3u, "3 directives");

    const qas_stmt *g = &P.list.items[0];
    QTEST_CHECK_EQ_INT(g->kind, QAS_STMT_DIRECTIVE, "directive");
    QTEST_CHECK_TRUE(span_eq(&P, g->name_off, g->name_len, ".global"));
    QTEST_CHECK_EQ_UINT(g->arg_count, 1u, "1 arg");
    QTEST_CHECK_EQ_INT(g->args[0].kind, QAS_DIR_ARG_SYMBOL, "symbol arg");
    QTEST_CHECK_TRUE(span_eq(&P, g->args[0].off, g->args[0].len, "main"));

    const qas_stmt *b = &P.list.items[1];
    QTEST_CHECK_EQ_UINT(b->arg_count, 3u, "3 ints");
    QTEST_CHECK_EQ_INT(b->args[0].kind, QAS_DIR_ARG_INT, "int arg");
    QTEST_CHECK_EQ_UINT(b->args[0].int_value, 1u, "1");
    QTEST_CHECK_EQ_UINT(b->args[2].int_value, 3u, "3");

    const qas_stmt *a = &P.list.items[2];
    QTEST_CHECK_EQ_UINT(a->arg_count, 1u, "1 string");
    QTEST_CHECK_EQ_INT(a->args[0].kind, QAS_DIR_ARG_STRING, "string arg");
    QTEST_CHECK_TRUE(span_eq(&P, a->args[0].off, a->args[0].len, "\"hi\"")); /* quotes kept */
    QTEST_CHECK_TRUE(!P.had_error);
    parsed_dispose(&P);
}

static void test_symbol_and_negative_immediate(void)
{
    parsed P;
    parse_text(&P, "mov rax, foo+8\nmov rcx, -1\n");
    const qas_operand *s = &P.list.items[0].operands[1];
    QTEST_CHECK_EQ_INT(s->kind, QAS_OPERAND_IMM, "imm");
    QTEST_CHECK_TRUE(s->imm.is_symbol);
    QTEST_CHECK_TRUE(span_eq(&P, s->imm.sym_off, s->imm.sym_len, "foo"));
    QTEST_CHECK_EQ_UINT(s->imm.value, 8u, "addend 8");

    const qas_operand *n = &P.list.items[1].operands[1];
    QTEST_CHECK_TRUE(!n->imm.is_symbol);
    QTEST_CHECK_EQ_UINT(n->imm.value, 0xFFFFFFFFFFFFFFFFull, "-1 two's complement");
    QTEST_CHECK_TRUE(!P.had_error);
    parsed_dispose(&P);
}

static void test_sib_rules(void)
{
    /* Bad scale and rsp-as-index are rejected at parse time. */
    parsed P;
    parse_text(&P, "lea rax, [rbx + rcx*3]\nlea rax, [rbx + rsp*2]\n");
    QTEST_CHECK_TRUE(P.had_error);
    QTEST_CHECK_TRUE(qas_diag_severity_count(&P.diags, QAS_DIAG_ERROR) >= 2);
    parsed_dispose(&P);
}

static void test_error_recovery(void)
{
    /* A broken first line is skipped; the following line still parses. */
    parsed P;
    parse_text(&P, "mov rax,\nret\n");
    QTEST_CHECK_TRUE(P.had_error);
    QTEST_CHECK_EQ_UINT(P.list.count, 1u, "ret survived");
    QTEST_CHECK_EQ_INT(P.list.items[0].kind, QAS_STMT_INSTRUCTION, "ret");
    QTEST_CHECK_TRUE(span_eq(&P, P.list.items[0].name_off, P.list.items[0].name_len, "ret"));
    parsed_dispose(&P);
}

int main(void)
{
    test_instruction();
    test_labels_and_no_operands();
    test_memory_operand();
    test_rip_relative_and_size_prefix();
    test_directives();
    test_symbol_and_negative_immediate();
    test_sib_rules();
    test_error_recovery();
    return qtest_report("parser");
}
