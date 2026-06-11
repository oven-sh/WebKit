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
#include "RetiredJITArtifacts.h"

#include "GCAwareJITStubRoutine.h"
#include "Heap.h"
#include "InlineCacheHandler.h"
#include "JITCode.h"
#include "Options.h"
#include "VM.h"
#include <wtf/Lock.h>

#if __has_include("HeapClientSet.h")
// Heap workstream landed: GCClient::Heap::server() resolution is meaningful
// (same gate as bytecode/JSThreadsSafepoint.cpp). THREADS-INTEGRATE(jit)
#define JSC_JIT_HAS_SHARED_HEAP_SERVER 1
#endif

// N6 shim (SPEC-jit section 2 / section 4.4): the epoch facility is owned by
// the heap workstream (heap/GCSafepointEpoch.h, SPEC-heap section 11). Bodies
// below compile against it iff it has landed; until then they are no-op
// leak-until-integration stubs. The leak is sound pre-integration: the phase-1
// GIL stub admits no concurrent retirement, and flag-off there are no callers
// (the Task-3 rerouting of resetStubAsJumpInAccess, initializeWithUnitHandler
// displacement, and jettison-time PropertyInlineCache::deref(VM&) is gated on
// Options::useJSThreads()). Ordering note recorded in INTEGRATE-jit.md: heap must land
// before Task 13's epoch tests run. THREADS-INTEGRATE(jit)
#if __has_include("GCSafepointEpoch.h")
#include "GCSafepointEpoch.h"
#define JSC_JIT_HAS_GC_SAFEPOINT_EPOCH 1
#endif

