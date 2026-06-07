/*
 * qas — arena allocator: implementation.
 *
 * A singly linked list of blocks; the head is the current bump target. Each
 * allocation aligns the next free address within the head block and bumps the
 * used count; if it does not fit, a fresh block is pushed (its own exactly-sized
 * block if the request exceeds the default). Alignment is computed on the
 * absolute address (uintptr_t), so it is correct regardless of where malloc
 * placed the block or how the block header is laid out.
 */
#include "arena/arena.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Smallest default block we will use, so a tiny init size still pools usefully. */
#define QAS_ARENA_MIN_BLOCK 4096u
/* Default when the caller does not specify (64 KiB: large enough that ordinary
   ASTs need very few blocks). */
#define QAS_ARENA_DEFAULT_BLOCK (64u * 1024u)

/*
 * A block header. The usable storage immediately follows the header in the same
 * allocation; we reach it with block_data() rather than a C99 flexible array
 * member, because MSVC reports a trailing `data[]` as a nonstandard extension
 * (C4200) and we build warnings-as-errors. Pointer arithmetic over a single
 * malloc is equivalent and fully portable.
 */
typedef struct qas_arena_block {
    struct qas_arena_block *next;
    size_t                  capacity; /* Usable bytes after the header.           */
    size_t                  used;     /* Bytes handed out so far.                 */
} qas_arena_block;

/* Start of the block's usable storage (just past the header). */
static unsigned char *block_data(qas_arena_block *block)
{
    return (unsigned char *)(block + 1);
}

void qas_arena_init_sized(qas_arena *arena, size_t default_block_size)
{
    if (arena == NULL) {
        return;
    }
    arena->head        = NULL;
    arena->total_bytes = 0;
    arena->default_block_size = (default_block_size < QAS_ARENA_MIN_BLOCK)
                                    ? QAS_ARENA_MIN_BLOCK
                                    : default_block_size;
}

void qas_arena_init(qas_arena *arena)
{
    qas_arena_init_sized(arena, QAS_ARENA_DEFAULT_BLOCK);
}

void qas_arena_dispose(qas_arena *arena)
{
    if (arena == NULL) {
        return;
    }
    qas_arena_block *block = arena->head;
    while (block != NULL) {
        qas_arena_block *next = block->next;
        free(block);
        block = next;
    }
    arena->head        = NULL;
    arena->total_bytes = 0;
    /* Keep default_block_size so a disposed arena can be reused as-configured. */
}

/* True iff `x` is a power of two (and nonzero) — the requirement for `align`. */
static bool is_power_of_two(size_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

/* Round `value` up to the next multiple of `align` (a power of two). Returns
   false on overflow. */
static bool align_up(uintptr_t value, size_t align, uintptr_t *out)
{
    uintptr_t mask = (uintptr_t)align - 1u;
    if (value > UINTPTR_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~mask;
    return true;
}

/* Try to carve `size` aligned bytes out of an existing block. Returns the
   pointer, or NULL if it does not fit. */
static void *carve(qas_arena_block *block, size_t size, size_t align)
{
    unsigned char *data = block_data(block);
    uintptr_t start   = (uintptr_t)data + block->used;
    uintptr_t aligned;
    if (!align_up(start, align, &aligned)) {
        return NULL;
    }
    size_t pad = (size_t)(aligned - (uintptr_t)data) - block->used;
    if (pad > block->capacity - block->used) {
        return NULL; /* Even the padding does not fit. */
    }
    if (size > block->capacity - block->used - pad) {
        return NULL;
    }
    block->used += pad + size;
    return (void *)aligned;
}

void *qas_arena_alloc(qas_arena *arena, size_t size, size_t align)
{
    if (arena == NULL || !is_power_of_two(align)) {
        return NULL;
    }

    if (arena->head != NULL) {
        void *p = carve(arena->head, size, align);
        if (p != NULL) {
            return p;
        }
    }

    /* Need a new block. Size it to hold this request even in the worst case
       (size + align-1 of padding), but at least the default block size. */
    size_t worst = size;
    if (worst > SIZE_MAX - (align - 1u)) {
        return NULL; /* size + padding would overflow. */
    }
    worst += align - 1u;
    size_t block_size = (worst > arena->default_block_size) ? worst
                                                            : arena->default_block_size;
    if (block_size > SIZE_MAX - sizeof(qas_arena_block)) {
        return NULL;
    }

    qas_arena_block *block =
        (qas_arena_block *)malloc(sizeof(qas_arena_block) + block_size);
    if (block == NULL) {
        return NULL;
    }
    block->capacity = block_size;
    block->used     = 0;
    block->next     = arena->head;
    arena->head     = block;
    arena->total_bytes += block_size;

    return carve(block, size, align); /* Cannot fail: block sized for it. */
}

void *qas_arena_alloc_array(qas_arena *arena, size_t count, size_t elem_size,
                            size_t align)
{
    if (elem_size != 0 && count > SIZE_MAX / elem_size) {
        return NULL; /* count * elem_size would overflow. */
    }
    return qas_arena_alloc(arena, count * elem_size, align);
}

char *qas_arena_strdup_span(qas_arena *arena, const char *text, size_t length)
{
    if (text == NULL && length != 0) {
        return NULL;
    }
    char *copy = (char *)qas_arena_alloc(arena, length + 1u, 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (length != 0) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    return copy;
}
