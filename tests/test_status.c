/*
 * Tests for the status module: the OK==0 invariant and stable name strings.
 */
#include "qtest.h"

#include "status/status.h"

static void test_ok_is_zero(void)
{
    /* The whole codebase relies on this (error-handling.md). */
    QTEST_CHECK_EQ_INT(QAS_OK, 0, "QAS_OK must be zero");
}

static void test_names_are_stable(void)
{
    QTEST_CHECK_SPAN(qas_status_str(QAS_OK), strlen(qas_status_str(QAS_OK)),
                     "ok", "QAS_OK name");
    QTEST_CHECK_SPAN(qas_status_str(QAS_ERR_OUT_OF_MEMORY),
                     strlen(qas_status_str(QAS_ERR_OUT_OF_MEMORY)),
                     "out of memory", "OOM name");
    QTEST_CHECK_SPAN(qas_status_str(QAS_ERR_INVALID_ARGUMENT),
                     strlen(qas_status_str(QAS_ERR_INVALID_ARGUMENT)),
                     "invalid argument", "invalid-arg name");
}

static void test_unknown_is_total(void)
{
    /* An out-of-range value must still yield a non-NULL sentinel string. */
    const char *s = qas_status_str((qas_status)9999);
    QTEST_CHECK_TRUE(s != NULL);
    QTEST_CHECK_SPAN(s, strlen(s), "unknown status", "out-of-range name");
}

int main(void)
{
    test_ok_is_zero();
    test_names_are_stable();
    test_unknown_is_total();
    return qtest_report("status");
}
