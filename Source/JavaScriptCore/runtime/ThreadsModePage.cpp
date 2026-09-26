/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include "ThreadsModePage.h"

#include <mutex>
#include <wtf/Assertions.h>
#include <wtf/InlineASM.h>
#include <wtf/WTFConfig.h>

// This file defines the page that ThreadsModePage.h declares const, and is the only one that writes it. The definition
// has another name in C++ and the declaration's name as its symbol: this file does not read through the declaration
// (it is not part of a unified source, so that nothing else in its translation unit does either).
extern "C" {
alignas(WTF::ConfigAlignment) JS_EXPORT_PRIVATE uint8_t g_jscThreadsModePageStorage[WTF::ConfigAlignment] __asm__(SYMBOL_STRING(g_jscThreadsModePage));
#if COMPILER(GCC_COMPATIBLE) && defined(__ELF__)
// The hidden name JavaScriptCore itself reads the page through (ThreadsModePage.h).
extern uint8_t g_jscThreadsModePageLocalStorage[WTF::ConfigAlignment] __asm__("g_jscThreadsModePageLocal") __attribute__((alias("g_jscThreadsModePage"), visibility("hidden")));
#endif
}

namespace JSC {

static bool s_threadsModePageIsFrozen;

void latchThreadsModePage(uint8_t mode)
{
    // A store to the frozen page would fault; a process without JS threads never stores (the page is zero from load).
    if (!mode)
        return;
    RELEASE_ASSERT(!s_threadsModePageIsFrozen);
#if PLATFORM(COCOA)
    WTF::makePagesFreezable(g_jscThreadsModePageStorage, WTF::ConfigAlignment);
#endif
    g_jscThreadsModePageStorage[ThreadsModeByteMode] = mode;
    g_jscThreadsModePageStorage[ThreadsModeByteJSThreads] = !!(mode & ThreadsModeJSThreads);
    g_jscThreadsModePageStorage[ThreadsModeByteTaggedButterflies] = !!(mode & ThreadsModeTaggedButterflies);
    g_jscThreadsModePageStorage[ThreadsModeByteGILOffProcess] = !!(mode & ThreadsModeGILOffProcess);
    g_jscThreadsModePageStorage[ThreadsModeByteSharedGCHeap] = !!(mode & ThreadsModeSharedGCHeap);
}

void threadsModeBodyMismatch()
{
    // A body compiled for one threads mode ran in a process of the other (JSC_THREADS_MODE_BODY).
    CRASH();
}

void freezeThreadsModePage()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (g_wtfConfig.disabledFreezingForTesting)
            return;
        WTF::permanentlyFreezePages(g_jscThreadsModePageStorage, WTF::ConfigAlignment, WTF::FreezePagePermission::ReadOnly);
        s_threadsModePageIsFrozen = true;
    });
}

} // namespace JSC
