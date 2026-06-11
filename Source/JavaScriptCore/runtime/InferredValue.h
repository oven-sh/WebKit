/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
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

#include "JSScope.h"
#include "VM.h"
#include <atomic>
#include <wtf/Lock.h>

namespace JSC {

template<typename JSCellType>
class InferredValue {
    WTF_MAKE_NONCOPYABLE(InferredValue);
    WTF_MAKE_NONMOVABLE(InferredValue);
public:
    // For the purpose of deciding whether or not to watch this variable, you only need
    // to inspect inferredValue(). If this returns something other than the empty
    // value, then it means that at all future safepoints, this watchpoint set will be
    // in one of these states:
    //
    //    IsWatched: in this case, the variable's value must still be the
    //        inferredValue.
    //
    //    IsInvalidated: in this case the variable's value may be anything but you'll
    //        either notice that it's invalidated and not install the watchpoint, or
    //        you will have been notified that the watchpoint was fired.
    JSCellType* inferredValue()
    {
        uintptr_t data = m_data.load(std::memory_order_relaxed);
        if (isFat(data))
            return fat(data)->inferredValue();
        return std::bit_cast<JSCellType*>(data & ValueMask);
    }

    explicit InferredValue()
        : m_data(encodeState(ClearWatchpoint))
    {
        ASSERT(inferredValue() == nullptr);
    }

    ~InferredValue()
    {
        if (isThin())
            return;
        freeFat();
    }

    // It is safe to call this from another thread. It may return a prior state,
    // but that should be fine since you should only perform actions based on the
    // state if you also add a watchpoint.
    WatchpointState state() const
    {
        uintptr_t data = m_data.load(std::memory_order_relaxed);
        if (isFat(data))
            return fat(data)->state();
        return decodeState(data);
    }

    // It is safe to call this from another thread. It may return false
    // even if the set actually had been invalidated, but that ought to happen
    // only in the case of races, and should be rare.
    bool hasBeenInvalidated() const
    {
        return state() == IsInvalidated;
    }
    
    // Like hasBeenInvalidated(), may be called from another thread.
    bool isStillValid() const
    {
        return !hasBeenInvalidated();
    }
    
    // AB18-H: GIL-off, this routes through inflate() -> WatchpointSet::add().
    // WatchpointSet::add()'s own GIL-off concurrency (plain m_state store and
    // unlocked m_set mutation in Watchpoint.cpp) is NOT covered by this
    // change; it is a separate AB18 follow-up item. This change only makes
    // the InferredValue-side transitions (thin word, fat m_value/m_state)
    // race-free among notifyWrite/invalidate/inflate callers.
    void add(Watchpoint*);

    void invalidate(VM& vm, const FireDetail& detail)
    {
        if (vm.gilOff()) [[unlikely]] {
            // AB18-H: CAS retry loop on the thin word. A plain store here
            // could overwrite a fat pointer published by a concurrent
            // inflateSlow(), leaking the fat set and silently dropping every
            // Watchpoint registered on it (no jettison on a real fire). If
            // we observe (or lose the CAS to) a fat pointer, re-dispatch to
            // the fat set's serialized invalidate.
            uintptr_t data = m_data.load(std::memory_order_acquire);
            for (;;) {
                if (isFat(data)) {
                    fat(data)->invalidate(vm, detail);
                    return;
                }
                if (decodeState(data) == IsInvalidated)
                    return;
                // Thin sets have no watchers (add() inflates first), so
                // invalidation is just the terminal state transition.
                if (m_data.compare_exchange_weak(data, encodeState(IsInvalidated), std::memory_order_acq_rel, std::memory_order_acquire))
                    return;
            }
        }
        if (isFat())
            fat()->invalidate(vm, detail);
        else
            m_data.store(encodeState(IsInvalidated), std::memory_order_relaxed);
    }
    
    bool isBeingWatched() const
    {
        if (isFat())
            return fat()->isBeingWatched();
        return false;
    }

    void notifyWrite(VM& vm, JSCell* owner, JSCellType* value, const FireDetail& detail)
    {
        if (state() == IsInvalidated) [[likely]]
            return;
        notifyWriteSlow(vm, owner, value, detail);
    }
    
    void notifyWrite(VM& vm, JSCell* owner, JSCellType* value, const char* reason)
    {
        if (state() == IsInvalidated) [[likely]]
            return;
        notifyWriteSlow(vm, owner, value, reason);
    }
    
