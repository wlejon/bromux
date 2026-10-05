#pragma once
// Test processes, and every child they start, must never stop on a modal
// error dialog (a crash, a failed DLL initialisation, a Debug CRT assert or
// abort()): a test must fail, not wait for someone to click. The error mode
// is inherited by child processes (nothing here spawns with
// CREATE_DEFAULT_ERROR_MODE), so programs that run under a test inherit it.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <crtdbg.h>
#include <cstdlib>
#endif

namespace check {

inline void no_error_dialogs() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // Debug CRT reports (assert, _ASSERTE, heap checks) go to stderr.
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
}

// Exit statuses that mean a Windows process crashed or never started
// (NTSTATUS errors: 0xC0000142 DLL initialisation failed, 0xC0000005 access
// violation, ...). -1 (unknown) and 0xC000013A (closed by Ctrl+C / console
// close) are not crashes.
inline bool is_crash_status(long long code) {
    const auto u = static_cast<unsigned long>(static_cast<long>(code));
    return code != -1 && (u & 0xF0000000ul) == 0xC0000000ul && u != 0xC000013Aul;
}

}  // namespace check
