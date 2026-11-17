#pragma once
// Tiny dependency-free test harness.
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ktest {

struct Case { std::string name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
struct Failure { std::string msg; };

inline std::string& tmpDir() { static std::string d; return d; }

} // namespace ktest

#define KT_CAT2(a, b) a##b
#define KT_CAT(a, b) KT_CAT2(a, b)
#define TEST(name)                                                                 \
    static void KT_CAT(test_, name)();                                             \
    static ktest::Registrar KT_CAT(reg_, name)(#name, KT_CAT(test_, name));        \
    static void KT_CAT(test_, name)()

#define CHECK(cond)                                                                \
    do { if (!(cond)) { std::ostringstream _o; _o << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ")"; throw ktest::Failure{_o.str()}; } } while (0)
#define CHECK_EQ(a, b)                                                             \
    do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { std::ostringstream _o; _o << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b ") got " << _a << " vs " << _b; throw ktest::Failure{_o.str()}; } } while (0)
#define CHECK_NEAR(a, b, eps)                                                      \
    do { double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (eps))) { std::ostringstream _o; _o << __FILE__ << ":" << __LINE__ << ": CHECK_NEAR(" #a ", " #b ") got " << _a << " vs " << _b << " (eps " << (eps) << ")"; throw ktest::Failure{_o.str()}; } } while (0)
#define CHECK_THROWS(expr)                                                         \
    do { bool _t = false; try { expr; } catch (...) { _t = true; } if (!_t) { std::ostringstream _o; _o << __FILE__ << ":" << __LINE__ << ": expected throw: " #expr; throw ktest::Failure{_o.str()}; } } while (0)
