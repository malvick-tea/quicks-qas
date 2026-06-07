/*
 * qas — x86-64 register model
 *
 * Responsibility
 * Define the set of x86-64 registers qas understands and map a register's
 * spelling to the facts the encoder needs: its class, its operand size, and the
 * 4-bit number that is split across the ModR/M / SIB fields and the REX prefix.
 * This module is a *pure lookup table*: it depends only on the C fixed-width
 * headers, so both the parser (which recognizes a register operand) and the
 * encoder (which encodes it) can share it without a dependency cycle — the same
 * design rule the token module follows.
 *
 * Why a register's "number" is 4 bits split in two
 *   In 64-bit mode a register field is 4 bits: the low 3 bits live in a 3-bit
 *   slot of the ModR/M or SIB byte (or are added to an opcode), and the 4th bit
 *   is supplied by a bit of the REX prefix (REX.R, REX.X, or REX.B depending on
 *   the slot). So r8..r15 are "rax..rdi with the REX extension bit set". We store
 *   the full 0..15 number and expose the split via qas_reg_low3 / qas_reg_ext.
 *   (Intel SDM Vol 2 §2.2.1.2, "More on REX Prefix Fields"; the register codes
 *   are Table 3-1, "+rb/+rw/+rd/+ro" in §3.1.1.1.)
 *
 * The two byte-register subtleties (Intel SDM Vol 2 §2.2.1.2; Vol 1 §3.4.1.1)
 *   - The legacy high-byte registers AH, CH, DH, BH occupy 8-bit encodings 4..7
 *     but are addressable ONLY when no REX prefix is present.
 *   - The uniform low-byte registers SPL, BPL, SIL, DIL reuse encodings 4..7 and
 *     are addressable ONLY when a REX prefix IS present (even a "REX.W=0, no
 *     extension" prefix, byte 0x40). With REX present, encodings 4..7 mean
 *     SPL/BPL/SIL/DIL, not AH/CH/DH/BH.
 *   We flag these with `high_byte` (forbids REX) and `rex_required` (forces REX)
 *   so the encoder can detect the illegal "AH with REX" combination and emit the
 *   mandatory 0x40 for "SPL" with no other reason for a REX byte.
 */
#ifndef QAS_REG_REG_H
#define QAS_REG_REG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Register class. Kept small on purpose: qas grows it (XMM/YMM, segment, control,
 * debug) only when an instruction it must assemble needs them (depth is earned —
 * Quicks-Meta ADR-0008). RIP is its own class because it is never a normal
 * operand: it appears only as the base of a RIP-relative memory reference, which
 * the encoder handles as a special ModR/M form (mod=00, r/m=101).
 */
typedef enum qas_reg_class {
    QAS_REG_CLASS_NONE = 0, /* Not a register / lookup miss.                     */
    QAS_REG_CLASS_GPR,      /* General-purpose integer register (8/16/32/64-bit).*/
    QAS_REG_CLASS_IP        /* The instruction pointer (rip), for RIP-relative.  */
} qas_reg_class;

/*
 * One register. `encoding` is the full 0..15 number; the encoder splits it with
 * qas_reg_low3 (the 3 bits that go in ModR/M/SIB/opcode) and qas_reg_ext (the
 * bit that goes in REX). `size_bits` is 8/16/32/64 for GPRs and 0 for rip.
 *
 * Pointers returned by the lookup are into a single static table that lives for
 * the whole program; callers borrow them and never free them.
 */
typedef struct qas_reg {
    const char   *name;         /* Canonical lowercase spelling, e.g. "rax".     */
    qas_reg_class reg_class;
    uint8_t       size_bits;    /* 8, 16, 32, 64; 0 for rip.                     */
    uint8_t       encoding;     /* Register number 0..15 (see header note).      */
    bool          rex_required; /* True for spl/bpl/sil/dil: a REX prefix must be
                                   present for this register to be addressable.   */
    bool          high_byte;    /* True for ah/ch/dh/bh: a REX prefix must NOT be
                                   present (they are unaddressable under REX).     */
} qas_reg;

/*
 * Look up a register by name given as a byte span [name, name+length) — the form
 * a lexer token provides, so callers need not NUL-terminate. Matching is ASCII
 * case-insensitive (NASM treats register names case-insensitively, ADR-0005),
 * done with explicit ASCII folding so it is locale-independent like the lexer.
 *
 * On a hit, *out points at the static table entry and the function returns true.
 * On a miss, *out is set to NULL and the function returns false. `out` must be
 * non-NULL; a NULL `name` with `length` 0 is simply a miss.
 */
bool qas_reg_lookup(const char *name, size_t length, const qas_reg **out);

/*
 * The low 3 bits of the register number — the value that occupies a 3-bit
 * register slot of a ModR/M byte (reg or r/m field), a SIB byte (index or base),
 * or the low bits of a "+r" opcode. (Intel SDM Vol 2 §2.1.5.)
 */
uint8_t qas_reg_low3(const qas_reg *reg);

/*
 * The 4th (high) bit of the register number — the value the REX prefix supplies
 * via REX.R, REX.X, or REX.B for the slot the register occupies. 0 for rax..rdi,
 * 1 for r8..r15. (Intel SDM Vol 2 §2.2.1.2.)
 */
uint8_t qas_reg_ext(const qas_reg *reg);

#endif /* QAS_REG_REG_H */
