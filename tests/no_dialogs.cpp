// Test executables never open a modal dialog: a Debug CRT assert, abort() or
// crash would otherwise block CI and the developer's desktop until clicked.
// Linked into every test executable; the static object runs before main().
#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#include <initializer_list>

namespace {
struct NoDialogs {
    NoDialogs() {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        for (int type : {_CRT_WARN, _CRT_ASSERT, _CRT_ERROR}) {
            _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
        }
    }
};
const NoDialogs kNoDialogs;
}  // namespace
#endif
