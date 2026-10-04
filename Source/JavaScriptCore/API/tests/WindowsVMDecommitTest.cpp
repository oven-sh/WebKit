/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WindowsVMDecommitTest.h"

#include <stdio.h>
#include <wtf/Assertions.h>

#if OS(WINDOWS)
#include <windows.h>
#include <psapi.h>
#include <wtf/OSAllocator.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if OS(WINDOWS)
static SIZE_T workingSetSize()
{
    PROCESS_MEMORY_COUNTERS counters;
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return 0;
    return counters.WorkingSetSize;
}
#endif

void testWindowsVMDecommit()
{
#if OS(WINDOWS)
    printf("Testing Windows OSAllocator::hintMemoryNotNeededSoon releases pages (bun#30562)\n");

    // 64 MiB is large enough to be unambiguous against background working-set
    // noise (a few MiB of jitter between snapshots is normal) yet small enough
    // to fit comfortably on any CI machine.
    const size_t regionSize = 64 * 1024 * 1024;

    // Reserve + commit a single region so we know the exact page range and so
    // VirtualQuery in the fallback path walks over reservations we control.
    void* region = VirtualAlloc(nullptr, regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    RELEASE_ASSERT(region);

    // Touch every page to bring them into the working set. Without this,
    // Windows lazy-commits — the pages won't show up in WorkingSetSize and
    // there'd be nothing to observe being released.
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    const size_t pageSize = systemInfo.dwPageSize;
    volatile unsigned char* bytes = static_cast<unsigned char*>(region);
    for (size_t offset = 0; offset < regionSize; offset += pageSize)
        bytes[offset] = static_cast<unsigned char>(offset & 0xff);

    const SIZE_T before = workingSetSize();

    // The call under test. Before the fix this was an empty stub — the
    // working set would not change. After the fix it calls DiscardVirtualMemory
    // (or the MEM_RESET + VirtualUnlock fallback) which evicts the pages.
    WTF::OSAllocator::hintMemoryNotNeededSoon(region, regionSize);

    const SIZE_T after = workingSetSize();

    VirtualFree(region, 0, MEM_RELEASE);

    printf("  working set before: %zu bytes\n", static_cast<size_t>(before));
    printf("  working set after : %zu bytes\n", static_cast<size_t>(after));

    // Allow for the OS to keep a small residue in the working set. We require
    // at least half the region to have been evicted — the previous empty-stub
    // implementation would produce ~0 bytes of drop.
    const SIZE_T minDrop = regionSize / 2;
    if (before < after || before - after < minDrop) {
        fprintf(stderr, "FAIL: hintMemoryNotNeededSoon did not release pages — "
            "working set drop was %zu bytes, expected at least %zu bytes.\n",
            before > after ? static_cast<size_t>(before - after) : static_cast<size_t>(0),
            static_cast<size_t>(minDrop));
        RELEASE_ASSERT_NOT_REACHED();
    }
    printf("PASS: hintMemoryNotNeededSoon released %zu bytes from the working set\n",
        static_cast<size_t>(before - after));
#else
    printf("testWindowsVMDecommit: skipped (not Windows)\n");
#endif
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
