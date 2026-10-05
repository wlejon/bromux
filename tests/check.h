#pragma once
// Minimal test harness. Checks never abort and are never compiled out: every
// failure is printed with its location and counted, and main() returns
// non-zero when any failed, so Release builds test exactly like Debug ones.
// A watchdog turns a hang into a reported failure naming the phase.

#include "no_dialogs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace check {

inline std::atomic<int> g_failures{0};
inline std::atomic<int> g_checks{0};

inline std::string escape(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == 0x1b) out += "\\e";
        else if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out.push_back(char(c));
        }
    }
    return out;
}

inline std::string show(const std::string& s) { return "\"" + escape(s) + "\""; }
inline std::string show(std::string_view s) { return show(std::string(s)); }
inline std::string show(const char* s) { return show(std::string(s)); }
inline std::string show(bool b) { return b ? "true" : "false"; }
template <class T>
std::string show(const T& v) {
    if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<long long>(v));
    else return std::to_string(v);
}

inline void fail(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::printf("FAIL %s:%d: %s\n", file, line, what.c_str());
    std::fflush(stdout);
}

template <class A, class B>
void eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++g_checks;
    if (!(a == b)) fail(file, line, std::string(ea) + " == " + eb + "\n     got  " + show(a) + "\n     want " + show(b));
}

inline int finish(const char* name) {
    std::printf("[%s] %d checks, %d failed\n", name, g_checks.load(), g_failures.load());
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}

inline std::atomic<const char*> g_phase{"init"};
inline std::atomic<long long> g_deadline_ms{0};

inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Name the phase and give it `seconds` before the watchdog fails the test.
inline void phase(const char* name, int seconds = 60) {
    std::printf("-- %s\n", name);
    std::fflush(stdout);
    g_phase = name;
    g_deadline_ms = now_ms() + seconds * 1000LL;
}

inline void start_watchdog(const char* test_name) {
    no_error_dialogs();
    std::thread([test_name] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            long long d = g_deadline_ms.load();
            if (d && now_ms() > d) {
                std::printf("FAIL HANG in phase '%s' (watchdog)\n", g_phase.load());
                ++g_failures;
                finish(test_name);
                std::_Exit(1);
            }
        }
    }).detach();
}

}  // namespace check

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++::check::g_checks;                                                \
        if (!(cond)) ::check::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                     \
    do {                                                                                         \
        ++::check::g_checks;                                                                     \
        if (!(cond)) ::check::fail(__FILE__, __LINE__, std::string("CHECK(" #cond "): ") + (msg)); \
    } while (0)

#define CHECK_EQ(a, b) ::check::eq((a), (b), #a, #b, __FILE__, __LINE__)
