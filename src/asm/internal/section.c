/*
 * qas — assembler driver: output sections (implementation).
 *
 * A dynamic array of sections plus a linear name lookup. Linear search is the
 * right complexity here: a real assembly unit has a handful of sections (.text,
 * .data, .rodata, .bss and maybe a few named ones), so a hash table would be more
 * code than the problem warrants — unlike the symbol table, which can hold
 * thousands of entries and therefore *does* get a hash (see symtab.c).
 */
#include "asm/internal/section.h"

#include <stdlib.h>
#include <string.h>

void qas_asm_section_set_init(qas_asm_section_set *set)
{
    if (set == NULL) {
        return;
    }
    set->items    = NULL;
    set->count    = 0;
    set->capacity = 0;
}

void qas_asm_section_set_dispose(qas_asm_section_set *set)
{
    if (set == NULL) {
        return;
    }
    for (size_t i = 0; i < set->count; ++i) {
        free(set->items[i].name);
        qas_buf_dispose(&set->items[i].data);
    }
    free(set->items);
    qas_asm_section_set_init(set);
}

bool qas_asm_section_set_find(const qas_asm_section_set *set, const char *name,
                              uint32_t *out_index)
{
    if (set == NULL || name == NULL || out_index == NULL) {
        return false;
    }
    for (size_t i = 0; i < set->count; ++i) {
        if (strcmp(set->items[i].name, name) == 0) {
            *out_index = (uint32_t)i;
            return true;
        }
    }
    return false;
}

qas_status qas_asm_section_set_add(qas_asm_section_set *set, const char *name,
                                   Elf64_Word type, Elf64_Xword flags,
                                   Elf64_Xword addralign, uint32_t *out_index)
{
    if (set == NULL || name == NULL || out_index == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (set->count >= set->capacity) {
        size_t new_cap = (set->capacity == 0) ? 8u : set->capacity * 2u;
        qas_asm_section *grown =
            (qas_asm_section *)realloc(set->items, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return QAS_ERR_OUT_OF_MEMORY;
        }
        set->items    = grown;
        set->capacity = new_cap;
    }

    /* Own a copy of the name so the section outlives the directive that named it. */
    size_t name_len = strlen(name);
    char  *name_copy = (char *)malloc(name_len + 1);
    if (name_copy == NULL) {
        return QAS_ERR_OUT_OF_MEMORY;
    }
    memcpy(name_copy, name, name_len + 1);

    qas_asm_section *sec = &set->items[set->count];
    sec->name      = name_copy;
    sec->type      = type;
    sec->flags     = flags;
    sec->addralign = (addralign == 0) ? 1u : addralign; /* sh_addralign 0/1 = none */
    qas_buf_init(&sec->data);
    sec->bss_size  = 0;

    *out_index = (uint32_t)set->count;
    set->count += 1;
    return QAS_OK;
}

qas_asm_section *qas_asm_section_set_at(qas_asm_section_set *set, uint32_t index)
{
    if (set == NULL || index >= set->count) {
        return NULL;
    }
    return &set->items[index];
}

uint64_t qas_asm_section_size(const qas_asm_section *section)
{
    if (section == NULL) {
        return 0;
    }
    return (section->type == SHT_NOBITS) ? section->bss_size
                                         : (uint64_t)section->data.len;
}
