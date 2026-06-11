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

#pragma once

#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>

namespace JSC {

class Heap;
class InlineCacheHandler;
class JITCode;
class VM;

// RetiredJITArtifacts (SPEC-jit section 4.4, P1): stateless adapter routing
// retired, NON-EXECUTABLE JIT-side data (handler-IC chain nodes, replaced
// call-link records, IC metadata) into the heap workstream's epoch facility
// (heap/GCSafepointEpoch.h, reached via the epoch heap's safepointEpoch()).
// Items retired here are destroyed only once every mutator has crossed a
// safepoint after retirement, so JIT'd code that loaded a pointer to the data
// before the retirement (and, per the safepoint-free-window rule G2/I16,
// cannot still hold it across a safepoint) can never touch freed memory.
//
// R4-2 (review round 4): the API takes VM&, NOT Heap&, and resolves the epoch
// heap internally as the SERVER heap the VM's GCClient::Heap attaches to
// (vm.clientHeap.server()). The §4.4 soundness argument requires the epoch to
// be advanced by the safepoints of the SAME mutator population that can hold
// pointers into the retired data; under useSharedGCHeap a client VM's own
// vm.heap is NOT the shared server (its local Heap is idle — see the R3-11
// notes in bytecode/JSThreadsSafepoint.cpp), so routing retirement through
// vm.heap there would free against the wrong (trivially-advancing) epoch — a
// use-after-free for a reader mid-dispatch on a server-tracked mutator — or
// never free at all. For the 1:1 case server() == vm.heap. Centralizing the
// resolution here keeps call sites (PropertyInlineCache, CallLinkInfo,
// CodeBlock jettison) unable to pick the wrong heap.
//
// Hard rule: epoch expiry frees only data whose every JIT-side access sits in
// a safepoint-free window - NEVER machine code. An expired handler chain drops
// its Ref<GCAwareJITStubRoutine>s into the existing jettison machinery; the
// executable memory itself is released only on the GC sweep after the
// conservative scan of all mutator stacks (R2, I7).
//
// Callable from any mutator thread; not from compilation threads when a
// retired chain carries a not-yet-GC-aware routine, since lazy promotion in
// retireHandlerChain goes through JITStubRoutineSet::add, which RELEASE_ASSERTs
// !isCompilationThread(). May be called while holding a cell/Structure
// 2-bit lock (lock-order rank 10); MUST NOT be called from heap-internal
// contexts of heap ranks 7-9 (SPEC-heap section 13.10f). Not async-signal-safe.
class RetiredCallback {
    WTF_MAKE_NONCOPYABLE(RetiredCallback);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(RetiredCallback);
public:
    RetiredCallback() = default;

    // Destruction IS the deferred action: the destructor runs at epoch expiry
    // (or never, under the pre-integration leak stub - see the .cpp).
    virtual ~RetiredCallback() = default;
};

// Thread-closeout final review (AB18-F amendment): whether retireHandlerChain
// disarms each node's PropertyInlineCacheClearingWatchpoint. Yes is correct
// ONLY for chains that are genuinely DISPLACED/DETACHED at the retire site
// (megamorphic promotion, resetStubAsJumpInAccess, reset()) or whose owner
// CodeBlock is dying (~CodeBlock) — there the disarm reproduces flag-off's
// inline destruction of the same watchpoint at the same program point. No is
// REQUIRED for the jettison-time extra-ref retire (PropertyInlineCache::
// deref(VM&) from CodeBlock::jettison): those chains are deliberately LEFT
// INSTALLED with a live owner ("dispatch keeps working until invalidation",
// I21), and disarming them would let a post-jettison fire of the watched set
// silently skip the IC reset — a straggler baseline frame (no invalidation
// points) would keep dispatching a stale handler and return wrong values.
// Flag-off at the jettison program point destroys NOTHING, so No is the
// flag-off-equivalent arm there. The ~CodeBlock re-retire of the same chain
// performs the disarm at death.
enum class DisarmClearingWatchpoints : bool { No, Yes };

class RetiredJITArtifacts {
public:
    // Retire a (detached) handler-IC chain head. Every node's stub routine
    // must be GC-aware by the time the chain is parked (RELEASE_ASSERTed);
    // non-GC-aware pre-compiled unit handlers (data-only handlers over shared
    // immutable thunk code, createPreCompiledICJITStubRoutine) are promoted
    // via makeGCAware() on entry. Node data is freed at epoch expiry, but the
    // machine code rides the jettisoned-stub-routine path and waits for R2's
    // conservative scan (I7).
#if ENABLE(JIT)
    static void retireHandlerChain(VM&, RefPtr<InlineCacheHandler>&& head, DisarmClearingWatchpoints);

    // Retire a dying optimized (DFG/FTL) CodeBlock's JITCode (called from
    // ~CodeBlock during the GC sweep). Flag-off this just drops the ref
    // inline - exactly today's behavior. Flag-on it NEVER frees: per the hard
    // rule above and I7, machine code may be released only after R2's
    // conservative scan of ALL mutator stacks proves it unreachable, and that
    // scan does not exist yet under the phase-1 GIL stub - the sweep cannot
    // see a sibling spawned thread parked with the GIL dropped, whose
    // call-link/IC dispatch state still targets this code's entrypoints
    // (observed as llint_op_call jumping into unmapped memory on resume,
    // JSTests/threads/jit/tid-tag-3-threads.js et al.). Until the heap
    // workstream's N-stack scan lands, the JITCode (and the CommonData /
    // CallLinkInfos it owns) is leaked - the same chartered
    // leak-until-integration behavior as the epoch paths. Note the leak also
    // keeps the CommonData's CallLinkInfos alive, so a dead caller's nodes
    // stay (validly) on other CodeBlocks' m_incomingCalls lists - their
    // ~CallLinkInfo never runs; record for R2 integration. Deliberately NOT
    // routed through the epoch facility even once it is live: epoch expiry
    // must never free machine code (I7). THREADS-INTEGRATE(jit)
    static void retireOptimizedJITCode(VM&, RefPtr<JITCode>&&);
#endif

    // Retire an arbitrary non-executable artifact: the callback is destroyed
    // at epoch expiry.
    static void retire(VM&, std::unique_ptr<RetiredCallback>&&);
};

} // namespace JSC
