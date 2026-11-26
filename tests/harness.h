/* A unit-test harness small enough to read in one go, shared by the C and C++ tests.
 *
 * No framework on purpose: the library has no dependencies and cross-compiles to small
 * targets, and its tests should do both too. A test is a void function; RUN calls it and
 * reports it; CHECK records a failure and carries on, so one run shows every broken check
 * rather than the first.
 *
 *     static void test_true_is_0x11(void) {
 *         ...
 *         EXPECT_BYTES(bytes, len, 0x11);
 *     }
 *
 *     int main(void) {
 *         RUN(test_true_is_0x11);
 *         return harness_report();
 *     }
 */
#ifndef TOLD_TEST_HARNESS_H
#define TOLD_TEST_HARNESS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int harness_tests = 0;
static int harness_failed_tests = 0;
static int harness_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            harness_failures++;                                                       \
            printf("        %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond);          \
        }                                                                             \
    } while (0)

/* Compare `len` bytes at `got` with the bytes listed, printing both on a mismatch. */
#define EXPECT_BYTES(got, len, ...)                                                   \
    do {                                                                              \
        static const uint8_t harness_want_[] = {__VA_ARGS__};                         \
        if (!harness_bytes_equal((got), (len), harness_want_, sizeof harness_want_)) { \
            harness_failures++;                                                       \
            printf("        %s:%d: bytes differ\n", __FILE__, __LINE__);              \
            harness_dump("got ", (got), (len));                                       \
            harness_dump("want", harness_want_, sizeof harness_want_);                \
        }                                                                             \
    } while (0)

#define RUN(test)                                                                     \
    do {                                                                              \
        const int harness_before_ = harness_failures;                                 \
        harness_tests++;                                                              \
        test();                                                                       \
        if (harness_failures == harness_before_) {                                    \
            printf("  ok    %s\n", #test);                                            \
        } else {                                                                      \
            harness_failed_tests++;                                                   \
            printf("  FAIL  %s\n", #test);                                            \
        }                                                                             \
    } while (0)

static int harness_bytes_equal(const uint8_t *got, size_t got_len, const uint8_t *want,
                               size_t want_len) {
    return got_len == want_len && (want_len == 0 || memcmp(got, want, want_len) == 0);
}

static void harness_dump(const char *label, const uint8_t *bytes, size_t len) {
    printf("          %s (%zu):", label, len);
    for (size_t i = 0; i < len && i < 24; i++) printf(" %02X", bytes[i]);
    printf("%s\n", len > 24 ? " ..." : "");
}

/* Prints the summary and returns the process exit code. */
static int harness_report(void) {
    if (harness_failed_tests) {
        printf("%d of %d tests failed (%d checks)\n", harness_failed_tests, harness_tests,
               harness_failures);
        return 1;
    }
    printf("all %d tests passed\n", harness_tests);
    return 0;
}

#endif /* TOLD_TEST_HARNESS_H */
