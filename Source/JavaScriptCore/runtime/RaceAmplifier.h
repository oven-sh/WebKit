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
// Intended call sites (slow paths ONLY — never in JIT-emitted fast paths,
// LLInt assembly, or allocation fast paths). These are the safepoint-adjacent
// windows where the shared-memory Thread work (see THREAD.md and
// docs/threads/SPEC-*.md) is most likely to harbor interleaving bugs.
// The list below is the integration plan; call sites land with the
// workstream that owns each file:
//
//   Object model / transitions:
//   - JSObject::putDirectSlow / putByIdSlow paths (JSObject.cpp), between
//     deciding a transition is needed and publishing the new butterfly —
//     widens the flat->segmented conversion race window.
//   - Structure::addPropertyTransition / nonPropertyTransition
//     (Structure.cpp), before and after the transition-table lookup.
//   - JSObject butterfly (re)allocation: allocateMoreOutOfLineStorage,
//     ensureLengthSlow / array storage conversions (JSObject.cpp,
//     JSArray.cpp), between allocate and store-with-fence.
//   - JSCellLock::lockSlow / unlockSlow (JSCellInlines.h / Heap.cpp):
//     immediately after acquiring and immediately before releasing the
//     per-object lock.
//
//   Heap / GC:
//   - LocalAllocator::allocateSlowCase (heap/LocalAllocator.cpp), at the
//     marked FIXMEs where synchronized block handout will live.
//   - BlockDirectory block handout / FreeList refill paths.
//   - Mutator-side handshake points: Heap::stopIfNecessarySlow, the
//     VMTraps deferred-work loop, and VMManager stop-the-world
//     entry/exit (the N-mutator safepoint machinery).
//
//   JIT / code lifecycle:
//   - Watchpoint fire sites: WatchpointSet::fireAllSlow (bytecode/
//     Watchpoint.cpp) — perturb just before invalidation is published,
//     to chase code running between watchpoint check and fire.
//   - Handler IC case append under CodeBlock::m_lock
//     (bytecode/StructureStubInfo.cpp / InlineCacheCompiler.cpp), between
//     building the new case list and the atomic publish.
//   - CodeBlock::jettison (CodeBlock.cpp), before reclamation is queued —
//     stresses epoch-based reclamation once it exists.
//
//   Shared VM state:
//   - Atom table insertion slow path (AtomStringTable / Identifier::add),
//     once the table goes process-global.
//   - Structure allocation lock in StructureIDTable / Structure creation.
//
// Usage at a call site:
//
//     #include "RaceAmplifier.h"
//     ...
//     RaceAmplifier::perturb();
//
// Initialization: RaceAmplifier::initialize() must be called once after
// Options are finalized (from initializeThreading() / VM construction).
// Calling perturb() before initialize() is safe and does nothing.

namespace JSC {

class RaceAmplifier {
    WTF_MAKE_NONCOPYABLE(RaceAmplifier);
    RaceAmplifier() = delete;

public:
    // Reads Options::randomYieldPeriod() / Options::randomYieldSeed() /
    // Options::randomYieldMaxMicroseconds() and arms the amplifier.
    // Idempotent; safe to call from multiple VM constructions.
    JS_EXPORT_PRIVATE static void initialize();

    static bool isEnabled() { return !!s_period; }

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
