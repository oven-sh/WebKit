/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <cstdint>
#include <wtf/Compiler.h>
#include <wtf/Noncopyable.h>

// RaceAmplifier: randomized scheduling perturbation for hunting data races.
//
// When enabled (Options::randomYieldPeriod() != 0), RaceAmplifier::perturb()
// injects a sched_yield or a short randomized sleep, on average once every
// `randomYieldPeriod` calls per thread. The per-thread decision stream is
// deterministic for a given Options::randomYieldSeed(), so a seed that
// surfaces a crash can be replayed (modulo OS scheduling). When the option is
// off (the default), perturb() is a single load of a process-global word and
// a never-taken predicted branch, and the static initialization is trivial —
// effectively zero cost, and exactly zero cost at the sites that gate it
// behind an existing slow-path branch.
//
// Call sites belong on slow paths only — never in JIT-emitted fast paths,
// LLInt assembly, or allocation fast paths. Each landed site sits in a
// window where one thread publishes state that another thread may be racing
// to observe (grep RaceAmplifier::perturb for the exact lines):
//
//   - Mutator allocation slow paths: LocalAllocator::allocateSlowCase and
//     tryAllocateWithoutCollecting, CompleteSubspace::tryAllocateSlow and
//     tryAllocateSlowForClient, around block handout.
//   - Heap access handshakes: Heap::acquireHeapAccess, releaseHeapAccess
//     and detachCurrentThread.
//   - Epoch-based reclamation: GCSafepointEpoch::retire and bumpAndReclaim.
//   - Thread-id lifecycle: ThreadManager::retireCarrierTID and the TID
//     rebias run under a shared-heap stop (heap/Heap.cpp).
//   - Thread and VM teardown: tearDownSpawnedThreadForExit
//     (runtime/ThreadManager.cpp), carrier teardown at thread death
//     (runtime/JSLock.cpp) and foreign-carrier collection during VM
//     destruction (runtime/VM.cpp), between the unregister, detach and
//     destroy steps.
//
// Usage at a call site:
//
//     #include "RaceAmplifier.h"
//     ...
//     RaceAmplifier::perturb();
//
// Initialization: RaceAmplifier::initialize() must be called once after
// Options are finalized; the VM constructor does this. Calling perturb()
// before initialize() is safe and does nothing.

namespace JSC {

class RaceAmplifier {
    WTF_MAKE_NONCOPYABLE(RaceAmplifier);
    RaceAmplifier() = delete;

public:
    // Reads Options::randomYieldPeriod() / Options::randomYieldSeed() /
    // Options::randomYieldMaxMicroseconds() and arms the amplifier.
    // Idempotent; safe to call from multiple VM constructions.
    JS_EXPORT_PRIVATE static void initialize();

    // The injection point. On the off path this is one non-atomic load and a
    // fall-through branch; keep call sites on slow paths regardless.
    ALWAYS_INLINE static void perturb()
    {
        if (s_period) [[unlikely]]
            perturbSlow();
    }

private:
    JS_EXPORT_PRIVATE static void perturbSlow();

    // 0 means disabled. Written once during initialize(), before any
    // amplified thread can observe it; read racily (benign) thereafter.
    JS_EXPORT_PRIVATE static unsigned s_period;
    JS_EXPORT_PRIVATE static uint64_t s_seed;
    JS_EXPORT_PRIVATE static unsigned s_maxSleepMicroseconds;
};

} // namespace JSC
