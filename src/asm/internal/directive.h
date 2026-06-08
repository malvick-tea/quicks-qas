/*
 * qas — assembler driver: directive handling (internal).
 *
 * Responsibility
 * Apply one parsed `.directive` statement to the assembly unit: switch sections,
 * declare/define symbols, and emit data. This is the policy layer that the
 * deliberately policy-free section/symbol/unit containers do not contain — it
 * decides, for example, that `.text` is allocated+executable and that `.quad` of a
 * symbol becomes an 8-byte absolute relocation.
 *
 * Directive set (a curated subset of the GNU as directive vocabulary, which is the
 * de-facto standard our own kernel/tools source targets):
 *   sections : .text .data .rodata .bss  (and .section "name"[, "flags"])
 *   symbols  : .globl/.global   .set/.equ
 *   data     : .byte  .word/.short/.value  .long/.int  .quad
 *              .ascii  .asciz/.string  .zero/.skip/.space
 *   align    : .align/.balign   .p2align
 * Unknown directives are reported as errors rather than ignored, because for our
 * own toolchain a stray directive is a bug, not noise to tolerate.
 *
 * Private to the asm module (ADR-0008).
 */
#ifndef QAS_ASM_INTERNAL_DIRECTIVE_H
#define QAS_ASM_INTERNAL_DIRECTIVE_H

#include "asm/internal/unit.h"
#include "ast/ast.h"
#include "status/status.h"

/*
 * Apply the directive statement `st` (st->kind == QAS_STMT_DIRECTIVE) to `unit`.
 * User errors (bad arguments, unknown directive, out-of-range data, emitting into
 * .bss) are reported as diagnostics and do not stop assembly; the return status is
 * QAS_OK in that case. A non-OK return is a fatal resource failure (OOM) that
 * should abort the run.
 */
qas_status qas_asm_apply_directive(qas_asm_unit *unit, const qas_stmt *st);

#endif /* QAS_ASM_INTERNAL_DIRECTIVE_H */
