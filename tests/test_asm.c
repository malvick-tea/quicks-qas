/*
 * End-to-end tests for the assembler driver (qas_assemble): source text in, an
 * ELF64 relocatable object out. We assemble small programs in memory and parse the
 * resulting object with a tiny inline ELF reader (the same technique as test_elf.c)
 * to assert, byte-exact, the emitted section contents, the symbol table, and the
 * relocations — including the headline behavior that a same-section local branch is
 * resolved in place (no relocation) while everything else becomes a relocation.
 *
 * Expected instruction bytes come from the Intel SDM Vol 2 opcode reference;
 * relocation types/addends from the System V x86-64 psABI.
 */
#include "qtest.h"

#include <stdbool.h>
#include <stdlib.h>

#include "asm/asm.h"
#include "diag/diag.h"
#include "source/source.h"

/* Little-endian readers over the produced image. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[(size_t)i];
    }
    return v;
}

static const uint8_t *shdr_at(const uint8_t *img, uint32_t i)
{
    return img + rd64(img + 40) + (uint64_t)i * 64u;
}
static uint64_t shstr_off(const uint8_t *img)
{
    return rd64(shdr_at(img, rd16(img + 62)) + 24);
}
static int find_section(const uint8_t *img, const char *name)
{
    uint16_t shnum = rd16(img + 60);
    uint64_t soff  = shstr_off(img);
    for (uint32_t i = 0; i < shnum; ++i) {
        uint32_t name_off = rd32(shdr_at(img, i));
        if (strcmp((const char *)(img + soff + name_off), name) == 0) {
            return (int)i;
        }
    }
    return -1;
}
static uint64_t sh_offset(const uint8_t *s) { return rd64(s + 24); }
static uint64_t sh_size(const uint8_t *s)   { return rd64(s + 32); }
static uint32_t sh_link(const uint8_t *s)   { return rd32(s + 40); }
static uint32_t sh_info(const uint8_t *s)   { return rd32(s + 44); }

/* The bytes and size of a section by name (NULL if absent). */
static const uint8_t *section_bytes(const uint8_t *img, const char *name, uint64_t *out_size)
{
    int i = find_section(img, name);
    if (i < 0) {
        *out_size = 0;
        return NULL;
    }
    const uint8_t *s = shdr_at(img, (uint32_t)i);
    *out_size = sh_size(s);
    return img + sh_offset(s);
}

/* Assert a section's contents equal want[0..n). */
static void check_section(const uint8_t *img, const char *name,
                          const uint8_t *want, size_t n)
{
    uint64_t size = 0;
    const uint8_t *got = section_bytes(img, name, &size);
    QTEST_CHECK_TRUE(got != NULL);
    QTEST_CHECK_EQ_UINT(size, n, name);
    if (got != NULL && size == n) {
        for (size_t i = 0; i < n; ++i) {
            QTEST_CHECK_EQ_UINT(got[i], want[i], name);
        }
    }
}

/* A symbol-table view. */
typedef struct symview {
    const uint8_t *syms;
    uint64_t       count;
    const char    *strs;
} symview;

static symview get_symbols(const uint8_t *img)
{
    symview v;
    v.syms = NULL;
    v.count = 0;
    v.strs = NULL;
    int i = find_section(img, ".symtab");
    if (i < 0) {
        return v;
    }
    const uint8_t *s = shdr_at(img, (uint32_t)i);
    v.syms  = img + sh_offset(s);
    v.count = sh_size(s) / 24u;
    v.strs  = (const char *)(img + sh_offset(shdr_at(img, sh_link(s))));
    return v;
}

