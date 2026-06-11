/*
 * Copyright (C) 2016-2022 Apple Inc. All rights reserved.
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

#pragma once

#include "DeferTermination.h"
#include "Heap.h"
#include "LazyProperty.h"
#include "VMTraps.h"
#include <wtf/Condition.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
#include <wtf/Seconds.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>

namespace JSC {

template<typename OwnerType, typename ElementType>
void LazyProperty<OwnerType, ElementType>::Initializer::set(ElementType* value) const
{
    property.set(vm, owner, value);
}

template<typename OwnerType, typename ElementType>
template<typename Func>
void LazyProperty<OwnerType, ElementType>::initLater(const Func&)
{
    static_assert(isStatelessLambda<Func>());
    // Logically we just want to stuff the function pointer into m_pointer, but then we'd be sad
    // because a function pointer is not guaranteed to be a multiple of anything. The tag bits
    // may be used for things. We address this problem by indirecting through a global const
    // variable. The "theFunc" variable is guaranteed to be native-aligned, i.e. at least a
    // multiple of 4.
    static constexpr FuncType theFunc = &callFunc<Func>;
    WTF::atomicStore(&m_pointer, lazyTag | std::bit_cast<uintptr_t>(&theFunc), std::memory_order_relaxed); // THREADS: see getInitializedOnMainThread().
}

template<typename OwnerType, typename ElementType>
void LazyProperty<OwnerType, ElementType>::setMayBeNull(VM& vm, const OwnerType* owner, ElementType* value)
{
    // THREADS: release store — publishes the fully initialized element to
    // concurrent relaxed/dependent readers on other mutators.
    uintptr_t pointer = std::bit_cast<uintptr_t>(value);
    RELEASE_ASSERT(!(pointer & lazyTag));
    WTF::atomicStore(&m_pointer, pointer, std::memory_order_release);
    vm.writeBarrier(owner, value);
}

template<typename OwnerType, typename ElementType>
void LazyProperty<OwnerType, ElementType>::set(VM& vm, const OwnerType* owner, ElementType* value)
{
    RELEASE_ASSERT(value);
    setMayBeNull(vm, owner, value);
}

template<typename OwnerType, typename ElementType>
template<typename Visitor>
void LazyProperty<OwnerType, ElementType>::visit(Visitor& visitor)
{
    uintptr_t pointer = WTF::atomicLoad(&m_pointer, std::memory_order_relaxed); // THREADS: concurrent marker vs mutator publication.
    if (pointer && !(pointer & lazyTag))
        visitor.appendUnbarriered(std::bit_cast<ElementType*>(pointer));
}

template<typename OwnerType, typename ElementType>
void LazyProperty<OwnerType, ElementType>::dump(PrintStream& out) const
{
    uintptr_t pointer = WTF::atomicLoad(const_cast<uintptr_t*>(&m_pointer), std::memory_order_relaxed);
    if (!pointer) {
        out.print("<null>");
        return;
    }
    if (pointer & lazyTag) {
        out.print("Lazy:", RawHex(pointer & ~lazyTag));
        if (pointer & initializingTag)
            out.print("(Initializing)");
        return;
    }
    out.print(RawHex(pointer));
}

// UNGIL §K.3 / ANNEX LZ1 (BINDING) side tables: the initializing CAS RECORDS
// the OWNER; foreign threads wait park-capably; OWNER re-entry (and LZ1.2
// cross-thread ownership cycles) return null — exactly the landed recursion
// contract extended per the annex. One process-wide leaf lock: first-touch
// initialization is cold by construction, and LZ2 forbids first-touch sites
// from holding any ranked lock, so this lock is a leaf in every legal
// caller. GIL-on / flag-off: the foreign arms are unreachable (phase-1 GIL
// initializers never yield the GIL across the window), so behavior reduces
// to the landed owner-recursion-null contract.
namespace LazyPropertyInternal {

struct InitTables {
    Lock lock;
    Condition condition;
    UncheckedKeyHashMap<const void*, Thread*> owners; // property address -> winner thread
    UncheckedKeyHashMap<Thread*, const void*> waits;  // waiter thread -> property it waits on
};

inline InitTables& initTables()
{
    static LazyNeverDestroyed<InitTables> tables;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] { tables.construct(); });
    return tables.get();
}

} // namespace LazyPropertyInternal

template<typename OwnerType, typename ElementType>
template<typename Func>
ElementType* LazyProperty<OwnerType, ElementType>::callFunc(const Initializer& initializer)
{
    LazyProperty& property = initializer.property;
    uintptr_t* slot = &property.m_pointer;

    // Flag-off / GIL-on: the landed contract verbatim — same-thread recursion
    // returns null, no side tables, no process-global lock. The §K.3/LZ1
    // protocol below is gilOffProcess-ONLY: its foreign-wait and cycle-walk
    // arms are unreachable under the GIL anyway, and entering the process-
    // global initTables() lock on every first-touch lazy init in every mode
    // would be new unconditional flag-off work (charter violation) and would
    // contend ALL VMs in the process on one lock.
    if (!VM::isGILOffProcess()) [[likely]] {
        uintptr_t current = WTF::atomicLoad(slot, std::memory_order_relaxed);
        if (!(current & lazyTag))
            return std::bit_cast<ElementType*>(current);
        if (current & initializingTag)
            return nullptr; // Same-thread recursion: the landed null contract.
        WTF::atomicStore(slot, current | initializingTag, std::memory_order_relaxed);
        DeferTerminationForAWhile deferTerminationForAWhile { initializer.vm };
        callStatelessLambda<void, Func>(initializer);
        uintptr_t result = WTF::atomicLoad(slot, std::memory_order_relaxed);
        RELEASE_ASSERT(!(result & lazyTag));
        RELEASE_ASSERT(!(result & initializingTag));
        return std::bit_cast<ElementType*>(result);
    }

    auto& tables = LazyPropertyInternal::initTables();
    Thread* self = &Thread::currentSingleton();
    const void* key = static_cast<const void*>(&property);

    for (;;) {
        uintptr_t current = WTF::atomicLoad(slot, std::memory_order_acquire);
        if (!(current & lazyTag))
            return std::bit_cast<ElementType*>(current); // A foreign winner published while we raced here.

        if (current & initializingTag) {
            // Pre-threads this arm was "return nullptr" unconditionally —
            // sound only for same-thread recursion (the only way to observe
            // the tag under the GIL). GIL-off a FOREIGN first-toucher landing
            // here returned null into callers that dereference the result
            // unconditionally (e.g. constructObjectFromPropertyDescriptor's
            // descriptor-object structures) => SEGV; observed via
            // mc-init-cloned-arguments-specials once the §10.4 liveness
            // fixes let shared GCs stretch the init window across a pause.
            {
                Locker locker { tables.lock };
                // Re-test under the lock: the winner publishes (release
                // store) BEFORE erasing its owner record, so a cleared tag
                // or absent record means re-test and adopt.
                uintptr_t reread = WTF::atomicLoad(slot, std::memory_order_acquire);
                if (!(reread & lazyTag))
                    return std::bit_cast<ElementType*>(reread);
                if (reread & initializingTag) {
                    // LZ1.2 cycle escape (owner re-entry is the 1-node case):
                    // follow owner-of -> waits-on edges; reaching SELF means
                    // this get() must return null to break the cycle. The
                    // walk is bounded: at most one in-flight init per thread,
                    // so the chain is a function and terminates at a running
                    // owner, an absent record, or a repeat.
                    // Hop bound: a cycle NOT involving self (its members
                    // detect and null it themselves) must not spin this walk
                    // under the lock; chains are <= live-thread count, so a
                    // generous cap only ever truncates a foreign cycle into
                    // "wait one quantum and re-walk".
                    const void* probe = key;
                    for (unsigned hops = 0; hops < 256; ++hops) {
                        auto ownerIt = tables.owners.find(probe);
                        if (ownerIt == tables.owners.end())
                            break; // Abandoned or just-finished: re-test.
                        Thread* ownerThread = ownerIt->value;
                        if (ownerThread == self)
                            return nullptr; // Recursion / cross-thread cycle: landed null contract.
                        auto waitIt = tables.waits.find(ownerThread);
                        if (waitIt == tables.waits.end())
                            break; // Owner is running its initializer: wait below.
                        probe = waitIt->value;
                    }
                    tables.waits.set(self, key); // LZ1.1 wait-for edge, published before the first park quantum.
                }
            }
            // K.3 foreign wait: park-capable bounded quantum with heap access
            // RELEASED (a waiter spinning WITH access while the winner's
            // allocating initializer triggers a collection is the r6 F2
            // three-way deadlock). Re-acquisition funnels through the
            // §A.3.2b/F8-gated AHA, which polls BOTH stop families (GSP leg
            // + §A.3 stop word + Mode leg) and parks across open windows.
            GCClient::Heap* client = GCClient::Heap::currentThreadClient();
            bool releasedAccess = client && client->hasHeapAccess();
            if (releasedAccess)
                client->releaseHeapAccess();
            {
                Locker locker { tables.lock };
                uintptr_t reread = WTF::atomicLoad(slot, std::memory_order_acquire);
                if ((reread & lazyTag) && (reread & initializingTag))
                    tables.condition.waitFor(tables.lock, Seconds::fromMilliseconds(1));
                tables.waits.remove(self);
            }
            if (releasedAccess)
                client->acquireHeapAccess();
            continue; // Re-test the acquire load.
        }

        // First-touch claim: CAS records the claim in the slot; the owner
        // identity goes to the side table (r16 F2). Weak CAS: spurious
        // failure just re-loops.
        if (!WTF::atomicCompareExchangeWeak(slot, current, current | initializingTag, std::memory_order_acquire))
            continue;
        {
            Locker locker { tables.lock };
            tables.owners.set(key, self);
        }
        {
            // LZ1.3 abandonment + winner cleanup: on ANY exit, erase the
            // owner record and wake waiters; if the initializer did NOT
            // publish (non-normal exit — exception, termination), restore
            // the pre-claim word so a later toucher re-runs it
            // ("initializers publish only on success; partial work is
            // garbage").
            uintptr_t preClaim = current;
            auto cleanup = WTF::makeScopeExit([&] {
                Locker locker { tables.lock };
                uintptr_t now = WTF::atomicLoad(slot, std::memory_order_relaxed);
                if (now & lazyTag)
                    WTF::atomicStore(slot, preClaim, std::memory_order_release);
                tables.owners.remove(key);
                tables.condition.notifyAll();
            });

            DeferTerminationForAWhile deferTerminationForAWhile { initializer.vm };
            callStatelessLambda<void, Func>(initializer);
            uintptr_t result = WTF::atomicLoad(slot, std::memory_order_relaxed);
            RELEASE_ASSERT(!(result & lazyTag));
            RELEASE_ASSERT(!(result & initializingTag));
            return std::bit_cast<ElementType*>(result);
        }
    }
}

} // namespace JSC