namespace JSC {

// R4-2 (review round 4): resolve the heap whose safepoint epoch governs the
// retired data's lifetime — the SERVER heap this VM's client attaches to.
// Under useSharedGCHeap a client VM's vm.heap is not the shared server and its
// local epoch does not track the real mutator population (see the header
// comment); for the 1:1 case server() == vm.heap. Gate fallback mirrors
// JSThreadsSafepoint.cpp: without HeapClientSet.h no foreign-client shared
// server can exist, so vm.heap is the only candidate.
[[maybe_unused]] static JSC::Heap& epochHeapFor(VM& vm)
{
#if defined(JSC_JIT_HAS_SHARED_HEAP_SERVER)
    return vm.clientHeap.server();
#else
    return vm.heap;
#endif
}

// Epoch-granularity guard (fix for spawned-thread UAF in unmapped JIT memory,
// JSTests/threads/jit/{tid-tag-3-threads,int-gate-stop-budget,
// spawned-thread-butterfly-stress}.js): the heap epoch facility
// (heap/GCSafepointEpoch.h) tracks safepoint crossings at GCClient::Heap
// granularity — ONE client per VM. Under the phase-1 GIL stub every
// Thread()-spawned JS thread runs in the SAME VM, i.e. the same single
// client, and the reclaim sequence (Heap::runSafepointHooksAndReclaim)
// unconditionally stamps that one client at every collection. A sibling
// thread parked with the GIL dropped (join / cond.wait / Atomics.wait /
// lock.hold — all mid-operation park sites) therefore never holds an epoch
// back: two collections can complete while it is parked, expiring artifacts
// (call-link records, handler-chain nodes, and the stub-routine refs they
// drop into jettison) that the parked thread's resumed LLInt/IC dispatch
// state still reaches — a stale entrypoint into reclaimed JIT memory.
// "Every registered MUTATOR THREAD crossed the epoch" is the §4.4 soundness
// requirement; the landed facility can only prove "every registered CLIENT
// was stamped". Until per-thread epoch publication lands in the heap
// workstream (clients stamped only when each spawned thread has passed a
// safepoint), flag-on retirement must keep the chartered
// leak-until-integration behavior (sound: never frees). Flag-off there are
// no callers, so nothing is lost by gating. THREADS-INTEGRATE(jit)
[[maybe_unused]] static bool epochCoversEveryJSThread(VM&)
{
    return !Options::useJSThreads();
}

#if ENABLE(JIT)

#if defined(JSC_JIT_HAS_GC_SAFEPOINT_EPOCH)
namespace {

// Holds a retired handler chain until its retirement epoch expires. The
// destructor (run at expiry) derefs the chain: node payloads are pure data
// (G2), and each node's Ref<GCAwareJITStubRoutine> drops into the jettisoned-
// stub-routine machinery, so the machine code is deleted only after the GC's
// conservative scan of all mutator stacks proves it off-stack (R2, I7) - never
// by epoch expiry alone.
class RetiredHandlerChain final : public RetiredCallback {
public:
    explicit RetiredHandlerChain(RefPtr<InlineCacheHandler>&& head)
        : m_head(WTF::move(head))
    {
    }

private:
    RefPtr<InlineCacheHandler> m_head;
};

} // anonymous namespace
#endif // defined(JSC_JIT_HAS_GC_SAFEPOINT_EPOCH)

void RetiredJITArtifacts::retireHandlerChain(VM& vm, RefPtr<InlineCacheHandler>&& head, DisarmClearingWatchpoints disarm)
{
    if (!head)
        return;

    // SPEC-jit section 4.4 hard rule: only GC-aware stub routines may ride a
    // retired chain; their executable memory is freed by the jettison + R2
    // scan path, never by epoch expiry.
    //
    // Pre-compiled unit handlers (createPreCompiledICJITStubRoutine: data-only
    // handlers whose machine code is a shared immutable CTI thunk owned by the
    // VM, e.g. GetByIdLoadOwnPropertyHandler) are created WITHOUT makeGCAware()
    // as a flag-off optimization, and a chain displaced by megamorphic
    // promotion (initializeWithUnitHandler) or reset can legally carry them.
    // Promote them lazily here: makeGCAware() registers the routine with the
    // heap's JITStubRoutineSet (immutable-code list, JITStubRoutineSet.cpp),
    // so the epoch-expiry deref jettisons it into the GC machinery instead of
    // running the non-GC-aware inline `delete this` - preserving the hard
    // rule's actual guarantee that nothing reachable from JIT'd dispatch is
    // freed by epoch expiry alone. JITStubRoutineSet::add RELEASE_ASSERTs
    // !isCompilationThread(), which matches this facility's mutator-side call
    // sites (IC slow paths, world-stopped resets, jettison).
    //
    // AB18: FIXME (a) is RESOLVED — flag-on, createPreCompiledICJITStubRoutine
    // now calls makeGCAware at CREATION (single-threaded per routine,
    // pre-publication), so the lazy promotion below is reachable flag-off ONLY
    // (single mutator, race-free). Flag-on it would be the race the old FIXME
    // described: two mutators retiring chains that share one stateless
    // precompiled stub racing the !isGCAware()/makeGCAware() pair and
    // double-appending to JITStubRoutineSet (double jettison/delete, same UAF
    // family as AB18-E/S1) — hence the fail-stop below instead of a mutation.
    //
    // FIXME(THREADS-INTEGRATE(jit)) (b, still open): makeGCAware registers
    // with vm.heap, not epochHeapFor(vm); revisit which heap's
    // JITStubRoutineSet shared-server clients register with when
    // useSharedGCHeap lands (this now applies uniformly to ALL stub creation
    // sites, which also register with the creating vm.heap).
    // TSAN ic-stubinfo §10.4 ("disarmClearingWatchpointOnRetire self", family-4
    // lock ruling): the SAME InlineCacheHandler node can be retired from two
    // threads at once (e.g. a chain retired at jettison and again at IC
    // teardown — the double-retire is documented as harmless for the epoch
    // refs in PropertyInlineCache::deref — or a SharedJITStubSet handler
    // reachable from two CodeBlocks). Both walkers then run
    // m_watchpoint.reset() concurrently: a write-write race on the unique_ptr
    // that can DOUBLE-DELETE the PropertyInlineCacheClearingWatchpoint. This
    // is a real bug, not an annotation candidate; serialize the disarm walk
    // under a process-wide leaf lock (retirement is a slow path: jettison,
    // reset, teardown). Lock order: this lock only wraps the walk below and
    // nests OVER g_watchpointMembershipLock (taken inside ~Watchpoint); it is
    // never taken while holding that lock, and no park/JS can run under it.
    static Lock retiredChainDisarmLock;
    {
        Locker locker { retiredChainDisarmLock };
        for (auto* cursor = head.get(); cursor; cursor = cursor->next()) {
            if (auto* routine = cursor->stubRoutine()) {
                if (!routine->isGCAware()) [[unlikely]] {
                    RELEASE_ASSERT(!Options::useJSThreads()); // Flag-on every retirable routine is GC-aware at creation; promoting a published routine here would race (see above).
                    routine->makeGCAware(vm);
                }
                RELEASE_ASSERT(routine->isGCAware());
            }
            // AB18-F (sig-1 family, amended at thread-closeout final review):
            // disarm each DISPLACED/DYING handler's owner-clearing watchpoint
            // NOW, on BOTH arms below. The epoch arm defers destruction past
            // the owner CodeBlock's possible death, and the leak arm never
            // destroys at all — either way an armed
            // PropertyInlineCacheClearingWatchpoint would survive on a live
            // WatchpointSet with m_owner pointing at a sweepable CodeBlock;
            // a later fire would read the dead cell (fireInternal's
            // wasDestructed()/isPendingDestruction() guards are themselves the
            // UAF once the MarkedBlock is freed). ~CodeBlock cannot cover these:
            // its aboutToDie() walk sees only chains still ATTACHED to the IC,
            // and displaced chains are by definition not. Disarming here is
            // exactly the flag-off behavior for the DISPLACEMENT and
            // ~CodeBlock callers (inline destruction of the displaced chain at
            // the same program point destroys the same watchpoint). It is NOT
            // flag-off behavior for the jettison-time extra-ref retire, whose
            // chains stay INSTALLED with a live owner — that caller passes
            // DisarmClearingWatchpoints::No so a post-jettison watched-set
            // fire still resets the chain for straggler baseline frames (see
            // the enum comment in RetiredJITArtifacts.h).
            if (disarm == DisarmClearingWatchpoints::Yes)
                cursor->disarmClearingWatchpointOnRetire();
        }
    }

#if defined(JSC_JIT_HAS_GC_SAFEPOINT_EPOCH)
    if (epochCoversEveryJSThread(vm)) {
        epochHeapFor(vm).safepointEpoch().retire(std::unique_ptr<RetiredCallback>(new RetiredHandlerChain(WTF::move(head))));
        return;
    }
#endif
    // Leak-until-integration stub (N6): never free inline - a JIT'd reader on
    // another thread (post-GIL) could still hold a node pointer inside its
    // safepoint-free window. Pre-integration the GIL makes the leak the only
    // cost. Also taken flag-on while the epoch facility's per-client
    // granularity cannot prove every spawned JS thread crossed the epoch
    // (see epochCoversEveryJSThread). THREADS-INTEGRATE(jit)
    UNUSED_PARAM(vm);
    (void)head.leakRef();
}

void RetiredJITArtifacts::retireOptimizedJITCode(VM& vm, RefPtr<JITCode>&& jitCode)
{
    UNUSED_PARAM(vm);
    if (!jitCode)
        return;

    if (!Options::useJSThreads()) [[likely]] {
        // Today's behavior: dropping the (typically last) ref releases the
        // ExecutableMemoryHandle inline during the sweep.
        jitCode = nullptr;
        return;
    }

    // Leak-until-integration (see the header comment): flag-on, the sweep
    // cannot prove that a parked sibling thread's stack and call-link/IC
    // records do not reach this machine code (R2's N-stack conservative scan
    // is a heap-workstream deliverable), and the epoch facility must never
    // free machine code (I7 hard rule). Leaking keeps every published
    // entrypoint, section-5.8 record, and CallLinkInfo address-valid, which
    // is sound. THREADS-INTEGRATE(jit)
    (void)jitCode.leakRef();
}

#endif // ENABLE(JIT)

void RetiredJITArtifacts::retire(VM& vm, std::unique_ptr<RetiredCallback>&& callback)
{
    if (!callback)
        return;

#if defined(JSC_JIT_HAS_GC_SAFEPOINT_EPOCH)
    if (epochCoversEveryJSThread(vm)) {
        epochHeapFor(vm).safepointEpoch().retire(WTF::move(callback));
        return;
    }
#endif
    // Leak-until-integration stub (N6): also taken flag-on while the epoch
    // facility's per-client granularity cannot prove every spawned JS thread
    // crossed the epoch (see epochCoversEveryJSThread). THREADS-INTEGRATE(jit)
    UNUSED_PARAM(vm);
    (void)callback.release();
}

} // namespace JSC
