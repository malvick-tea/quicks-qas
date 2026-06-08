/*
 * qas — assembler driver: symbol table (implementation).
 *
 * Open-addressing hash with linear probing. The bucket array stores 1-based
 * indices into the `syms` record array (0 = empty), so growing `syms` never
 * invalidates the buckets, and growing the buckets is a pure rehash that does not
 * touch the records. Load factor is held below 7/8 to keep probe chains short.
 */
#include "asm/internal/symtab.h"

#include <stdlib.h>
#include <string.h>

/*
 * FNV-1a, 64-bit (public-domain hash; not a project spec — a house choice, see
 * the header). offset basis and prime are the canonical 64-bit FNV parameters.
 */
static uint64_t fnv1a(const char *bytes, size_t len)
{
    uint64_t h = 1469598103934665603ULL; /* FNV offset basis (64-bit). */
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)(unsigned char)bytes[i];
        h *= 1099511628211ULL;           /* FNV prime (64-bit). */
    }
    return h;
}

/* True if record `s` has exactly the name [name, name+len). */
static bool name_eq(const qas_asm_sym *s, const char *name, size_t len)
{
    return strlen(s->name) == len && memcmp(s->name, name, len) == 0;
}

void qas_asm_symtab_init(qas_asm_symtab *table)
{
    if (table == NULL) {
        return;
    }
    table->syms         = NULL;
    table->count        = 0;
    table->capacity     = 0;
    table->buckets      = NULL;
    table->bucket_count = 0;
}

void qas_asm_symtab_dispose(qas_asm_symtab *table)
{
    if (table == NULL) {
        return;
    }
    for (size_t i = 0; i < table->count; ++i) {
        free(table->syms[i].name);
    }
    free(table->syms);
    free(table->buckets);
    qas_asm_symtab_init(table);
}

qas_asm_sym *qas_asm_symtab_at(qas_asm_symtab *table, uint32_t index)
{
    if (table == NULL || index >= table->count) {
        return NULL;
    }
    return &table->syms[index];
}

/* Insert an already-existing record index into the bucket array (no resize). */
static void bucket_insert(uint32_t *buckets, size_t bucket_count,
                          const qas_asm_sym *syms, uint32_t rec_index)
{
    size_t mask = bucket_count - 1u;
    size_t slot = (size_t)(fnv1a(syms[rec_index].name,
                                 strlen(syms[rec_index].name)) & mask);
    while (buckets[slot] != 0u) {
        slot = (slot + 1u) & mask;
    }
    buckets[slot] = rec_index + 1u; /* store 1-based so 0 stays "empty". */
}

/* Grow (or create) the bucket array to `new_count` (power of two) and rehash. */
static qas_status rehash(qas_asm_symtab *table, size_t new_count)
{
    uint32_t *buckets = (uint32_t *)calloc(new_count, sizeof(uint32_t));
    if (buckets == NULL) {
        return QAS_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < table->count; ++i) {
        bucket_insert(buckets, new_count, table->syms, (uint32_t)i);
    }
    free(table->buckets);
    table->buckets      = buckets;
    table->bucket_count = new_count;
    return QAS_OK;
}

/* Append a new record with a copied name; returns its index via *out_index. */
static qas_status append_record(qas_asm_symtab *table, const char *name,
                                size_t len, uint32_t *out_index)
{
    if (table->count >= table->capacity) {
        size_t new_cap = (table->capacity == 0) ? 32u : table->capacity * 2u;
        qas_asm_sym *grown =
            (qas_asm_sym *)realloc(table->syms, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return QAS_ERR_OUT_OF_MEMORY;
        }
        table->syms     = grown;
        table->capacity = new_cap;
    }
    char *name_copy = (char *)malloc(len + 1);
    if (name_copy == NULL) {
        return QAS_ERR_OUT_OF_MEMORY;
    }
    memcpy(name_copy, name, len);
    name_copy[len] = '\0';

    qas_asm_sym *s = &table->syms[table->count];
    s->name       = name_copy;
    s->defined    = false;
    s->where      = QAS_ASM_SYM_UNDEF;
    s->section    = 0;
    s->value      = 0;
    s->is_global  = false;
    s->referenced = false;
    s->emitted    = false;
    s->elf_handle = 0;

    *out_index = (uint32_t)table->count;
    table->count += 1;
    return QAS_OK;
}

qas_status qas_asm_symtab_intern(qas_asm_symtab *table, const char *name,
                                 size_t len, uint32_t *out_index)
{
    if (table == NULL || out_index == NULL || (name == NULL && len != 0)) {
        return QAS_ERR_INVALID_ARGUMENT;
    }

    /* Ensure the bucket array exists and has headroom (keep load factor < 7/8 so
       probe sequences stay short — a standard open-addressing rule of thumb). */
    if (table->bucket_count == 0) {
        qas_status st = rehash(table, 64u);
        if (st != QAS_OK) {
            return st;
        }
    } else if ((table->count + 1u) * 8u >= table->bucket_count * 7u) {
        qas_status st = rehash(table, table->bucket_count * 2u);
        if (st != QAS_OK) {
            return st;
        }
    }

    size_t mask = table->bucket_count - 1u;
    size_t slot = (size_t)(fnv1a(name, len) & mask);
    while (table->buckets[slot] != 0u) {
        uint32_t rec = table->buckets[slot] - 1u;
        if (name_eq(&table->syms[rec], name, len)) {
            *out_index = rec; /* Already interned. */
            return QAS_OK;
        }
        slot = (slot + 1u) & mask;
    }

    /* Not present: create the record, then publish it into the empty slot. */
    uint32_t new_index;
    qas_status st = append_record(table, name, len, &new_index);
    if (st != QAS_OK) {
        return st;
    }
    table->buckets[slot] = new_index + 1u;
    *out_index = new_index;
    return QAS_OK;
}
