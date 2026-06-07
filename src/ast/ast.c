/*
 * qas — AST: the kind-name helpers.
 *
 * As elsewhere, each switch lists every enumerator so -Wswitch flags an
 * unhandled case if the enum grows, with a trailing return keeping the function
 * total for out-of-range values.
 */
#include "ast/ast.h"

const char *qas_operand_kind_name(qas_operand_kind kind)
{
    switch (kind) {
    case QAS_OPERAND_NONE: return "none";
    case QAS_OPERAND_REG:  return "register";
    case QAS_OPERAND_IMM:  return "immediate";
    case QAS_OPERAND_MEM:  return "memory";
    }
    return "operand";
}

const char *qas_stmt_kind_name(qas_stmt_kind kind)
{
    switch (kind) {
    case QAS_STMT_LABEL:       return "label";
    case QAS_STMT_DIRECTIVE:   return "directive";
    case QAS_STMT_INSTRUCTION: return "instruction";
    }
    return "statement";
}
