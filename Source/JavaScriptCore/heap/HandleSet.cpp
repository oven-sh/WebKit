/*
 * Copyright (C) 2011-2021 Apple Inc. All rights reserved.
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
#include "HandleSet.h"

#include "HandleBlock.h"
#include "HandleBlockInlines.h"
#include "JSCJSValueInlines.h"
#include "VM.h"
#include <mutex>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>

namespace JSC {

// ============================================================================
// UNGIL §F.3 — Strong-handle discipline (U-T8; api 5.10).
//
// ONE shared HandleSet per VM, with a new LEAF lock — the spec'd
// HandleSet::m_strongLock — taken inside Strong allocate/free/set-slot ONLY
// (never across user code). Mutation additionally requires an entered thread
// WITH heap access (§E.2's close re-acquires first); GC scans the set under
// the heap §10 stop (NOT §A.3), so visitStrongHandles below takes NO lock.
//
// Per-thread HandleSets were REJECTED: Strong lifetime is not thread-affine —
// the 5.10 finalizer hook (ThreadObject.cpp:96-131) and ~AsyncTicket
// (ThreadManager.cpp:48-59) exist precisely because last-refs drop on foreign
// threads; a leaf lock is two uncontended atomic ops on a non-hot path;
// revisit only on bench evidence.
//
// Carve-outs (r10 F1):
//  (a) in-lock-sweep Strong FREES under MSPL/BVL/9b are LEGAL — the lock
//      joins the destructor-leaf class (§LK.8; verified chain:
//      JSLockObject::destroy -> ~NativeLockState -> Deque<Ref<AsyncTicket>>
//      m_asyncWaiters -> ~AsyncTicket destroys a STILL-SET Strong<JSPromise>
//      for never-settled tickets; a Strong free is list-splice + fastMalloc,
//      acquires nothing else, never waits — the §LK.8 proof shape; the
//      epoch-retire alternative was REJECTED: heap §9 forbids retire() under
//      ranks 7-9b too). The ~AsyncTicket assert GIL-off = token meaning
//      (§F.2 IU row 32).
//  (b) heap finalizers clearing Strongs (api 5.10/D5-companion addFinalizer
//      lambdas — they need m_strongLock + access) run entered-with-access
//      OUTSIDE the stop window (heap §10B(5) JS-finalizer ban respected; the
//      conductor runs them after resume, before releasing its own client's
//      access).
//
// STORAGE (recorded deviation, U-T8): HandleSet.h is OUTSIDE U-T8's owned
// file set, so the lock cannot be a class member yet; it lives in the
// process-wide side table below, created eagerly in the HandleSet ctor so
// that the §LK.8 in-lock-sweep lookup path NEVER allocates, and torn down in
// ~HandleSet (legal: ~HandleSet runs at ~VM, after every mutator has exited).
//
// WIRING (U-T8): the LOCKED mutation entry points are landed BELOW in this
// TU — strongHandleAllocateSlow / strongHandleDeallocateSlow /
// strongHandleWriteBarrierSlow<bool> — wrapping HandleSet's inline
// allocate()/deallocate()/writeBarrier() (HandleSet.h:112/:122/:153) under
// m_strongLock when the owning VM is gilOff (GIL-on: no lock, bit-identical
// to the landed inlines). Strong.h/StrongInlines.h (allocate/free/set-slot
// call sites) are ALSO outside U-T8's owned set, so re-pointing those three
// call sites at these functions is the residual wiring.
// WIRED (review fix; closes the former U-T9 HARD-GATE open obligation):
// HandleSet.h now declares the three seams (JS_EXPORT_PRIVATE) and Strong.h /
// StrongInlines.h route every Strong allocate / free / set-slot call site
// through them, so GIL-off Strong traffic from two threads serializes on
// m_strongLock instead of racing m_freeList/m_strongList. When a later task
// may restructure HandleSet.h, fold the lock into the class as
// `Lock m_strongLock;` and collapse this side table — handleSetStrongLock()
// and the three functions below are the stable seam either way.
//
// Lock-context rule (§F.3/U20 lint): m_strongLock is a LEAF — nothing is
// acquired under it, it is never held across user JS, and it is legal under
// the sweep ranks per carve-out (a). The side-table lock below is a strict
// sub-leaf of it (only ctor/dtor/first-lookup take it; never under
// m_strongLock... the accessor takes table-then-returns, callers lock the
// returned leaf AFTER the table lock is dropped).
// ============================================================================

static Lock s_strongLockTableLock;

static UncheckedKeyHashMap<HandleSet*, std::unique_ptr<Lock>>& strongLockTable() WTF_REQUIRES_LOCK(s_strongLockTableLock)
{
    static LazyNeverDestroyed<UncheckedKeyHashMap<HandleSet*, std::unique_ptr<Lock>>> table;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        table.construct();
    });
    return table;
}

Lock& handleSetStrongLock(HandleSet& handleSet)
{
    Locker locker { s_strongLockTableLock };
    auto iterator = strongLockTable().find(&handleSet);
    RELEASE_ASSERT(iterator != strongLockTable().end()); // Ctor-registered; a miss means a destroyed (or foreign) HandleSet.
    return *iterator->value;
}

// ============================================================================
// §F.3 LOCKED Strong-mutation entry points (U-T8). These are the functions
// Strong allocate/free/set-slot must route through GIL-off (the
// StrongInlines.h re-point is the OPEN OBLIGATION above — a U-T9 entry gate).
// Namespace-scope, same library: callers redeclare them (the recorded U-T8
// seam pattern; no header outside the owned set changes).
//
// Lock context: m_strongLock is a LEAF taken around the list/free-list
// mutation only — never across user JS, nothing acquired under it except
// fastMalloc (allocate's grow() HandleBlock creation — no MSPL, legal per
// WS(i)), and legal under the sweep ranks per carve-out (a) (the deallocate
// path is list-splice only). GIL-on (gilOff() false — every shipping
// configuration) all three are lock-free and bit-identical to the inlines
// they wrap.
// ============================================================================

// Flag-off codegen review round: these are the gilOff-ONLY arms — the
// ALWAYS_INLINE strongHandle* wrappers (HandleSet.h) dispatch here only
// when the set's cached gilOff byte is set, so the per-call mode test no
// longer lives in this TU and GIL-on traffic never crosses into it.
HandleSlot strongHandleAllocateSlow(HandleSet& set)
{
    ASSERT(set.gilOff());
    Locker locker { handleSetStrongLock(set) };
    return set.allocate();
}

void strongHandleDeallocateSlow(HandleSet& set, HandleSlot slot)
{
    ASSERT(set.gilOff());
    Locker locker { handleSetStrongLock(set) };
    set.deallocate(slot);
}

template<bool isCellOnly>
void strongHandleWriteBarrierSlow(HandleSet& set, HandleSlot slot, JSValue value)
{
    ASSERT(set.gilOff());
    Locker locker { handleSetStrongLock(set) };
    set.writeBarrier<isCellOnly>(slot, value);
}

template void strongHandleWriteBarrierSlow<true>(HandleSet&, HandleSlot, JSValue);
template void strongHandleWriteBarrierSlow<false>(HandleSet&, HandleSlot, JSValue);

HandleSet::HandleSet(VM& vm)
    : m_vm(vm)
    , m_gilOff(vm.gilOff())
{
    grow();
    // §F.3 (U-T8): eager registration so the in-lock-sweep lookup
    // (carve-out (a)) never allocates under the sweep ranks.
    Locker locker { s_strongLockTableLock };
    auto addResult = strongLockTable().add(this, makeUnique<Lock>());
    RELEASE_ASSERT(addResult.isNewEntry);
}

HandleSet::~HandleSet()
{
    while (!m_blockList.isEmpty())
        HandleBlock::destroy(m_blockList.removeHead());
    // §F.3 (U-T8): runs at ~VM, after every mutator exited — no thread can
    // be inside the lock (Strong mutation requires an entered thread, and
    // ~VM's EXIT1.9 fence orders all spawned exits before teardown).
    Locker locker { s_strongLockTableLock };
    strongLockTable().remove(this);
}

void HandleSet::grow()
{
    HandleBlock* newBlock = HandleBlock::create(this);
    m_blockList.append(newBlock);

    for (int i = newBlock->nodeCapacity() - 1; i >= 0; --i) {
        Node* node = newBlock->nodeAtIndex(i);
        new (NotNull, node) Node;
        m_freeList.push(node);
    }
}

template<typename Visitor>
void HandleSet::visitStrongHandles(Visitor& visitor)
{
    // §F.3 (U-T8): deliberately NO m_strongLock here — GC scans the strong
    // set under the heap §10 stop (every mutator parked/access-released),
    // NOT under §A.3 and not under the leaf lock. Taking the lock here would
    // also violate carve-out (a)'s rank proof (the collector must not wait
    // on a mutator-held leaf).
    for (Node& node : m_strongList) {
#if ENABLE(GC_VALIDATION)
        RELEASE_ASSERT(isLiveNode(&node));
#endif
        visitor.appendUnbarriered(*node.slot());
    }
}

template void HandleSet::visitStrongHandles(AbstractSlotVisitor&);
template void HandleSet::visitStrongHandles(SlotVisitor&);

unsigned HandleSet::protectedGlobalObjectCount()
{
    // §F.3 (U-T8): API-statistics path — GIL-off this walks m_strongList
    // while spawned mutators may be running Strong set-slot mutations, so it
    // takes the leaf lock (legal: nothing acquired under it, no user JS
    // inside). GIL-on (gilOff() false — every shipping configuration): no
    // lock, bit-identical to the landed walk.
    auto walk = [&] {
        unsigned count = 0;
        for (Node& node : m_strongList) {
            JSValue value = *node.slot();
            if (value.isObject() && asObject(value.asCell())->isGlobalObject())
                count++;
        }
        return count;
    };
    if (m_vm.gilOff()) [[unlikely]] {
        Locker locker { handleSetStrongLock(*this) };
        return walk();
    }
    return walk();
}

#if ENABLE(GC_VALIDATION) || ASSERT_ENABLED
bool HandleSet::isLiveNode(Node* node)
{
    if (node->prev()->next() != node)
        return false;
    if (node->next()->prev() != node)
        return false;
        
    return true;
}
#endif // ENABLE(GC_VALIDATION) || ASSERT_ENABLED

} // namespace JSC
