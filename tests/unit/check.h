#ifndef NVFLUX_TEST_CHECK_H
#define NVFLUX_TEST_CHECK_H

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        long long _a = (long long)(a), _b = (long long)(b);                      \
        checks++;                                                                \
        if (_a != _b) {                                                          \
            failures++;                                                          \
            fprintf(stderr, "%s:%d: FAIL: %s == %s (%lld != %lld)\n", __FILE__,  \
                    __LINE__, #a, #b, _a, _b);                                   \
        }                                                                        \
    } while (0)

#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        checks++;                                                                \
        if (strcmp((a), (b)) != 0) {                                             \
            failures++;                                                          \
            fprintf(stderr, "%s:%d: FAIL: \"%s\" != \"%s\"\n", __FILE__,         \
                    __LINE__, (a), (b));                                         \
        }                                                                        \
    } while (0)

static int check_report(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, checks, failures);
    return failures ? 1 : 0;
}

#endif
