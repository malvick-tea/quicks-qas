/*
 * qas — assembler driver: the assembly unit (implementation).
 *
 * These operations are intentionally small and total; the interesting policy
 * (which directive maps to which section attributes, how a fixup is resolved) is
 * elsewhere. Here we only keep the current-section bookkeeping correct and route
 * emissions to either the byte buffer (PROGBITS) or the logical size (NOBITS).
 */
#include "asm/internal/unit.h"

#include <string.h>

/* Section attributes for the initial .text: allocated, executable instructions
   (System V gABI: SHF_ALLOC | SHF_EXECINSTR; SHT_PROGBITS holds program code). */
static const char  K_TEXT_NAME[] = ".text";
#define K_TEXT_FLAGS  (SHF_ALLOC | SHF_EXECINSTR)
#define K_TEXT_ALIGN  1u

qas_status qas_asm_unit_init(qas_asm_unit *unit, const qas_source *src,
                             qas_diag_sink *diags)
{
    if (unit == NULL || src == NULL || diags == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    unit->src     = src;
    unit->diags   = diags;
    unit->current = 0;
    qas_asm_section_set_init(&unit->sections);
    qas_asm_symtab_init(&unit->syms);
    qas_asm_fix_list_init(&unit->fixes);

    /* Assembly starts in .text (GNU as default location counter). */
    uint32_t text_index;
    qas_status st = qas_asm_section_set_add(&unit->sections, K_TEXT_NAME,
                                            SHT_PROGBITS, K_TEXT_FLAGS,
                                            K_TEXT_ALIGN, &text_index);
    if (st != QAS_OK) {
        qas_asm_unit_dispose(unit);
        return st;
    }
    unit->current = text_index;
    return QAS_OK;
}

void qas_asm_unit_dispose(qas_asm_unit *unit)
{
    if (unit == NULL) {
        return;
    }
    qas_asm_fix_list_dispose(&unit->fixes);
    qas_asm_symtab_dispose(&unit->syms);
    qas_asm_section_set_dispose(&unit->sections);
    unit->src     = NULL;
    unit->diags   = NULL;
    unit->current = 0;
}

qas_asm_section *qas_asm_unit_current(qas_asm_unit *unit)
{
    return qas_asm_section_set_at(&unit->sections, unit->current);
}

qas_status qas_asm_unit_select_section(qas_asm_unit *unit, const char *name,
                                       Elf64_Word type, Elf64_Xword flags,
                                       Elf64_Xword addralign)
{
    uint32_t index;
    if (qas_asm_section_set_find(&unit->sections, name, &index)) {
        unit->current = index; /* Existing section keeps its attributes. */
        return QAS_OK;
    }
    qas_status st =
        qas_asm_section_set_add(&unit->sections, name, type, flags, addralign, &index);
    if (st != QAS_OK) {
        return st;
    }
    unit->current = index;
    return QAS_OK;
}

uint64_t qas_asm_unit_here(qas_asm_unit *unit)
{
    return qas_asm_section_size(qas_asm_unit_current(unit));
}

qas_status qas_asm_unit_emit_bytes(qas_asm_unit *unit, const void *bytes, size_t len)
{
    qas_asm_section *sec = qas_asm_unit_current(unit);
    if (sec == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (sec->type == SHT_NOBITS) {
        /* A NOBITS section holds no file bytes; emitting content into it is a
           caller error that should already have produced a diagnostic. */
        return QAS_ERR_INVALID_ARGUMENT;
    }
    return qas_buf_append(&sec->data, bytes, len);
}

qas_status qas_asm_unit_emit_zeros(qas_asm_unit *unit, uint64_t count)
{
    qas_asm_section *sec = qas_asm_unit_current(unit);
    if (sec == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (sec->type == SHT_NOBITS) {
        sec->bss_size += count; /* No file bytes; just grow the logical size. */
        return QAS_OK;
    }
    return qas_buf_append_zeros(&sec->data, (size_t)count);
}

qas_status qas_asm_unit_align(qas_asm_unit *unit, uint64_t alignment)
{
    if (alignment <= 1u) {
        return QAS_OK;
    }
    qas_asm_section *sec = qas_asm_unit_current(unit);
    if (sec == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    uint64_t cur = qas_asm_section_size(sec);
    uint64_t rem = cur % alignment;
    if (rem != 0u) {
        qas_status st = qas_asm_unit_emit_zeros(unit, alignment - rem);
        if (st != QAS_OK) {
            return st;
        }
    }
    /* The section must be at least this aligned in the final image, so the linker
       does not undo the padding (gABI sh_addralign). */
    if (alignment > sec->addralign) {
        sec->addralign = alignment;
    }
    return QAS_OK;
}

qas_status qas_asm_unit_define_label(qas_asm_unit *unit, size_t name_off,
                                     size_t name_len)
{
    uint32_t index;
    qas_status st = qas_asm_symtab_intern(&unit->syms, unit->src->data + name_off,
                                          name_len, &index);
    if (st != QAS_OK) {
        return st;
    }
    qas_asm_sym *sym = qas_asm_symtab_at(&unit->syms, index);
    if (sym->defined) {
        return qas_diag_emit(unit->diags, QAS_DIAG_ERROR, unit->src, name_off,
                             name_len, "symbol '%.*s' is already defined",
                             (int)name_len, unit->src->data + name_off);
    }
    sym->defined = true;
    sym->where   = QAS_ASM_SYM_SECTION;
    sym->section = unit->current;
    sym->value   = qas_asm_unit_here(unit);
    return QAS_OK;
}

qas_status qas_asm_unit_add_fix(qas_asm_unit *unit, qas_asm_fix_kind kind,
                                size_t sym_off, size_t sym_len, int64_t addend,
                                uint64_t field_offset)
{
    qas_asm_fix fix;
    fix.out_section = unit->current;
    fix.offset      = field_offset;
    fix.kind        = kind;
    fix.sym_off     = sym_off;
    fix.sym_len     = sym_len;
    fix.addend      = addend;
    return qas_asm_fix_list_push(&unit->fixes, &fix);
}
