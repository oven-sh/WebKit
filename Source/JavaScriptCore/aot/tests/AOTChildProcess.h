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
    String description() const;

    ChildProcessOutcome outcome { ChildProcessOutcome::NotStarted };
    int code { 0 };
    String output;
};

void setProgramPath(String&&);
const String& programPath();

// Gives this process a folder of its own for temporary files, inside the folder of the process that started it. What this
// process starts inherits it. The folder goes when this process exits, and ChildProcessLauncher removes it after a child that
// ended in any other way.
void enterTemporaryFolder();
String temporaryFolder();

// Before this process dies of a trap, it prints the thread, the registers, the return addresses and the top of the stack.
void installTrapReporter();

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
