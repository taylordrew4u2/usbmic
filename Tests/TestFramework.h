#pragma once
// Minimal hand-rolled test runner. Catch2 would normally be FetchContent'd,
// but this sandbox can't reliably reach the network, so tests here use this
// tiny header-only framework instead. Real assertions, real failures, real
// exit code -- just no external dependency.
#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <cmath>
#include <sstream>

namespace mmatest {

struct TestCase
{
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar
{
    Registrar (const std::string& name, std::function<void()> fn)
    {
        registry().push_back ({ name, std::move (fn) });
    }
};

struct AssertionFailure : std::exception
{
    std::string message;
    explicit AssertionFailure (std::string m) : message (std::move (m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

inline void failAt (const char* file, int line, const std::string& msg)
{
    std::ostringstream oss;
    oss << file << ":" << line << ": " << msg;
    throw AssertionFailure (oss.str());
}

} // namespace mmatest

#define TEST_CASE(name) \
    static void name(); \
    static mmatest::Registrar registrar_##name (#name, &name); \
    static void name()

#define REQUIRE(cond) \
    do { if (!(cond)) mmatest::failAt(__FILE__, __LINE__, "REQUIRE failed: " #cond); } while (0)

#define REQUIRE_FALSE(cond) REQUIRE(!(cond))

#define REQUIRE_NEAR(a, b, eps) \
    do { \
        double av = (a); double bv = (b); double e = (eps); \
        if (std::abs(av - bv) > e) { \
            std::ostringstream _oss; _oss << "REQUIRE_NEAR failed: " #a " (" << av << ") vs " #b " (" << bv << "), eps=" << e; \
            mmatest::failAt(__FILE__, __LINE__, _oss.str()); \
        } \
    } while (0)

inline int runAllTests()
{
    int failures = 0;
    for (auto& t : mmatest::registry())
    {
        try
        {
            t.fn();
            std::cout << "[PASS] " << t.name << "\n";
        }
        catch (const mmatest::AssertionFailure& e)
        {
            std::cout << "[FAIL] " << t.name << " - " << e.what() << "\n";
            ++failures;
        }
        catch (const std::exception& e)
        {
            std::cout << "[FAIL] " << t.name << " - unexpected exception: " << e.what() << "\n";
            ++failures;
        }
    }

    std::cout << "\n" << mmatest::registry().size() << " tests run, " << failures << " failed.\n";
    return failures;
}
