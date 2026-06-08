/*
 * qas — assembler driver: the pending-fixup list (implementation).
 *
 * A textbook geometrically-growing dynamic array. It holds plain-old-data
 * (qas_asm_fix has no owned pointers — the symbol is a span into the source), so
 * dispose simply frees the array; there is nothing per-element to release.
 */
#include "asm/internal/fixups.h"

#include <stdlib.h>

void qas_asm_fix_list_init(qas_asm_fix_list *list)
{
    if (list == NULL) {
        return;
    }
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void qas_asm_fix_list_dispose(qas_asm_fix_list *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    qas_asm_fix_list_init(list);
}

qas_status qas_asm_fix_list_push(qas_asm_fix_list *list, const qas_asm_fix *fix)
{
    if (list == NULL || fix == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (list->count >= list->capacity) {
        /* Double the capacity (amortized O(1) append); start at 16 because an
           assembly unit with any symbolic references usually has several. */
        size_t new_cap = (list->capacity == 0) ? 16u : list->capacity * 2u;
        qas_asm_fix *grown =
            (qas_asm_fix *)realloc(list->items, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return QAS_ERR_OUT_OF_MEMORY;
        }
        list->items    = grown;
        list->capacity = new_cap;
    }
    list->items[list->count++] = *fix;
    return QAS_OK;
}
