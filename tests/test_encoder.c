/*
 * Tests for the instruction encoder: byte-exact output for a representative slice
 * of the instruction set, with special attention to the encoding corners — REX.W
 * and REX extension bits, the RSP/R12 SIB escape, the RBP/R13 displacement rule,
 * RIP-relative addressing, the preference for short immediate forms, and the
 * fixups emitted for symbolic branch/RIP-relative/immediate operands.
 *
 * Expected bytes were derived from the Intel SDM Vol 2 opcode reference and match
 * what a conforming assembler emits.
 */
#include "qtest.h"

#include "arena/arena.h"
#include "ast/ast.h"
#include "diag/diag.h"
#include "encoder/encoder.h"
#include "parser/parser.h"
#include "source/source.h"

typedef struct enc1 {
    qas_source    src;
    qas_diag_sink diags;
    qas_arena     arena;
    qas_stmt_list list;
    qas_encoded   out;
    bool          ok;
} enc1;

static void enc1_run(enc1 *E, const char *text)
{
    E->ok = false;
    memset(&E->out, 0, sizeof E->out);
    qas_source_from_memory("t", text, strlen(text), &E->src);
    qas_diag_sink_init(&E->diags);
    qas_arena_init(&E->arena);
    qas_parser p;
    qas_parser_init(&p, &E->src, &E->diags, &E->arena);
    qas_parser_parse(&p, &E->list);
    if (E->list.count >= 1 && E->list.items[0].kind == QAS_STMT_INSTRUCTION) {
        E->ok = (qas_encode(&E->src, &E->list.items[0], &E->diags, &E->out) == QAS_OK);
    }
}

static void enc1_dispose(enc1 *E)
{
    qas_stmt_list_dispose(&E->list);
    qas_arena_dispose(&E->arena);
    qas_diag_sink_dispose(&E->diags);
    qas_source_dispose(&E->src);
}

static void check_bytes(const char *text, const uint8_t *want, size_t n)
{
    enc1 E;
    enc1_run(&E, text);
    QTEST_CHECK_TRUE(E.ok);
    QTEST_CHECK_EQ_UINT(E.out.len, n, text);
    if (E.out.len == n) {
        for (size_t i = 0; i < n; ++i) {
            QTEST_CHECK_EQ_UINT(E.out.bytes[i], want[i], text);
        }
    }
    enc1_dispose(&E);
}

#define EXP(text, ...)                                                          \
    do {                                                                       \
        static const uint8_t w[] = {__VA_ARGS__};                              \
        check_bytes((text), w, sizeof w);                                      \
    } while (0)

static void test_mov(void)
{
    EXP("mov rax, rbx",            0x48, 0x89, 0xD8);
    EXP("mov rax, 0x10",           0x48, 0xC7, 0xC0, 0x10, 0x00, 0x00, 0x00);
    EXP("mov eax, 1",              0xB8, 0x01, 0x00, 0x00, 0x00);
    EXP("mov al, 5",               0xB0, 0x05);
    EXP("mov r8, r9",              0x4D, 0x89, 0xC8);
    EXP("mov rax, 0x1122334455667788",
        0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11);
    EXP("mov byte [rax], 5",       0xC6, 0x00, 0x05);
    EXP("mov [rsp], rax",          0x48, 0x89, 0x04, 0x24); /* RSP SIB escape   */
    EXP("mov rax, [rsp+8]",        0x48, 0x8B, 0x44, 0x24, 0x08);
    EXP("mov rax, [rbp]",          0x48, 0x8B, 0x45, 0x00); /* RBP disp8=0 rule */
}

static void test_arithmetic(void)
{
    EXP("add rax, rcx",  0x48, 0x01, 0xC8);
    EXP("add rax, 0x10", 0x48, 0x83, 0xC0, 0x10);        /* imm8 sign-extended */
    EXP("add eax, 0x1234", 0x05, 0x34, 0x12, 0x00, 0x00); /* accumulator imm32 */
    EXP("add ecx, 0x1234", 0x81, 0xC1, 0x34, 0x12, 0x00, 0x00); /* general imm */
    EXP("sub rsp, 8",    0x48, 0x83, 0xEC, 0x08);
    EXP("xor eax, eax",  0x31, 0xC0);
    EXP("cmp byte [rax], 5", 0x80, 0x38, 0x05);
    EXP("and rax, rax",  0x48, 0x21, 0xC0);
    EXP("test rax, rax", 0x48, 0x85, 0xC0);
}

