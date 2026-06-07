/*
 * qtest — a minimal, dependency-free unit-test harness for qas.
 *
 * Why our own harness?
 *   The project forbids third-party libraries (and a test framework would be
 *   one). The needs here are modest — assert a condition, compare integers and
 *   text spans, print a readable failure with file:line, and return a process
 *   exit code — so a small header covers it. Each test file is its own program:
 *   it calls some test functions and ends with `return qtest_report("name");`.
 *
 * Usage
 *   #include "qtest.h"
 *   static void test_thing(void) { QTEST_CHECK_TRUE(1 + 1 == 2); }
 *   int main(void) { test_thing(); return qtest_report("thing"); }
 *
 * The counters are file-local (static): each test executable has its own, which
 * is exactly right because each is a separate program.
 */
#ifndef QAS_TESTS_QTEST_H
#define QAS_TESTS_QTEST_H

#include <stdio.h>
#include <string.h>

static int qtest_checks   = 0;
static int qtest_failures = 0;

/* Assert a boolean condition with a custom message. */
#define QTEST_CHECK(cond, msg)                                                  \
    do {                                                                        \
        qtest_checks += 1;                                                       \
        if (!(cond)) {                                                           \
            qtest_failures += 1;                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));    \
        }                                                                        \
    } while (0)

/* Assert a boolean condition, using its source text as the message. */
#define QTEST_CHECK_TRUE(cond) QTEST_CHECK((cond), #cond)

/* Compare two values as signed integers (covers enums, ints, line/col). */
#define QTEST_CHECK_EQ_INT(actual, expected, msg)                              \
    do {                                                                        \
        long long qa_ = (long long)(actual);                                    \
        long long qe_ = (long long)(expected);                                  \
        qtest_checks += 1;                                                       \
        if (qa_ != qe_) {                                                        \
            qtest_failures += 1;                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s (got %lld, want %lld)\n",          \
                    __FILE__, __LINE__, (msg), qa_, qe_);                        \
        }                                                                        \
    } while (0)

/* Compare two values as unsigned integers (covers size_t and uint64 values). */
#define QTEST_CHECK_EQ_UINT(actual, expected, msg)                             \
    do {                                                                        \
        unsigned long long qa_ = (unsigned long long)(actual);                  \
        unsigned long long qe_ = (unsigned long long)(expected);                \
        qtest_checks += 1;                                                       \
        if (qa_ != qe_) {                                                        \
            qtest_failures += 1;                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s (got %llu, want %llu)\n",          \
                    __FILE__, __LINE__, (msg), qa_, qe_);                        \
        }                                                                        \
    } while (0)

/*
 * Assert that the byte span [ptr, ptr+len) equals the NUL-terminated `expected`.
 * Used to check token lexemes against the expected text without copying.
 */
#define QTEST_CHECK_SPAN(ptr, len, expected, msg)                              \
    do {                                                                        \
        const char *qp_ = (ptr);                                                \
        size_t      ql_ = (size_t)(len);                                        \
        size_t      qel_ = strlen(expected);                                    \
        qtest_checks += 1;                                                       \
        if (ql_ != qel_ || memcmp(qp_, (expected), ql_) != 0) {                  \
            qtest_failures += 1;                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s (got \"%.*s\", want \"%s\")\n",    \
                    __FILE__, __LINE__, (msg), (int)ql_, qp_, (expected));       \
        }                                                                        \
    } while (0)

/* Print a one-line summary and return 0 if all checks passed, else 1. */
static int qtest_report(const char *suite)
{
    if (qtest_failures == 0) {
        fprintf(stderr, "PASS %s: %d checks\n", suite, qtest_checks);
        return 0;
    }
    fprintf(stderr, "FAIL %s: %d of %d checks failed\n", suite, qtest_failures,
            qtest_checks);
    return 1;
}

#endif /* QAS_TESTS_QTEST_H */
