/* Minimal single-header test harness for the host suite. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_NEAR(a, b, tol) do { \
    g_tests_run++; \
    double _a = (a), _b = (b); \
    if (fabs(_a - _b) > (tol)) { \
        g_tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: %s=%g vs %s=%g (tol %g)\n", \
                __FILE__, __LINE__, #a, _a, #b, _b, (double)(tol)); \
    } \
} while (0)

#define TEST_MAIN_END() do { \
    printf("%s: %d checks, %d failed\n", __FILE__, g_tests_run, g_tests_failed); \
    return g_tests_failed ? 1 : 0; \
} while (0)

#endif
