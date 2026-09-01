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
#include "GCSafepointEpoch.h"
#include "Heap.h"
#include "InlineCacheHandler.h"
#include "Options.h"
#include "VM.h"
#include <wtf/Lock.h>

namespace JSC {

// The epoch governing retired data must be advanced by the safepoints of the
// same mutator population that can hold pointers into it, so it lives on the
// SERVER heap this VM's client attaches to (== vm.heap for the 1:1 case).
//
// Epoch expiry is a sound free only because every JS-executing thread owns its
// own GCClient::Heap registered in the server's clientSet(): the
// min-over-clients scan in GCSafepointEpoch::bumpAndReclaim is then a minimum
// over JS threads, and each client's epoch is stamped only while that thread
// is stopped at a safepoint (Heap::runSafepointHooksAndReclaim). A thread that
// could run JS without its own registered client would turn every expired
// artifact into a use-after-free; there is no separate per-thread witness.
static JSC::Heap& epochHeapFor(VM& vm)
{
    return vm.clientHeap.server();
}

#if ENABLE(JIT)

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

void RetiredJITArtifacts::retireHandlerChain(VM& vm, RefPtr<InlineCacheHandler>&& head, DisarmClearingWatchpoints disarm)
{
    if (!head)
        return;

    // Only GC-aware stub routines may ride a retired chain: their executable
    // memory is freed by the jettison + conservative-scan path, never by epoch
    // expiry. Every caller is useJSThreads-gated, and flag-on every IC stub
    // routine is made GC-aware at creation (createICJITStubRoutine always,
    // createPreCompiledICJITStubRoutine under the flag), so nothing is
    // promoted here: a retire-time makeGCAware() on a published routine would
    // race a sibling retiring a chain that shares the same stateless
    // precompiled stub and double-append it to the JITStubRoutineSet.
    //
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
            if (auto* routine = cursor->stubRoutine())
                RELEASE_ASSERT(routine->isGCAware());
            // AB18-F (sig-1 family, amended at thread-closeout final review):
            // disarm each DISPLACED/DYING handler's owner-clearing watchpoint
            // NOW. Epoch expiry defers destruction past the owner CodeBlock's
            // possible death, so an armed
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

    epochHeapFor(vm).safepointEpoch().retire(std::unique_ptr<RetiredCallback>(new RetiredHandlerChain(WTF::move(head))));
}

#endif // ENABLE(JIT)

void RetiredJITArtifacts::retire(VM& vm, std::unique_ptr<RetiredCallback>&& callback)
{
    if (!callback)
        return;

    epochHeapFor(vm).safepointEpoch().retire(WTF::move(callback));
}

namespace {

// Holds a retired §5.8 call-link record until its retirement epoch expires.
// While held, the record's named CodeBlock stays pinned as a validated GC root
// on the record's pin heap — the pin was taken at PUBLISH time
// (pinPublishedCallLinkRecordCodeBlock; w16 amend) and this holder owns it for
// the retirement tail. The destructor
// (epoch expiry) unpins, then frees the record: the §4.4 epoch guarantee that
// no straggler still holds the record pointer is exactly the guarantee that
// no straggler can still transfer the named CodeBlock, so pin and record
// share one lifetime by construction.
class RetiredCallLinkRecordWithPin final : public RetiredCallback {
public:
    explicit RetiredCallLinkRecordWithPin(CallLinkRecord* record)
        : m_record(record)
    {
    }

    ~RetiredCallLinkRecordWithPin() final
    {
        destroyUnreachableCallLinkRecord(m_record);
    }

private:
    CallLinkRecord* m_record;
};

} // anonymous namespace

JSC::Heap* RetiredJITArtifacts::pinPublishedCallLinkRecordCodeBlock(VM& vm, CodeBlock* codeBlock)
{
    if (!codeBlock)
        return nullptr;
    // Records exist only with the flag on (CallLinkInfo::publishRecord is a
    // no-op flag-off), so this path is flag-on by construction.
    ASSERT(Options::useJSThreads());
    // R4-2: the pin lives on the epoch heap (the client's SERVER under
    // useSharedGCHeap); the record remembers it so the unpin — at epoch
    // expiry or in the owning CallLinkInfo's destructor — balances on the
    // same heap.
    JSC::Heap& heap = epochHeapFor(vm);
    heap.pinRetiredCallLinkRecordCodeBlock(codeBlock);
    return &heap;
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
    retire(vm, std::unique_ptr<RetiredCallback>(new RetiredCallLinkRecordWithPin(record)));
}

} // namespace JSC
