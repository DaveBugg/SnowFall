// Tiny dependency-free unit-test harness.
//   TEST(name) { CHECK(cond); CHECK_NEAR(a, b, eps); }
// Exactly one .cpp per test executable must define SNOW_TEST_MAIN before including this.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace snowtest {
struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); }
};
inline int runAll(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failedCases = 0;
    for (auto& c : cases()) {
        if (filter && !strstr(c.name, filter)) continue;
        int before = failures();
        c.fn();
        ++ran;
        if (failures() != before) {
            ++failedCases;
            printf("[FAIL] %s\n", c.name);
        } else {
            printf("[ ok ] %s\n", c.name);
        }
    }
    printf("\n%d test(s), %d failed\n", ran, failedCases);
    return failedCases ? 1 : 0;
}
}  // namespace snowtest

#define TEST(name)                                                      \
    static void snowtest_##name();                                      \
    static ::snowtest::Reg snowtest_reg_##name(#name, snowtest_##name); \
    static void snowtest_##name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++::snowtest::failures();                                     \
            printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
    do {                                                                                           \
        double snowtest_a = static_cast<double>(a), snowtest_b = static_cast<double>(b);           \
        if (!(std::fabs(snowtest_a - snowtest_b) <= (eps))) {                                      \
            ++::snowtest::failures();                                                              \
            printf("  %s:%d: CHECK_NEAR(%s, %s) failed: %g vs %g\n", __FILE__, __LINE__, #a, #b,   \
                   snowtest_a, snowtest_b);                                                        \
        }                                                                                          \
    } while (0)

#ifdef SNOW_TEST_MAIN
int main(int argc, char** argv) { return ::snowtest::runAll(argc, argv); }
#endif
