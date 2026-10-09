/* check.h - tiny test harness shared by the unit tests. */
#ifndef LUMA_CHECK_H
#define LUMA_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures = 0;
static int check_count = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        check_count++;                                                           \
        if (!(cond)) {                                                           \
            check_failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                       \
    do {                                                                         \
        long long _a = (long long)(a), _b = (long long)(b);                      \
        check_count++;                                                           \
        if (_a != _b) {                                                          \
            check_failures++;                                                    \
            printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                   #a, #b, _a, _b);                                              \
        }                                                                        \
    } while (0)

static inline int check_report(const char *suite) {
    printf("%s: %d checks, %d failure(s)\n", suite, check_count, check_failures);
    return check_failures ? 1 : 0;
}

#endif
