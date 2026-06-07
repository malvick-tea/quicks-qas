/*
 * qas — x86-64 register model: the register table and lookup.
 *
 * See reg.h for the data model and the byte-register subtleties. The table below
 * is a transcription of the x86-64 register encodings; every register number is
 * the value the Intel SDM assigns it (Vol 2 Table 3-1, register codes; the legacy
 * A/C/D/B/SP/BP/SI/DI ordering, then r8..r15 = 8..15). Encoding qas as a table the
 * encoder iterates — rather than a switch per register — follows coding-standard
 * §3 (table-driven where the spec is a table) and ADR-0011.
 */
#include "reg/reg.h"

/*
 * Row constructors. They exist only to keep the table dense and aligned so a
 * wrong number is easy to spot; each expands to a plain struct initializer. The
 * three forms capture the only per-register variation that matters:
 *   GPR     - an ordinary general register (no byte-register special case);
 *   GPR_REX - an 8-bit uniform low byte (spl/bpl/sil/dil): REX must be present;
 *   GPR_HI  - a legacy high byte (ah/ch/dh/bh): REX must be absent.
 * Undefined right after the table so the macros do not leak out of this file.
 */
#define GPR(name, size, enc)  { (name), QAS_REG_CLASS_GPR, (uint8_t)(size), (uint8_t)(enc), false, false }
#define GPR_REX(name, enc)    { (name), QAS_REG_CLASS_GPR, 8, (uint8_t)(enc), true,  false }
#define GPR_HI(name, enc)     { (name), QAS_REG_CLASS_GPR, 8, (uint8_t)(enc), false, true  }

static const qas_reg qas_reg_table[] = {
    /* 64-bit general-purpose registers (Intel SDM Vol 2 Table 3-1, +ro codes). */
    GPR("rax", 64, 0),  GPR("rcx", 64, 1),  GPR("rdx", 64, 2),  GPR("rbx", 64, 3),
    GPR("rsp", 64, 4),  GPR("rbp", 64, 5),  GPR("rsi", 64, 6),  GPR("rdi", 64, 7),
    GPR("r8",  64, 8),  GPR("r9",  64, 9),  GPR("r10", 64, 10), GPR("r11", 64, 11),
    GPR("r12", 64, 12), GPR("r13", 64, 13), GPR("r14", 64, 14), GPR("r15", 64, 15),

    /* 32-bit general-purpose registers (+rd codes). */
    GPR("eax", 32, 0),  GPR("ecx", 32, 1),  GPR("edx", 32, 2),  GPR("ebx", 32, 3),
    GPR("esp", 32, 4),  GPR("ebp", 32, 5),  GPR("esi", 32, 6),  GPR("edi", 32, 7),
    GPR("r8d", 32, 8),  GPR("r9d", 32, 9),  GPR("r10d",32, 10), GPR("r11d",32, 11),
    GPR("r12d",32, 12), GPR("r13d",32, 13), GPR("r14d",32, 14), GPR("r15d",32, 15),

    /* 16-bit general-purpose registers (+rw codes). */
    GPR("ax",  16, 0),  GPR("cx",  16, 1),  GPR("dx",  16, 2),  GPR("bx",  16, 3),
    GPR("sp",  16, 4),  GPR("bp",  16, 5),  GPR("si",  16, 6),  GPR("di",  16, 7),
    GPR("r8w", 16, 8),  GPR("r9w", 16, 9),  GPR("r10w",16, 10), GPR("r11w",16, 11),
    GPR("r12w",16, 12), GPR("r13w",16, 13), GPR("r14w",16, 14), GPR("r15w",16, 15),

    /* 8-bit low registers, REX-free encodings 0..3 (al/cl/dl/bl) and the r8..r15
       byte forms 8..15 (+rb codes). */
    GPR("al",  8, 0),   GPR("cl",  8, 1),   GPR("dl",  8, 2),   GPR("bl",  8, 3),
    GPR("r8b", 8, 8),   GPR("r9b", 8, 9),   GPR("r10b",8, 10),  GPR("r11b",8, 11),
    GPR("r12b",8, 12),  GPR("r13b",8, 13),  GPR("r14b",8, 14),  GPR("r15b",8, 15),

    /* 8-bit uniform low bytes 4..7: addressable only WITH a REX prefix present
       (Intel SDM Vol 2 §2.2.1.2; Vol 1 §3.4.1.1). */
    GPR_REX("spl", 4),  GPR_REX("bpl", 5),  GPR_REX("sil", 6),  GPR_REX("dil", 7),

    /* 8-bit legacy high bytes 4..7: addressable only WITHOUT a REX prefix. They
       share the encodings of spl/bpl/sil/dil; REX presence selects which. */
    GPR_HI("ah", 4),    GPR_HI("ch", 5),    GPR_HI("dh", 6),    GPR_HI("bh", 7),

    /* The instruction pointer, for RIP-relative addressing only (encoder handles
       it as ModR/M mod=00, r/m=101 — Intel SDM Vol 2 §2.2.1.6). */
    { "rip", QAS_REG_CLASS_IP, 0, 0, false, false },
};

#undef GPR
#undef GPR_REX
#undef GPR_HI

static const size_t qas_reg_table_count =
    sizeof(qas_reg_table) / sizeof(qas_reg_table[0]);

/*
 * ASCII-only case fold of one byte: 'A'..'Z' -> 'a'..'z', everything else
 * unchanged. We do not use <ctype.h> (tolower) because its result depends on the
 * current locale, and register recognition must be deterministic — the same rule
 * the lexer follows for character classification.
 */
static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/*
 * Compare a byte span [span, span+length) to a NUL-terminated lowercase `name`,
 * ASCII case-insensitively. Returns true on an exact (folded) match. `name` is
 * already lowercase in the table, so only `span` needs folding.
 */
static bool span_ieq(const char *span, size_t length, const char *name)
{
    for (size_t i = 0; i < length; ++i) {
        if (name[i] == '\0' || ascii_lower(span[i]) != name[i]) {
            return false;
        }
    }
    return name[length] == '\0'; /* Both ended together => equal length. */
}

bool qas_reg_lookup(const char *name, size_t length, const qas_reg **out)
{
    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (name == NULL || length == 0) {
        return false;
    }

    /*
     * Linear scan. The table is small (under a hundred short strings) and a
     * register lookup happens once per register operand, so a hash or sorted
     * search would add complexity with no measurable benefit (coding-standard
     * §10: clarity wins). Revisit only if profiling ever says otherwise.
     */
    for (size_t i = 0; i < qas_reg_table_count; ++i) {
        if (span_ieq(name, length, qas_reg_table[i].name)) {
            *out = &qas_reg_table[i];
            return true;
        }
    }
    return false;
}

uint8_t qas_reg_low3(const qas_reg *reg)
{
    return (reg != NULL) ? (uint8_t)(reg->encoding & 0x7u) : 0u;
}

uint8_t qas_reg_ext(const qas_reg *reg)
{
    return (reg != NULL) ? (uint8_t)((reg->encoding >> 3) & 0x1u) : 0u;
}
