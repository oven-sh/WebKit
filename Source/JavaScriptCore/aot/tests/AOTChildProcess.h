/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <span>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

enum class ChildProcessOutcome : uint8_t { Exited, Crashed, Hung, NotStarted };

struct ChildProcessResult {
    bool exitedWith(int exitCode) const { return outcome == ChildProcessOutcome::Exited && code == exitCode; }
    bool hasCrashedOrHung() const { return outcome == ChildProcessOutcome::Crashed || outcome == ChildProcessOutcome::Hung; }
    String description() const;
    String descriptionOfOutcome() const;

    ChildProcessOutcome outcome { ChildProcessOutcome::NotStarted };
    int code { 0 };
    String output;
    String commandLine { };
};

void setProgramPath(String&&);
const String& programPath();

void enterTemporaryFolder();
String temporaryFolder();

void installTrapReporter();

inline constexpr const char* crashRequestVariable = "TESTAOT_CRASH_ON_REQUEST";

inline constexpr ASCIILiteral lineAfterChildProcess = "a child process has ended"_s;

void printLineAfterEachChildProcess();

class ChildProcessLauncher {
public:
    ChildProcessLauncher(String&& program, Vector<String>&& sharedArguments);

    ChildProcessResult run(std::span<const String> arguments) const;

private:
    String m_program;
    Vector<String> m_sharedArguments;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
