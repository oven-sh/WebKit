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

#include "CallLinkInfo.h"
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

// Epoch-granularity guard — B14 / MC-DOS S7 RESOLVED (per-thread epoch
// publication landed; CVE-AUDIT-RESULTS.md:142, map-MC-DOS.md S7).
//
// Original concern (kept for the regression record): the epoch facility
// (heap/GCSafepointEpoch.h) tracks safepoint crossings at GCClient::Heap
// granularity. Under the phase-1 GIL stub every Thread()-spawned JS thread
// shared the SAME single client, so the reclaim sequence's per-client stamp
// could not prove "every registered MUTATOR THREAD crossed the epoch" — only
// "the one client was stamped". A parked sibling could in principle have its
// epoch advanced underneath it.
//
// Why it now holds, both modes — and what it RESTS ON:
//   (a) Per-thread clients (U-T6, ANNEX A36C / JSLock.cpp
//       perThreadClientForCarrierEntry + the spawned-lite client in
//       ThreadManager.cpp) give every JS thread its OWN GCClient::Heap
//       registered in clientSet(), so the per-client m_localEpoch stamp in
//       Heap::runSafepointHooksAndReclaim IS per-thread, and
//       bumpAndReclaim()'s min-over-clients scan (GCSafepointEpoch.cpp:148)
//       IS the per-thread global minimum the §4.4 soundness argument needs.
//       This is the LOAD-BEARING invariant — there is no separate per-lite
//       witness (an earlier B14 revision added one; dropped at adversarial
//       review as write-only dead code with no reader in the min scan).
//       bumpAndReclaim()'s RELEASE_ASSERT(minLocalEpoch >= oldEpoch)
//       backstops a missed stamp but NOT a missing client.
//   (b) The stamp is sound because it runs only while the world is stopped
//       for all clients (§10 step 7) or legacy worldIsStopped()
//       (runEndPhase): every lite is provably at a safepoint, so per the
//       G2/I16 safepoint-free-window rule cannot be holding a pointer into
//       any data this facility retires (handler-chain nodes, call-link
//       records, generic RetiredCallback payloads — all §4.4 "non-executable,
//       every JIT-side access in a safepoint-free window" by construction).
//       A thread parked at join/cond.wait/Atomics.wait/lock.hold IS at a
//       safepoint for this purpose (it released heap access; the §10 stop
//       counted it); the original "stale entrypoint into reclaimed JIT
//       memory" worry is about MACHINE CODE, which the I7 hard rule keeps
//       out of epoch reclaim entirely (see retireOptimizedJITCode below —
//       that leg is bounded by R2, not by this predicate).
//
// STANDING OBLIGATION (recorded at the B14 hunk in
// Heap::runSafepointHooksAndReclaim): if U-T6 ever weakens — a lite
// executing JS without its own registered GCClient::Heap, or two lites
// sharing one client under a future carrier path — this predicate becomes
// UNSOUND (handler-chain/record UAF) with no functional guard. That change
// MUST either wire a per-lite witness into bumpAndReclaim()'s min scan or
// revert this to false.
//
// Flag-off there are no callers (every retire path is useJSThreads-gated),
// so this returning true unconditionally is flag-off-invisible.
[[maybe_unused]] static bool epochCoversEveryJSThread(VM&)
{
    return true;
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
    // N6 leak stub: now reachable ONLY when GCSafepointEpoch.h has not landed
    // (the !JSC_JIT_HAS_GC_SAFEPOINT_EPOCH build config) —
    // epochCoversEveryJSThread() above always returns true since B14, so the
    // epoch arm is taken whenever the facility exists. Never free inline: a
    // JIT'd reader on another thread could still hold a node pointer inside
    // its safepoint-free window. THREADS-INTEGRATE(jit)
    UNUSED_PARAM(vm);
    (void)head.leakRef();
}

