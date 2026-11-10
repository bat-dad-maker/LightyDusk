#pragma once

#ifdef _WIN32
#include <cstdlib>
#include <crtdbg.h>

namespace {
// Standalone tests must report failures to CTest, never block on desktop UI.
struct TestRuntime {
    TestRuntime() {
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    }
};
inline TestRuntime testRuntime;
}
#endif
