/*
 * Tests for the byte buffer: exact little-endian byte order for each width
 * (the property the ELF writer and encoder depend on), zero-fill, in-place
 * patching, signed addend encoding, and ownership transfer via take.
 */
#include "qtest.h"

#include <stdlib.h> /* free() for the take() ownership-transfer test */

#include "buf/buf.h"

static void test_append_and_bytes(void)
{
    qas_buf b;
    qas_buf_init(&b);

    QTEST_CHECK_EQ_INT(qas_buf_append_u8(&b, 0xAB), QAS_OK, "append_u8");
    const uint8_t three[] = { 0x01, 0x02, 0x03 };
    QTEST_CHECK_EQ_INT(qas_buf_append(&b, three, sizeof three), QAS_OK, "append");
    QTEST_CHECK_EQ_UINT(b.len, 4u, "len after appends");
    QTEST_CHECK_EQ_UINT(b.data[0], 0xABu, "b0");
    QTEST_CHECK_EQ_UINT(b.data[3], 0x03u, "b3");

    /* Appending zero bytes from a NULL pointer is allowed and is a no-op. */
    QTEST_CHECK_EQ_INT(qas_buf_append(&b, NULL, 0), QAS_OK, "append nothing");
    QTEST_CHECK_EQ_UINT(b.len, 4u, "len unchanged");

    qas_buf_dispose(&b);
}

static void test_little_endian_widths(void)
{
    qas_buf b;
    qas_buf_init(&b);

    QTEST_CHECK_EQ_INT(qas_buf_append_u16le(&b, 0x1122), QAS_OK, "u16");
    QTEST_CHECK_EQ_INT(qas_buf_append_u32le(&b, 0x11223344u), QAS_OK, "u32");
    QTEST_CHECK_EQ_INT(qas_buf_append_u64le(&b, 0x1122334455667788ull), QAS_OK, "u64");

    const uint8_t want[] = {
        0x22, 0x11,                                     /* u16le              */
        0x44, 0x33, 0x22, 0x11,                         /* u32le              */
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, /* u64le              */
    };
    QTEST_CHECK_EQ_UINT(b.len, sizeof want, "total len");
    for (size_t i = 0; i < sizeof want; ++i) {
        QTEST_CHECK_EQ_UINT(b.data[i], want[i], "le byte");
    }

    qas_buf_dispose(&b);
}

static void test_signed_addend(void)
{
    /* Elf64_Rela.r_addend is signed 64-bit; -1 must be all 0xFF bytes. */
    qas_buf b;
    qas_buf_init(&b);
    QTEST_CHECK_EQ_INT(qas_buf_append_i64le(&b, -1), QAS_OK, "i64 -1");
    for (size_t i = 0; i < 8; ++i) {
        QTEST_CHECK_EQ_UINT(b.data[i], 0xFFu, "ff byte");
    }
    qas_buf_dispose(&b);

    qas_buf c;
    qas_buf_init(&c);
    QTEST_CHECK_EQ_INT(qas_buf_append_i64le(&c, -2), QAS_OK, "i64 -2");
    QTEST_CHECK_EQ_UINT(c.data[0], 0xFEu, "low byte -2");
    QTEST_CHECK_EQ_UINT(c.data[7], 0xFFu, "high byte -2");
    qas_buf_dispose(&c);
}

static void test_zeros_and_patch(void)
{
    qas_buf b;
    qas_buf_init(&b);

    QTEST_CHECK_EQ_INT(qas_buf_append_zeros(&b, 4), QAS_OK, "zeros");
    QTEST_CHECK_EQ_UINT(b.len, 4u, "len");
    QTEST_CHECK_EQ_UINT(b.data[0] | b.data[1] | b.data[2] | b.data[3], 0u, "all zero");

    /* Back-patch the 4-byte field at offset 0. */
    QTEST_CHECK_EQ_INT(qas_buf_patch_u32le(&b, 0, 0xDEADBEEFu), QAS_OK, "patch");
    QTEST_CHECK_EQ_UINT(b.data[0], 0xEFu, "patch b0");
    QTEST_CHECK_EQ_UINT(b.data[1], 0xBEu, "patch b1");
    QTEST_CHECK_EQ_UINT(b.data[2], 0xADu, "patch b2");
    QTEST_CHECK_EQ_UINT(b.data[3], 0xDEu, "patch b3");

    /* Out-of-range patch is rejected without touching the buffer. */
    QTEST_CHECK_EQ_INT(qas_buf_patch_u32le(&b, 2, 0), QAS_ERR_INVALID_ARGUMENT,
                       "patch oob");

    qas_buf_dispose(&b);
}

static void test_take(void)
{
    qas_buf b;
    qas_buf_init(&b);
    QTEST_CHECK_EQ_INT(qas_buf_append_u32le(&b, 0x01020304u), QAS_OK, "append");

    uint8_t *data = NULL;
    size_t   len  = 0;
    QTEST_CHECK_EQ_INT(qas_buf_take(&b, &data, &len), QAS_OK, "take");
    QTEST_CHECK_TRUE(data != NULL);
    QTEST_CHECK_EQ_UINT(len, 4u, "taken len");
    QTEST_CHECK_EQ_UINT(b.len, 0u, "buffer emptied");
    QTEST_CHECK_TRUE(b.data == NULL);
    free(data);

    qas_buf_dispose(&b); /* safe on an emptied buffer */
}

static void test_growth(void)
{
    /* Force several reallocations and verify contents survive intact. */
    qas_buf b;
    qas_buf_init(&b);
    for (size_t i = 0; i < 1000; ++i) {
        QTEST_CHECK_EQ_INT(qas_buf_append_u8(&b, (uint8_t)(i & 0xFF)), QAS_OK, "grow");
    }
    QTEST_CHECK_EQ_UINT(b.len, 1000u, "len");
    QTEST_CHECK_EQ_UINT(b.data[0], 0u, "first");
    QTEST_CHECK_EQ_UINT(b.data[255], 255u, "wrap");
    QTEST_CHECK_EQ_UINT(b.data[256], 0u, "wrap2");
    QTEST_CHECK_EQ_UINT(b.data[999], (uint8_t)(999 & 0xFF), "last");
    qas_buf_dispose(&b);
}

int main(void)
{
    test_append_and_bytes();
    test_little_endian_widths();
    test_signed_addend();
    test_zeros_and_patch();
    test_take();
    test_growth();
    return qtest_report("buf");
}
