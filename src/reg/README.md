# module: reg

**Responsibility:** define the x86-64 registers qas understands and map a
register spelling to the facts the encoder needs — class, operand size, and the
4-bit register number that is split between a ModR/M/SIB field and the REX prefix.

**Public interface:** `reg/reg.h` (`qas_reg_class`, `qas_reg`, `qas_reg_lookup`,
`qas_reg_low3`, `qas_reg_ext`).

**What it models now:** the general-purpose registers in all four operand sizes
(8/16/32/64-bit, including `r8`..`r15` and their sub-registers), the two byte-
register special cases (`ah/ch/dh/bh` vs `spl/bpl/sil/dil`), and `rip` for
RIP-relative addressing. XMM/segment/control registers are added when an
instruction qas must assemble first needs them (depth is earned — ADR-0008).

**Design notes:**
- A *pure lookup table*, dependency-free like `token`, so both the parser and the
  encoder can use it without a dependency cycle.
- Register numbers are the values from Intel SDM Vol 2 Table 3-1; `qas_reg_low3`
  and `qas_reg_ext` split each into its ModR/M/SIB part and its REX bit
  (Vol 2 §2.1.5, §2.2.1.2).
- `rex_required` (spl/bpl/sil/dil) and `high_byte` (ah/ch/dh/bh) let the encoder
  reject the illegal "high byte register with REX" combination and force the
  mandatory REX for the uniform byte registers (Vol 2 §2.2.1.2; Vol 1 §3.4.1.1).
- Name matching is ASCII case-insensitive and locale-independent (no `<ctype.h>`).

**Dependencies:** none (beyond the C standard headers).
