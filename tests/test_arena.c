/*
 * Tests for the arena allocator: alignment is honored, distinct allocations do
 * not overlap, requests larger than a block get their own block, small block
 * sizes force multiple blocks, and strdup_span copies and terminates correctly.
 */
#include "qtest.h"

#include <stdint.h>

#include "arena/arena.h"

/* True if `p` is aligned to `align`. */
static int aligned(const void *p, size_t align)
{
    return ((uintptr_t)p & (uintptr_t)(align - 1)) == 0;
}

static void test_alignment(void)
{
    qas_arena a;
    qas_arena_init(&a);

    /* Interleave odd sizes and increasing alignments; each result must satisfy
       its requested alignment. */
    for (size_t align = 1; align <= 64; align *= 2) {
        void *p = qas_arena_alloc(&a, 1, align);
        QTEST_CHECK_TRUE(p != NULL);
        QTEST_CHECK_TRUE(aligned(p, align));
        /* A 1-byte odd allocation between to perturb the offset. */
        (void)qas_arena_alloc(&a, 1, 1);
    }

    qas_arena_dispose(&a);
}

static void test_no_overlap(void)
{
    qas_arena a;
    qas_arena_init(&a);

    /* Allocate several regions, fill each with a distinct byte, then verify none
       was clobbered by a later allocation. */
    enum { N = 16, SZ = 40 };
    unsigned char *regions[N];
    for (int i = 0; i < N; ++i) {
        regions[i] = (unsigned char *)qas_arena_alloc(&a, SZ, 8);
        QTEST_CHECK_TRUE(regions[i] != NULL);
        for (int j = 0; j < SZ; ++j) {
            regions[i][j] = (unsigned char)(i + 1);
        }
    }
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < SZ; ++j) {
            QTEST_CHECK_EQ_UINT(regions[i][j], (unsigned char)(i + 1), "intact");
        }
    }

    qas_arena_dispose(&a);
}

static void test_large_allocation(void)
{
    /* Force a small default block, then request more than a block: it must
       succeed in its own block. */
    qas_arena a;
    qas_arena_init_sized(&a, 1); /* clamped up to the minimum block size */

    void *big = qas_arena_alloc(&a, 100000, 16);
    QTEST_CHECK_TRUE(big != NULL);
    QTEST_CHECK_TRUE(aligned(big, 16));
    QTEST_CHECK_TRUE(a.total_bytes >= 100000u);

    qas_arena_dispose(&a);
}

static void test_many_blocks(void)
{
    /* A tiny default size makes many allocations spill into many blocks; all
       must remain valid. */
    qas_arena a;
    qas_arena_init_sized(&a, 64);

    enum { N = 500 };
    int *ptrs[N];
    for (int i = 0; i < N; ++i) {
        ptrs[i] = (int *)qas_arena_alloc(&a, sizeof(int), _Alignof(int));
        QTEST_CHECK_TRUE(ptrs[i] != NULL);
        *ptrs[i] = i * 7;
    }
    for (int i = 0; i < N; ++i) {
        QTEST_CHECK_EQ_INT(*ptrs[i], i * 7, "value survived");
    }

    qas_arena_dispose(&a);
}

static void test_strdup_span(void)
{
    qas_arena a;
    qas_arena_init(&a);

    const char *src = "rax, rbx";
    char       *r = qas_arena_strdup_span(&a, src, 3); /* "rax" */
    QTEST_CHECK_TRUE(r != NULL);
    QTEST_CHECK_SPAN(r, strlen(r), "rax", "strdup span");
    QTEST_CHECK_EQ_UINT(r[3], 0u, "nul terminated");

    char *empty = qas_arena_strdup_span(&a, src, 0);
    QTEST_CHECK_TRUE(empty != NULL && empty[0] == '\0');

    qas_arena_dispose(&a);
}

static void test_invalid_args(void)
{
    qas_arena a;
    qas_arena_init(&a);
    QTEST_CHECK_TRUE(qas_arena_alloc(&a, 8, 3) == NULL);  /* align not power of 2 */
    QTEST_CHECK_TRUE(qas_arena_alloc(NULL, 8, 8) == NULL);
    /* Array overflow is rejected. */
    QTEST_CHECK_TRUE(qas_arena_alloc_array(&a, SIZE_MAX, 2, 8) == NULL);
    /* A zero-size allocation is still a valid, aligned pointer. */
    QTEST_CHECK_TRUE(qas_arena_alloc(&a, 0, 8) != NULL);
    qas_arena_dispose(&a);
}

int main(void)
{
    test_alignment();
    test_no_overlap();
    test_large_allocation();
    test_many_blocks();
    test_strdup_span();
    test_invalid_args();
    return qtest_report("arena");
}