    void finalizeUnconditionally(VM&, CollectionScope);

private:
    class InferredValueWatchpointSet final : public WatchpointSet {
    public:
        InferredValueWatchpointSet(WatchpointState state, JSCellType* value)
            : WatchpointSet(state)
        {
            // TSAN r12 (splay-like flicker): initialize via a relaxed STORE,
            // not the Atomic value constructor — the value constructor is a
            // plain store (wave-5 precedent, Watchpoint.cpp), and it runs
            // AFTER the base constructor's HAPPENS_BEFORE annotation, so it
            // paired against compiler-thread inferredValue() probes. Re-issue
            // the publication annotation after the member is initialized.
            m_value.store(value, std::memory_order_relaxed);
            TSAN_ANNOTATE_HAPPENS_BEFORE(this);
        }

        // May be called from compiler threads; a stale value is already part
        // of the contract (see class comment), so relaxed is sufficient.
        // TSAN r11 (reports 25/28): the HAPPENS_AFTER pairs with the
        // HAPPENS_BEFORE at the end of the WatchpointSet constructor (this
        // class derives from it) — same consume-publication shape as
        // state(); see Watchpoint.cpp. `this` aliases the base for TSAN's
        // address-keyed annotation map (single-inheritance first base).
        JSCellType* inferredValue() const
        {
            TSAN_ANNOTATE_HAPPENS_AFTER(this);
            return m_value.load(std::memory_order_relaxed);
        }

        void invalidate(VM& vm, const FireDetail& detail)
        {
            if (vm.gilOff()) [[unlikely]] {
                invalidateGILOff(vm, detail);
                return;
            }
            m_value.store(nullptr, std::memory_order_relaxed);
            WatchpointSet::invalidate(vm, detail);
        }

        void notifyWriteSlow(VM&, JSCell* owner, JSCellType*, const FireDetail&);

    private:
        // AB18-H (V3): GIL-off, N mutators may notifyWrite/invalidate this
        // set concurrently. All GIL-off transition *decisions* are made under
        // m_gilOffLock so that startWatching()'s IsWatched store and
        // WatchpointSet::invalidate()'s IsInvalidated store are never an
        // unordered pair of plain stores. The lock is held only across plain
        // memory operations -- never across WatchpointSet::invalidate(),
        // which blocks for the Class-A coalescing fire (SPEC-jit 5.6) -- so
        // a thread parked on the lock can never stall a stop-the-world.
        //
        // IsInvalidated is terminal: once m_gilOffInvalidationPending is set
        // (always under the lock, before the lock is released), every later
        // gilOff notifyWriteSlow sees the flag under the same lock and joins
        // the invalidation instead of calling startWatching(). The Class-A
        // coalescing fire queue makes the joined WatchpointSet::invalidate()
        // idempotent, and every joiner returns only after the fire has run.
        void invalidateGILOff(VM& vm, const FireDetail& detail)
        {
            {
                Locker locker { m_gilOffLock };
                m_gilOffInvalidationPending.store(true, std::memory_order_release);
                m_value.store(nullptr, std::memory_order_relaxed);
            }
            WatchpointSet::invalidate(vm, detail);
        }

        std::atomic<JSCellType*> m_value;
        Lock m_gilOffLock;
        std::atomic<bool> m_gilOffInvalidationPending { false };
    };

    static constexpr uintptr_t IsThinFlag        = 1;
    static constexpr uintptr_t StateMask         = 6;
    static constexpr uintptr_t StateShift        = 1;
    static constexpr uintptr_t ValueMask         = ~static_cast<uintptr_t>(IsThinFlag | StateMask);
    
    static bool isThin(uintptr_t data) { return data & IsThinFlag; }
    static bool isFat(uintptr_t data) { return !isThin(data); }
    
    static WatchpointState decodeState(uintptr_t data)
    {
        ASSERT(isThin(data));
        return static_cast<WatchpointState>((data & StateMask) >> StateShift);
    }
    
    static uintptr_t encodeState(WatchpointState state)
    {
        return (static_cast<uintptr_t>(state) << StateShift) | IsThinFlag;
    }
    
    bool isThin() const { return isThin(m_data.load(std::memory_order_relaxed)); }
    bool isFat() const { return isFat(m_data.load(std::memory_order_relaxed)); };
    
