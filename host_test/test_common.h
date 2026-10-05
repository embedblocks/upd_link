#pragma once
#include <stdio.h>
#include <stdlib.h>

static int g_checks, g_failures;

#define CHECK(cond) do {                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b) do {                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                     \
        g_checks++;                                                             \
        if (_a != _b) {                                                         \
            g_failures++;                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld / 0x%llx vs 0x%llx)\n", \
                    __FILE__, __LINE__, #a, #b, _a, _b, _a, _b);                \
        }                                                                       \
    } while (0)

#define RUN(fn) do {                                                            \
        int _f = g_failures;                                                    \
        fn();                                                                   \
        printf("%-58s %s\n", #fn, g_failures == _f ? "ok" : "FAILED");          \
    } while (0)

#define SUMMARY() (printf("\n%d checks, %d failures\n", g_checks, g_failures), g_failures ? 1 : 0)
