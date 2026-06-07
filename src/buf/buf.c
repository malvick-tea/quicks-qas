/*
 * qas — growable byte buffer: implementation.
 *
 * The little-endian writers shift the value down one byte at a time and append
 * the low 8 bits, so the result is identical on any host regardless of native
 * byte order (see buf.h for why that matters). Growth is geometric for amortized
 * O(1) appends, the same strategy the diagnostics sink uses.
 */
#include "buf/buf.h"

#include <stdlib.h>
#include <string.h>

void qas_buf_init(qas_buf *buf)
{
    if (buf == NULL) {
        return;
    }
    buf->data = NULL;
    buf->len  = 0;
    buf->cap  = 0;
}

void qas_buf_dispose(qas_buf *buf)
{
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    qas_buf_init(buf);
}

qas_status qas_buf_reserve(qas_buf *buf, size_t additional)
{
    if (buf == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (additional <= buf->cap - buf->len) {
        return QAS_OK; /* Already room (note: cap >= len always holds). */
    }

    size_t needed = buf->len + additional;
    if (needed < buf->len) {
        return QAS_ERR_OVERFLOW; /* size_t wraparound: a pathological request. */
    }

    size_t new_cap = (buf->cap == 0) ? 64u : buf->cap;
    while (new_cap < needed) {
        size_t doubled = new_cap * 2u;
        if (doubled < new_cap) {        /* Growth overflowed: clamp to exact need. */
            new_cap = needed;
            break;
        }
        new_cap = doubled;
    }

    uint8_t *grown = (uint8_t *)realloc(buf->data, new_cap);
    if (grown == NULL) {
        return QAS_ERR_OUT_OF_MEMORY;
    }
    buf->data = grown;
    buf->cap  = new_cap;
    return QAS_OK;
}

qas_status qas_buf_append(qas_buf *buf, const void *bytes, size_t len)
{
    if (buf == NULL || (bytes == NULL && len != 0)) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (len == 0) {
        return QAS_OK;
    }
    qas_status st = qas_buf_reserve(buf, len);
    if (st != QAS_OK) {
        return st;
    }
    memcpy(buf->data + buf->len, bytes, len);
    buf->len += len;
    return QAS_OK;
}

qas_status qas_buf_append_u8(qas_buf *buf, uint8_t value)
{
    qas_status st = qas_buf_reserve(buf, 1);
    if (st != QAS_OK) {
        return st;
    }
    buf->data[buf->len++] = value;
    return QAS_OK;
}

qas_status qas_buf_append_zeros(qas_buf *buf, size_t count)
{
    if (count == 0) {
        return (buf != NULL) ? QAS_OK : QAS_ERR_INVALID_ARGUMENT;
    }
    qas_status st = qas_buf_reserve(buf, count);
    if (st != QAS_OK) {
        return st;
    }
    memset(buf->data + buf->len, 0, count);
    buf->len += count;
    return QAS_OK;
}

/*
 * Append the low `n` bytes of `value`, least significant first. A single helper
 * backs all the fixed-width little-endian writers so the byte-order logic exists
 * in exactly one place.
 */
static qas_status append_le(qas_buf *buf, uint64_t value, unsigned n)
{
    qas_status st = qas_buf_reserve(buf, n);
    if (st != QAS_OK) {
        return st;
    }
    for (unsigned i = 0; i < n; ++i) {
        buf->data[buf->len++] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
    return QAS_OK;
}

qas_status qas_buf_append_u16le(qas_buf *buf, uint16_t value)
{
    return append_le(buf, value, 2);
}

qas_status qas_buf_append_u32le(qas_buf *buf, uint32_t value)
{
    return append_le(buf, value, 4);
}

qas_status qas_buf_append_u64le(qas_buf *buf, uint64_t value)
{
    return append_le(buf, value, 8);
}

qas_status qas_buf_append_i64le(qas_buf *buf, int64_t value)
{
    /*
     * Reinterpret the signed value's bits as unsigned before emitting. On a
     * two's-complement host (which x86-64 is, and which C23 mandates universally)
     * this is the identity on the bit pattern; the cast is well-defined in C for
     * any int64_t value, unlike left-shifting a negative number.
     */
    return append_le(buf, (uint64_t)value, 8);
}

qas_status qas_buf_patch_u32le(qas_buf *buf, size_t offset, uint32_t value)
{
    if (buf == NULL || offset + 4u < offset || offset + 4u > buf->len) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    for (unsigned i = 0; i < 4u; ++i) {
        buf->data[offset + i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
    return QAS_OK;
}

qas_status qas_buf_take(qas_buf *buf, uint8_t **out_data, size_t *out_len)
{
    if (buf == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    if (out_data != NULL) {
        *out_data = buf->data;
    } else {
        free(buf->data); /* Caller does not want the bytes: do not leak them. */
    }
    if (out_len != NULL) {
        *out_len = buf->len;
    }
    qas_buf_init(buf); /* Buffer no longer owns the storage. */
    return QAS_OK;
}
