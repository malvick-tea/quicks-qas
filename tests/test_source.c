/*
 * Tests for the source module: line indexing, offset->(line,col) mapping, line
 * text extraction (including CRLF handling), and EOF-offset clamping.
 */
#include "qtest.h"

#include "source/source.h"
#include "status/status.h"

static void test_line_index_lf(void)
{
    /* "abc\ndef\n" : two newlines -> three lines (the last one empty). */
    const char *text = "abc\ndef\n";
    qas_source src;
    QTEST_CHECK_EQ_INT(qas_source_from_memory("t", text, strlen(text), &src),
                       QAS_OK, "load");

    QTEST_CHECK_EQ_UINT(src.size, 8u, "size");
    QTEST_CHECK_EQ_UINT(src.line_count, 3u, "line count");

    uint32_t line = 0, col = 0;
    qas_source_location(&src, 0, &line, &col); /* 'a' */
    QTEST_CHECK_EQ_UINT(line, 1u, "off0 line");
    QTEST_CHECK_EQ_UINT(col, 1u, "off0 col");

    qas_source_location(&src, 4, &line, &col); /* 'd' (start of line 2) */
    QTEST_CHECK_EQ_UINT(line, 2u, "off4 line");
    QTEST_CHECK_EQ_UINT(col, 1u, "off4 col");

    qas_source_location(&src, 6, &line, &col); /* 'f' */
    QTEST_CHECK_EQ_UINT(line, 2u, "off6 line");
    QTEST_CHECK_EQ_UINT(col, 3u, "off6 col");

    size_t len = 0;
    const char *l1 = qas_source_line_text(&src, 1, &len);
    QTEST_CHECK_SPAN(l1, len, "abc", "line 1 text");
    const char *l2 = qas_source_line_text(&src, 2, &len);
    QTEST_CHECK_SPAN(l2, len, "def", "line 2 text");

    QTEST_CHECK_TRUE(qas_source_line_text(&src, 0, &len) == NULL);
    QTEST_CHECK_TRUE(qas_source_line_text(&src, 4, &len) == NULL);

    qas_source_dispose(&src);
    /* Disposed source is zeroed; a second dispose must be a safe no-op. */
    qas_source_dispose(&src);
    QTEST_CHECK_EQ_UINT(src.size, 0u, "disposed size");
}

static void test_line_text_crlf(void)
{
    /* "ab\r\ncd" : the \r before \n must be trimmed from line 1's text. */
    const char *text = "ab\r\ncd";
    qas_source src;
    QTEST_CHECK_EQ_INT(qas_source_from_memory("t", text, strlen(text), &src),
                       QAS_OK, "load");
    QTEST_CHECK_EQ_UINT(src.line_count, 2u, "line count");

    size_t len = 0;
    const char *l1 = qas_source_line_text(&src, 1, &len);
    QTEST_CHECK_SPAN(l1, len, "ab", "CRLF line 1 trimmed");
    const char *l2 = qas_source_line_text(&src, 2, &len);
    QTEST_CHECK_SPAN(l2, len, "cd", "line 2 text");

    uint32_t line = 0, col = 0;
    qas_source_location(&src, 4, &line, &col); /* 'c' */
    QTEST_CHECK_EQ_UINT(line, 2u, "off4 line");
    QTEST_CHECK_EQ_UINT(col, 1u, "off4 col");

    qas_source_dispose(&src);
}

static void test_offset_clamp_and_empty(void)
{
    /* Empty input is one (empty) line; past-the-end offsets clamp to EOF. */
    qas_source src;
    QTEST_CHECK_EQ_INT(qas_source_from_memory("t", "", 0, &src), QAS_OK, "load empty");
    QTEST_CHECK_EQ_UINT(src.line_count, 1u, "empty line count");

    uint32_t line = 0, col = 0;
    qas_source_location(&src, 1000, &line, &col); /* beyond end */
    QTEST_CHECK_EQ_UINT(line, 1u, "clamped line");
    QTEST_CHECK_EQ_UINT(col, 1u, "clamped col");

    qas_source_dispose(&src);
}

int main(void)
{
    test_line_index_lf();
    test_line_text_crlf();
    test_offset_clamp_and_empty();
    return qtest_report("source");
}