    static InferredValueWatchpointSet* fat(uintptr_t data)
    {
        return std::bit_cast<InferredValueWatchpointSet*>(data);
    }
    
    InferredValueWatchpointSet* fat()
    {
        ASSERT(isFat());
        return fat(m_data.load(std::memory_order_relaxed));
    }

    const InferredValueWatchpointSet* fat() const
    {
        ASSERT(isFat());
        return fat(m_data.load(std::memory_order_relaxed));
    }
    
    InferredValueWatchpointSet* inflate()
    {
        if (isFat()) [[likely]]
            return fat();
        return inflateSlow();
    }

    InferredValueWatchpointSet* inflateSlow();
    void freeFat();

    void notifyWriteSlow(VM&, JSCell* owner, JSCellType*, const FireDetail&);
    void notifyWriteSlow(VM&, JSCell* owner, JSCellType*, const char* reason);

    // AB18-H: the thin word is the single GIL-off linearization point.
    // Thin-state transitions and the thin->fat inflate are CASes; GIL-on
    // paths use relaxed loads/stores (plain-equivalent codegen).
    std::atomic<uintptr_t> m_data;
};

template<typename JSCellType>
void InferredValue<JSCellType>::InferredValueWatchpointSet::notifyWriteSlow(VM& vm, JSCell* owner, JSCellType* value, const FireDetail& detail)
{
    if (vm.gilOff()) [[unlikely]] {
        // AB18-H (V3): all transition decisions under m_gilOffLock; the
        // blocking Class-A fire (WatchpointSet::invalidate) runs after the
        // lock is dropped. See invalidateGILOff() for the full protocol.
        bool joinInvalidation = false;
        {
            Locker locker { m_gilOffLock };
            if (m_gilOffInvalidationPending.load(std::memory_order_acquire)) {
                // A concurrent invalidation is in flight (its fire may not
                // have flipped m_state yet). Join it below, outside the
                // lock; the coalescing fire queue makes the join idempotent
                // and we return only after the fire is complete. Never
                // startWatching() past this point: IsInvalidated must stay
                // terminal (Watchpoint.h monotonicity contract relied on by
                // the DFG watch-then-optimize protocol).
                joinInvalidation = true;
            } else {
                switch (state()) {
                case ClearWatchpoint:
                    m_value.store(value, std::memory_order_relaxed);
                    vm.writeBarrier(owner, value);
                    // Safe: lock held and no invalidation pending, so no
                    // concurrent IsInvalidated store can exist; any later
                    // gilOff invalidate orders after our IsWatched store via
                    // the lock.
                    startWatching();
                    return;

                case IsWatched: {
                    JSCellType* current = m_value.load(std::memory_order_relaxed);
                    if (current == value)
                        return;
                    // Mismatch (or, defensively, a cleared value): begin the
                    // invalidation while holding the lock, fire after
                    // releasing it. Cannot call invalidate() here -- the
                    // lock is not recursive.
                    m_gilOffInvalidationPending.store(true, std::memory_order_release);
                    m_value.store(nullptr, std::memory_order_relaxed);
                    joinInvalidation = true;
                    break;
                }

                case IsInvalidated:
                    // Reachable GIL-off: notifyWrite()'s unlocked fast-path
                    // state() check raced an invalidation that completed
                    // before we took the lock. Nothing to do.
                    return;
                }
            }
        }
        if (joinInvalidation)
            WatchpointSet::invalidate(vm, detail);
        return;
    }

    switch (state()) {
    case ClearWatchpoint:
        m_value.store(value, std::memory_order_relaxed);
        vm.writeBarrier(owner, value);
        startWatching();
        return;

    case IsWatched:
        ASSERT(!!m_value.load(std::memory_order_relaxed));
        if (m_value.load(std::memory_order_relaxed) == value)
            return;
        invalidate(vm, detail);
        return;

    case IsInvalidated:
        ASSERT_NOT_REACHED();
        return;
    }

    ASSERT_NOT_REACHED();
}

template<typename JSCellType>
void InferredValue<JSCellType>::notifyWriteSlow(VM& vm, JSCell* owner, JSCellType* value, const FireDetail& detail)
{
    if (vm.gilOff()) [[unlikely]] {
        // AB18-H (V3): CAS retry loop with the thin word as the single
        // linearization point. ClearWatchpoint -> (IsWatched, value) is ONE
        // atomic step, so there is no window in which another thread can
        // observe a value without IsWatched or vice versa, and a concurrent
        // inflateSlow() either snapshots the word before or after the whole
        // transition. If the word is (or becomes) fat, re-dispatch to the
        // fat set's lock-serialized slow path.
        uintptr_t data = m_data.load(std::memory_order_acquire);
        for (;;) {
            if (isFat(data)) {
                fat(data)->notifyWriteSlow(vm, owner, value, detail);
                return;
            }
            switch (decodeState(data)) {
            case ClearWatchpoint: {
                uintptr_t desired = (std::bit_cast<uintptr_t>(value) & ValueMask) | encodeState(IsWatched);
                if (m_data.compare_exchange_weak(data, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    vm.writeBarrier(owner, value);
                    return;
                }
                continue; // 'data' was refreshed by the failed CAS.
            }

            case IsWatched: {
                if (std::bit_cast<JSCellType*>(data & ValueMask) == value)
                    return;
                // Thin sets have no watchers (add() inflates first), so
                // invalidation is just the terminal state transition.
                if (m_data.compare_exchange_weak(data, encodeState(IsInvalidated), std::memory_order_acq_rel, std::memory_order_acquire))
                    return;
                continue;
            }

            case IsInvalidated:
                // Reachable GIL-off: notifyWrite()'s fast-path state() check
                // raced an invalidation that completed. Nothing to do.
                return;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    uintptr_t data = m_data.load(std::memory_order_relaxed);
    if (isFat(data)) {
        fat(data)->notifyWriteSlow(vm, owner, value, detail);
        return;
    }

    switch (state()) {
    case ClearWatchpoint:
        ASSERT(decodeState(m_data.load(std::memory_order_relaxed)) != IsInvalidated);
        m_data.store((std::bit_cast<uintptr_t>(value) & ValueMask) | encodeState(IsWatched), std::memory_order_relaxed);
        vm.writeBarrier(owner, value);
        return;

    case IsWatched:
        ASSERT(!!inferredValue());
        if (inferredValue() == value)
            return;
        invalidate(vm, detail);
        return;

    case IsInvalidated:
        ASSERT_NOT_REACHED();
        return;
    }

    ASSERT_NOT_REACHED();
}

template<typename JSCellType>
void InferredValue<JSCellType>::notifyWriteSlow(VM& vm, JSCell* owner, JSCellType* value, const char* reason)
{
    notifyWriteSlow(vm, owner, value, StringFireDetail(reason));
}

template<typename JSCellType>
void InferredValue<JSCellType>::add(Watchpoint* watchpoint)
{
    inflate()->add(watchpoint);
}

template<typename JSCellType>
auto InferredValue<JSCellType>::inflateSlow() -> InferredValueWatchpointSet*
{
    // AB18-H: CAS-publish retry loop, unconditional (GIL-on the CAS succeeds
    // on the first iteration with the same observable behavior as before;
    // this is an out-of-line slow path, so no flag-off fast-path codegen is
    // affected). GIL-off, two mutators can race to inflate: exactly one fat
    // set wins the CAS. The loser's set was never published and never
    // received a Watchpoint -- add() only runs against the set inflate()
    // returns -- so no watchpoint list can split across two fat sets, and
    // deref() frees the refcount-1 loser. A concurrent thin-word transition
    // (notifyWriteSlow/invalidate CAS) also fails our CAS; we rebuild from
    // the fresh snapshot, so a fat set is only ever born with a
    // (state, value) pair that was atomically current.
    ASSERT(!isCompilationThread());
    uintptr_t data = m_data.load(std::memory_order_acquire);
    for (;;) {
        if (isFat(data))
            return fat(data); // Lost an inflate race; adopt the winner's set.
        InferredValueWatchpointSet* fatSet = adoptRef(new InferredValueWatchpointSet(decodeState(data), std::bit_cast<JSCellType*>(data & ValueMask))).leakRef();
        WTF::storeStoreFence();
        if (m_data.compare_exchange_strong(data, std::bit_cast<uintptr_t>(fatSet), std::memory_order_acq_rel, std::memory_order_acquire))
            return fatSet;
        fatSet->deref(); // 'data' was refreshed by the failed CAS; retry.
    }
}

template<typename JSCellType>
void InferredValue<JSCellType>::freeFat()
{
    ASSERT(isFat());
    fat()->deref();
}

} // namespace JSC
