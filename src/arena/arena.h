/*
 * qas — arena (region) allocator
 *
 * Responsibility
 * Hand out small, variable-sized allocations from large pooled blocks and free
 * them all at once. The parser builds an abstract syntax tree of many small nodes
 * with the same lifetime (one assembly unit); an arena is the right tool for that
 * — allocation is a pointer bump, and teardown is freeing a handful of blocks
 * rather than chasing thousands of individual frees. The coding standard calls
 * out arena/region allocation as the preferred strategy for compiler/assembler
 * data (coding-standard §5, error-handling.md "resource safety").
 *
 * What it is NOT
 *   There is no per-allocation free: an arena frees everything together at
 *   dispose. That is the point — it trades the ability to free one object for
 *   speed and leak-proof bulk teardown. Use it for data whose lifetime matches
 *   the arena's; use malloc/free (or the buf module) for anything else.
 *
 * Standard: ISO/IEC 9899 (C11), portable subset; uses malloc/free as a seed
 * dependency until qlibc (ADR-0009). Alignment is computed on the object's
 * absolute address, so any alignment up to the platform maximum is honored.
 */
#ifndef QAS_ARENA_ARENA_H
#define QAS_ARENA_ARENA_H

#include <stddef.h>

/* A block of pooled storage. Opaque; defined in arena.c. */
struct qas_arena_block;

/*
 * An arena. Treat the fields as private. A zero-initialized arena is valid-empty
 * (the first allocation creates the first block), but call qas_arena_init for
 * clarity. `default_block_size` is the size of blocks allocated for ordinary
 * requests; a request larger than it gets its own exactly-sized block.
 */
typedef struct qas_arena {
    struct qas_arena_block *head;              /* Most-recent block (bump target).*/
    size_t                  default_block_size;
    size_t                  total_bytes;       /* Total block storage allocated.  */
} qas_arena;

/*
 * Initialize an empty arena with a sensible default block size. Always succeeds;
 * no memory is allocated until the first qas_arena_alloc.
 */
void qas_arena_init(qas_arena *arena);

/*
 * Initialize with an explicit default block size (rounded up to a small minimum).
 * Useful for tests that want to force many blocks. Always succeeds.
 */
void qas_arena_init_sized(qas_arena *arena, size_t default_block_size);

/* Free every block. The arena becomes empty and may be reused. NULL is a no-op. */
void qas_arena_dispose(qas_arena *arena);

/*
 * Allocate `size` bytes aligned to `align` (which must be a power of two). The
 * memory is uninitialized and valid until qas_arena_dispose. Returns NULL on
 * allocation failure or on an invalid argument (NULL arena, non-power-of-two
 * align, or a size/align combination that would overflow). A `size` of 0 returns
 * a valid, suitably aligned non-NULL pointer (to zero usable bytes).
 */
void *qas_arena_alloc(qas_arena *arena, size_t size, size_t align);

/*
 * Allocate an array of `count` elements of `elem_size` bytes, aligned to `align`,
 * with an overflow-checked total size. Returns NULL on overflow or failure.
 */
void *qas_arena_alloc_array(qas_arena *arena, size_t count, size_t elem_size,
                            size_t align);

/*
 * Copy the byte span [text, text+length) into the arena and NUL-terminate it,
 * returning the new string (alignment 1). Returns NULL on failure. Handy for
 * turning a source-token span into an owned, NUL-terminated name.
 */
char *qas_arena_strdup_span(qas_arena *arena, const char *text, size_t length);

#endif /* QAS_ARENA_ARENA_H */
