/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSystemResources.h"

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <stdlib.h>
#include <wtf/OSAllocator.h>
#include <wtf/StackAllocation.h>

#if OS(WINDOWS)
#include <io.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace JSC { namespace AOT {

// Allocates nothing, and leaves the other threads of the build as they are.
[[noreturn]] static void exitWithError(ASCIILiteral lines)
{
#if OS(WINDOWS)
    _write(2, lines.characters(), static_cast<unsigned>(lines.length()));
#else
    [[maybe_unused]] ssize_t written = write(STDERR_FILENO, lines.characters(), lines.length());
#endif
    _exit(1);
}

[[noreturn]] static void exitOutOfMemory()
{
    exitWithError("error: Out of memory while compiling\n"_s);
}

Ref<Thread> startBuildThread(ASCIILiteral name, Function<void()>&& function)
{
    RefPtr thread = Thread::tryCreate(name, WTF::move(function), ThreadType::Compiler, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(ParallelLoops::workerStackSize));
    if (!thread) [[unlikely]]
        exitWithError("error: Failed to start a compiler thread\nnote: The process or thread limit may have been reached (ulimit -u, or the container's pids limit); raise it or reduce concurrency\n"_s);
    return thread.releaseNonNull();
}

void* allocatePages(size_t bytes)
{
    void* pages = OSAllocator::tryReserveAndCommit(bytes);
    if (!pages) [[unlikely]]
        exitOutOfMemory();
    return pages;
}

void* reservePages(size_t bytes)
{
    void* pages = OSAllocator::tryReserveUncommitted(bytes, OSAllocator::FastMallocPages);
    if (!pages) [[unlikely]]
        exitOutOfMemory();
    return pages;
}

void commitPages(void* pages, size_t bytes)
{
#if OS(WINDOWS)
    if (!VirtualAlloc(pages, bytes, MEM_COMMIT, PAGE_READWRITE)) [[unlikely]]
        exitOutOfMemory();
#else
    OSAllocator::commit(pages, bytes, true, false);
#endif
}

void releasePages(void* pages, size_t bytes)
{
#if OS(WINDOWS)
    UNUSED_PARAM(bytes);
    VirtualFree(pages, 0, MEM_RELEASE);
#else
    munmap(pages, bytes);
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