static void test_unary_and_shift(void)
{
    EXP("inc rax",       0x48, 0xFF, 0xC0);
    EXP("dec qword [rax]", 0x48, 0xFF, 0x08);
    EXP("neg rax",       0x48, 0xF7, 0xD8);
    EXP("not rbx",       0x48, 0xF7, 0xD3);
    EXP("imul rcx",      0x48, 0xF7, 0xE9);
    EXP("shl rax, 1",    0x48, 0xD1, 0xE0);
    EXP("shl rax, cl",   0x48, 0xD3, 0xE0);
    EXP("shr rax, 5",    0x48, 0xC1, 0xE8, 0x05);
    EXP("sar eax, 1",    0xD1, 0xF8);
}

static void test_stack_and_control(void)
{
    EXP("push rax",  0x50);
    EXP("push r12",  0x41, 0x54);
    EXP("pop rbp",   0x5D);
    EXP("push 5",    0x6A, 0x05);
    EXP("ret",       0xC3);
    EXP("leave",     0xC9);
    EXP("nop",       0x90);
    EXP("syscall",   0x0F, 0x05);
    EXP("int3",      0xCC);
    EXP("cqo",       0x48, 0x99);
    EXP("jmp rax",   0xFF, 0xE0);
    EXP("call rax",  0xFF, 0xD0);
}

static void test_memory_forms(void)
{
    EXP("lea rax, [rbp + rcx*8 - 16]", 0x48, 0x8D, 0x44, 0xCD, 0xF0);
    EXP("lea rax, [rax + rax]",        0x48, 0x8D, 0x04, 0x00); /* index scale 1 */
    EXP("mov eax, [r12]",              0x41, 0x8B, 0x04, 0x24); /* R12 SIB escape */
    EXP("mov rax, [rip + 0]",          0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00);
}

/* Symbolic operands produce zeroed fields plus the right fixup. */
static void test_fixups(void)
{
    enc1 E;

    enc1_run(&E, "call target");
    QTEST_CHECK_TRUE(E.ok);
    QTEST_CHECK_EQ_UINT(E.out.len, 5u, "call len");
    QTEST_CHECK_EQ_UINT(E.out.bytes[0], 0xE8u, "call opcode");
    QTEST_CHECK_EQ_UINT(E.out.fixup_count, 1u, "one fixup");
    QTEST_CHECK_EQ_INT(E.out.fixups[0].kind, QAS_FIXUP_PC32, "pc32");
    QTEST_CHECK_EQ_UINT(E.out.fixups[0].offset, 1u, "fixup at 1");
    QTEST_CHECK_EQ_UINT(E.out.fixups[0].size, 4u, "fixup size 4");
    QTEST_CHECK_EQ_INT((int)E.out.fixups[0].addend, -4, "addend -4");
    QTEST_CHECK_SPAN(E.src.data + E.out.fixups[0].sym_off, E.out.fixups[0].sym_len,
                     "target", "fixup symbol");
    enc1_dispose(&E);

    enc1_run(&E, "lea rax, [rip + msg]");
    QTEST_CHECK_TRUE(E.ok);
    QTEST_CHECK_EQ_UINT(E.out.fixup_count, 1u, "rip fixup");
    QTEST_CHECK_EQ_INT(E.out.fixups[0].kind, QAS_FIXUP_PC32, "rip pc32");
    QTEST_CHECK_EQ_UINT(E.out.fixups[0].offset, 3u, "rip fixup offset");
    QTEST_CHECK_EQ_INT((int)E.out.fixups[0].addend, -4, "rip addend -4");
    enc1_dispose(&E);

    enc1_run(&E, "je loop");
    QTEST_CHECK_TRUE(E.ok);
    QTEST_CHECK_EQ_UINT(E.out.len, 6u, "je near len");
    QTEST_CHECK_EQ_UINT(E.out.bytes[0], 0x0Fu, "je 0F");
    QTEST_CHECK_EQ_UINT(E.out.bytes[1], 0x84u, "je 84");
    QTEST_CHECK_EQ_INT(E.out.fixups[0].kind, QAS_FIXUP_PC32, "je pc32");
    enc1_dispose(&E);
}

static void test_errors(void)
{
    /* High-byte register with a REX-requiring partner is rejected. */
    enc1 E;
    enc1_run(&E, "mov ah, r8b");
    QTEST_CHECK_TRUE(!E.ok);
    QTEST_CHECK_TRUE(qas_diag_severity_count(&E.diags, QAS_DIAG_ERROR) >= 1);
    enc1_dispose(&E);

    /* Unknown mnemonic. */
    enc1_run(&E, "frobnicate rax");
    QTEST_CHECK_TRUE(!E.ok);
    enc1_dispose(&E);
}

int main(void)
{
    test_mov();
    test_arithmetic();
    test_unary_and_shift();
    test_stack_and_control();
    test_memory_forms();
    test_fixups();
    test_errors();
    return qtest_report("encoder");
}
