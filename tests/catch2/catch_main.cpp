// Test runner for the Catch2 shim (equivalent of Catch2WithMain).
#include "catch_test_macros.hpp"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    const char* filter = nullptr;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) verbose = true;
        else if (argv[i][0] != '-') filter = argv[i];
    }

    auto& reg = Catch::Registry::get();
    int passedCases = 0, failedCases = 0, totalAssertions = 0;

    std::printf("\n VoxCast test suite — %zu cases\n", reg.cases().size());
    std::printf(" ---------------------------------------------------------------\n");

    for (const auto& tc : reg.cases()) {
        if (filter && !std::strstr(tc.name, filter) && !std::strstr(tc.tags, filter))
            continue;

        reg.assertions = 0;
        reg.failures = 0;
        reg.failureLog.clear();

        try {
            tc.fn();
        } catch (const std::exception& e) {
            ++reg.failures;
            reg.failureLog.push_back(std::string("      threw: ") + e.what());
        } catch (...) {
            ++reg.failures;
            reg.failureLog.emplace_back("      threw: unknown exception");
        }

        totalAssertions += reg.assertions;
        const bool ok = reg.failures == 0;
        ok ? ++passedCases : ++failedCases;

        if (!ok || verbose) {
            std::printf("  %s %-52s %s %2d assert\n",
                        ok ? "[ok]  " : "[FAIL]", tc.name, tc.tags, reg.assertions);
            for (const auto& f : reg.failureLog) std::printf("%s\n", f.c_str());
        } else {
            std::printf("  [ok]   %-52s %-14s %2d assert\n",
                        tc.name, tc.tags, reg.assertions);
        }
    }

    std::printf(" ---------------------------------------------------------------\n");
    if (failedCases == 0)
        std::printf(" PASSED  %d cases, %d assertions\n\n", passedCases, totalAssertions);
    else
        std::printf(" FAILED  %d of %d cases (%d assertions)\n\n",
                    failedCases, passedCases + failedCases, totalAssertions);
    return failedCases == 0 ? 0 : 1;
}
