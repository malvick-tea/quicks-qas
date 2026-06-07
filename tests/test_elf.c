/*
 * Tests for the ELF64 writer. We build a small relocatable object with the
 * builder, then parse the bytes back with a tiny inline reader and assert the
 * header, section table, symbol table (including local-before-global ordering and
 * sh_info), string tables, and a relocation are all byte-exact against the
 * System V gABI / x86-64 psABI layout.
 */
#include "qtest.h"

#include <stdlib.h> /* free() */

#include "elf/elf.h"

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

/* Pointer to section header `i` (each is 64 bytes at e_shoff). */
static const uint8_t *shdr_at(const uint8_t *img, uint32_t i)
{
    return img + rd64(img + 40) + (uint64_t)i * 64u;
}

/* Offset of the .shstrtab contents. */
static uint64_t shstr_off(const uint8_t *img)
{
    return rd64(shdr_at(img, rd16(img + 62)) + 24);
}

/* Find a section header index by name, or -1. */
static int find_section(const uint8_t *img, const char *name)
{
    uint16_t shnum = rd16(img + 60);
    uint64_t soff  = shstr_off(img);
    for (uint32_t i = 0; i < shnum; ++i) {
        uint32_t name_off = rd32(shdr_at(img, i)); /* sh_name @ 0 */
        if (strcmp((const char *)(img + soff + name_off), name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Section header field accessors (offsets per the gABI Elf64_Shdr layout). */
static uint32_t sh_type(const uint8_t *s)      { return rd32(s + 4); }
static uint64_t sh_flags(const uint8_t *s)     { return rd64(s + 8); }
static uint64_t sh_offset(const uint8_t *s)    { return rd64(s + 24); }
static uint64_t sh_size(const uint8_t *s)      { return rd64(s + 32); }
static uint32_t sh_link(const uint8_t *s)      { return rd32(s + 40); }
static uint32_t sh_info(const uint8_t *s)      { return rd32(s + 44); }
static uint64_t sh_addralign(const uint8_t *s) { return rd64(s + 48); }
static uint64_t sh_entsize(const uint8_t *s)   { return rd64(s + 56); }

static void test_header_and_sections(void)
{
    qas_elf_builder b;
    qas_elf_builder_init(&b);

    uint32_t text = 0, data = 0;
    QTEST_CHECK_EQ_INT(qas_elf_builder_add_section(&b, ".text", SHT_PROGBITS,
                       SHF_ALLOC | SHF_EXECINSTR, 16, &text), QAS_OK, "add .text");
    QTEST_CHECK_EQ_INT(qas_elf_builder_add_section(&b, ".data", SHT_PROGBITS,
                       SHF_ALLOC | SHF_WRITE, 8, &data), QAS_OK, "add .data");

    /* .text = nop; ret; then a 4-byte relocation site. */
    const uint8_t code[] = { 0x90, 0xC3 };
    uint64_t off = 99;
    QTEST_CHECK_EQ_INT(qas_elf_builder_append(&b, text, code, sizeof code, &off),
                       QAS_OK, "append code");
    QTEST_CHECK_EQ_UINT(off, 0u, "code at offset 0");
    uint64_t site = 0;
    const uint8_t zeros[4] = { 0, 0, 0, 0 };
    QTEST_CHECK_EQ_INT(qas_elf_builder_append(&b, text, zeros, 4, &site), QAS_OK,
                       "append site");
    QTEST_CHECK_EQ_UINT(site, 2u, "site at offset 2");

    const uint8_t datum = 0x2A;
    QTEST_CHECK_EQ_INT(qas_elf_builder_append(&b, data, &datum, 1, NULL), QAS_OK,
                       "append datum");

    /* Symbols added out of binding order, to prove finish reorders them. */
    uint32_t sym_main = 0, sym_loc = 0, sym_ext = 0;
    QTEST_CHECK_EQ_INT(qas_elf_builder_add_symbol(&b, "main", STB_GLOBAL, STT_FUNC,
                       QAS_ELF_SYMREF_SECTION, text, 0, 3, &sym_main), QAS_OK, "main");
    QTEST_CHECK_EQ_INT(qas_elf_builder_add_symbol(&b, "loc", STB_LOCAL, STT_NOTYPE,
                       QAS_ELF_SYMREF_SECTION, text, 2, 0, &sym_loc), QAS_OK, "loc");
    QTEST_CHECK_EQ_INT(qas_elf_builder_add_symbol(&b, "ext", STB_GLOBAL, STT_NOTYPE,
                       QAS_ELF_SYMREF_UNDEF, 0, 0, 0, &sym_ext), QAS_OK, "ext");

    /* call ext: PC-relative 32-bit, addend -4 (psABI R_X86_64_PC32 = S + A - P). */
    QTEST_CHECK_EQ_INT(qas_elf_builder_add_rela(&b, text, site, sym_ext,
                       R_X86_64_PC32, -4), QAS_OK, "rela");

    uint8_t *img = NULL;
    size_t   len = 0;
    QTEST_CHECK_EQ_INT(qas_elf_builder_finish(&b, &img, &len), QAS_OK, "finish");
    QTEST_CHECK_TRUE(img != NULL && len > 0);

    /* ELF identification. */
    QTEST_CHECK_EQ_UINT(img[EI_MAG0], 0x7Fu, "mag0");
    QTEST_CHECK_EQ_UINT(img[EI_MAG1], (unsigned char)'E', "mag1");
    QTEST_CHECK_EQ_UINT(img[EI_MAG2], (unsigned char)'L', "mag2");
    QTEST_CHECK_EQ_UINT(img[EI_MAG3], (unsigned char)'F', "mag3");
    QTEST_CHECK_EQ_UINT(img[EI_CLASS], ELFCLASS64, "class64");
    QTEST_CHECK_EQ_UINT(img[EI_DATA], ELFDATA2LSB, "lsb");
    QTEST_CHECK_EQ_UINT(img[EI_VERSION], EV_CURRENT, "version");
    QTEST_CHECK_EQ_UINT(img[EI_OSABI], ELFOSABI_SYSV, "osabi");

    /* Header fields. */
    QTEST_CHECK_EQ_UINT(rd16(img + 16), ET_REL, "e_type");
    QTEST_CHECK_EQ_UINT(rd16(img + 18), EM_X86_64, "e_machine");
    QTEST_CHECK_EQ_UINT(rd32(img + 20), EV_CURRENT, "e_version");
    QTEST_CHECK_EQ_UINT(rd16(img + 52), 64u, "e_ehsize");
    QTEST_CHECK_EQ_UINT(rd16(img + 58), 64u, "e_shentsize");
    QTEST_CHECK_TRUE(rd64(img + 40) >= 64u); /* e_shoff past the header */

    /* Section[0] is the null header. */
    QTEST_CHECK_EQ_UINT(sh_type(shdr_at(img, 0)), SHT_NULL, "null type");

    /* .text contents and attributes. */
    int it = find_section(img, ".text");
    QTEST_CHECK_TRUE(it >= 0);
    const uint8_t *stext = shdr_at(img, (uint32_t)it);
    QTEST_CHECK_EQ_UINT(sh_type(stext), SHT_PROGBITS, ".text type");
    QTEST_CHECK_EQ_UINT(sh_flags(stext), (SHF_ALLOC | SHF_EXECINSTR), ".text flags");
    QTEST_CHECK_EQ_UINT(sh_addralign(stext), 16u, ".text align");
    QTEST_CHECK_EQ_UINT(sh_size(stext), 6u, ".text size");
    const uint8_t *tb = img + sh_offset(stext);
    QTEST_CHECK_EQ_UINT(tb[0], 0x90u, "nop");
    QTEST_CHECK_EQ_UINT(tb[1], 0xC3u, "ret");

    /* .data present and writable. */
    int id = find_section(img, ".data");
    QTEST_CHECK_TRUE(id >= 0);
    QTEST_CHECK_EQ_UINT(sh_flags(shdr_at(img, (uint32_t)id)),
                        (SHF_ALLOC | SHF_WRITE), ".data flags");
    QTEST_CHECK_EQ_UINT(sh_size(shdr_at(img, (uint32_t)id)), 1u, ".data size");

    /* .symtab: locals before globals, sh_info = first global, links to .strtab. */
    int isym = find_section(img, ".symtab");
    int istr = find_section(img, ".strtab");
    QTEST_CHECK_TRUE(isym >= 0 && istr >= 0);
    const uint8_t *ssym = shdr_at(img, (uint32_t)isym);
    QTEST_CHECK_EQ_UINT(sh_type(ssym), SHT_SYMTAB, "symtab type");
    QTEST_CHECK_EQ_UINT(sh_entsize(ssym), 24u, "sym entsize");
    QTEST_CHECK_EQ_UINT(sh_link(ssym), (uint32_t)istr, "symtab link=strtab");
    QTEST_CHECK_EQ_UINT(sh_info(ssym), 2u, "first global index"); /* null+loc */
    QTEST_CHECK_EQ_UINT(sh_size(ssym), 4u * 24u, "4 symbols");

    const uint8_t *syms = img + sh_offset(ssym);
    const uint8_t *strs = img + sh_offset(shdr_at(img, (uint32_t)istr));
    const char    *name1 = (const char *)(strs + rd32(syms + 1 * 24 + 0));
    const char    *name2 = (const char *)(strs + rd32(syms + 2 * 24 + 0));
    const char    *name3 = (const char *)(strs + rd32(syms + 3 * 24 + 0));
    /* sym[1] is the local "loc"; sym[2] "main"; sym[3] "ext". */
    QTEST_CHECK_SPAN(name1, strlen(name1), "loc", "sym1 name");
    QTEST_CHECK_EQ_UINT(syms[1 * 24 + 4], QAS_ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE),
                        "loc info");
    QTEST_CHECK_EQ_UINT(rd64(syms + 1 * 24 + 8), 2u, "loc value");
    QTEST_CHECK_SPAN(name2, strlen(name2), "main", "sym2 name");
    QTEST_CHECK_EQ_UINT(syms[2 * 24 + 4], QAS_ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
                        "main info");
    QTEST_CHECK_EQ_UINT(rd64(syms + 2 * 24 + 16), 3u, "main size");
    QTEST_CHECK_SPAN(name3, strlen(name3), "ext", "sym3 name");
    QTEST_CHECK_EQ_UINT(rd16(syms + 3 * 24 + 6), SHN_UNDEF, "ext undef");

    /* .rela.text: one PC32 relocation against "ext" (final symbol index 3). */
    int irela = find_section(img, ".rela.text");
    QTEST_CHECK_TRUE(irela >= 0);
    const uint8_t *srela = shdr_at(img, (uint32_t)irela);
    QTEST_CHECK_EQ_UINT(sh_type(srela), SHT_RELA, "rela type");
    QTEST_CHECK_EQ_UINT(sh_entsize(srela), 24u, "rela entsize");
    QTEST_CHECK_EQ_UINT(sh_link(srela), (uint32_t)isym, "rela link=symtab");
    QTEST_CHECK_EQ_UINT(sh_info(srela), (uint32_t)it, "rela info=.text");
    QTEST_CHECK_EQ_UINT(sh_size(srela), 24u, "one reloc");
    const uint8_t *rel = img + sh_offset(srela);
    QTEST_CHECK_EQ_UINT(rd64(rel + 0), 2u, "r_offset");
    uint64_t info = rd64(rel + 8);
    QTEST_CHECK_EQ_UINT(info >> 32, 3u, "r_sym=3 (ext)");
    QTEST_CHECK_EQ_UINT(info & 0xffffffffu, R_X86_64_PC32, "r_type=PC32");
    QTEST_CHECK_EQ_INT((int64_t)rd64(rel + 16), -4, "r_addend");

    free(img);
    qas_elf_builder_dispose(&b);
}

static void test_empty_object(void)
{
    /* Even with no sections/symbols, finish must produce a valid minimal ELF:
       null + .symtab + .strtab + .shstrtab = 4 section headers. */
    qas_elf_builder b;
    qas_elf_builder_init(&b);

    uint8_t *img = NULL;
    size_t   len = 0;
    QTEST_CHECK_EQ_INT(qas_elf_builder_finish(&b, &img, &len), QAS_OK, "finish empty");
    QTEST_CHECK_EQ_UINT(img[EI_MAG0], 0x7Fu, "mag0");
    QTEST_CHECK_EQ_UINT(rd16(img + 16), ET_REL, "e_type");
    QTEST_CHECK_EQ_UINT(rd16(img + 60), 4u, "shnum=4");
    QTEST_CHECK_TRUE(find_section(img, ".symtab") >= 0);
    QTEST_CHECK_TRUE(find_section(img, ".shstrtab") >= 0);

    free(img);
    qas_elf_builder_dispose(&b);
}

int main(void)
{
    test_header_and_sections();
    test_empty_object();
    return qtest_report("elf");
}