void RetiredJITArtifacts::retireOptimizedJITCode(VM& vm, RefPtr<JITCode>&& jitCode)
{
    UNUSED_PARAM(vm);
    if (!jitCode)
        return;

    // B14 / MC-DOS S7 RESOLVED (R2 leg). Flag-off AND flag-on now drop the
    // ref inline: R2's N-stack conservative scan has landed
    // (Heap::gatherStackRoots, "one MachineThreads scan covers all N
    // mutators", §10.6/T6 — the *m_codeBlocks argument keeps any CodeBlock
    // whose machine code appears as a return address on ANY registered
    // thread's stack), so the very GC whose sweep is calling this already
    // proved no thread's stack reaches this code. The remaining reach paths
    // are closed at that same GC:
    //   - live callers' CallLinkInfos: visitWeak ran during marking and
    //     unlinked every monomorphic/record link to the now-unmarked callee;
    //   - §5.8 records: their codeBlockToTransfer is publish-time pinned
    //     (pinPublishedCallLinkRecordCodeBlock) — a record naming this
    //     CodeBlock would have kept it marked, so ~CodeBlock running means
    //     no live or retired-but-unexpired record names it (and with
    //     epochCoversEveryJSThread now true above, retired records DO expire
    //     and unpin, so this path no longer accumulates a permanent pin set);
    //   - a sibling mid-dispatch (loaded {entrypoint, codeBlock} but not yet
    //     jumped): the load->jump window is safepoint-free (G2). PREMISE:
    //     while mutator-concurrent sweeping is disabled (SPEC-heap.md:23,
    //     useSharedGCIncrementalSweep=false default OptionsList.h:435) the
    //     §10 stop that licensed this sweep had every sibling at a safepoint,
    //     so the window cannot exist at sweep time. Under SPEC-congc §7.3
    //     that option turns on and ~CodeBlock runs on a mutator outside the
    //     stop — the closure THEN holds by reachability instead: post-resume
    //     no live path can re-reach the dead code (R2 + visitWeak unlinked
    //     it; §5.8 pins would have kept it marked), and ~CallLinkInfoBase's
    //     gilOff arm takes s_callLinkSerializationLock unconditionally
    //     (CallLinkInfoBase.h:111-130) so the m_incomingCalls delist below is
    //     serialized against linkIncomingCall's locked push
    //     (CodeBlock.cpp:2476-2481). Either way, sound.
    // Deliberately NOT routed through the epoch facility (I7 hard rule:
    // epoch expiry must never free machine code). The sole caller
    // (CodeBlock.cpp ~CodeBlock) is itself useJSThreads-gated, so flag-off
    // codegen at the call site is unchanged; here both arms are now the same
    // ref drop, identical to what the flag-off ~RefPtr<JITCode> member
    // destructor would have done.
    jitCode = nullptr;
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
    // N6 leak stub: now reachable ONLY when GCSafepointEpoch.h has not landed
    // (the !JSC_JIT_HAS_GC_SAFEPOINT_EPOCH build config) — see the matching
    // note in retireHandlerChain. THREADS-INTEGRATE(jit)
    UNUSED_PARAM(vm);
    (void)callback.release();
}

namespace {

// Holds a retired §5.8 call-link record until its retirement epoch expires
// (or forever, under the flag-on leak arm). While held, the record's named
// CodeBlock stays pinned as a validated GC root on the epoch heap — the pin
// was taken at PUBLISH time (pinPublishedCallLinkRecordCodeBlock; w16 amend)
// and this holder owns it for the retirement tail. The destructor (epoch
// expiry) unpins, then frees the record: the §4.4 epoch guarantee that no
// straggler still holds the record pointer is exactly the guarantee that no
// straggler can still transfer the named CodeBlock, so pin and record share
// one lifetime by construction.
class RetiredCallLinkRecordWithPin final : public RetiredCallback {
public:
    RetiredCallLinkRecordWithPin(JSC::Heap& epochHeap, CallLinkRecord* record)
        : m_epochHeap(epochHeap)
        , m_record(record)
    {
    }

    ~RetiredCallLinkRecordWithPin() final
    {
        if (m_record->codeBlockToTransfer)
            m_epochHeap.unpinRetiredCallLinkRecordCodeBlock(m_record->codeBlockToTransfer);
        delete m_record;
    }

private:
    JSC::Heap& m_epochHeap;
    CallLinkRecord* m_record;
};

} // anonymous namespace

void RetiredJITArtifacts::pinPublishedCallLinkRecordCodeBlock(VM& vm, CodeBlock* codeBlock)
{
    if (!codeBlock)
        return;
    // Records exist only with the flag on (CallLinkInfo::publishRecord is a
    // no-op flag-off), so this path is flag-on by construction.
    ASSERT(Options::useJSThreads());
    // R4-2: the pin lives on the epoch heap (the client's SERVER under
    // useSharedGCHeap) — the same heap retireCallLinkRecord resolves later,
    // so the publish-time pin and the retirement-tail unpin balance.
    epochHeapFor(vm).pinRetiredCallLinkRecordCodeBlock(codeBlock);
}

void RetiredJITArtifacts::retireCallLinkRecord(VM& vm, CallLinkRecord* record)
{
    if (!record)
        return;
    // Records exist only with the flag on (CallLinkInfo::publishRecord is a
    // no-op flag-off), so this path is flag-on by construction.
    ASSERT(Options::useJSThreads());
    // w16 amend: the record's codeBlockToTransfer was pinned at publish time
    // (pinPublishedCallLinkRecordCodeBlock) — do NOT pin again here. The
    // retired holder takes over that pin and releases it at epoch expiry.
    JSC::Heap& heap = epochHeapFor(vm);
    retire(vm, std::unique_ptr<RetiredCallback>(new RetiredCallLinkRecordWithPin(heap, record)));
}

} // namespace JSC
