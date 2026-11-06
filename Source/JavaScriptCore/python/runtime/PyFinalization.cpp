/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyRuntime.h"

#include "CApiHooks.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
#include "PreventCollectionScope.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyCtypes.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyExec.h"
#include "PyGenerator.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyMonitoring.h"
#include "PyStr.h"
#include "PyStructSeq.h"
#include "PyThreads.h"
#include "PyWeakRef.h"
#include "SubspaceInlines.h"
#include "TopExceptionScope.h"
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC {


namespace Python {

// Finalizers, the callbacks of weak references, and the module gc. See the comment in PyRuntime.h.

static FinalizerState* finalizerStateOf(JSCell* cell)
{
    if (cell->type() != PyObjectType)
        return nullptr;
    const ClassInfo* info = cell->classInfo();
    if (info == PyRecord::info())
        return &uncheckedDowncast<PyRecord>(cell)->finalizerState();
    if (info == PyManagedBuffer::info())
        return &uncheckedDowncast<PyManagedBuffer>(cell)->finalizerState();
    return nullptr;
}

// These are registered once, by whoever makes them.
static bool isRegisteredWhenMade(JSCell* cell)
{
    return cell->type() == PyGeneratorType || cell->classInfo() == PyAsyncGenAwaitable::info();
}

void Runtime::registerFinalizable(JSCell* cell)
{
    if (FinalizerState* state = finalizerStateOf(cell)) {
        state->isWanted = true;
        if (std::exchange(state->isListed, true))
            return;
    } else if (cell->type() == PyInstanceType || !isRegisteredWhenMade(cell)) {
        // Once in its life: it is in the list, or queued, or has been finalized. The bit is the one that JSC leaves to each class of
        // cells to use as it likes; objects have no use for it.
        ASSERT(cell->isObject());
        if (cell->perCellBit())
            return;
        cell->setPerCellBit(true);
    }
    m_youngFinalizable.append(Finalizable::of(cell, m_currentThread, m_era));
    if (allocationTracebackLimit) [[unlikely]]
        didRegisterWhileTracingAllocations(cell);
}

void Runtime::didRegisterWhileTracingAllocations(JSCell* cell)
{
    noteWhereObjectWasMade(m_globalObject, cell);
}

void Runtime::unregisterFinalizable(PyRecord* record)
{
    record->finalizerState().isWanted = false;
}

void Runtime::unregisterFinalizable(PyManagedBuffer* buffer)
{
    buffer->finalizerState().isWanted = false;
}

void Runtime::registerFinalizableLate(const MarkedArgumentBuffer& objects)
{
    for (unsigned i = 0; i < objects.size(); ++i) {
        JSCell* cell = objects.at(i).asCell();
        // A finalizer of the program's is called once.
        FinalizerState* state = finalizerStateOf(cell);
        if (!state || !state->wasFoundDead)
            registerFinalizable(cell);
    }
}

// Whether it is in the list for nothing: whatever it had to finalize has been seen to.
static bool isNoLongerWanted(JSCell* cell)
{
    if (cell->type() == PyInstanceType)
        return false;
    if (cell->type() == PyGeneratorType)
        return uncheckedDowncast<PyGenerator>(cell)->finalization() == PyGenerator::Finalization::None;
    if (FinalizerState* state = finalizerStateOf(cell))
        return !state->isWanted;
    if (auto* awaitable = PyAsyncGenAwaitable::tryFrom(cell))
        return awaitable->state() != PyAsyncGenAwaitable::State::Initial;
    return false;
}

// Dead or alive.
void Runtime::didTakeOffList(JSCell* cell, bool isToBeFinalized)
{
    if (cell->type() == PyInstanceType)
        return;
    if (cell->type() == PyGeneratorType)
        uncheckedDowncast<PyGenerator>(cell)->setFinalization(PyGenerator::Finalization::None);
    else if (FinalizerState* state = finalizerStateOf(cell))
        *state = { false, false, state->wasFoundDead || isToBeFinalized };
}

// Those that still have something to finalize, as far as they know, in the order in which they were registered.
void Runtime::forEachFinalizable(const ScopedLambda<void(JSCell*)>& function)
{
    for (auto* list : { &m_oldFinalizable, &m_youngFinalizable }) {
        for (auto& entry : *list) {
            ASSERT(entry.cell());
            if (!isNoLongerWanted(entry.cell()))
                function(entry.cell());
        }
    }
}

// ---- Who owes what ----

Runtime::CallsOwed& Runtime::callsOwedBy(uint32_t thread)
{
    for (auto& owed : m_callsOwed) {
        if (owed->thread == thread)
            return *owed;
    }
    // The main thread's comes first.
    if (thread && m_callsOwed.isEmpty())
        m_callsOwed.append(makeUnique<CallsOwed>());
    m_callsOwed.append(makeUnique<CallsOwed>());
    m_callsOwed.last()->thread = thread;
    return *m_callsOwed.last();
}

void Runtime::rankIfItHasNone(CallsOwed& owed)
{
    if (owed.thread && !owed.rank)
        owed.rank = ++m_lastRank;
}

void Runtime::didTakeFrom(CallsOwed& owed)
{
    if (std::exchange(owed.hasStoodSinceFullCollection, false))
        m_vm.heap.didBeginToFinalizeWhatWaitedIndefinitely();
}

bool Runtime::isOfEarlierEra(JSCell* cell, uint32_t era, bool isWeakRef) const
{
    return isWeakRef ? uncheckedDowncast<PyWeakRef>(cell)->serial() < m_firstWeakRefOfEraThatIsServed : era < m_eraThatIsServed;
}

Deque<Runtime::CallOwed>& Runtime::halfFor(CallsOwed& owed, JSCell* cell, uint32_t era, bool isWeakRef)
{
    if (m_collectionIsWaitedFor && (m_callsThatAskerMakes == CallsOf::AllThreads || (m_callsThatAskerMakes == CallsOf::OfEarlierEras && isOfEarlierEra(cell, era, isWeakRef))))
        return owed.taken;
    rankIfItHasNone(owed);
    return owed.calls;
}

void Runtime::queueCall(Deque<CallOwed>& queue, JSCell* cell, uint32_t era, bool isWeakRef, bool continuesGroup)
{
    queue.append(CallOwed { cell, m_sequenceOfNextCall++, era & ((1u << 30) - 1), isWeakRef, continuesGroup });
}

void Runtime::noteCallback(CallsOwed& owed, PyWeakRef* weakRef)
{
    owed.callbacksOfThisCollection.append(weakRef);
    halfFor(owed, weakRef, 0, true); // (For the rank.)
}

// Each into its place among the callbacks with which its queue begins.
void Runtime::queueCallbacks(CallsOwed& owed, Vector<PyWeakRef*>& callbacks)
{
    std::ranges::sort(callbacks, [](PyWeakRef* a, PyWeakRef* b) {
        return a->serial() > b->serial();
    });
    for (auto* half : { &owed.taken, &owed.calls }) {
        Vector<CallOwed, 8> front;
        for (PyWeakRef* weakRef : callbacks) {
            if (&halfFor(owed, weakRef, 0, true) != half)
                continue;
            CallOwed call { weakRef, m_sequenceOfNextCall++, 0, true, false };
            while (!half->isEmpty() && half->first().comesBefore(call))
                front.append(half->takeFirst());
            front.append(call);
        }
        for (auto& call : front | std::views::reverse)
            half->prepend(call);
        assertIsInOrder(*half);
    }
    callbacks.shrink(0);
}

// Each half of a queue is, group by group: whoever looks for the next call looks at the heads.
void Runtime::assertIsInOrder(const Deque<CallOwed>& queue)
{
#if ASSERT_ENABLED
    const CallOwed* last = nullptr;
    unsigned groupsLeftToLookAt = 1000;
    for (auto& call : queue) {
        if (call.continuesGroup)
            continue;
        ASSERT(!last || !call.comesBefore(*last));
        last = &call;
        if (!--groupsLeftToLookAt)
            break;
    }
#else
    UNUSED_PARAM(queue);
#endif
}

bool Runtime::isLeftToOneThatServes(const CallOwed& call) const
{
    bool isLeft = false;
    for (auto* server : m_thoseThatServeOthers) {
        // (One that serves leaves nothing to another.)
        if (server->m_thread->number() == m_currentThread)
            return false;
        isLeft = isLeft || server->m_callsOf == CallsOf::AllThreads || isOfEarlierEra(call.cell, call.era, call.isWeakRef);
    }
    return isLeft;
}

// (Another that serves, and serves everything, may have taken what is not of an earlier era.)
bool Runtime::mayBeginWhatIsTakenOf(const CallsOwed& owed, CallsOf callsOf) const
{
    if (owed.taken.isEmpty())
        return false;
    if (owed.thread == m_currentThread)
        return callsOf != CallsOf::ThisThread || !isLeftToOneThatServes(owed.taken.first());
    switch (callsOf) {
    case CallsOf::ThisThread:
        return false;
    case CallsOf::AllThreads:
        return true;
    case CallsOf::OfEarlierEras: {
        auto& first = owed.taken.first();
        return isOfEarlierEra(first.cell, first.era, first.isWeakRef);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// To another thread, which has been told, and may come first. (The exit's looks at the head of the queue and no further: behind something younger there may be something older. That is for the thread itself, which is
// awake if it has made something since; or it is taken before the next collection of the exit's.)
bool Runtime::mayBeginWhatIsHandedOutTo(const CallsOwed& owed, CallsOf callsOf) const
{
    ASSERT(owed.thread != m_currentThread);
    if (owed.calls.isEmpty())
        return false;
    switch (callsOf) {
    case CallsOf::ThisThread:
        return false;
    case CallsOf::AllThreads:
        return true;
    case CallsOf::OfEarlierEras: {
        auto& first = owed.calls.first();
        return isOfEarlierEra(first.cell, first.era, first.isWeakRef);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What another thread has begun is that thread's.
bool Runtime::mayBeginCallsOf(const CallsOwed& owed, CallsOf callsOf) const
{
    if (owed.thread == m_currentThread)
        return !owed.inHand.isEmpty() || !owed.calls.isEmpty() || mayBeginWhatIsTakenOf(owed, callsOf);
    return mayBeginWhatIsHandedOutTo(owed, callsOf) || mayBeginWhatIsTakenOf(owed, callsOf);
}

// Callbacks of weak references come before finalizers. Of two callbacks the one whose weak reference was made later: the references to an object are told of its death latest first, and a reference that a
// callback has made, to what it then drops, is later than all that wait. Of two finalizers the one that was queued first.
bool Runtime::CallOwed::comesBefore(const CallOwed& other) const
{
    if (isWeakRef != other.isWeakRef)
        return isWeakRef;
    if (isWeakRef)
        return uncheckedDowncast<PyWeakRef>(cell)->serial() > uncheckedDowncast<PyWeakRef>(other.cell)->serial();
    return static_cast<int32_t>(sequence - other.sequence) < 0;
}

// Which call next: what it would be if, as it used to be, each thread had one queue, and what a collection finds that somebody waits for who makes the calls of all threads went into the queue of that one.
// - One that makes the calls of all threads takes first what other collections have handed out to other threads, thread by thread: it was found earlier than what its own collections have found.
// - Its own `calls`, and every `taken` that it may begin, are one queue to it: of their heads, what comes first. To the exit's also what other collections have handed out to other threads: while that step
//   lasted, whatever was of an earlier era went into the main thread's queue, whichever collection found it.
// - The rest of a group stands, for the thread that has it in its hand, where it stood: at the head of the finalizers of the queue that the group was taken from. If another group from the same place is begun
//   in the middle of it (their numbers may go by turns: cyclic garbage of several makers that one collection has found), the two are one from then on, in the order of their numbers.
bool Runtime::takeNextCall(CallsOwed& own, CallsOf callsOf, CallOwed& call)
{
    ASSERT(own.inHand.isEmpty() == own.groupsInHand.isEmpty());
    bool isInGroup = !own.inHand.isEmpty();
    uint32_t threadWhereHandStands = isInGroup ? own.groupsInHand.last().threadFromWhoseCalls : noThread;
    bool takesFromHand = false;
    CallsOwed* from = nullptr;
    Deque<CallOwed>* queue = nullptr;
    uint32_t threadFromWhoseCalls = noThread;
    if (callsOf == CallsOf::AllThreads) {
        for (auto& owed : m_callsOwed) {
            if (owed.get() != &own && (!owed->calls.isEmpty() || owed->thread == threadWhereHandStands) && (!from || owed->rank < from->rank))
                from = owed.get();
        }
        if (from) {
            if (from->thread == threadWhereHandStands && (from->calls.isEmpty() || !from->calls.first().isWeakRef))
                takesFromHand = true;
            else {
                queue = &from->calls;
                threadFromWhoseCalls = from->thread;
            }
        }
    }
    if (!queue && !takesFromHand) {
        auto look = [&](CallsOwed& owed, Deque<CallOwed>& candidate) {
            if (candidate.isEmpty() || (queue && !candidate.first().comesBefore(queue->first())))
                return;
            from = &owed;
            queue = &candidate;
        };
        look(own, own.calls);
        for (auto& owed : m_callsOwed) {
            if (mayBeginWhatIsTakenOf(*owed, callsOf))
                look(*owed, owed->taken);
            if (callsOf == CallsOf::OfEarlierEras && owed.get() != &own && mayBeginWhatIsHandedOutTo(*owed, callsOf))
                look(*owed, owed->calls);
        }
        takesFromHand = isInGroup && (!queue || !queue->first().comesBefore(own.inHand.first()));
    }
    if (takesFromHand) {
        call = own.inHand.takeFirst();
        if (own.inHand.size() == own.groupsInHand.last().restOfHand)
            own.groupsInHand.removeLast();
        didTakeFrom(own);
        return true;
    }
    if (!queue)
        return false;
    call = queue->takeFirst();
    ASSERT(!call.continuesGroup);
    didTakeFrom(*from);
    if (queue->isEmpty() || !queue->first().continuesGroup)
        return true;
    // The rest of the group, in front of what is in hand already.
    size_t restOfHand = own.inHand.size();
    if (!isInGroup) {
        while (!queue->isEmpty() && queue->first().continuesGroup)
            own.inHand.append(queue->takeFirst());
    } else {
        Vector<CallOwed, 8> rest;
        if (threadFromWhoseCalls == threadWhereHandStands) {
            restOfHand = own.groupsInHand.takeLast().restOfHand;
            while (own.inHand.size() > restOfHand) {
                while (!queue->isEmpty() && queue->first().continuesGroup && queue->first().comesBefore(own.inHand.first()))
                    rest.append(queue->takeFirst());
                rest.append(own.inHand.takeFirst());
            }
        }
        while (!queue->isEmpty() && queue->first().continuesGroup)
            rest.append(queue->takeFirst());
        for (auto& other : rest | std::views::reverse)
            own.inHand.prepend(other);
    }
    own.groupsInHand.append({ restOfHand, threadFromWhoseCalls });
    return true;
}

template<typename T>
static void moveAll(Deque<T>& from, Deque<T>& to)
{
    while (!from.isEmpty())
        to.append(from.takeFirst());
}

void Runtime::putBackWhatIsInHand(CallsOwed& owed)
{
    while (!owed.groupsInHand.isEmpty()) {
        size_t restOfHand = owed.groupsInHand.takeLast().restOfHand;
        Deque<CallOwed> group;
        while (owed.inHand.size() > restOfHand)
            group.append(owed.inHand.takeFirst());
        group.first().continuesGroup = false;
        mergeCalls(group, owed.calls);
    }
}

void Runtime::didQueueCallsIn(CallsOwed& owed)
{
    // For what is taken nobody is woken, by whatever collection: whoever has taken it tells the maker if it gives it back.
    if (owed.thread == m_currentThread || !owed.calls.isEmpty())
        didQueueCallsFor(owed.thread);
}

// Whether they have been told before or not: one that has looked while somebody served has left its queue alone.
void Runtime::tellThoseThatOwe()
{
    for (auto& owed : m_callsOwed) {
        if (owed->thread != m_currentThread && owed->hasCallsThatNobodyHasBegun())
            didQueueCallsFor(owed->thread);
    }
}

// Group by group. Each of the two is in order (CallOwed::comesBefore()), and so is what comes of them.
void Runtime::mergeCalls(Deque<CallOwed>& from, Deque<CallOwed>& into)
{
    if (from.isEmpty())
        return;
    Deque<CallOwed> merged;
    while (!from.isEmpty() && !into.isEmpty()) {
        auto& next = from.first().comesBefore(into.first()) ? from : into;
        do {
            merged.append(next.takeFirst());
        } while (!next.isEmpty() && next.first().continuesGroup);
    }
    moveAll(from, merged);
    moveAll(into, merged);
    into = WTF::move(merged);
    assertIsInOrder(into);
}

// From here on it is what another collection has handed out. (A fact that stays, not a state of whoever had taken it: a thread that has been told looks once, whenever it gets to it.)
bool Runtime::takenBecomesItsMakers()
{
    bool wasTaken = false;
    for (auto& owed : m_callsOwed) {
        if (owed->taken.isEmpty())
            continue;
        wasTaken = true;
        rankIfItHasNone(*owed);
        mergeCalls(owed->taken, owed->calls);
    }
    return wasTaken;
}

bool Runtime::giveBackWhatIsTaken()
{
    Locker locker { m_finalizationLock };
    // If nothing was taken this is no news: of two threads that are stuck in calls of their own and owe more, each would wake the other for ever.
    if (!takenBecomesItsMakers())
        return false;
    tellThoseThatOwe();
    return !m_isRunningPendingCalls && std::ranges::any_of(m_callsOwed, [&](auto& owed) WTF_REQUIRES_LOCK(m_finalizationLock) {
        return owed->thread == m_currentThread && mayBeginCallsOf(*owed, CallsOf::ThisThread);
    });
}

Runtime::ServesOtherThreads::ServesOtherThreads(Runtime& runtime, CallsOf callsOf)
    : m_runtime(runtime)
    , m_callsOf(callsOf)
{
    if (callsOf == CallsOf::ThisThread)
        return;
    m_thread = &runtime.threads().current();
    runtime.threads().didBeginToServeOthers();
    Locker locker { runtime.m_finalizationLock };
    runtime.assertNothingIsTakenUnlessSomebodyServes();
    // A thread serves once at a time: code of the program runs in the life of one of these only within runPendingCalls(), and whoever is about to serve asks isRunningPendingCalls() first.
    ASSERT(std::ranges::none_of(runtime.m_thoseThatServeOthers, [&](auto* other) {
        return other->m_thread == m_thread;
    }));
    runtime.m_thoseThatServeOthers.append(this);
    if (callsOf == CallsOf::OfEarlierEras) {
        ASSERT(!runtime.m_eraThatIsServed);
        runtime.m_eraThatIsServed = ++runtime.m_era;
        runtime.m_firstWeakRefOfEraThatIsServed = nextWeakRefSerial();
        takeWhatIsOwed();
    }
}

void Runtime::ServesOtherThreads::takeWhatIsOwed()
{
    for (auto& owed : m_runtime.m_callsOwed) {
        // Group by group.
        Deque<CallOwed> taken;
        Deque<CallOwed> left;
        bool isTaken = false;
        while (!owed->calls.isEmpty()) {
            CallOwed call = owed->calls.takeFirst();
            if (!call.continuesGroup)
                isTaken = m_runtime.isOfEarlierEra(call.cell, call.era, call.isWeakRef);
            (isTaken ? taken : left).append(call);
        }
        owed->calls = WTF::move(left);
        mergeCalls(taken, owed->taken);
    }
}

// What it serves of what is taken goes into its own queue. Whether there was any.
bool Runtime::ServesOtherThreads::keepWhatIsTaken()
{
    CallsOwed& own = m_runtime.callsOwedBy(m_runtime.m_currentThread);
    m_runtime.rankIfItHasNone(own);
    bool keepsSomething = false;
    for (auto& owed : m_runtime.m_callsOwed) {
        // Group by group.
        Deque<CallOwed> kept;
        Deque<CallOwed> left;
        bool isKept = false;
        while (!owed->taken.isEmpty()) {
            CallOwed call = owed->taken.takeFirst();
            if (!call.continuesGroup)
                isKept = m_callsOf == CallsOf::AllThreads || m_runtime.isOfEarlierEra(call.cell, call.era, call.isWeakRef);
            (isKept ? kept : left).append(call);
        }
        owed->taken = WTF::move(left);
        if (kept.isEmpty())
            continue;
        keepsSomething = true;
        mergeCalls(kept, own.calls);
        if (owed.get() != &own)
            m_runtime.didTakeFrom(*owed);
    }
    return keepsSomething;
}

void Runtime::ServesOtherThreads::willCollectAgain()
{
    if (m_callsOf != CallsOf::OfEarlierEras)
        return;
    Locker locker { m_runtime.m_finalizationLock };
    takeWhatIsOwed();
}

Runtime::ServesOtherThreads::~ServesOtherThreads()
{
    if (m_callsOf == CallsOf::ThisThread)
        return;
    m_runtime.threads().didCeaseToServeOthers();
    Locker locker { m_runtime.m_finalizationLock };
    // If something is still taken it leaves early: the handler of a signal has raised, say. What its collections have found is still its own to make, at its next safe point, like the rest of a group that it
    // has in its hand: the makers are in no better state for it than they were. (If there is another that serves, nobody can tell which of the two has taken what.)
    if (keepWhatIsTaken())
        m_runtime.didQueueCallsFor(m_runtime.m_currentThread);
    m_runtime.m_thoseThatServeOthers.removeFirst(this);
    if (m_callsOf == CallsOf::OfEarlierEras)
        m_runtime.m_eraThatIsServed = 0;
    m_runtime.assertNothingIsTakenUnlessSomebodyServes();
    m_runtime.tellThoseThatOwe();
}

void Runtime::assertNothingIsTakenUnlessSomebodyServes()
{
#if ASSERT_ENABLED
    if (!m_thoseThatServeOthers.isEmpty())
        return;
    for (auto& owed : m_callsOwed)
        ASSERT(owed->taken.isEmpty());
#endif
}

// With the GIL, or for the collector while whoever holds it is stopped.
uint32_t Runtime::threadToMakeCallsOf(uint32_t thread)
{
    if (!thread || threads().isFinalized())
        return 0;
    bool isThere = false;
    threads().forEachThread([&](ThreadState& other) {
        isThere |= other.number() == thread;
    });
    return isThere ? thread : 0;
}

void Runtime::threadDidEnd(uint32_t thread)
{
    ASSERT(thread);
    Locker locker { m_finalizationLock };
    size_t index = m_callsOwed.findIf([&](auto& owed) {
        return owed->thread == thread;
    });
    if (index == notFound)
        return;
    std::unique_ptr<CallsOwed> owed = WTF::move(m_callsOwed[index]);
    m_callsOwed.removeAt(index);
    CallsOwed& heir = callsOwedBy(0);
    bool leavesSomething = owed->hasFinalizers() || m_threadBetweenStartAndStop == thread;
    didTakeFrom(*owed);
    putBackWhatIsInHand(*owed);
    mergeCalls(owed->taken, heir.taken);
    mergeCalls(owed->calls, heir.calls);
    // What another thread has in its hand of this one's `calls` stands at the heir's from now on.
    for (auto& other : m_callsOwed) {
        for (auto& group : other->groupsInHand) {
            if (group.threadFromWhoseCalls == thread)
                group.threadFromWhoseCalls = other.get() == &heir ? noThread : heir.thread;
        }
    }
    if (m_threadBetweenStartAndStop == thread)
        m_threadBetweenStartAndStop = heir.thread;
    if (leavesSomething)
        didQueueCallsFor(0);
}

// A generator that has not begun has nothing to end. (A coroutine that has not begun is complained about.)
static bool hasNothingToFinalize(JSCell* cell)
{
    if (cell->type() != PyGeneratorType)
        return false;
    auto* generator = uncheckedDowncast<PyGenerator>(cell);
    return !generator->isStarted() && generator->kind() != PyGenerator::Kind::Coroutine && !generator->finalizer();
}

// Its finalizer is code of the program, or may be. What the finalizers of the built-in types do (close a file) looks the same in any
// order, as long as an object comes before what it refers to.
static bool hasFinalizerOfProgram(Runtime& rt, JSCell* cell)
{
    return cell->type() == PyGeneratorType || rt.typeOf(cell)->isHeapType();
}

bool Runtime::keepUnreachableObjectsToFinalize(SlotVisitor& visitor, StepOfKeeping step)
{
    Locker locker { m_finalizationLock };
    if (step == StepOfKeeping::WhatObserversKeep)
        return m_markingObserver && m_markingObserver->didFinishMarking(visitor);
    // This is called again and again until it finds no more, and in a collection that the engine began by itself the program may
    // run in between, while what this has kept is marked. So each time the lists are left as the program may find them: whatever is
    // registered and not queued is in one of them, and there is no hole.
    // What is queued is kept, whoever queued it and whenever: an earlier collection, or the program, which need not tell the collector. Nothing else marks it.
    if (step != StepOfKeeping::WhatHasDied) {
        bool keptSomething = false;
        auto keep = [&](JSCell* cell) {
            if (visitor.isMarked(cell))
                return;
            visitor.appendUnbarriered(cell);
            keptSomething = true;
        };
        // First the queues that stand still: the heap wants to know how much that is.
        for (auto& owed : m_callsOwed) {
            if (owed->hasStoodSinceFullCollection != (step == StepOfKeeping::QueuesThatStandStill))
                continue;
            owed->forEachCall([&](const CallOwed& call) {
                keep(call.cell);
            });
        }
        return keptSomething;
    }
    // All of them are found before any is marked.
    Vector<JSCell*> candidates;
    Vector<Finalizable*> places;
    Vector<Finalizable*> callbacksOfForeignCode;
    unsigned finalizersOfProgram = 0;
    auto look = [&](Vector<Finalizable>& list) {
        size_t kept = 0;
        for (auto entry : list) {
            JSCell* cell = entry.cell();
            if (isNoLongerWanted(cell)) {
                didTakeOffList(cell);
                continue;
            }
            Finalizable& place = list[kept++];
            place = entry;
            // What has nothing to finalize is left to die, unless one of the others reaches it: see didMarkAll().
            if (visitor.isMarked(cell) || hasNothingToFinalize(cell))
                continue;
            if (cell->classInfo() == Ctypes::PyCThunk::info()) {
                callbacksOfForeignCode.append(&place);
                continue;
            }
            candidates.append(cell);
            places.append(&place);
            finalizersOfProgram += hasFinalizerOfProgram(*this, cell);
        }
        list.shrink(kept);
    };
    bool isFull = m_vm.heap.collectionScope() == CollectionScope::Full;
    if (isFull)
        look(m_oldFinalizable);
    look(m_youngFinalizable);
    // Nothing in the heap says who calls a callback of ctypes: a library has its address. Finalizers do, by way of the library: they
    // close what it opened, and it tells the program so. And a package keeps its callbacks in its modules, which CPython lets go
    // after the program's own at exit, where here all that dies is found at once. So the code of a callback is freed when no finalizer
    // is left to run: until then it is kept, and comes up again in the next collection.
    if (!callbacksOfForeignCode.isEmpty()) {
        bool finalizersAreLeft = !candidates.isEmpty() || std::ranges::any_of(m_callsOwed, [](auto& owed) {
            return owed->hasFinalizers();
        });
        for (Finalizable* place : callbacksOfForeignCode) {
            if (finalizersAreLeft)
                visitor.appendUnbarriered(place->cell());
            else {
                candidates.append(place->cell());
                places.append(place);
            }
        }
        if (finalizersAreLeft && candidates.isEmpty())
            return true;
    }
    bool wasAskedFor = m_collectionIsWaitedFor;
    bool wantsGarbage = wasAskedFor && m_markingObserver && m_markingObserver->wantsGarbage();
    // That takes time for every dead cell, which at exit is every cell there is.
    bool looksAtAllThatDied = wasAskedFor && (wantsGarbage || finalizersOfProgram > 1);
    if (candidates.isEmpty() && !wantsGarbage)
        return false;

    // An object is finalized before what it refers to.
    Vector<PyWeakRef*> weakRefs;
    if (!candidates.isEmpty() && !m_leavesCyclicGarbageForLater)
        weakRefs = weakRefsWithCallbacksToUnmarkedObjects(visitor);
    Vector<JSCell*> referents = weakRefs.map([](PyWeakRef* weakRef) {
        return weakRef->referent();
    });
    bool areOfSeveralThreads = std::ranges::any_of(places, [&](Finalizable* entry) {
        return entry->thread != places[0]->thread;
    });
    // It goes through what has died, and the global object may be among that: see visitPendingCalls().
    m_isPuttingFinalizersInOrder = true;
    FinalizationOrder order = finalizationOrder(m_vm.heap, visitor, candidates.span(), {
        .askedAbout = referents.span(),
        .looksAtAllThatDied = looksAtAllThatDied,
        .wantsGarbage = wantsGarbage,
        .wantsToKnowWhatReferenceCountsWouldFree = m_leavesCyclicGarbageForLater,
        .wantsPieces = areOfSeveralThreads,
        .holdersComeFirst = m_hasLetGoOfModules,
        .namespaces = m_namespacesOfModulesLetGoOf.span(),
        .mostLooksAtInnerReferences = m_mostLooksAtInnerReferences,
    });
    m_isPuttingFinalizersInOrder = false;

    // All that the candidates reach is about to be marked, so the weak references to it stay as they are until the finalizers have
    // run and another collection finds it dead: first __del__, then the weak references, as with reference counts. But what is
    // in a cycle, or hangs off one, is not freed by reference counts, and CPython's collector does it the other way round for
    // the weak references that have callbacks: it clears them, calls the callbacks of those that are not garbage
    // themselves, and then calls the finalizers. (So such a reference stays dead if a finalizer brings its referent back.)
    //
    // Whose queue: each object is its maker's, whoever waits for this collection and is going to make the calls (see CallsOwed). But what has to happen in a
    // certain order is not split among threads: in a round that takes parents and children at once, what the going of an object frees goes with that object, to its maker; and
    // of what CPython's collector would have found, which calls all the finalizers itself, one after the other, each piece that
    // hangs together goes to the maker of its oldest, with the callbacks of the weak references to it. And whoever begins such a thing makes all of it: such a tree is a group (CallOwed::continuesGroup),
    // and so are the finalizers of all the cyclic garbage that a thread is given. No more than that: what has nothing to do with one another is not to wait for one another (of two objects that two
    // threads have dropped, the finalizer of the one may wait for that of the other, whatever else has died with them).
    auto cyclicGarbage = order.now.span().subspan(order.freedByReferenceCounts);
    auto makerOf = [&](unsigned candidate) {
        return threadToMakeCallsOf(places[candidate]->thread);
    };
    auto takerOfPieceWithOldest = [&](unsigned oldest) {
        return makerOf(oldest < candidates.size() ? oldest : cyclicGarbage.empty() ? 0 : cyclicGarbage[0]);
    };

    for (unsigned i = 0; i < weakRefs.size(); ++i) {
        if (!order.isCyclicGarbageToFinalize[i])
            continue;
        PyWeakRef* weakRef = weakRefs[i];
        weakRef->setReferent(nullptr);
        if (visitor.isMarked(weakRef))
            noteCallback(callsOwedBy(takerOfPieceWithOldest(areOfSeveralThreads ? order.oldestOfPieceOfCellAskedAbout[i] : FinalizationOrder::none)), weakRef);
        else
            weakRef->clearCallback();
    }
    // (Now, not when the collection ends: the program may run before that, and the finalizers of that garbage are about to be queued.)
    for (auto& owed : m_callsOwed) {
        if (!owed->callbacksOfThisCollection.isEmpty())
            queueCallbacks(*owed, owed->callbacksOfThisCollection);
    }

    // Where the group of each thread that has one has begun.
    Vector<std::pair<uint32_t, Deque<CallOwed>*>, 4> groups;
    auto queueInGroup = [&](uint32_t taker, JSCell* cell, uint32_t era) WTF_REQUIRES_LOCK(m_finalizationLock) {
        for (auto& [thread, queue] : groups) {
            if (thread == taker) {
                queueCall(*queue, cell, era, false, true);
                return;
            }
        }
        auto& queue = halfFor(callsOwedBy(taker), cell, era, false);
        groups.append({ taker, &queue });
        queueCall(queue, cell, era);
    };
    Vector<uint32_t> takers;
    takers.fill(noThread, candidates.size());
    // They come tree by tree, each from its top down: see DeadGraph::order().
    uint32_t takerOfTree = 0;
    Deque<CallOwed>* queueOfTree = nullptr;
    for (unsigned n = 0; n < order.now.size(); ++n) {
        unsigned candidate = order.now[n];
        Finalizable& entry = *places[candidate];
        JSCell* cell = entry.cell();
        visitor.appendUnbarriered(cell);
        bool isCyclicGarbage = n >= order.freedByReferenceCounts;
        if (m_leavesCyclicGarbageForLater && isCyclicGarbage)
            continue;
        if (isCyclicGarbage) {
            takers[candidate] = takerOfPieceWithOldest(areOfSeveralThreads ? order.oldestOfPiece[candidate] : FinalizationOrder::none);
            queueInGroup(takers[candidate], cell, entry.era);
        } else {
            bool continuesTree = order.takesAllLevels && order.letGoBy[candidate] != FinalizationOrder::none;
            if (!continuesTree) {
                takerOfTree = makerOf(candidate);
                queueOfTree = &halfFor(callsOwedBy(takerOfTree), cell, entry.era, false);
            }
            takers[candidate] = takerOfTree;
            queueCall(*queueOfTree, cell, entry.era, false, continuesTree);
        }
        entry.clear();
        didTakeOffList(cell, true);
    }
    // The others are kept alive by those. They come up again, in this order, in the collection that finds those gone: they go to the
    // end of the list. With reference counts the thread that finalizes an object frees what it alone refers to: what waits for
    // another goes to the thread of that one.
    Vector<Finalizable> toComeLast;
    for (unsigned candidate : order.later) {
        Finalizable entry = *places[candidate];
        unsigned first = candidate;
        while (order.letGoBy[first] != FinalizationOrder::none)
            first = order.letGoBy[first];
        if (takers[first] != noThread)
            entry.thread = takers[first];
        toComeLast.append(entry);
        places[candidate]->clear();
    }
    if (!candidates.isEmpty()) {
        auto isHole = [](auto& entry) {
            return !entry.cell();
        };
        if (isFull)
            m_oldFinalizable.removeAllMatching(isHole);
        m_youngFinalizable.removeAllMatching(isHole);
        m_youngFinalizable.appendVector(toComeLast);
    }

    Vector<JSCell*> toKeep;
    if (wantsGarbage)
        m_markingObserver->didFindGarbage(order.garbage.span(), order.cellsLookedAt, toKeep);
    for (JSCell* cell : toKeep)
        visitor.appendUnbarriered(cell);
    return !candidates.isEmpty() || !toKeep.isEmpty();
}

// Dead or alive.
Vector<PyWeakRef*> Runtime::weakRefsWithCallbacksToUnmarkedObjects(SlotVisitor& visitor)
{
    Vector<PyWeakRef*> result;
    auto* space = m_vm.heap.pyWeakRefSpace<SubspaceAccess::Concurrently>();
    if (!space)
        return result;
    auto look = [&](HeapCell* cell) {
        auto* weakRef = static_cast<PyWeakRef*>(cell);
        if (!weakRef->structureID() || weakRef->isInternal() || !weakRef->callback())
            return;
        JSCell* referent = weakRef->referent();
        if (referent && !visitor.isMarked(referent) && weakRef->structure()->realm() == m_globalObject)
            result.append(weakRef);
    };
    space->forEachMarkedBlock([&](MarkedBlock::Handle* handle) {
        handle->forEachLiveCell([&](size_t, HeapCell* cell, HeapCell::Kind) {
            look(cell);
            return IterationStatus::Continue;
        });
    });
    // One that an earlier collection found dead may still stand, as in a block: see Heap::mayRunEmbedderFinalizers(). One that was alive when this collection began is live until it ends.
    space->forEachPreciseAllocation([&](PreciseAllocation* allocation) {
        if (allocation->isLive())
            look(allocation->cell());
    });
    return result;
}

void Runtime::didMarkAll(SlotVisitor& visitor)
{
    Locker locker { m_finalizationLock };
    bool isFull = m_vm.heap.collectionScope() == CollectionScope::Full;
    if (isFull) {
        for (auto& owed : m_callsOwed)
            owed->hasStoodSinceFullCollection = owed->hasFinalizers();
    }
    auto stays = [&](JSCell* cell) {
        if (visitor.isMarked(cell))
            return true;
        // What had nothing to finalize was not kept. Anything else would be freed now and never be finalized.
        ASSERT(hasNothingToFinalize(cell));
        didTakeOffList(cell);
        return false;
    };
    if (isFull) {
        m_oldFinalizable.removeAllMatching([&](auto& entry) {
            return !stays(entry.cell());
        });
    }
    for (auto& entry : m_youngFinalizable) {
        if (stays(entry.cell()))
            m_oldFinalizable.append(entry);
    }
    m_youngFinalizable.shrink(0);
    if (isFull && m_oldFinalizable.size() < m_oldFinalizable.capacity() / 4)
        m_oldFinalizable.shrinkToFit();

#if ASSERT_ENABLED
    if (isFull) {
        UncheckedKeyHashSet<JSCell*> seen;
        for (auto& entry : m_oldFinalizable) {
            JSCell* cell = entry.cell();
            if (!visitor.isMarked(cell) || !seen.add(cell).isNewEntry) [[unlikely]] {
                dataLogLn("A registered object of type ", typeOf(cell)->nameInMessages(), visitor.isMarked(cell) ? " is in the list twice" : " is about to be freed");
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
    }
    // A generator that is running is referred to by the frame of its body, in whatever thread.
    auto assertAreMarked = [&](PyGenerator* running) {
        for (PyGenerator* generator = running; generator; generator = generator->resumedBy())
            ASSERT(visitor.isMarked(generator) && generator->isRunning());
    };
    assertAreMarked(m_runningGenerator);
    threads().forEachStateSetAsideOf(*this, [&](ThreadState&, ThreadLocalState& state) {
        assertAreMarked(state.runningGenerator);
    });
#endif
    // What a collection has seen alive is old.
    if (isFull) {
        m_finalized.removeIf([&](JSCell* cell) {
            return !visitor.isMarked(cell);
        });
        for (JSCell* cell : m_justFinalized) {
            if (visitor.isMarked(cell))
                m_finalized.add(cell);
        }
        m_justFinalized.clear();
    } else {
        m_justFinalized.removeAllMatching([&](JSCell* cell) {
            return !visitor.isMarked(cell);
        }, m_justFinalizedAndOld);
    }
    m_justFinalizedAndOld = m_justFinalized.size();
    forgetDeadTemporaries(visitor);
    m_namespacesOfModulesLetGoOf.removeAllMatching([&](PyDict* dict) {
        return !visitor.isMarked(dict);
    });

    // Those that owe since an earlier collection are told again: a thread may have been about to wait when it was told.
    for (auto& owed : m_callsOwed) {
        if (owed->hasCallsThatNobodyHasBegun())
            didQueueCallsIn(*owed);
    }

    double duration = m_collectionBegan ? (MonotonicTime::now() - m_collectionBegan).seconds() : 0;
    m_collectionBegan = { };
    // gc.collect() does its own accounting. See gcDisable().
    if (gc.isCollecting || gc.collectsForItself || !gc.isEnabled)
        return;
    int generation = m_vm.heap.collectionScope() == CollectionScope::Full ? 2 : 0;
    ++gc.stats[generation].collections;
    gc.stats[generation].duration += duration;
    // In CPython nothing is collected between "start" and "stop". What comes in between here goes untold: what it finds is called before "stop" like the rest, and a
    // callback that brings about a collection whenever it is called would otherwise never let the program go on.
    if (hasCollectionCallbacks() && m_threadBetweenStartAndStop == noThread) {
        m_collectionsToTellOf.append({ generation, duration });
        didQueueCallsFor(m_currentThread, false);
    }
}

bool Runtime::willTellOfCollect()
{
    Locker locker { m_finalizationLock };
    if (m_threadBetweenStartAndStop != noThread)
        return false;
    m_threadBetweenStartAndStop = m_currentThread;
    return true;
}

void Runtime::didTellOfCollect()
{
    Locker locker { m_finalizationLock };
    ASSERT(m_threadBetweenStartAndStop == m_currentThread && !m_collectionBeingToldOf);
    m_threadBetweenStartAndStop = noThread;
    if (!m_collectionsToTellOf.isEmpty())
        didQueueCallsFor(m_currentThread, false);
}

void Runtime::WeakCellTable::removeUnmarked(JSC::Heap& heap)
{
    for (unsigned i = 0; i < m_cells.size(); ++i) {
        if (m_cells[i] && !heap.isMarked(m_cells[i])) {
            m_cells[i] = nullptr;
            m_free.append(i);
        }
    }
    if (m_free.size() == m_cells.size()) {
        m_cells.clear();
        m_free.clear();
    }
}

void Runtime::reconcileWeakReferencesAtGCEnd()
{
    scheduledTasks.removeUnmarked(m_vm.heap);
    unpinMemoryOfDeadViews();
    m_typeLookupCache->removeUnmarked(m_vm.heap);
    if (CApi::isSetUpFor(*this))
        CApi::didFinishMarking(m_vm.heap);
    Locker locker { m_finalizationLock };
    // Each thread is told once of all that there is for it: to tell may be a system call.
    for (auto& owed : m_callsOwed) {
        if (owed->callbacksOfThisCollection.isEmpty())
            continue;
        queueCallbacks(*owed, owed->callbacksOfThisCollection);
        didQueueCallsIn(*owed);
    }
}

bool Runtime::hasBeenFinalized(JSCell* cell)
{
    Locker locker { m_finalizationLock };
    return m_finalized.contains(cell) || m_justFinalized.contains(cell);
}

void Runtime::forEachUnmarkedObjectWithFinalizer(SlotVisitor& visitor, const ScopedLambda<void(JSCell*, bool)>& function)
{
    Locker locker { m_finalizationLock };
    for (auto& owed : m_callsOwed) {
        owed->forEachCall([&](const CallOwed& call) {
            if (!call.isWeakRef && !visitor.isMarked(call.cell))
                function(call.cell, true);
        });
    }
    // What is in a list for nothing stays there until the next collection has looked, and may be in a queue as well (Runtime::releaseUnreachableBuffers()).
    for (auto* list : { &m_oldFinalizable, &m_youngFinalizable }) {
        for (auto& entry : *list) {
            if (JSCell* cell = entry.cell(); cell && !visitor.isMarked(cell) && !isNoLongerWanted(cell) && !hasNothingToFinalize(cell))
                function(cell, false);
        }
    }
}

void Runtime::forEachObjectToBeFinalized(const ScopedLambda<void(JSCell*)>& function)
{
    Locker locker { m_finalizationLock };
    for (auto& owed : m_callsOwed) {
        owed->forEachCall([&](const CallOwed& call) {
            if (!call.isWeakRef)
                function(call.cell);
        });
    }
}

void Runtime::queueWeakRefCallback(PyWeakRef* weakRef)
{
    Locker locker { m_finalizationLock };
    noteCallback(callsOwedBy(threadToMakeCallsOf(weakRef->thread())), weakRef);
}

template<typename Visitor>
void Runtime::visitPendingCalls(Visitor& visitor)
{
    // A realm has been dropped, and the collector looks at what the dead global object refers to in order to tell in which order the realm's objects are to be finalized: then it has the lock already
    // (keepUnreachableObjectsToFinalize()). The world is stopped then, so nobody else reads the flag. Where it looks on behalf of an observer (didMarkAllThatIsReachable()) nobody has the lock.
    std::optional<Locker<Lock>> locker;
    if (!m_isPuttingFinalizersInOrder)
        locker.emplace(m_finalizationLock);
    for (auto& owed : m_callsOwed) {
        owed->forEachCall([&](const CallOwed& call) {
            visitor.appendUnbarriered(call.cell);
        });
        for (PyWeakRef* weakRef : owed->callbacksOfThisCollection)
            visitor.appendUnbarriered(weakRef);
    }
}

template void Runtime::visitPendingCalls(AbstractSlotVisitor&);

// ---- Exceptions nobody can catch ----

static constexpr ASCIILiteral unraisableFields[] = { "exc_type"_s, "exc_value"_s, "exc_traceback"_s, "err_msg"_s, "object"_s };
static constexpr StructSeqDef unraisableDef { "UnraisableHookArgs"_s, unraisableFields, 5, true, true };

PyType* unraisableHookArgsType(JSGlobalObject* globalObject) { return structSeqTypeFor(globalObject, unraisableDef); }

void reportUnraisable(JSGlobalObject* globalObject, const String& message, JSValue object)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    // The host is ending what runs here, which goes on to the caller. (If it ended the look at what JS threw, that is still there.)
    auto wasEnded = [&] {
        std::ignore = scope.tryClearException();
    };
    JSValue thrown = scope.exception() ? scope.exception()->value() : JSValue();
    JSValue exception = takePendingException(globalObject);
    if (!exception)
        return wasEnded();
    if (rt.keepIfRaisedBySignalHandler(thrown))
        return;

    JSValue arguments;
    {
        // Nothing of the program's runs here. If the host ends what runs here, that waits for what is called next.
        DeferTerminationForAWhile deferred(vm);
        JSValue traceback = PyException::from(exception)->traceback();
        arguments = makeStructSeq(globalObject, unraisableHookArgsType(globalObject), {
            rt.typeOf(exception), exception, traceback ? traceback : jsUndefined(),
            message.isNull() ? jsUndefined() : JSValue(makeStr(vm, message)), object ? object : jsUndefined() });
    }

    JSValue hook = rt.sysDict()->get(internString(vm, "unraisablehook"_s));
    if (hook && failed(scope, audit(globalObject, "sys.unraisablehook"_s, hook, arguments))) {
        JSValue second = takePendingException(globalObject);
        if (!second) [[unlikely]]
            return wasEnded();
        writeToStderr(globalObject, "Exception ignored in audit hook:\n"_s);
        printException(globalObject, second);
        hook = { };
    }
    if (hook && !isNone(hook)) {
        call(globalObject, hook, arguments);
        if (!scope.exception())
            return;
        JSValue second = takePendingException(globalObject);
        if (!second) [[unlikely]]
            return wasEnded();
        writeToStderr(globalObject, "Exception ignored in sys.unraisablehook:\n"_s);
        printException(globalObject, second);
    }
    writeToStderr(globalObject, makeString(message.isNull() ? "Exception ignored"_str : message, ":\n"_s));
    printException(globalObject, exception);
}

// ---- Running what is queued ----

// What goes wrong is reported. Only a termination is left pending.
void warnOfUnawaitedCoroutine(JSGlobalObject* globalObject, PyGenerator* coroutine)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto report = [&] {
        if (scope.exception() && !vm.hasPendingTerminationException())
            reportUnraisable(globalObject, makeString("Exception ignored while finalizing coroutine "_s, reprForMessage(globalObject, coroutine)), { });
    };
    // warnings.py words it, as it can say where the coroutine was made. If that fails for any reason but that the filters make
    // the warning an exception, the warning is given here.
    bool hasWarned = false;
    JSValue module = importModule(globalObject, "warnings"_s);
    if (scope.exception() && clearPendingExceptionIfMatches(globalObject, rt.ImportError()))
        module = { };
    JSValue function = module ? getAttrIfPresent(globalObject, module, internString(vm, "_warn_unawaited_coroutine"_s)) : JSValue();
    EXCEPTION_ASSERT(!scope.exception() || !function);
    if (function) {
        call(globalObject, function, coroutine);
        hasWarned = !scope.exception() || pendingExceptionMatches(globalObject, rt.RuntimeWarning());
    }
    report();
    if (hasWarned || vm.hasPendingTerminationException())
        return;
    warnAbout(globalObject, rt.RuntimeWarning(), makeString("coroutine "_s, reprForMessage(globalObject, coroutine->qualname()), " was never awaited"_s), 1, coroutine);
    report();
}

static void finalizeAwaitable(JSGlobalObject* globalObject, PyAsyncGenAwaitable* awaitable)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(rt.vm());
    if (awaitable->state() != PyAsyncGenAwaitable::State::Initial)
        return;
    using Kind = PyAsyncGenAwaitable::Kind;
    ASCIILiteral method = awaitable->kind() == Kind::Send ? "asend"_s : awaitable->kind() == Kind::Throw ? "athrow"_s : "aclose"_s;
    PyGenerator* generator = awaitable->generator();
    warnAbout(globalObject, rt.RuntimeWarning(), makeString("coroutine method '"_s, method, "' of "_s, reprForMessage(globalObject, generator->qualname()), " was never awaited"_s), 1, generator);
    if (scope.exception())
        reportUnraisable(globalObject, makeString("Exception ignored while finalizing async generator "_s, reprForMessage(globalObject, generator)), { });
}

static void finalizeGenerator(JSGlobalObject* globalObject, PyGenerator* generator)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (generator->isCompleted() || generator->isRunning())
        return;

    if (generator->kind() == PyGenerator::Kind::AsyncGenerator) {
        // The event loop asked to be the one to close it.
        if (JSValue finalizer = generator->finalizer(); finalizer && !generator->isClosed) {
            call(globalObject, finalizer, generator);
            if (scope.exception())
                reportUnraisable(globalObject, makeString("Exception ignored while finalizing generator "_s, reprForMessage(globalObject, generator)), { });
            return;
        }
    }
    if (generator->kind() == PyGenerator::Kind::Coroutine && !generator->isStarted()) {
        warnOfUnawaitedCoroutine(globalObject, generator);
        generator->complete();
        return;
    }
    generatorClose(globalObject, generator);
    if (scope.exception())
        reportUnraisable(globalObject, makeString("Exception ignored while closing generator "_s, reprForMessage(globalObject, generator)), { });
}

void finalizeGeneratorNow(JSGlobalObject* globalObject, PyGenerator* generator)
{
    // Not while the host is ending what runs here: `finally` is code of the program. It is left to the collector, which does not know
    // of one that has no handler of its own but something to close all the same: whom it delegates to, the generator of a loop.
    if (getVM(globalObject).hasPendingTermination()) [[unlikely]] {
        if (generator->finalization() == PyGenerator::Finalization::None) {
            generator->setFinalization(PyGenerator::Finalization::Registered);
            runtime(globalObject).registerFinalizable(generator);
        }
        return;
    }
    // Once. If it does not let itself be closed, the collector is not to try again.
    generator->setFinalization(PyGenerator::Finalization::None);
    finalizeGenerator(globalObject, generator);
}

static void finalizeObject(JSGlobalObject* globalObject, JSCell* cell)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (auto* generator = PyGenerator::tryFrom(cell))
        return finalizeGenerator(globalObject, generator);
    if (auto* awaitable = PyAsyncGenAwaitable::tryFrom(cell))
        return finalizeAwaitable(globalObject, awaitable);
    JSValue method = rt.typeOf(cell)->lookup(Special::Del);
    if (!method)
        return;
    callSpecial(globalObject, method, cell);
    if (scope.exception() && !getVM(globalObject).hasPendingTerminationException())
        reportUnraisable(globalObject, makeString("Exception ignored while calling deallocator "_s, reprForMessage(globalObject, method)), { });
}

// The collector's thread may be the one that calls this, while the thread that holds the GIL is stopped.
void Runtime::didQueueCallsFor(uint32_t thread, bool mayUncoverMore)
{
    if (thread == m_currentThread) {
        m_hasPendingCalls = true;
        noteCallsOwed(m_vm);
    } else
        threads().wakeToMakeCalls(thread);
    if (mayUncoverMore)
        threads().didQueueCallsThatMayUncoverMore(thread);
}

bool Runtime::owesCalls(CallsOf callsOf)
{
    Locker locker { m_finalizationLock };
    if (m_threadBetweenStartAndStop == noThread ? !m_collectionsToTellOf.isEmpty() : m_threadBetweenStartAndStop == m_currentThread && m_collectionBeingToldOf)
        return true;
    return std::ranges::any_of(m_callsOwed, [&](auto& owed) WTF_REQUIRES_LOCK(m_finalizationLock) {
        return mayBeginCallsOf(*owed, callsOf);
    });
}

static std::atomic<void (*)(VM&)> s_callsOwedFunction;

void setCallsOwedFunction(void (*function)(VM&))
{
    s_callsOwedFunction.store(function);
}

void noteCallsOwed(VM& vm)
{
    vm.traps().fireTrap(VMTraps::NeedPythonCalls);
    if (auto* function = s_callsOwedFunction.load())
        function(vm);
}

static bool makeCallsOwedToProgram(VM&);

void makeCallsOwed(VM& vm)
{
    vm.traps().clearTrap(VMTraps::NeedPythonCalls);
    if (vm.heap.pythonRuntimes().isEmpty())
        return;
    auto scope = DECLARE_THROW_SCOPE(vm);
    // (One runtime's frame is at the safe point. The others have none.)
    Vector<std::pair<Runtime*, Monitoring::NotesOfJump>, 1> notesOfJump;
    for (Runtime* runtime : vm.heap.pythonRuntimes())
        notesOfJump.append({ runtime, runtime->monitoring().takeNotesOfJump() });
    auto putNotesBack = makeScopeExit([&] {
        for (auto& [runtime, notes] : notesOfJump) {
            if (vm.heap.pythonRuntimes().contains(runtime))
                runtime->monitoring().putBack(notes);
        }
    });
    // Another thread has waited long enough for its turn.
    Threads& threads = vm.heap.pythonRuntimes().first()->threads();
    if (threads.dropIsRequested()) [[unlikely]] {
        threads.yield();
        // That may have been the last thread of the last runtime, whose realm had been dropped, and a collection has come since.
        if (vm.heap.pythonRuntimes().isEmpty()) [[unlikely]]
            return;
    }
    if (threads.throwIfStopping()) [[unlikely]]
        return;
    // What other threads had to leave to the main one.
    if (vm.heap.hasPostponedEmbedderFinalizers() && vm.heap.mayRunEmbedderFinalizers()) [[unlikely]] {
        vm.heap.runPostponedEmbedderFinalizers();
        if (vm.heap.pythonRuntimes().isEmpty()) [[unlikely]]
            return;
    }
    // The frames of C++ from here down to a finalizer or a handler stay while that runs, and it may ask for a collection.
    wipeStackBelow(vm.heap.pythonRuntimes().first()->globalObject(), stackBytesWipedForCallsOwed);
    bool hasFinalized = makeCallsOwedToProgram(vm);
    scope.release();
    // What has been finalized is to be found dead by the next collection. The frames of its finalizer are gone, but not what they
    // held, and this may be the loop of the host, where the same few frames of C++ come to lie there again and again.
    if (hasFinalized && !vm.heap.pythonRuntimes().isEmpty())
        wipeStackBelow(vm.heap.pythonRuntimes().first()->globalObject());
}

bool safePointSlow(VM& vm)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    makeCallsOwed(vm);
    return !scope.exception();
}

// Whether it has called anything but handlers of signals.
static NEVER_INLINE bool makeCallsOwedToProgram(VM& vm)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasFinalized = false;
    // What is called may start Python in another global object, and a collection may find one gone.
    Vector<Runtime*, 1> runtimes = vm.heap.pythonRuntimes();
    for (Runtime* runtime : runtimes) {
        if (!vm.heap.pythonRuntimes().contains(runtime))
            continue;
        // What the handler of a signal raises is thrown. The trap is set again then: the rest is for the next safe point.
        if (failed(scope, checkOnlySignals(runtime->globalObject())))
            return hasFinalized;
        if (runtime->hasPendingCalls()) {
            hasFinalized = true;
            runtime->runPendingCalls(true);
            RETURN_IF_EXCEPTION(scope, hasFinalized);
        }
    }
    return hasFinalized;
}

void Runtime::didRaiseInSignalHandler(JSValue thrown)
{
    // Where CPython would be running the finalizer too, it is as in CPython: in gc.collect(), and at exit. (What is finalized at once, a
    // temporary say, is not called from the queue.)
    if (!m_isRunningPendingCalls || isFinalized || gc.collectingThread == m_currentThread)
        return;
    setSlot(Slot::RaisedBySignalHandler, thrown);
    m_threadWhereSignalHandlerRaised = m_currentThread;
    m_hasKeptWhatSignalHandlerRaised = false;
}

bool Runtime::keepIfRaisedBySignalHandler(JSValue thrown)
{
    if (m_threadWhereSignalHandlerRaised != m_currentThread || thrown != slot(Slot::RaisedBySignalHandler))
        return false;
    m_hasKeptWhatSignalHandlerRaised = true;
    return true;
}

void Runtime::runPendingCalls(bool mayPutOff, CallsOf callsOf)
{
    // What is queued while these run is run by the loop below, not within one of the calls.
    if (m_isRunningPendingCalls)
        return;

    // With reference counts a finalizer runs where its object is dropped. Here it runs wherever the program happens to be when the
    // collector has come, which may be at the recursion limit, where every call fails, and its effect would be lost. So what is
    // called here has this many frames to itself, whatever the depth. (The depth is lowered and the limit left alone:
    // sys.getrecursionlimit() says what the program set.) The stack has a limit of its own. Near that, this waits for better times.
    constexpr int framesForPendingCalls = 200;
#if ASAN_ENABLED || ASSERT_ENABLED
    constexpr size_t stackForPendingCalls = 2 * MB;
#else
    constexpr size_t stackForPendingCalls = 256 * KB;
#endif
    if (mayPutOff && !isSafeToRecurseWithMargin(m_vm, std::min(stackForPendingCalls, Thread::currentSingleton().stack().size() / 4))) [[unlikely]] {
        m_vm.traps().fireTrap(VMTraps::NeedPythonCalls);
        return;
    }
    SetForScope loweredDepth(frameDepth, std::min(frameDepth, std::max(recursionLimit - framesForPendingCalls, 0)));

    SetForScope isRunning(m_isRunningPendingCalls, true);
    // What a handler raises between two calls is thrown from here as it is.
    auto forgetWhatSignalHandlerRaised = makeScopeExit([&] {
        if (m_threadWhereSignalHandlerRaised != m_currentThread) [[likely]]
            return;
        setSlot(Slot::RaisedBySignalHandler, { });
        m_threadWhereSignalHandlerRaised = noThread;
    });
    SetForScope stackWhereTheyBegan(m_stackWhereUnaskedCallsBegan, mayPutOff ? currentStackPointer() : nullptr);
    JSGlobalObject* globalObject = m_globalObject;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    SetForScope savedFrame(reraisingFrame, nullptr);
    // For finalizers that make new garbage with such finalizers for ever.
    unsigned collectionsLeft = 8;
    // It stays where it is until this thread ends.
    CallsOwed* own;
    {
        Locker locker { m_finalizationLock };
        own = &callsOwedBy(m_currentThread);
        rankIfItHasNone(*own);
    }

    while (true) {
        // A long queue does not hold up Ctrl-C, and a handler is called with the frame of the program, not with that of a finalizer
        // that happens to come next.
        if (!isFinalized && failed(scope, checkOnlySignals(globalObject))) [[unlikely]] {
            didQueueCallsFor(m_currentThread); // (It may have served others only, and have the rest of a group in its hand.)
            return;
        }
        // A finalizer written in C++ drops what the code it calls raises. If that was ended by the host, this is where it is thrown
        // again, before anything more is taken off the queue.
        if (m_vm.traps().maybeNeedHandling() && m_vm.hasExceptionsAfterHandlingTraps()) [[unlikely]] {
            didQueueCallsFor(m_currentThread);
            return;
        }
        // Once out of the queue, only the stack holds it.
        JSCell* object = nullptr;
        PyWeakRef* weakRef = nullptr;
        bool isDone = false;
        bool collectionStarts = false;
        bool collectionIsOver = false;
        CollectionNotice collection { };
        {
            Locker locker { m_finalizationLock };
            CallOwed next;
            if (m_threadBetweenStartAndStop == noThread && !m_collectionsToTellOf.isEmpty()) {
                m_threadBetweenStartAndStop = m_currentThread;
                m_collectionBeingToldOf = collection = m_collectionsToTellOf.takeFirst();
                collectionStarts = true;
            } else if (takeNextCall(*own, callsOf, next)) {
                if (next.isWeakRef)
                    weakRef = uncheckedDowncast<PyWeakRef>(next.cell);
                else {
                    object = next.cell;
                    m_justFinalized.append(object);
                }
            } else if (m_threadBetweenStartAndStop == m_currentThread && m_collectionBeingToldOf) {
                collection = *m_collectionBeingToldOf;
                collectionIsOver = true;
            } else {
                m_hasPendingCalls = false;
                isDone = true;
            }
        }
        if (collectionStarts || collectionIsOver) {
            tellOfCollection(collection.generation, collection.duration, collectionIsOver);
            if (collectionIsOver) {
                Locker locker { m_finalizationLock };
                m_collectionBeingToldOf = std::nullopt;
                m_threadBetweenStartAndStop = noThread;
            }
        } else if (isDone) {
            if (!std::exchange(m_shouldCollectAfterPendingCalls, false) || !collectionsLeft--)
                return;
            m_vm.clearLastException();
            m_vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
            continue;
        } else if (weakRef) {
            JSValue callback = weakRef->callback();
            weakRef->clearCallback();
            if (callback) {
                call(globalObject, callback, weakRef);
                if (scope.exception() && !m_vm.hasPendingTerminationException())
                    reportUnraisable(globalObject, makeString("Exception ignored while calling weakref callback "_s, reprForMessage(globalObject, callback)), { });
            }
        } else
            finalizeObject(globalObject, object);
        ensureStillAliveHere(object);
        ensureStillAliveHere(weakRef);
        if (m_threadWhereSignalHandlerRaised == m_currentThread) [[unlikely]] {
            // The call has caught it, or it has come as far as where it would be reported.
            JSValue raised = slot(Slot::RaisedBySignalHandler);
            setSlot(Slot::RaisedBySignalHandler, { });
            m_threadWhereSignalHandlerRaised = noThread;
            if (std::exchange(m_hasKeptWhatSignalHandlerRaised, false)) {
                {
                    auto throwScope = DECLARE_THROW_SCOPE(m_vm);
                    throwScope.throwException(globalObject, Exception::create(m_vm, raised, Exception::StackCaptureAction::DoNotCaptureStack));
                }
                EXCEPTION_ASSERT(scope.exception());
                didQueueCallsFor(m_currentThread);
                return;
            }
        }
        // The host is ending what runs here, and that cannot be reported and forgotten. The rest is for another time.
        if (m_vm.hasPendingTerminationException()) [[unlikely]] {
            didQueueCallsFor(m_currentThread);
            return;
        }
    }
}

// The threads that are still there when the interpreter exits never run again. So their frames never let go of what they refer to,
// and in CPython none of that, nor what it leads to, is ever finalized: the locals of the thread, and by way of the functions that it
// is in their globals, with all that hangs on those. A finalizer would find things as no code was meant to see them: the thread
// may be half way through close(). But CPython empties the modules that are still referred to when sys.modules is gone, and in the end
// sys and builtins. What is only held by one of those goes. So does what belongs to the thread and not to its frames: the values
// of its threading.local(), the exception it is handling.
UncheckedKeyHashSet<JSCell*> Runtime::whatOtherThreadsHold()
{
    Vector<JSCell*> roots;
    forEachValueInFramesOfParkedThreads(m_vm, [&](JSValue value) {
        if (value && value.isCell())
            roots.append(value.asCell());
    });
    if (roots.isEmpty())
        return { };

    DeferGC deferGC(m_vm);
    UncheckedKeyHashSet<JSCell*> emptied { sysDict(), builtins() };
    for (JSCell* cell : cellsReachedFrom(m_vm, roots.span(), [](JSCell* cell) { return isCellOfTheProgram(cell); })) {
        if (auto* module = PyModule::tryFrom(cell)) {
            emptied.add(module);
            if (PyDict* dict = module->dict())
                emptied.add(dict);
        }
    }
    return cellsReachedFrom(m_vm, roots.span(), [&](JSCell* cell) {
        return isCellOfTheProgram(cell) && !emptied.contains(cell);
    });
}

// (In a function of its own: nothing of what it goes through is to be left in the frame of finalizeAllAtExit(), from which the collector is called.)
NEVER_INLINE void Runtime::noteNamespacesOfModules()
{
    JSValue name;
    JSValue module;
    for (unsigned position = 0; modules()->next(position, name, module);) {
        if (auto* pythonModule = PyModule::tryFrom(module); pythonModule && pythonModule->dict())
            m_namespacesOfModulesLetGoOf.append(pythonModule->dict());
    }
    // -X finalization_order_looks=N, or sys._xoptions["finalization_order_looks"] = "N", lowers the bound: for tests of what is done where it is reached.
    auto* options = PyDict::tryFrom(sysDict()->get(internString(m_vm, "_xoptions"_s)));
    JSString* looks = options ? tryStrValue(options->get(internString(m_vm, "finalization_order_looks"_s))) : nullptr;
    if (const StringImpl* impl = looks ? looks->tryGetValueImpl() : nullptr)
        m_mostLooksAtInnerReferences = std::min(m_mostLooksAtInnerReferences, parseInteger<uint64_t>(StringView(impl)).value_or(m_mostLooksAtInnerReferences));
}

void Runtime::finalizeAllAtExit()
{
    // The host exits from within a call that is owed (process.exit() in a finalizer): no other is made within it.
    if (m_isRunningPendingCalls) [[unlikely]]
        return;
    ServesOtherThreads serves(*this, CallsOf::AllThreads);
    {
        // The other threads never run again. What they had begun is for whoever comes.
        Locker locker { m_finalizationLock };
        for (auto& owed : m_callsOwed) {
            if (owed->thread != m_currentThread)
                putBackWhatIsInHand(*owed);
        }
    }
    auto isDone = [&] {
        bool hasSomethingToFinalize = hasPendingFinalizers();
        forEachFinalizable([&](JSCell* cell) {
            hasSomethingToFinalize |= !hasNothingToFinalize(cell);
        });
        return !hasSomethingToFinalize;
    };
    // (The stack is wiped before every call on the way to a collection, because what was called before, the last collection for
    // one, has left what it dealt with where the frames of the next come to lie. See wipeStackBelow().)
    auto collect = [&] {
        for (unsigned round = 0; round < 64 && !isDone(); ++round) {
            wipeStackBelow(m_globalObject);
            collectAsAskedFor();
            if (!owesCalls(CallsOf::AllThreads))
                break;
            bool mayFindMore = hasPendingFinalizers();
            runPendingCalls(false, CallsOf::AllThreads);
            if (!mayFindMore || m_vm.hasPendingTerminationException())
                break;
        }
    };
    // A program that leaves nothing to finalize exits without further ado.
    // Otherwise, first what is garbage already, as it would have been all along with reference counts.
    wipeStackBelow(m_globalObject);
    collect();
    if (isDone() || m_vm.hasPendingTerminationException())
        return;
    // Then the modules go, as they do when CPython exits, and with them what only they hold: the globals of the program.
    // The collector finds that, and puts it in order. (CPython also sets the globals to None, one after the other, to get at
    // what cycles keep alive. Here finalizers find them as they were.)
    for (auto name : { "path"_s, "argv"_s, "ps1"_s, "ps2"_s, "last_exc"_s, "last_type"_s, "last_value"_s, "last_traceback"_s, "__interactivehook__"_s, "path_hooks"_s, "path_importer_cache"_s, "meta_path"_s })
        sysDict()->set(m_vm, internString(m_vm, name), jsUndefined()); // None, not gone: importlib then says that the interpreter is shutting down.
    builtins()->remove(m_vm, internString(m_vm, "_"_s));
    // An object with a finalizer that has a name in one of them is held by nothing, as far as the order goes, and the ways from the names into what holds one another begin there.
    auto forgetNamespaces = makeScopeExit([&] {
        m_namespacesOfModulesLetGoOf.clear();
        m_hasLetGoOfModules = false;
    });
    noteNamespacesOfModules();
    modules()->clear(m_vm);
    m_hasLetGoOfModules = true;
    wipeStackBelow(m_globalObject);
    collect();
    if (m_vm.hasPendingTerminationException()) [[unlikely]]
        return;
    // Then what something else holds on to: JS, which has imported a module, a frame of this thread that is still on the stack. In the order it was made, a holder before what only it holds, like the rest.
    {
        forgetThreadLocalsOfOtherThreads(m_globalObject);
        // A collection that the finalizers have set off may be under way. It ends here, before the lists are read: it takes off them what it finds dead. Nothing from here on allocates.
        PreventCollectionScope preventCollection(m_vm.heap);
        auto heldByOtherThreads = whatOtherThreadsHold();
        Vector<JSCell*> rest;
        Vector<JSCell*> callbacksOfForeignCode; // Which the finalizers of the others may call.
        forEachFinalizable([&](JSCell* cell) {
            // A generator someone still holds is not closed, nor is a coroutine complained about.
            if (cell->type() != PyGeneratorType && !heldByOtherThreads.contains(cell))
                (cell->classInfo() == Ctypes::PyCThunk::info() ? callbacksOfForeignCode : rest).append(cell);
        });
        if (rest.size() > 1) {
            rest = orderWithHoldersFirst(m_vm.heap, rest.span(), m_namespacesOfModulesLetGoOf.span(), m_mostLooksAtInnerReferences).map([&](unsigned index) {
                return rest[index];
            });
        }
        rest.appendVector(callbacksOfForeignCode);
        for (auto* list : { &m_oldFinalizable, &m_youngFinalizable }) {
            for (auto& entry : *list)
                didTakeOffList(entry.cell());
            list->clear();
        }
        Locker locker { m_finalizationLock };
        CallsOwed& owed = callsOwedBy(m_currentThread);
        for (JSCell* cell : rest)
            queueCall(owed.calls, cell, 0);
        didQueueCallsFor(m_currentThread);
    }
    runPendingCalls(false, CallsOf::AllThreads);
}

// ---- Collections that are asked for ----

// A program that asks for a collection expects what it has dropped to be gone afterwards. The collector takes every word of the
// stack that looks like a reference for one, and the frames of C++ between the program's call and the collector lie where those
// of its earlier calls were: what they do not write would still refer to what those dealt with. So that is wiped first, before
// the first of those frames (NativeFlag::OnWipedStack).
// What is wiped lies below the stack pointer of whoever asks for it. The frame of that function is not wiped, and later frames come
// to lie there too: so it is to be such that it has no slot that it does not write, in a build that is not optimized either, where
// every variable and many a temporary has a slot, written or not, and a local of the last function of Python that returned may lie
// just there. See callNativeFunctionOnWipedStack(). What wipes is not written in C++, so that it has no frame itself.
#if ENABLE(C_LOOP)
NEVER_INLINE void wipeStackBelow(JSGlobalObject* globalObject, size_t size)
{
    char* top = static_cast<char*>(currentStackPointer());
    char* limit = static_cast<char*>(getVM(globalObject).softStackLimit());
    if (top <= limit || static_cast<size_t>(top - limit) < size + 16 * KB)
        return;
    volatile char* wiped = static_cast<volatile char*>(alloca(size));
    for (size_t i = 0; i < size; i += sizeof(uintptr_t))
        *reinterpret_cast<volatile uintptr_t*>(wiped + i) = 0;
}
#endif

bool Runtime::hasPendingFinalizers(CallsOf callsOf)
{
    Locker locker { m_finalizationLock };
    return std::ranges::any_of(m_callsOwed, [&](auto& owed) WTF_REQUIRES_LOCK(m_finalizationLock) {
        return mayBeginCallsOf(*owed, callsOf);
    });
}

void Runtime::collectAsAskedFor(CollectionScope collectionScope, bool leavesCyclicGarbageForLater, CallsOf callsOf)
{
    // The VM remembers the last exception thrown, for the debugger, long after it has been handled, and with it the
    // traceback and all that the frames in it refer to.
    m_vm.clearLastException();
    forgetCatchesInJSThatAreOver();
    WaitForCollection wait(*this, leavesCyclicGarbageForLater, callsOf);
    // The one is asked for to get rid of things, the other to be quick: what it frees is swept when the memory is wanted.
    if (collectionScope == CollectionScope::Full)
        m_vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
    else
        m_vm.heap.collectSync(CollectionScope::Eden);
}

void Runtime::finalizeWhatReferenceCountsWouldHaveFreed()
{
    if (m_isRunningPendingCalls) [[unlikely]]
        return;
    // See m_era.
    ServesOtherThreads serves(*this, CallsOf::OfEarlierEras);
    for (unsigned round = 0; round < 64; ++round) {
        if (round)
            serves.willCollectAgain();
        bool hasSomethingToDo = hasPendingFinalizers(CallsOf::OfEarlierEras);
        forEachFinalizable([&](JSCell* cell) {
            hasSomethingToDo |= hasFinalizerOfProgram(*this, cell) && !hasNothingToFinalize(cell);
        });
        if (!hasSomethingToDo)
            return;
        wipeStackBelow(m_globalObject);
        collectAsAskedFor(CollectionScope::Full, true, CallsOf::OfEarlierEras);
        if (!owesCalls(CallsOf::OfEarlierEras))
            return;
        bool mayFindMore = hasPendingFinalizers(CallsOf::OfEarlierEras);
        runPendingCalls(false, CallsOf::OfEarlierEras);
        if (!mayFindMore || m_vm.hasPendingTerminationException())
            return;
    }
}

void collectAndFinalize(JSGlobalObject* globalObject, Runtime::MarkingObserver* observer, CollectionScope collectionScope, bool leavesCyclicGarbageForLater, Runtime::CallsOf callsOf)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    ASSERT(!rt.isRunningPendingCalls());
    writeOutPendingOutput(globalObject); // Until then they are not to be collected.
    Runtime::ServesOtherThreads serves(rt, callsOf);
    // An object with a finalizer takes two rounds: one finds it, the next frees it, which may clear weak references in turn
    // and finds what only it referred to. The limit is for finalizers that make new garbage with finalizers for ever.
    for (unsigned round = 0; round < 64; ++round) {
        if (round)
            serves.willCollectAgain();
        {
            // What an observer listens to is the collection asked for here, from beginning to end. Not one that was under way, or had been asked for: this one would follow, and
            // free what the observer has been told of and has noted by its address. Nor does another come before the observer has heard that it is over.
            // The one that is finished first is waited for all the same: it finds what has just been dropped, and orders and hands out the finalizers as this one would have.
            Runtime::WaitForCollection wait(rt, leavesCyclicGarbageForLater, callsOf);
            std::optional<PreventCollectionScope> preventCollection;
            if (observer) {
                observer->willCollect(); // It may allocate.
                preventCollection.emplace(vm.heap);
                rt.setMarkingObserver(observer);
            }
            wipeStackBelow(globalObject);
            rt.collectAsAskedFor(collectionScope, leavesCyclicGarbageForLater, callsOf);
            if (observer) {
                rt.setMarkingObserver(nullptr);
                observer->didCollect();
            }
        }
        if (!rt.owesCalls(callsOf))
            break;
        bool mayFindMore = rt.hasPendingFinalizers(callsOf);
        // What the handler of a signal that came meanwhile raises is for whoever waits for this. The calls that are left are made
        // at the next safe point then.
        rt.runPendingCalls(false, callsOf);
        if (!mayFindMore || collectionScope == CollectionScope::Eden || vm.exceptionForInspection())
            break;
    }
}

} // namespace Python
} // namespace JSC
