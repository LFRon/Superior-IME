// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <iostream>

namespace superior::test {

inline int failures = 0;
inline int checks = 0;

inline void record(bool ok, const char *expression, const char *file, int line) {
    ++checks;
    if (!ok) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
        ++failures;
    }
}

inline int finish(const char *suite) {
    if (failures == 0) {
        std::cout << suite << ": " << checks << " checks passed\n";
        return 0;
    }
    std::cerr << suite << ": " << failures << " failure(s) out of " << checks << " checks\n";
    return 1;
}

inline bool near(double lhs, double rhs, double epsilon = 1e-9) {
    const double delta = lhs - rhs;
    return (delta < 0 ? -delta : delta) <= epsilon;
}

} // namespace superior::test

#define CHECK(condition)                                                                           \
    ::superior::test::record(static_cast<bool>(condition), #condition, __FILE__, __LINE__)
