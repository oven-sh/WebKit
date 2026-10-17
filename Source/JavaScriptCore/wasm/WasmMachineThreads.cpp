/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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
#include "WasmMachineThreads.h"

#if ENABLE(WEBASSEMBLY)

#include "MachineStackMarker.h"
#include <wtf/NeverDestroyed.h>

#if OS(FREEBSD)
#include <sys/membarrier.h>
#endif

namespace JSC { namespace Wasm {


inline MachineThreads& wasmThreads()
{
    static LazyNeverDestroyed<MachineThreads> threads;
    static std::once_flag once;
    std::call_once(once, [] {
        threads.construct();
    });

    return threads;
}

void startTrackingCurrentThread()
{
    wasmThreads().addCurrentThread();
}

void barrierInstructionCacheOnAllThreads()
{
#if CPU(X86_64)
    return;
#else
#if OS(FREEBSD)
    // Ask the kernel to force a context-synchronizing event (an ISB on ARM64) on every running
    // thread in this process, over IPIs. This is the mechanism the old FIXME here wanted: it
    // publishes the modified code without any signal handler, without ThreadSuspendLocker, and
    // without touching the GC's thread-suspension machinery -- so it cannot participate in the
    // two-GC / JITWorklist stop-the-world inversion, and it has no handshake that could be lost.
    static std::once_flag registerOnce;
    static bool membarrierSyncCoreAvailable = false;
    std::call_once(registerOnce, [] {
        membarrierSyncCoreAvailable = membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0) == 0;
    });
    if (membarrierSyncCoreAvailable && membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0) == 0)
        return;
#endif
    // Fallback for kernels without membarrier (or non-FreeBSD).
    Locker locker { wasmThreads().getLock() };
    for (auto& thread : wasmThreads().threads(locker))
        thread->barrierInstructionCache();
#endif
}

    
} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY)