/* Find a symbol by name; returns its index or -1. */
static int find_symbol(const symview *v, const char *name)
{
    for (uint64_t i = 0; i < v->count; ++i) {
        const char *nm = v->strs + rd32(v->syms + i * 24u + 0u);
        if (strcmp(nm, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}
static uint16_t sym_shndx(const symview *v, int i) { return rd16(v->syms + (size_t)i * 24u + 6u); }
static uint64_t sym_value(const symview *v, int i) { return rd64(v->syms + (size_t)i * 24u + 8u); }
static uint8_t  sym_info(const symview *v, int i)  { return v->syms[(size_t)i * 24u + 4u]; }

/* The assembly harness: assemble `text`, keep the image for inspection. */
typedef struct asmres {
    uint8_t      *img;
    size_t        len;
    qas_diag_sink diags;
    bool          ok;       /* assembly produced an object */
    size_t        errors;   /* error-severity diagnostics */
} asmres;

static void asm_run(asmres *R, const char *text)
{
    qas_source src;
    qas_source_from_memory("t", text, strlen(text), &src);
    qas_diag_sink_init(&R->diags);
    R->img = NULL;
    R->len = 0;
    qas_status st = qas_assemble(&src, &R->diags, &R->img, &R->len);
    R->errors = qas_diag_severity_count(&R->diags, QAS_DIAG_ERROR);
    R->ok = (st == QAS_OK && R->img != NULL);
    /* The image is self-contained (names are copied), so the source can go now;
       we must not print diagnostics after this, and we don't. */
    qas_source_dispose(&src);
}

static void asm_free(asmres *R)
{
    free(R->img);
    R->img = NULL;
    qas_diag_sink_dispose(&R->diags);
}

/*
 * A same-section backward branch to a local label is resolved at assembly time:
 * the rel32 is patched in place and NO relocation is produced.
 */
static void test_local_branch_resolved(void)
{
    asmres R;
    asm_run(&R,
            ".text\n"
            ".globl _start\n"
            "_start:\n"
            "    xor eax, eax\n"   /* 31 C0           @0 */
            "loop:\n"
            "    inc eax\n"        /* FF C0           @2 */
            "    cmp eax, 10\n"    /* 83 F8 0A        @4 */
            "    jne loop\n"       /* 0F 85 rel32     @7, field @9 */
            "    ret\n");          /* C3              @13 */
    QTEST_CHECK_TRUE(R.ok);

    /* rel32 = target(2) - next_insn(13) = -11 = 0xFFFFFFF5. */
    static const uint8_t text[] = {
        0x31, 0xC0, 0xFF, 0xC0, 0x83, 0xF8, 0x0A,
        0x0F, 0x85, 0xF5, 0xFF, 0xFF, 0xFF, 0xC3
    };
    check_section(R.img, ".text", text, sizeof text);

    /* No relocations: the branch was resolved locally. */
    QTEST_CHECK_TRUE(find_section(R.img, ".rela.text") < 0);

    /* _start is a defined global at 0; "loop" is local and was not emitted. */
    symview sv = get_symbols(R.img);
    int start = find_symbol(&sv, "_start");
    QTEST_CHECK_TRUE(start >= 0);
    QTEST_CHECK_EQ_UINT(sym_value(&sv, start), 0u, "_start value");
    QTEST_CHECK_TRUE(find_symbol(&sv, "loop") < 0);

    asm_free(&R);
}

/* A call to an undefined symbol becomes a PC32 relocation against an UNDEF global. */
static void test_external_call_reloc(void)
{
    asmres R;
    asm_run(&R,
            ".globl main\n"
            "main:\n"
            "    call puts\n"   /* E8 00 00 00 00 */
            "    ret\n");       /* C3 */
    QTEST_CHECK_TRUE(R.ok);

    static const uint8_t text[] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3 };
    check_section(R.img, ".text", text, sizeof text);

    int irela = find_section(R.img, ".rela.text");
    QTEST_CHECK_TRUE(irela >= 0);
    const uint8_t *sr = shdr_at(R.img, (uint32_t)irela);
    QTEST_CHECK_EQ_UINT(sh_size(sr), 24u, "one reloc");
    const uint8_t *rel = R.img + sh_offset(sr);
    QTEST_CHECK_EQ_UINT(rd64(rel + 0), 1u, "r_offset = 1");          /* after E8 */
    uint64_t info = rd64(rel + 8);
    QTEST_CHECK_EQ_UINT(info & 0xffffffffu, 2u, "R_X86_64_PC32");    /* type 2 */
    QTEST_CHECK_EQ_INT((int64_t)rd64(rel + 16), -4, "addend -4");

    /* The relocation points at "puts", which is an UNDEF global symbol. */
    symview sv = get_symbols(R.img);
    int puts_sym = find_symbol(&sv, "puts");
    QTEST_CHECK_TRUE(puts_sym >= 0);
    QTEST_CHECK_EQ_UINT(sym_shndx(&sv, puts_sym), 0u, "puts undef (SHN_UNDEF)");
    QTEST_CHECK_EQ_UINT(info >> 32, (uint64_t)puts_sym, "reloc -> puts");

    asm_free(&R);
}

/* Data directives populate .data/.rodata/.bss; a .quad of a symbol relocates. */
static void test_data_directives(void)
{
    asmres R;
    asm_run(&R,
            ".globl main\n"
            ".text\n"
            "main:\n"
            "    ret\n"                 /* .text: C3 */
            ".data\n"
            ".globl table\n"
            "table:\n"
            "    .quad main\n"          /* 8 zero bytes + R_X86_64_64 main */
            "    .long 0xDEADBEEF\n"    /* EF BE AD DE */
            ".rodata\n"
            "    .asciz \"Hi\"\n"       /* 48 69 00 */
            ".bss\n"
            "    .zero 16\n");          /* NOBITS size 16 */
    QTEST_CHECK_TRUE(R.ok);

    static const uint8_t text[]   = { 0xC3 };
    static const uint8_t data[]   = { 0,0,0,0,0,0,0,0, 0xEF,0xBE,0xAD,0xDE };
    static const uint8_t rodata[] = { 0x48, 0x69, 0x00 };
    check_section(R.img, ".text", text, sizeof text);
    check_section(R.img, ".data", data, sizeof data);
    check_section(R.img, ".rodata", rodata, sizeof rodata);

    /* .bss is NOBITS: it has a size but occupies no file bytes. */
    int ibss = find_section(R.img, ".bss");
    QTEST_CHECK_TRUE(ibss >= 0);
    QTEST_CHECK_EQ_UINT(sh_size(shdr_at(R.img, (uint32_t)ibss)), 16u, ".bss size");

    /* .rela.data: one R_X86_64_64 against main, addend 0, at offset 0. */
    int irela = find_section(R.img, ".rela.data");
    QTEST_CHECK_TRUE(irela >= 0);
    const uint8_t *rel = R.img + sh_offset(shdr_at(R.img, (uint32_t)irela));
    QTEST_CHECK_EQ_UINT(rd64(rel + 0), 0u, "r_offset 0");
    QTEST_CHECK_EQ_UINT(rd64(rel + 8) & 0xffffffffu, 1u, "R_X86_64_64"); /* type 1 */
    QTEST_CHECK_EQ_INT((int64_t)rd64(rel + 16), 0, "addend 0");

    symview sv = get_symbols(R.img);
    int table = find_symbol(&sv, "table");
    QTEST_CHECK_TRUE(table >= 0);
    QTEST_CHECK_EQ_UINT(sym_value(&sv, table), 0u, "table at .data+0");

    asm_free(&R);
}

/* An .equ constant used as an immediate relocates against an absolute symbol. */
static void test_equ_absolute(void)
{
    asmres R;
    asm_run(&R,
            ".equ SYS_WRITE, 1\n"
            ".globl _start\n"
            "_start:\n"
            "    mov eax, SYS_WRITE\n"); /* B8 00 00 00 00 + R_X86_64_32S */
    QTEST_CHECK_TRUE(R.ok);

    static const uint8_t text[] = { 0xB8, 0x00, 0x00, 0x00, 0x00 };
    check_section(R.img, ".text", text, sizeof text);

    int irela = find_section(R.img, ".rela.text");
    QTEST_CHECK_TRUE(irela >= 0);
    const uint8_t *rel = R.img + sh_offset(shdr_at(R.img, (uint32_t)irela));
    QTEST_CHECK_EQ_UINT(rd64(rel + 0), 1u, "r_offset 1");
    QTEST_CHECK_EQ_UINT(rd64(rel + 8) & 0xffffffffu, 11u, "R_X86_64_32S"); /* 11 */

    /* SYS_WRITE is an absolute symbol (SHN_ABS = 0xfff1) with value 1. */
    symview sv = get_symbols(R.img);
    int s = find_symbol(&sv, "SYS_WRITE");
    QTEST_CHECK_TRUE(s >= 0);
    QTEST_CHECK_EQ_UINT(sym_shndx(&sv, s), 0xfff1u, "SHN_ABS");
    QTEST_CHECK_EQ_UINT(sym_value(&sv, s), 1u, "value 1");

    asm_free(&R);
}

/* A RIP-relative reference across sections cannot be resolved locally: it stays a
   relocation, and the (local) target symbol is emitted so the linker can find it. */
static void test_cross_section_rip(void)
{
    asmres R;
    asm_run(&R,
            ".text\n"
            ".globl f\n"
            "f:\n"
            "    lea rax, [rip + msg]\n" /* 48 8D 05 rel32, field @3 */
            "    ret\n"
            ".rodata\n"
            "msg:\n"
            "    .asciz \"x\"\n");
    QTEST_CHECK_TRUE(R.ok);

    static const uint8_t text[] = { 0x48, 0x8D, 0x05, 0,0,0,0, 0xC3 };
    check_section(R.img, ".text", text, sizeof text);

    int irela = find_section(R.img, ".rela.text");
    QTEST_CHECK_TRUE(irela >= 0);
    const uint8_t *rel = R.img + sh_offset(shdr_at(R.img, (uint32_t)irela));
    QTEST_CHECK_EQ_UINT(rd64(rel + 0), 3u, "r_offset 3");
    QTEST_CHECK_EQ_UINT(rd64(rel + 8) & 0xffffffffu, 2u, "R_X86_64_PC32");
    QTEST_CHECK_EQ_INT((int64_t)rd64(rel + 16), -4, "addend -4");

    symview sv = get_symbols(R.img);
    int msg = find_symbol(&sv, "msg");
    QTEST_CHECK_TRUE(msg >= 0); /* emitted because a relocation references it */

    asm_free(&R);
}

/* The symbol table orders locals before globals; sh_info marks the boundary. */
static void test_symtab_ordering(void)
{
    asmres R;
    asm_run(&R,
            ".text\n"
            ".globl g\n"
            "lref:\n"               /* local, but referenced by a cross-section reloc */
            "    ret\n"
            "g:\n"
            "    ret\n"
            ".data\n"
            "    .quad lref\n");     /* forces local "lref" into .symtab */
    QTEST_CHECK_TRUE(R.ok);

    int isym = find_section(R.img, ".symtab");
    QTEST_CHECK_TRUE(isym >= 0);
    const uint8_t *ss = shdr_at(R.img, (uint32_t)isym);
    uint32_t first_global = sh_info(ss);

    symview sv = get_symbols(R.img);
    int lref = find_symbol(&sv, "lref");
    int g    = find_symbol(&sv, "g");
    QTEST_CHECK_TRUE(lref >= 0 && g >= 0);
    /* The local symbol precedes the first global; the global is at/after it. */
    QTEST_CHECK_TRUE((uint32_t)lref < first_global);
    QTEST_CHECK_TRUE((uint32_t)g >= first_global);
    /* Binding nibble (high 4 bits of st_info): 0 local, 1 global. */
    QTEST_CHECK_EQ_UINT(sym_info(&sv, lref) >> 4, 0u, "lref local");
    QTEST_CHECK_EQ_UINT(sym_info(&sv, g) >> 4, 1u, "g global");

    asm_free(&R);
}

/* Errors are reported and suppress the object (no half-built output). */
static void test_errors_suppress_output(void)
{
    asmres R;

    asm_run(&R, "frobnicate rax\n"); /* unknown mnemonic */
    QTEST_CHECK_TRUE(!R.ok);
    QTEST_CHECK_TRUE(R.errors >= 1);
    asm_free(&R);

    asm_run(&R, "dup:\n dup:\n");     /* duplicate label */
    QTEST_CHECK_TRUE(!R.ok);
    QTEST_CHECK_TRUE(R.errors >= 1);
    asm_free(&R);

    asm_run(&R, ".bss\n .long 5\n");  /* data into NOBITS section */
    QTEST_CHECK_TRUE(!R.ok);
    QTEST_CHECK_TRUE(R.errors >= 1);
    asm_free(&R);

    asm_run(&R, ".frobticate 1\n");   /* unknown directive */
    QTEST_CHECK_TRUE(!R.ok);
    QTEST_CHECK_TRUE(R.errors >= 1);
    asm_free(&R);
}

/* A valid header is always produced for a clean source. */
static void test_elf_header(void)
{
    asmres R;
    asm_run(&R, ".text\n nop\n");
    QTEST_CHECK_TRUE(R.ok);
    QTEST_CHECK_EQ_UINT(R.img[0], 0x7Fu, "ELF mag0");
    QTEST_CHECK_EQ_UINT(R.img[1], (unsigned char)'E', "mag1");
    QTEST_CHECK_EQ_UINT(rd16(R.img + 16), 1u, "ET_REL");
    QTEST_CHECK_EQ_UINT(rd16(R.img + 18), 62u, "EM_X86_64");
    static const uint8_t nop[] = { 0x90 };
    check_section(R.img, ".text", nop, sizeof nop);
    asm_free(&R);
}

int main(void)
{
    test_local_branch_resolved();
    test_external_call_reloc();
    test_data_directives();
    test_equ_absolute();
    test_cross_section_rip();
    test_symtab_ordering();
    test_errors_suppress_output();
    test_elf_header();
    return qtest_report("asm");
}
