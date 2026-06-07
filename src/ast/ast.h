/*
 * qas — abstract syntax tree (parsed statements and operands)
 *
 * Responsibility
 * Define the typed representation the parser produces and the encoder consumes:
 * a statement (a label, a directive, or an instruction) and the operands of an
 * instruction (register, immediate, or memory reference). Like the token module,
 * this is mostly a *data type* shared by two stages, so it is kept to a thin
 * dependency: it needs the register model (an operand can be a register) and the
 * fixed-width headers, nothing else. Keeping it separate from the parser lets the
 * encoder depend on the shape of the data without depending on how it is built.
 *
 * Names are stored as source spans, not copies
 *   A label name, a mnemonic, a directive name, and any symbol referenced by an
 *   operand are recorded as (offset, length) byte spans into the originating
 *   qas_source — exactly as tokens are (see token.h). The source outlives the
 *   AST, so this avoids copying identifiers and keeps nodes small. The encoder
 *   resolves a span to text via src->data[offset .. offset+length).
 *
 * Numeric values
 *   Integer literals and displacements are stored as the raw 64-bit value with
 *   any leading sign already applied in two's complement (so "-1" is stored as
 *   0xFFFFFFFFFFFFFFFF). The encoder is responsible for checking that a value
 *   fits the operand width it is asked to encode (Intel SDM Vol 2 immediate/
 *   displacement sizes), not the parser.
 */
#ifndef QAS_AST_AST_H
#define QAS_AST_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reg/reg.h"

/* x86-64 instructions take at most a handful of operands; 4 covers everything
   the legacy and VEX forms need, with headroom. */
#define QAS_MAX_OPERANDS 4

/* What an operand is. */
typedef enum qas_operand_kind {
    QAS_OPERAND_NONE = 0, /* Unused operand slot.                                */
    QAS_OPERAND_REG,      /* A register: `rax`.                                  */
    QAS_OPERAND_IMM,      /* An immediate: a literal value or a symbol address.  */
    QAS_OPERAND_MEM       /* A memory reference: `[base + index*scale + disp]`.  */
} qas_operand_kind;

/*
 * An immediate operand: either a literal value, or a symbol's address (with an
 * optional integer addend). When is_symbol is true, `value` holds the addend
 * (usually 0) and the symbol name is sym_off/sym_len; the encoder emits a
 * relocation. When false, `value` is the literal (sign already applied).
 */
typedef struct qas_imm {
    bool     is_symbol;
    size_t   sym_off;   /* Symbol name span, when is_symbol.                     */
    size_t   sym_len;
    uint64_t value;     /* Literal value, or addend when is_symbol.             */
} qas_imm;

/*
 * A memory operand: any of `[base]`, `[base + index*scale]`, `[base + disp]`,
 * `[index*scale + disp]`, `[disp]`, `[rip + symbol]`, and combinations. Absent
 * components are NULL/false. `scale` is meaningful only when `index` is present
 * and is one of 1/2/4/8 (Intel SDM Vol 2 §2.1.5, the SIB scale field). A `base`
 * of class QAS_REG_CLASS_IP marks RIP-relative addressing.
 *
 * The displacement is `disp`; when disp_is_symbol it is a relocation against the
 * named symbol with `disp` as the addend (e.g. `[rip + foo]`). `size` is an
 * optional operand-size hint in bits from a `byte/word/dword/qword` prefix, or 0
 * if none was given (the encoder must then infer the size from the other operand).
 */
typedef struct qas_mem {
    const qas_reg *base;
    const qas_reg *index;
    uint8_t        scale;
    bool           has_disp;
    bool           disp_is_symbol;
    size_t         sym_off;   /* Symbol name span, when disp_is_symbol.          */
    size_t         sym_len;
    int64_t        disp;      /* Displacement, or addend when disp_is_symbol.    */
    uint8_t        size;      /* Operand-size hint in bits (0 = none).           */
} qas_mem;

/* One operand. The reg/imm/mem fields are valid according to `kind`. `offset`/
   `length` span the operand in the source, for diagnostics. */
typedef struct qas_operand {
    qas_operand_kind kind;
    size_t           offset;
    size_t           length;
    const qas_reg   *reg;  /* QAS_OPERAND_REG.                                   */
    qas_imm          imm;  /* QAS_OPERAND_IMM.                                   */
    qas_mem          mem;  /* QAS_OPERAND_MEM.                                   */
} qas_operand;

/* What a statement is. */
typedef enum qas_stmt_kind {
    QAS_STMT_LABEL = 0,   /* `name:` — defines a symbol at the current location. */
    QAS_STMT_DIRECTIVE,   /* `.name args` — assembler directive.                 */
    QAS_STMT_INSTRUCTION  /* `mnemonic operands` — a machine instruction.        */
} qas_stmt_kind;

/* What a directive argument is (a small closed set; expressions come later). */
typedef enum qas_dir_arg_kind {
    QAS_DIR_ARG_INT = 0,  /* Integer literal (sign applied in `int_value`).      */
    QAS_DIR_ARG_STRING,   /* String literal; span includes the quotes.           */
    QAS_DIR_ARG_SYMBOL    /* Identifier / symbol name.                           */
} qas_dir_arg_kind;

/* One argument to a directive. `off`/`len` span the argument in the source: for a
   string it includes the quotes (decoded by the directive handler); for a symbol
   it is the bare name. `int_value` is meaningful only for QAS_DIR_ARG_INT. */
typedef struct qas_dir_arg {
    qas_dir_arg_kind kind;
    size_t           off;
    size_t           len;
    uint64_t         int_value;
} qas_dir_arg;

/*
 * One statement. `name_off`/`name_len` hold the label name, the mnemonic, or the
 * directive name (including its leading '.') depending on `kind`. Instruction
 * operands are in `operands[0 .. operand_count)`. Directive arguments are in the
 * arena-allocated `args[0 .. arg_count)`. `offset`/`length`/`line` locate the
 * statement for diagnostics.
 */
typedef struct qas_stmt {
    qas_stmt_kind kind;
    size_t        offset;
    size_t        length;
    uint32_t      line;

    size_t        name_off;
    size_t        name_len;

    qas_operand   operands[QAS_MAX_OPERANDS];
    size_t        operand_count;

    const qas_dir_arg *args;
    size_t             arg_count;
} qas_stmt;

/* Stable lowercase names for diagnostics/tests. Total over out-of-range input. */
const char *qas_operand_kind_name(qas_operand_kind kind);
const char *qas_stmt_kind_name(qas_stmt_kind kind);

#endif /* QAS_AST_AST_H */
