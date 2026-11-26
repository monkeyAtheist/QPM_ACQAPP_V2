#pragma once

// Minimal, dependency-free test harness (Catch2-like syntax, so a later switch is mechanical).
//
//   TEST_CASE("name", "[tag1][tag2]") { CHECK(a == b); REQUIRE(ok); CHECK_REL(x, ref, 1e-9); }
//
// Tag "[defect]" marks a known signal-processing defect found by the audit: the test asserts the
// CORRECT behaviour and is expected to fail today. The harness counts a failing defect test
// as "confirmed" (not a failure) and a passing one as a failure ("defect fixed? remove the
// tag"), so a fix can never go unnoticed.
//
// Command line: qacq_tests [--verbose] [filter...]  where a filter is "[tag]" or a substring of a name.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qacq::test
{
struct Case
{
    std::string name;
    std::string tags;
    std::function<void()> body;
};

inline std::vector<Case> &registry()
{
    static std::vector<Case> cases;
    return cases;
}

struct Registrar
{
    Registrar(const char *name, const char *tags, std::function<void()> body)
    {
        registry().push_back({name, tags, std::move(body)});
    }
};

struct State
{
    int failures = 0;
    int checks = 0;
    bool quiet = false;
    bool verbose = false;
};

inline State &state()
{
    static State s;
    return s;
}

struct RequireFailed : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

inline void report(bool ok, const char *file, int line, const std::string &what)
{
    ++state().checks;
    if (ok) return;
    ++state().failures;
    if (!state().quiet) std::printf("    %s:%d: FAILED: %s\n", file, line, what.c_str());
}

inline bool relClose(double value, double reference, double relTol, double absTol)
{
    if (!std::isfinite(value) || !std::isfinite(reference)) return false;
    return std::abs(value - reference) <= std::max(absTol, relTol * std::abs(reference));
}

inline std::string describeRel(double value, double reference, double relTol, double absTol)
{
    std::ostringstream s;
    s.precision(12);
    s << "value " << value << " vs reference " << reference << " (rel " << relTol << ", abs " << absTol << ", rel err "
      << (reference != 0.0 ? std::abs(value - reference) / std::abs(reference) : value) << ")";
    return s.str();
}

inline void checkNear(double value, double reference, double relTol, double absTol, const char *file, int line,
                      const char *expr)
{
    report(relClose(value, reference, relTol, absTol), file, line,
           std::string(expr) + ": " + describeRel(value, reference, relTol, absTol));
}

inline bool matches(const Case &c, const std::vector<std::string> &filters)
{
    if (filters.empty()) return true;
    for (const auto &f : filters)
    {
        if (!f.empty() && f.front() == '[' ? c.tags.find(f) != std::string::npos : c.name.find(f) != std::string::npos)
            return true;
    }
    return false;
}

inline int runAll(int argc, char **argv)
{
    std::vector<std::string> filters;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--verbose")
            state().verbose = true; // also print the failed checks of [defect] tests
        else
            filters.push_back(arg);
    }
    int failed = 0, passed = 0, defectsConfirmed = 0, run = 0;
    for (const auto &c : registry())
    {
        if (!matches(c, filters)) continue;
        ++run;
        const bool defect = c.tags.find("[defect]") != std::string::npos;
        state().failures = 0;
        state().quiet = defect && !state().verbose;
        const auto t0 = std::chrono::steady_clock::now();
        std::string exception;
        try
        {
            c.body();
        }
        catch (const RequireFailed &)
        {
        }
        catch (const std::exception &e)
        {
            exception = e.what();
            ++state().failures;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool bodyFailed = state().failures > 0;
        if (defect)
        {
            if (bodyFailed)
            {
                ++defectsConfirmed;
                std::printf("[defect ok] %s (%.0f ms)\n", c.name.c_str(), ms);
            }
            else
            {
                ++failed;
                std::printf("[FIXED?   ] %s: passes now, remove the [defect] tag\n", c.name.c_str());
            }
        }
        else if (bodyFailed)
        {
            ++failed;
            std::printf("[FAIL     ] %s %s\n", c.name.c_str(), exception.c_str());
        }
        else
        {
            ++passed;
            std::printf("[pass     ] %s (%.0f ms)\n", c.name.c_str(), ms);
        }
    }
    std::printf("\n%d test cases: %d passed, %d known defects confirmed, %d failed\n", run, passed, defectsConfirmed,
                failed);
    return (failed == 0 && run > 0) ? 0 : 1;
}
} // namespace qacq::test

#define QACQ_CAT2(a, b) a##b
#define QACQ_CAT(a, b) QACQ_CAT2(a, b)
#define TEST_CASE(name, tags)                                                                                          \
    static void QACQ_CAT(qacq_test_fn_, __LINE__)();                                                                   \
    static const ::qacq::test::Registrar QACQ_CAT(qacq_test_reg_, __LINE__){name, tags,                                \
                                                                            &QACQ_CAT(qacq_test_fn_, __LINE__)};       \
    static void QACQ_CAT(qacq_test_fn_, __LINE__)()

#define CHECK(expr) ::qacq::test::report(static_cast<bool>(expr), __FILE__, __LINE__, #expr)
#define REQUIRE(expr)                                                                                                  \
    do {                                                                                                               \
        const bool qacq_ok_ = static_cast<bool>(expr);                                                                 \
        ::qacq::test::report(qacq_ok_, __FILE__, __LINE__, #expr);                                                     \
        if (!qacq_ok_) throw ::qacq::test::RequireFailed(#expr);                                                       \
    } while (false)
// |value - reference| <= max(absTol, relTol * |reference|). Each argument is evaluated once.
#define CHECK_REL(value, reference, relTol)                                                                            \
    ::qacq::test::checkNear((value), (reference), (relTol), 0.0, __FILE__, __LINE__, #value)
#define CHECK_NEAR(value, reference, relTol, absTol)                                                                   \
    ::qacq::test::checkNear((value), (reference), (relTol), (absTol), __FILE__, __LINE__, #value)
