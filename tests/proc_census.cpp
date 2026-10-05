// Leak census for the test suite (ctest runs `baseline` before every test
// and `check` after all of them):
//   proc_census baseline <file>   record the processes running now
//   proc_census check <file>      fail when the suite left processes behind
// A leak is a process started during the suite that is still running:
// a test helper (mux_child, mux_client_child), a bromux server, or -- on
// Windows -- a console host (conhost / OpenConsole, one per pseudoconsole)
// whose parent is gone. Leaked console hosts exhaust the desktop heap until
// new console processes fail to start (0xC0000142).
#include "no_dialogs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <tlhelp32.h>
#endif

namespace {

struct Proc {
    unsigned long pid{0};
    unsigned long ppid{0};
    std::string name;  // lower case, without ".exe"
};

std::string normalise(std::string n) {
    for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".exe") == 0) n.resize(n.size() - 4);
    const size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n.erase(0, slash + 1);
    return n;
}

std::vector<Proc> processes() {
    std::vector<Proc> out;
#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        Proc p;
        p.pid = e.th32ProcessID;
        p.ppid = e.th32ParentProcessID;
        char name[MAX_PATH * 3];
        const int n = WideCharToMultiByte(CP_UTF8, 0, e.szExeFile, -1, name, sizeof name, nullptr, nullptr);
        p.name = normalise(n > 0 ? std::string(name) : std::string());
        out.push_back(std::move(p));
    }
    CloseHandle(snap);
#else
    std::FILE* f = ::popen("ps -axo pid=,ppid=,comm=", "r");
    if (!f) return out;
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
        Proc p;
        char comm[4096] = {};
        if (std::sscanf(line, "%lu %lu %4095[^\n]", &p.pid, &p.ppid, comm) != 3) continue;
        p.name = normalise(comm);
        out.push_back(std::move(p));
    }
    ::pclose(f);
#endif
    return out;
}

bool is_helper(const std::string& n) { return n == "mux_child" || n == "mux_client_child" || n == "bromux"; }
bool is_console_host(const std::string& n) { return n == "conhost" || n == "openconsole"; }

std::vector<std::string> leaks(const std::set<unsigned long>& before) {
    const std::vector<Proc> now = processes();
    std::set<unsigned long> alive;
    for (const Proc& p : now) alive.insert(p.pid);
    std::vector<std::string> out;
    for (const Proc& p : now) {
        if (before.count(p.pid)) continue;
        const bool orphan_console = is_console_host(p.name) && !alive.count(p.ppid);
#if !defined(_WIN32)
        (void)orphan_console;
        if (!is_helper(p.name)) continue;
#else
        if (!is_helper(p.name) && !orphan_console) continue;
#endif
        out.push_back(p.name + " (pid " + std::to_string(p.pid) + ", parent " + std::to_string(p.ppid) +
                      (alive.count(p.ppid) ? "" : ", gone") + ")");
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    check::no_error_dialogs();
    if (argc < 3) {
        std::printf("usage: proc_census baseline|check <file>\n");
        return 2;
    }
    const std::string mode = argv[1];
    const std::string file = argv[2];
    if (mode == "baseline") {
        std::ofstream f(file, std::ios::trunc);
        int helpers = 0;
        for (const Proc& p : processes()) {
            f << p.pid << "\n";
            if (is_helper(p.name) && p.name != "bromux") ++helpers;
        }
        if (helpers) std::printf("note: %d test helper processes were already running before the suite\n", helpers);
        return f ? 0 : 1;
    }
    if (mode != "check") return 2;
    std::set<unsigned long> before;
    {
        std::ifstream f(file);
        if (!f) {
            std::printf("proc_census: no baseline %s\n", file.c_str());
            return 1;
        }
        for (unsigned long pid; f >> pid;) before.insert(pid);
    }
    // Teardown is asynchronous (reaper threads, console hosts closing).
    std::vector<std::string> found;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
        found = leaks(before);
        if (found.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    } while (std::chrono::steady_clock::now() < until);
    for (const std::string& s : found) std::printf("FAIL leaked process: %s\n", s.c_str());
    std::printf("[proc_census] %zu leaked processes\n", found.size());
    return found.empty() ? 0 : 1;
}
