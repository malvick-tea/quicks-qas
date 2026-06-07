/*
 * qas — growable byte buffer
 *
 * Responsibility
 * A small dynamic array of bytes with append and little-endian integer writers,
 * plus in-place patching of an already-written 32-bit field. It is the shared
 * substrate for code generation (the encoder builds a section's bytes here) and
 * object emission (the ELF writer assembles the file image here), so the two do
 * not each reinvent a resizable buffer.
 *
 * Why little-endian writers (not memcpy of a struct)
 *   ELF64 for x86-64 is little-endian (EI_DATA = ELFDATA2LSB), and instruction
 *   immediates/displacements are little-endian (Intel SDM Vol 1 §1.3.* on byte
 *   order). Writing each field byte-by-byte in explicit little-endian order makes
 *   the output independent of the *host's* endianness and free of struct-padding
 *   surprises — the encoded bytes are defined by us, not by the compiler's layout.
 *   This honors coding-standard §6 ("never assume int width / layout").
 *
 * Standard: ISO/IEC 9899 (C11), portable subset; uses the seed allocator
 * (realloc/free) as permitted for host tools until qlibc (ADR-0009).
 */
#ifndef QAS_BUF_BUF_H
#define QAS_BUF_BUF_H

#include <stddef.h>
#include <stdint.h>

#include "status/status.h"

/*
 * A growable byte buffer. `data` holds `len` valid bytes within `cap` allocated
 * bytes. Treat the fields as read-mostly: read `data`/`len` freely, but mutate
 * only through the functions below. A zero-initialized struct (all NULL/0) is a
 * valid empty buffer, so qas_buf_init is for clarity/reuse, not correctness.
 *
 * Ownership: the buffer owns `data`; release it with qas_buf_dispose. To hand the
 * bytes off to a caller, use qas_buf_take (which transfers ownership and empties
 * the buffer).
 */
typedef struct qas_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} qas_buf;

/* Initialize an empty buffer. Always succeeds. */
void qas_buf_init(qas_buf *buf);

/* Free the backing storage and reset to empty. Safe on a zeroed/empty buffer. */
void qas_buf_dispose(qas_buf *buf);

/*
 * Ensure capacity for at least `additional` more bytes (amortized O(1) growth).
 * Returns QAS_OK or QAS_ERR_OUT_OF_MEMORY (leaving the buffer unchanged on
 * failure). Rarely needed directly; the append functions call it.
 */
qas_status qas_buf_reserve(qas_buf *buf, size_t additional);

/* Append `len` raw bytes from `bytes` (which may be NULL iff len == 0). */
qas_status qas_buf_append(qas_buf *buf, const void *bytes, size_t len);

/* Append a single byte. */
qas_status qas_buf_append_u8(qas_buf *buf, uint8_t value);

/* Append `count` zero bytes (e.g. padding to an alignment, or a .skip). */
qas_status qas_buf_append_zeros(qas_buf *buf, size_t count);

/* Append a 16/32/64-bit value in little-endian byte order. */
qas_status qas_buf_append_u16le(qas_buf *buf, uint16_t value);
qas_status qas_buf_append_u32le(qas_buf *buf, uint32_t value);
qas_status qas_buf_append_u64le(qas_buf *buf, uint64_t value);

/*
 * Append a signed 64-bit value in little-endian two's-complement order. Provided
 * for ELF r_addend (Elf64_Rela.r_addend is a signed 64-bit field).
 */
qas_status qas_buf_append_i64le(qas_buf *buf, int64_t value);

/*
 * Overwrite the 4 bytes at [offset, offset+4) with `value` in little-endian
 * order. Used to back-patch a 32-bit displacement once a forward label's address
 * is known. Requires offset + 4 <= len; returns QAS_ERR_INVALID_ARGUMENT
 * otherwise (a programming error — the site must already have been emitted).
 */
qas_status qas_buf_patch_u32le(qas_buf *buf, size_t offset, uint32_t value);

/*
 * Transfer ownership of the bytes to the caller: *out_data receives the buffer
 * (caller frees with free()), *out_len its length, and the qas_buf is left empty
 * (safe to reuse or dispose). Either out pointer may be NULL to discard that
 * datum. Returns QAS_OK, or QAS_ERR_INVALID_ARGUMENT if buf is NULL.
 */
qas_status qas_buf_take(qas_buf *buf, uint8_t **out_data, size_t *out_len);

#endif /* QAS_BUF_BUF_H */
