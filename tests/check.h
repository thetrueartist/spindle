// Spindle - the harness shared by every test binary.
//
// A suite is a function registered with SUITE(identifier, "Name"), and
// CHECK records one assertion. Main() runs every suite, or the ones whose
// name matches --filter, prints each suite's check count and time, and
// exits non-zero if anything failed. A failure names the file, the line,
// the message, the expression and the suite, so one red line is enough
// to go on without opening the test.
//
//   ./build/test_core                  run everything
//   ./build/test_core --list           print the suite names
//   ./build/test_core --filter cache   run the suites whose name contains it
//   ./build/test_core --repeat 20      run the selection twenty times over:
//                                      a race that shows once in ten runs
//                                      is still a race
//   ./build/test_mft --image FILE      any other --name value pair is kept
//                                      for the suites, read with Option()
//
// Nothing but the standard library, on purpose: the tests exist to hold
// the program to its no-dependency promise, so they keep it themselves.
#pragma once

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace spindle::testing {

struct Suite {
    const char* name;
    void (*fn)();
};

inline std::vector<Suite>& Suites() {
    static std::vector<Suite> suites;
    return suites;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Suites().push_back(Suite{name, fn}); }
};

struct State {
    int pass = 0;
    int fail = 0;
    int suiteFail = 0;
    const char* suite = "";
};

inline State& Current() {
    static State state;
    return state;
}

// Extra --name value pairs from the command line, for suites that want an
// input the binary cannot make itself (an image built by another tool).
inline std::vector<std::pair<std::string, std::string>>& Options() {
    static std::vector<std::pair<std::string, std::string>> options;
    return options;
}

inline std::string Option(const char* name) {
    for (const auto& kv : Options()) {
        if (kv.first == name) return kv.second;
    }
    return std::string();
}

inline void Record(bool ok, const char* file, int line, const char* what,
                   const char* expr) {
    State& s = Current();
    if (ok) {
        ++s.pass;
        return;
    }
    ++s.fail;
    ++s.suiteFail;
    std::printf("  FAIL %s:%d  %s\n        %s\n        in suite: %s\n",
                file, line, what, expr, s.suite);
    std::fflush(stdout);
}

inline std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline bool NameMatches(const char* name, const std::string& filter) {
    return filter.empty() ||
           Lower(name).find(Lower(filter)) != std::string::npos;
}

inline int Main(const char* title, int argc, char** argv) {
    std::string filter;
    int repeat = 1;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a == "--filter" && i + 1 < argc) {
            filter = argv[++i];
        } else if (a == "--repeat" && i + 1 < argc) {
            repeat = std::atoi(argv[++i]);
            if (repeat < 1) repeat = 1;
        } else if (a.rfind("--", 0) == 0 && a.size() > 2 && i + 1 < argc) {
            Options().emplace_back(a.substr(2), argv[++i]);
        } else {
            std::printf("usage: %s [--list] [--filter NAME] [--repeat N]\n",
                        argc > 0 ? argv[0] : "test");
            return 2;
        }
    }
    if (list) {
        for (const Suite& s : Suites()) {
            if (NameMatches(s.name, filter)) std::printf("%s\n", s.name);
        }
        return 0;
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    std::printf("\n=== %s ===\n\n", title);
    int ran = 0;
    for (int round = 0; round < repeat; ++round) {
        for (const Suite& s : Suites()) {
            if (!NameMatches(s.name, filter)) continue;
            State& st = Current();
            st.suite = s.name;
            st.suiteFail = 0;
            const int before = st.pass + st.fail;
            std::printf("%s\n", s.name);
            std::fflush(stdout);
            const auto t0 = Clock::now();
            try {
                s.fn();
            } catch (const std::exception& e) {
                Record(false, __FILE__, __LINE__, "the suite threw", e.what());
            } catch (...) {
                Record(false, __FILE__, __LINE__, "the suite threw",
                       "an exception that is not a std::exception");
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                Clock::now() - t0).count();
            std::printf("    %d checks, %lld ms%s\n", st.pass + st.fail - before,
                        static_cast<long long>(ms),
                        st.suiteFail ? "  <-- FAILED" : "");
            ++ran;
        }
    }
    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - started).count();
    const State& st = Current();
    std::printf("\n=== %d passed, %d failed  (%d suites, %lld ms) ===\n\n",
                st.pass, st.fail, ran, static_cast<long long>(total));
    if (ran == 0) {
        std::printf("no suite matches \"%s\"\n", filter.c_str());
        return 1;
    }
    return st.fail == 0 ? 0 : 1;
}

// The Unicode entry point on Windows hands over wide arguments; the flags
// are ASCII, so narrowing them is enough.
inline int Main(const char* title, int argc, wchar_t** argv) {
    std::vector<std::string> narrow;
    for (int i = 0; i < argc; ++i) {
        std::string a;
        for (const wchar_t* p = argv[i]; *p; ++p) {
            a.push_back(*p < 128 ? static_cast<char>(*p) : '?');
        }
        narrow.push_back(a);
    }
    std::vector<char*> ptrs;
    for (std::string& a : narrow) ptrs.push_back(&a[0]);
    return Main(title, argc, ptrs.data());
}

}  // namespace spindle::testing

// SUITE(identifier, "Name") { ... }  defines and registers one suite.
#define SUITE(id, name)                                                     \
    static void id();                                                       \
    static const spindle::testing::Registrar id##_registrar(name, &id);     \
    static void id()

// CHECK(condition, "what it means"): one assertion. The expression text
// travels with the message, so the failure line reads on its own.
#define CHECK(cond, what) \
    spindle::testing::Record(!!(cond), __FILE__, __LINE__, what, #cond)
