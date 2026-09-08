// ============================================================================
//  Minimal Catch2 v3-compatible shim.
//
//  Real Catch2 is fetched by CMake when VOXCAST_FETCH_CATCH2=ON. For hermetic
//  / offline builds (including the MinGW + Wine cross build) this shim provides
//  the exact subset the VoxCast tests use, so not one line of a test file
//  changes between the two.
//
//  Supported: TEST_CASE, SECTION (flattened), REQUIRE, REQUIRE_FALSE,
//             CHECK, REQUIRE_THROWS, Catch::Approx with .margin()/.epsilon().
// ============================================================================
#pragma once

#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace Catch {

// ---------------------------------------------------------------- Approx ----
class Approx {
public:
    explicit Approx(double v) : value_(v) {}
    Approx& margin(double m) { margin_ = m; return *this; }
    Approx& epsilon(double e) { epsilon_ = e; return *this; }
    double value() const { return value_; }

    friend bool operator==(double lhs, const Approx& rhs) {
        if (std::fabs(lhs - rhs.value_) <= rhs.margin_) return true;
        double scale = std::fmax(std::fabs(lhs), std::fabs(rhs.value_));
        return std::fabs(lhs - rhs.value_) <= rhs.epsilon_ * scale;
    }
    friend bool operator==(const Approx& lhs, double rhs) { return rhs == lhs; }
    friend bool operator!=(double lhs, const Approx& rhs) { return !(lhs == rhs); }
    friend bool operator!=(const Approx& lhs, double rhs) { return !(rhs == lhs); }
    friend bool operator==(float lhs, const Approx& rhs)  { return double(lhs) == rhs; }
    friend bool operator==(const Approx& lhs, float rhs)  { return double(rhs) == lhs; }

    std::string describe() const {
        char buf[96];
        std::snprintf(buf, sizeof buf, "Approx(%.6g) margin=%.3g", value_, margin_);
        return buf;
    }
private:
    double value_;
    double margin_{0.0};
    double epsilon_{std::numeric_limits<double>::epsilon() * 100};
};

// ------------------------------------------------------------- registry -----
struct TestCase {
    const char* name;
    const char* tags;
    void (*fn)();
};

class Registry {
public:
    static Registry& get() { static Registry r; return r; }
    void add(TestCase tc) { cases_.push_back(tc); }
    const std::vector<TestCase>& cases() const { return cases_; }

    // Per-assertion counters, reset for each test case.
    int assertions{0};
    int failures{0};
    std::vector<std::string> failureLog;
private:
    std::vector<TestCase> cases_;
};

struct AutoReg {
    AutoReg(const char* name, const char* tags, void (*fn)()) {
        Registry::get().add({name, tags, fn});
    }
};

inline void recordPass() { ++Registry::get().assertions; }

inline void recordFail(const char* file, int line, const char* expr) {
    auto& r = Registry::get();
    ++r.assertions;
    ++r.failures;
    char buf[512];
    std::snprintf(buf, sizeof buf, "      %s:%d\n        FAILED: %s", file, line, expr);
    r.failureLog.emplace_back(buf);
}

} // namespace Catch

// ----------------------------------------------------------------- macros ---
#define VOX_CAT2(a, b) a##b
#define VOX_CAT(a, b) VOX_CAT2(a, b)

#define TEST_CASE(name, ...)                                                    \
    static void VOX_CAT(vox_test_, __LINE__)();                                 \
    static ::Catch::AutoReg VOX_CAT(vox_reg_, __LINE__){                        \
        name, "" __VA_ARGS__, &VOX_CAT(vox_test_, __LINE__)};                   \
    static void VOX_CAT(vox_test_, __LINE__)()

// Sections are flattened: the body simply runs inline.
#define SECTION(name) if (true)

#define REQUIRE(expr)                                                           \
    do {                                                                        \
        if (expr) ::Catch::recordPass();                                        \
        else      ::Catch::recordFail(__FILE__, __LINE__, #expr);               \
    } while (0)

#define CHECK(expr) REQUIRE(expr)

#define REQUIRE_FALSE(expr)                                                     \
    do {                                                                        \
        if (!(expr)) ::Catch::recordPass();                                     \
        else         ::Catch::recordFail(__FILE__, __LINE__, "!(" #expr ")");   \
    } while (0)

#define REQUIRE_THROWS(expr)                                                    \
    do {                                                                        \
        bool threw = false;                                                     \
        try { (void)(expr); } catch (...) { threw = true; }                     \
        if (threw) ::Catch::recordPass();                                       \
        else ::Catch::recordFail(__FILE__, __LINE__, "throws: " #expr);         \
    } while (0)

#define REQUIRE_NOTHROW(expr)                                                   \
    do {                                                                        \
        bool threw = false;                                                     \
        try { (void)(expr); } catch (...) { threw = true; }                     \
        if (!threw) ::Catch::recordPass();                                      \
        else ::Catch::recordFail(__FILE__, __LINE__, "nothrow: " #expr);        \
    } while (0)
