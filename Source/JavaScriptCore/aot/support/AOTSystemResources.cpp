/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSystemResources.h"

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTThreads.h"
#include <array>
#include <stdlib.h>
#include <wtf/StackAllocation.h>

#if OS(WINDOWS)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace JSC { namespace AOT {

static void writeToStandardError(std::span<const char> lines)
{
#if OS(WINDOWS)
    _write(2, lines.data(), static_cast<unsigned>(lines.size()));
#else
    [[maybe_unused]] ssize_t written = write(STDERR_FILENO, lines.data(), lines.size());
#endif
}

[[noreturn]] static void exitWithError(ASCIILiteral lines)
{
    writeToStandardError(lines.span());
    _exit(1);
}

Ref<Thread> startBuildThread(ASCIILiteral name, Function<void()>&& function)
{
    RefPtr thread = Thread::tryCreate(name, WTF::move(function), ThreadType::Compiler, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(ParallelLoops::workerStackSize));
    if (!thread) [[unlikely]]
        exitWithError("error: Failed to start a compiler thread\nnote: The process or thread limit may have been reached (ulimit -u, or the container's pids limit); raise it or reduce concurrency\n"_s);
    return thread.releaseNonNull();
}

void exitIfBuildIsOutOfMemory()
{
    std::array<char, 768> place;
    size_t length = describeCurrentThreadWork(place);
    if (!length)
        return;
    writeToStandardError("error: Out of memory while compiling\n"_s.span());
    writeToStandardError(std::span<const char>(place).first(length));
    _exit(1);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
