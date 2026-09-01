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
struct CallLinkRecord;

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
    // ~CodeBlock during the GC sweep). Drops the ref inline in BOTH flag
    // arms (B14 / MC-DOS S7: the chartered flag-on leak is closed). Per the
    // hard rule above and I7, machine code may be released only after R2's
    // conservative scan of ALL mutator stacks proves it unreachable — and
    // that scan now exists (Heap::gatherStackRoots, §10.6/T6: one
    // MachineThreads scan covers all N mutators, with *m_codeBlocks keeping
    // any block whose code is on any stack). The very GC whose sweep is
    // calling this ran R2; CallLinkInfo::visitWeak unlinked dead callees;
    // §5.8 publish-time pins kept any record-named block alive (and those
    // pins now expire via the epoch path above, so they no longer accumulate).
    // Deliberately NOT routed through the epoch facility: epoch expiry must
    // never free machine code (I7). The CommonData / CallLinkInfos this owns
    // are released with it; ~CallLinkInfo unlinks each from the callee's
    // m_incomingCalls list (the prior leak-arm "stays on the list forever"
    // residual is gone).
    static void retireOptimizedJITCode(VM&, RefPtr<JITCode>&&);
#endif

    // Retire an arbitrary non-executable artifact: the callback is destroyed
    // at epoch expiry.
    static void retire(VM&, std::unique_ptr<RetiredCallback>&&);

    // Pin a §5.8 call-link record's named CodeBlock as a validated GC root on
    // the epoch heap. Called by the publishRecord paths BEFORE the record
    // becomes reachable, while the caller still provably holds the cell live
    // (it is mid-link, on the publishing mutator's stack/registers), so the
    // marking constraint keeps the cell continuously marked from the first
    // cycle the pin is visible — the cell can never become unmarked, never
    // leaves codeBlockSet, and the validation arm never has to skip it.
    //
    // Rationale (w16 follow-up jit-null-metadatatable-counter-bump, AMENDED):
    // a dispatcher loads r = m_record and stores r->codeBlockToTransfer into
    // the callee frame; until that store lands, the named CodeBlock CELL is
    // reachable only through the record, which the conservative scan does not
    // trace through. The original retire-time-only pin left two arms open,
    // and the crash family reproduced on it (callee-frame CodeBlock with a
    // null baseline-only field, e.g. m_argumentValueProfiles at +0xa0 or the
    // jitData tier-up counter at +0x28):
    //  (1) LIVE records had no pin at all — nothing kept a still-published
    //      record's named cell alive against a marking cycle that the
    //      dispatch window straddles (DataIC spills / DirectCallLinkInfo
    //      carry no comparand whose conservative liveness could stand in);
    //  (2) records retired on GC-internal paths (visitWeak ->
    //      unlinkOrUpgrade -> clearRecord with the cell already unmarked)
    //      pinned too late: the cell left codeBlockSet at the end of that
    //      same cycle, so the validation arm skipped the pin forever
    //      ("retention, never resurrection") and the slot was recycled.
    // Pinning at publish closes both: the pin spans the record's whole
    // reachable lifetime (live, then retired until epoch expiry — or forever
    // under the flag-on leak arm, matching the record leak it makes sound).
    // No-op for null codeBlockToTransfer (virtual/host records).
    static void pinPublishedCallLinkRecordCodeBlock(VM&, CodeBlock*);

    // Retire a replaced/unlinked §5.8 call-link record (the sole owner of the
    // record pointer after the m_record exchange). Takes over the record's
    // publish-time pin (see pinPublishedCallLinkRecordCodeBlock above): the
    // callback's destructor unpins at epoch expiry — never, under the flag-on
    // leak arm. See Heap::pinRetiredCallLinkRecordCodeBlock for the
    // marking/validation contract. No-op for null records.
    static void retireCallLinkRecord(VM&, CallLinkRecord*);
};

} // namespace JSC
