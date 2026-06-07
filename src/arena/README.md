# module: arena

**Responsibility:** a region (bump) allocator that hands out many small,
variable-sized allocations from pooled blocks and frees them all at once. The
parser's AST nodes share one lifetime (a single assembly unit), which is exactly
what an arena is for: allocation is a pointer bump and teardown frees a few
blocks instead of thousands of objects (coding-standard §5).

**Public interface:** `arena/arena.h` (`qas_arena`, `qas_arena_init`,
`qas_arena_init_sized`, `qas_arena_dispose`, `qas_arena_alloc`,
`qas_arena_alloc_array`, `qas_arena_strdup_span`).

**Design notes:**
- Singly linked list of blocks; the head is the current bump target. A request
  larger than the default block size gets its own exactly-sized block.
- Alignment is computed on the object's absolute address, so any power-of-two
  alignment up to the platform maximum is honored regardless of block layout.
- Size/alignment arithmetic is overflow-checked; bad requests return NULL rather
  than wrapping.
- No per-object free by design — that is the trade that buys O(1) allocation and
  leak-proof bulk teardown. Data whose lifetime differs uses malloc/free or `buf`.

**Dependencies:** none (beyond the C standard headers).
