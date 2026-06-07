# module: buf

**Responsibility:** a growable byte buffer with append, little-endian fixed-width
integer writers, in-place 32-bit patching, and ownership transfer. Shared by the
encoder (building a section's machine code) and the ELF writer (assembling the
object-file image) so neither reinvents a resizable buffer.

**Public interface:** `buf/buf.h` (`qas_buf`, `qas_buf_init`, `qas_buf_dispose`,
`qas_buf_reserve`, `qas_buf_append`, `qas_buf_append_u8`, `qas_buf_append_zeros`,
`qas_buf_append_u16le/u32le/u64le`, `qas_buf_append_i64le`, `qas_buf_patch_u32le`,
`qas_buf_take`).

**Design notes:**
- Little-endian writers emit one byte at a time, so output is independent of the
  host's byte order and of struct padding — the encoded bytes are defined by us,
  not the compiler (coding-standard §6). x86-64 / ELF64 are little-endian.
- Geometric growth for amortized O(1) appends; overflow-checked so a pathological
  size request fails cleanly instead of wrapping.
- `qas_buf_patch_u32le` back-patches an already-emitted 32-bit displacement once a
  forward label's address is known.
- `qas_buf_take` hands the bytes to the caller and empties the buffer, so the ELF
  writer can return the finished image without a copy.

**Dependencies:** `status`.
