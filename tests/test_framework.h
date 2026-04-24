#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <cstdio>
#include <cstring>
#include <cmath>

// These must be defined in each test file's main() scope or as true globals.
// Declare as extern here; each test .cpp defines them.
extern int test_count;
extern int test_failures;

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
        test_failures++; return; \
    } \
} while(0)

#define ASSERT_NEQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) { \
        printf("  FAIL %s:%d: %s != %s (both %lld)\n", __FILE__, __LINE__, #a, #b, _a); \
        test_failures++; return; \
    } \
} while(0)

#define ASSERT_STREQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); \
        test_failures++; return; \
    } \
} while(0)

#define ASSERT_STRCONTAINS(haystack, needle) do { \
    if (strstr((haystack), (needle)) == NULL) { \
        printf("  FAIL %s:%d: \"%s\" not found in \"%s\"\n", __FILE__, __LINE__, (needle), (haystack)); \
        test_failures++; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        test_failures++; return; \
    } \
} while(0)

#define ASSERT_FLOAT_EQ(a, b, eps) do { \
    double _a = (a), _b = (b); \
    if (fabs(_a - _b) > (eps)) { \
        printf("  FAIL %s:%d: %s ~= %s (%f != %f)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
        test_failures++; return; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    printf("  %-60s", #fn); \
    int _before = test_failures; \
    fn(); \
    printf("%s\n", (_before == test_failures) ? "ok" : ""); \
    test_count++; \
} while(0)

#define TEST_REPORT() do { \
    printf("\n%d tests, %d failures\n", test_count, test_failures); \
    return test_failures > 0 ? 1 : 0; \
} while(0)

#endif // TEST_FRAMEWORK_H
