/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && OS(WINDOWS)

#include <windows.h>
#include <wtf/Assertions.h>

namespace JSC { namespace AOT {

// The test programs call this at the start of main().
// abort() fails fast on Windows: no handler in the process runs, Windows Error Reporting takes over, and the exit code is usually 0xC0000409 but not always.
// A parent process then cannot tell a crash from exit(). Terminating the process here leaves nothing to Windows Error Reporting.
inline void terminateWithExceptionCodeOnCrash()
{
    WTFSetAbortReporter([] {
        constexpr DWORD failFastExitCode = 0xC0000409;
        TerminateProcess(GetCurrentProcess(), failFastExitCode);
    });
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exception) -> LONG {
        TerminateProcess(GetCurrentProcess(), exception->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && OS(WINDOWS)
