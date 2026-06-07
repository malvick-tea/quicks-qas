/*
 * qas — ELF writer: the builder (mutable accumulation).
 *
 * This file owns the *growing* side of the module: adding sections, appending
 * code/data bytes, recording symbols and relocations, and releasing it all. The
 * one-shot translation of the accumulated model into ELF bytes lives in
 * elf/internal/serialize.c, keeping each file to a single responsibility and
 * within a readable length (coding-standard §2).
 */
#include "elf/elf.h"

#include <stdlib.h>
#include <string.h>

#include "elf/internal/model.h"
#include "elf/internal/serialize.h"

/* Duplicate a NUL-terminated string into a fresh allocation. A local helper
   because strdup is POSIX, not ISO C11 (ADR-0006 forbids non-standard
   extensions in host tools). A NULL input duplicates the empty string. */
static char *dup_string(const char *s)
{
    if (s == NULL) {
        s = "";
    }
    size_t n = strlen(s) + 1;
    char  *copy = (char *)malloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

/* Geometric growth of a typed dynamic array. Returns QAS_OK or OOM, leaving the
   array untouched on failure. `*items` and `*cap` are updated on success. */
static qas_status grow_array(void **items, size_t *cap, size_t count,
                             size_t elem_size)
{
    if (count < *cap) {
        return QAS_OK;
    }
    size_t new_cap = (*cap == 0) ? 8u : *cap * 2u;
    void  *grown = realloc(*items, new_cap * elem_size);
    if (grown == NULL) {
        return QAS_ERR_OUT_OF_MEMORY;
    }
    *items = grown;
    *cap   = new_cap;
    return QAS_OK;
}

void qas_elf_builder_init(qas_elf_builder *builder)
{
    if (builder == NULL) {
        return;
    }
    builder->sections      = NULL;
    builder->section_count = 0;
    builder->section_cap   = 0;
    builder->symbols       = NULL;
    builder->symbol_count  = 0;
    builder->symbol_cap    = 0;
}

void qas_elf_builder_dispose(qas_elf_builder *builder)
{
    if (builder == NULL) {
        return;
    }
    for (size_t i = 0; i < builder->section_count; ++i) {
        qas_elf_section *s = &builder->sections[i];
        free(s->name);
        qas_buf_dispose(&s->data);
        free(s->relocs);
    }
    free(builder->sections);
    for (size_t i = 0; i < builder->symbol_count; ++i) {
        free(builder->symbols[i].name);
    }
    free(builder->symbols);
    qas_elf_builder_init(builder);
}

/* Resolve a section handle to its record, or NULL if out of range. */
static qas_elf_section *section_at(qas_elf_builder *builder, uint32_t handle)
{
    if (builder == NULL || handle >= builder->section_count) {
        return NULL;
    }
    return &builder->sections[handle];
}

qas_status qas_elf_builder_add_section(qas_elf_builder *builder, const char *name,
                                       Elf64_Word type, Elf64_Xword flags,
                                       Elf64_Xword addralign, uint32_t *out_section)
{
    if (builder == NULL || name == NULL || out_section == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    /* We only emit program-bytes and no-bytes sections as caller-defined output
       sections; the string/symbol/rela sections are synthesized at finish. */
    if (type != SHT_PROGBITS && type != SHT_NOBITS) {
        return QAS_ERR_INVALID_ARGUMENT;
    }

    qas_status st = grow_array((void **)&builder->sections, &builder->section_cap,
                               builder->section_count, sizeof(qas_elf_section));
    if (st != QAS_OK) {
        return st;
    }

    char *name_copy = dup_string(name);
    if (name_copy == NULL) {
        return QAS_ERR_OUT_OF_MEMORY;
    }

    qas_elf_section *s = &builder->sections[builder->section_count];
    s->name       = name_copy;
    s->type       = type;
    s->flags      = flags;
    s->addralign  = (addralign == 0) ? 1u : addralign;
    qas_buf_init(&s->data);
    s->bss_size   = 0;
    s->relocs     = NULL;
    s->reloc_count = 0;
    s->reloc_cap  = 0;

    *out_section = (uint32_t)builder->section_count;
    builder->section_count += 1;
    return QAS_OK;
}

qas_status qas_elf_builder_append(qas_elf_builder *builder, uint32_t section,
                                  const void *bytes, size_t len, uint64_t *out_offset)
{
    qas_elf_section *s = section_at(builder, section);
    if (s == NULL || (bytes == NULL && len != 0)) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (s->type != SHT_PROGBITS) {
        return QAS_ERR_INVALID_ARGUMENT; /* NOBITS holds no bytes; use reserve_bss. */
    }
    if (out_offset != NULL) {
        *out_offset = s->data.len;
    }
    return qas_buf_append(&s->data, bytes, len);
}

qas_status qas_elf_builder_reserve_bss(qas_elf_builder *builder, uint32_t section,
                                       uint64_t bytes, uint64_t *out_offset)
{
    qas_elf_section *s = section_at(builder, section);
    if (s == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (s->type != SHT_NOBITS) {
        return QAS_ERR_INVALID_ARGUMENT; /* Only .bss-style sections reserve space. */
    }
    if (out_offset != NULL) {
        *out_offset = s->bss_size;
    }
    s->bss_size += bytes;
    return QAS_OK;
}

uint64_t qas_elf_builder_section_size(const qas_elf_builder *builder, uint32_t section)
{
    if (builder == NULL || section >= builder->section_count) {
        return 0;
    }
    const qas_elf_section *s = &builder->sections[section];
    return (s->type == SHT_NOBITS) ? s->bss_size : (uint64_t)s->data.len;
}

qas_status qas_elf_builder_add_symbol(qas_elf_builder *builder, const char *name,
                                      unsigned char bind, unsigned char type,
                                      qas_elf_symref ref, uint32_t section,
                                      uint64_t value, uint64_t size,
                                      uint32_t *out_symbol)
{
    if (builder == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (ref == QAS_ELF_SYMREF_SECTION && section >= builder->section_count) {
        return QAS_ERR_INVALID_ARGUMENT;
    }

    qas_status st = grow_array((void **)&builder->symbols, &builder->symbol_cap,
                               builder->symbol_count, sizeof(qas_elf_symbol));
    if (st != QAS_OK) {
        return st;
    }

    char *name_copy = dup_string(name);
    if (name_copy == NULL) {
        return QAS_ERR_OUT_OF_MEMORY;
    }

    qas_elf_symbol *sym = &builder->symbols[builder->symbol_count];
    sym->name        = name_copy;
    sym->bind        = bind;
    sym->type        = type;
    sym->ref         = ref;
    sym->section     = section;
    sym->value       = value;
    sym->size        = size;

    if (out_symbol != NULL) {
        *out_symbol = (uint32_t)builder->symbol_count;
    }
    builder->symbol_count += 1;
    return QAS_OK;
}

qas_status qas_elf_builder_add_rela(qas_elf_builder *builder, uint32_t section,
                                    uint64_t offset, uint32_t symbol,
                                    Elf64_Word reloc_type, Elf64_Sxword addend)
{
    qas_elf_section *s = section_at(builder, section);
    if (s == NULL || symbol >= builder->symbol_count) {
        return QAS_ERR_INVALID_ARGUMENT;
    }

    qas_status st = grow_array((void **)&s->relocs, &s->reloc_cap, s->reloc_count,
                               sizeof(qas_elf_reloc));
    if (st != QAS_OK) {
        return st;
    }

    qas_elf_reloc *r = &s->relocs[s->reloc_count];
    r->offset = offset;
    r->symbol = symbol;
    r->type   = reloc_type;
    r->addend = addend;
    s->reloc_count += 1;
    return QAS_OK;
}

qas_status qas_elf_builder_finish(qas_elf_builder *builder, uint8_t **out_image,
                                  size_t *out_size)
{
    if (builder == NULL || out_image == NULL || out_size == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    return qas_elf_serialize(builder, out_image, out_size);
}
