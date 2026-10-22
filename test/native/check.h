// Minimal test macros shared by the host-side test files.
#pragma once

#include <cstdio>
#include <string>

extern int failures;
extern int checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++checks;                                                                \
        auto _a = (a);                                                           \
        auto _b = (b);                                                           \
        if (!(_a == _b)) {                                                       \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                        #a, #b, (long long)_a, (long long)_b);                   \
        }                                                                        \
    } while (0)
#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        ++checks;                                                                \
        std::string _a = (a);                                                    \
        std::string _b = (b);                                                    \
        if (_a != _b) {                                                          \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d:\n    got:  \"%s\"\n    want: \"%s\"\n",    \
                        __FILE__, __LINE__, _a.c_str(), _b.c_str());             \
        }                                                                        \
    } while (0)

#define TEST(name) static void name()
#define RUN(name)                                     \
    do {                                              \
        std::printf("%s\n", #name);                   \
        name();                                       \
    } while (0)
