/*
 * Tests for the register model: that each register maps to the size and encoding
 * the Intel SDM assigns it, that the 4-bit number splits correctly into its
 * ModR/M/SIB part (low3) and REX bit (ext), that the byte-register special cases
 * carry the right REX flags, and that lookup is span-bounded and case-insensitive.
 */
#include "qtest.h"

#include "reg/reg.h"

/* Look up by NUL-terminated name (tests pass C string literals). */
static const qas_reg *find(const char *name)
{
    const qas_reg *r = NULL;
    (void)qas_reg_lookup(name, strlen(name), &r);
    return r;
}

static void test_gpr64(void)
{
    const qas_reg *rax = find("rax");
    QTEST_CHECK_TRUE(rax != NULL);
    QTEST_CHECK_EQ_INT(rax->reg_class, QAS_REG_CLASS_GPR, "rax class");
    QTEST_CHECK_EQ_UINT(rax->size_bits, 64u, "rax size");
    QTEST_CHECK_EQ_UINT(rax->encoding, 0u, "rax enc");
    QTEST_CHECK_EQ_UINT(qas_reg_low3(rax), 0u, "rax low3");
    QTEST_CHECK_EQ_UINT(qas_reg_ext(rax), 0u, "rax ext");

    /* rsp/rbp/rsi/rdi take the 4..7 slots (the SP/BP/SI/DI ordering). */
    QTEST_CHECK_EQ_UINT(find("rsp")->encoding, 4u, "rsp enc");
    QTEST_CHECK_EQ_UINT(find("rbp")->encoding, 5u, "rbp enc");
    QTEST_CHECK_EQ_UINT(find("rsi")->encoding, 6u, "rsi enc");
    QTEST_CHECK_EQ_UINT(find("rdi")->encoding, 7u, "rdi enc");

    /* r8..r15 are rax..rdi with the REX extension bit set. */
    const qas_reg *r8 = find("r8");
    QTEST_CHECK_EQ_UINT(r8->encoding, 8u, "r8 enc");
    QTEST_CHECK_EQ_UINT(qas_reg_low3(r8), 0u, "r8 low3");
    QTEST_CHECK_EQ_UINT(qas_reg_ext(r8), 1u, "r8 ext");

    const qas_reg *r15 = find("r15");
    QTEST_CHECK_EQ_UINT(r15->encoding, 15u, "r15 enc");
    QTEST_CHECK_EQ_UINT(qas_reg_low3(r15), 7u, "r15 low3");
    QTEST_CHECK_EQ_UINT(qas_reg_ext(r15), 1u, "r15 ext");
}

static void test_subregisters(void)
{
    QTEST_CHECK_EQ_UINT(find("eax")->size_bits, 32u, "eax size");
    QTEST_CHECK_EQ_UINT(find("eax")->encoding, 0u, "eax enc");
    QTEST_CHECK_EQ_UINT(find("r13d")->size_bits, 32u, "r13d size");
    QTEST_CHECK_EQ_UINT(qas_reg_low3(find("r13d")), 5u, "r13d low3");
    QTEST_CHECK_EQ_UINT(qas_reg_ext(find("r13d")), 1u, "r13d ext");

    QTEST_CHECK_EQ_UINT(find("ax")->size_bits, 16u, "ax size");
    QTEST_CHECK_EQ_UINT(find("r10w")->encoding, 10u, "r10w enc");

    QTEST_CHECK_EQ_UINT(find("al")->size_bits, 8u, "al size");
    QTEST_CHECK_EQ_UINT(find("al")->encoding, 0u, "al enc");
    QTEST_CHECK_EQ_UINT(find("r9b")->encoding, 9u, "r9b enc");
}

static void test_byte_register_rex_rules(void)
{
    /* Uniform low bytes: encodings 4..7, require a REX prefix. */
    const qas_reg *spl = find("spl");
    QTEST_CHECK_EQ_UINT(spl->encoding, 4u, "spl enc");
    QTEST_CHECK_TRUE(spl->rex_required);
    QTEST_CHECK_TRUE(!spl->high_byte);
    QTEST_CHECK_EQ_UINT(find("dil")->encoding, 7u, "dil enc");
    QTEST_CHECK_TRUE(find("dil")->rex_required);

    /* Legacy high bytes: same encodings 4..7, forbid a REX prefix. */
    const qas_reg *ah = find("ah");
    QTEST_CHECK_EQ_UINT(ah->encoding, 4u, "ah enc");
    QTEST_CHECK_TRUE(ah->high_byte);
    QTEST_CHECK_TRUE(!ah->rex_required);
    QTEST_CHECK_EQ_UINT(find("bh")->encoding, 7u, "bh enc");
    QTEST_CHECK_TRUE(find("bh")->high_byte);

    /* al..bl (0..3) are ordinary bytes: neither flag set. */
    QTEST_CHECK_TRUE(!find("al")->rex_required && !find("al")->high_byte);
}

static void test_rip(void)
{
    const qas_reg *rip = find("rip");
    QTEST_CHECK_TRUE(rip != NULL);
    QTEST_CHECK_EQ_INT(rip->reg_class, QAS_REG_CLASS_IP, "rip class");
    QTEST_CHECK_EQ_UINT(rip->size_bits, 0u, "rip size");
}

static void test_case_insensitive_and_misses(void)
{
    /* NASM treats register names case-insensitively (ADR-0005). */
    QTEST_CHECK_TRUE(find("RAX") != NULL && find("RAX")->encoding == 0u);
    QTEST_CHECK_TRUE(find("Rax") == find("rax"));

    /* Misses leave *out NULL and return false. */
    const qas_reg *r = (const qas_reg *)1; /* poison, must be cleared on miss */
    QTEST_CHECK_TRUE(!qas_reg_lookup("nope", 4, &r));
    QTEST_CHECK_TRUE(r == NULL);
    QTEST_CHECK_TRUE(find("") == NULL);
    QTEST_CHECK_TRUE(find("rax1") == NULL);
}

static void test_span_bounded(void)
{
    /* Lookup honors `length`: "rax" is matched from a longer buffer when the
       length is 3, and "ra" (length 2) is not a prefix-match for "rax". */
    const qas_reg *r = NULL;
    QTEST_CHECK_TRUE(qas_reg_lookup("raxext", 3, &r) && r != NULL);
    QTEST_CHECK_EQ_UINT(r->encoding, 0u, "span rax enc");

    r = (const qas_reg *)1;
    QTEST_CHECK_TRUE(!qas_reg_lookup("rax", 2, &r)); /* "ra" must not match "rax" */
    QTEST_CHECK_TRUE(r == NULL);
}

int main(void)
{
    test_gpr64();
    test_subregisters();
    test_byte_register_rex_rules();
    test_rip();
    test_case_insensitive_and_misses();
    test_span_bounded();
    return qtest_report("reg");
}
