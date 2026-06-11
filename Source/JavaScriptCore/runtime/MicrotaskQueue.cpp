/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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
#include "MicrotaskQueue.h"

#include "Debugger.h"
#include "DeferTermination.h"
#include "GlobalObjectMethodTable.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSMicrotask.h"
#include "JSMicrotaskDispatcher.h"
#include "JSObject.h"
#include "MicrotaskCallInlines.h"
#include "MicrotaskQueueInlines.h"
#include "Options.h"
#include "ScriptProfilingScope.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#include "VMLiteShared.h"
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MicrotaskQueue);
WTF_MAKE_COMPACT_TZONE_ALLOCATED_IMPL(MicrotaskDispatcher);
WTF_MAKE_COMPACT_TZONE_ALLOCATED_IMPL(DebuggableMicrotaskDispatcher);

bool QueuedTask::isRunnable() const
{
    if (isJSMicrotaskDispatcher()) [[unlikely]]
        return uncheckedDowncast<JSMicrotaskDispatcher>(dispatcher())->dispatcher()->isRunnable();
    return uncheckedDowncast<JSGlobalObject>(dispatcher())->microtaskRunnability() == QueuedTaskResult::Executed;
}

static bool runMicrotask(JSGlobalObject* globalObject, TopExceptionScope& catchScope, VM& vm, QueuedTask& task, MicrotaskCall* microtaskCall)
{
    runInternalMicrotask(globalObject, vm, task.job(), task.payload(), task.arguments(), microtaskCall);
    if (auto* exception = catchScope.exception()) [[unlikely]] {
        if (!catchScope.clearExceptionExceptTermination()) [[unlikely]]
            return false;
        globalObject->globalObjectMethodTable()->reportUncaughtExceptionAtEventLoop(globalObject, exception);
        return catchScope.clearExceptionExceptTermination();
    }
    return true;
}

void runMicrotaskWithDebugger(JSGlobalObject* globalObject, VM& vm, QueuedTask& task)
{
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto identifier = task.identifier();

    if (auto* debugger = globalObject->debugger(); debugger && identifier) [[unlikely]] {
        DeferTerminationForAWhile deferTerminationForAWhile(vm);
        debugger->willRunMicrotask(globalObject, identifier.value());
        if (!catchScope.clearExceptionExceptTermination()) [[unlikely]]
            return;
    }

    if (!runMicrotask(globalObject, catchScope, vm, task, nullptr)) [[unlikely]]
        return;

    if (auto* debugger = globalObject->debugger(); debugger && identifier) [[unlikely]] {
        DeferTerminationForAWhile deferTerminationForAWhile(vm);
        debugger->didRunMicrotask(globalObject, identifier.value());
        catchScope.clearExceptionExceptTermination();
    }
}

QueuedTaskResult DebuggableMicrotaskDispatcher::run(QueuedTask& task)
{
    auto* globalObject = task.globalObject();
    runMicrotaskWithDebugger(globalObject, globalObject->vm(), task);
    return QueuedTask::Result::Executed;
}

bool DebuggableMicrotaskDispatcher::isRunnable() const
{
    return true;
}

MicrotaskQueue::MicrotaskQueue(VM& vm)
{
    // UNGIL §E.1/§E.4 (TSAN family 30): arm the foreign-enqueue inbox in a
    // GIL-off process. Keyed on the PROCESS predicate, not vm.gilOff() —
    // the VM's m_gilOff is computed in the VM ctor BODY, after the default
    // queue is constructed in its init list, so the member is not yet
    // meaningful here; over-approximating to a designation-losing VM is
    // harmless (its inbox stays empty; one predicted-not-taken test per
    // isEmpty()/clear()). Flag-off: isGILOffProcess() is false and every
    // inbox branch in this class is dead.
    if (VM::isGILOffProcess()) [[unlikely]]
        m_acceptsForeignTasks = true;

    // SPEC-vmstate §6.5(a): list mutation under the registry leaf lock so a
    // spawned thread's lazy queue creation (VMLite::ensureDefaultMicrotaskQueue)
    // cannot corrupt LIST MEMBERSHIP against GC-marker iteration (M11). The
    // lock covers membership only; queue CONTENTS visiting is safe because
    // deque contents are GC-read only with all mutators suspended (see the
    // M11 scope note and cross-WS item 12 in INTEGRATE-vmstate.md). Gated:
    // flag-off stays lock-free (R3).
    if (Options::useVMLite()) [[unlikely]] {
        Locker locker { VMLiteRegistry::singleton().lock };
        vm.m_microtaskQueues.append(this);
        return;
    }
    vm.m_microtaskQueues.append(this);
}

Ref<MicrotaskQueue> MicrotaskQueue::create(VM& vm)
{
    return adoptRef(*new MicrotaskQueue(vm));
}

MicrotaskQueue::~MicrotaskQueue()
{
    // SPEC-vmstate §6.5(b). The isOnList() check must be under the same lock
    // as the removal: ~VM's force-removal (M11) can race a dying queue's
    // dtor on another thread post-GIL.
    if (Options::useVMLite()) [[unlikely]] {
        Locker locker { VMLiteRegistry::singleton().lock };
        if (isOnList())
            remove();
        return;
    }
    if (isOnList())
        remove();
}

template<typename Visitor>
void MicrotaskQueue::visitAggregateImpl(Visitor& visitor)
{
    m_queue.visitAggregate(visitor);
    m_toKeep.visitAggregate(visitor);
    // UNGIL §E.1/§E.4: tasks parked in the foreign inbox hold JS values and
    // must be marked like queued tasks. Visiting runs with mutators
    // suspended (same M11 scope note as the deques above), but we take the
    // leaf lock anyway — it is uncontended at that point and keeps every
    // inbox access lock-disciplined. Flag-off: branch not taken.
    if (m_acceptsForeignTasks) [[unlikely]] {
        Locker locker { m_foreignTasksLock };
        for (auto& task : m_foreignTasks) {
            visitor.appendUnbarriered(task.dispatcher());
            visitor.appendUnbarriered(task.m_arguments, QueuedTask::maxArguments);
        }
    }
}
DEFINE_VISIT_AGGREGATE(MicrotaskQueue);

void MicrotaskQueue::enqueueFromForeignThread(QueuedTask&& task)
{
    // See MicrotaskQueue.h: the lock-guarded inbox is the only storage of
    // this queue a non-owner thread may touch; the owner splices it into
    // m_queue under the same lock (takeForeignTasks). Deliberately NO
    // debugger didQueueMicrotask and NO scheduleToRunIfNeeded here — both
    // mutate owner-thread state; the task is serviced at the owner's next
    // drain (AB-20/AB-23/AB-25 service-request word).
    ASSERT(m_acceptsForeignTasks);
    Locker locker { m_foreignTasksLock };
    m_foreignTasks.append(WTF::move(task));
}

bool MicrotaskQueue::takeForeignTasks()
{
    // Owner-only (called from drainImpl). The lock acquire here pairs with
    // the enqueuer's release in enqueueFromForeignThread: every word of a
    // foreign QueuedTask happens-before the owner's dequeue/run/free of it.
    if (!m_acceptsForeignTasks) [[likely]]
        return false;
    Deque<QueuedTask> taken;
    {
        Locker locker { m_foreignTasksLock };
        m_foreignTasks.swap(taken);
    }
    if (taken.isEmpty())
        return false;
    while (!taken.isEmpty())
        m_queue.enqueue(taken.takeFirst());
    return true;
}

bool MicrotaskQueue::hasForeignTasksSlow() const
{
    Locker locker { m_foreignTasksLock };
    return !m_foreignTasks.isEmpty();
}

void MicrotaskQueue::clearForeignTasksSlow()
{
    Locker locker { m_foreignTasksLock };
    m_foreignTasks.clear();
}

void MicrotaskQueue::enqueueSlow(QueuedTask&& task)
{
    auto* globalObject = task.globalObject();
    auto identifier = task.identifier();
    m_queue.enqueue(WTF::move(task));
    if (globalObject) {
        if (auto* debugger = globalObject->debugger(); debugger && identifier) [[unlikely]]
            debugger->didQueueMicrotask(globalObject, identifier.value());
    }
    if (!m_isScheduledToRun) [[unlikely]]
        scheduleToRunIfNeeded();
}

bool MarkedMicrotaskDeque::hasMicrotasksForFullyActiveDocument() const
{
    for (auto& task : m_queue) {
        if (task.isRunnable())
            return true;
    }
    return false;
}

template<typename Visitor>
void MarkedMicrotaskDeque::visitAggregateImpl(Visitor& visitor)
{
    // Because content in the queue will not be changed, we need to scan it only once per an entry during one GC cycle.
    // We record the previous scan's index, and restart scanning again in CollectorPhase::FixPoint from that.
    // When new GC phase begins, this cursor is reset to zero (beginMarking). This optimization is introduced because
    // some of application have massive size of MicrotaskQueue depth. For example, in parallel-promises-es2015-native.js
    // benchmark, it becomes 251670 at most.
    // This cursor is adjusted when an entry is dequeued. And we do not use any locking here, and that's fine: these
    // values are read by GC when CollectorPhase::FixPoint and CollectorPhase::Begin, and both suspend the mutator, thus,
    // there is no concurrency issue.
    for (auto iterator = m_queue.begin() + m_markedBefore, end = m_queue.end(); iterator != end; ++iterator) {
        auto& task = *iterator;
        visitor.appendUnbarriered(task.dispatcher());
        visitor.appendUnbarriered(task.m_arguments, QueuedTask::maxArguments);
    }
    m_markedBefore = m_queue.size();
}
DEFINE_VISIT_AGGREGATE(MarkedMicrotaskDeque);

template<bool useCallOnEachMicrotask>
ALWAYS_INLINE std::pair<JSGlobalObject*, bool> MicrotaskQueue::drainImpl(JSGlobalObject* currentGlobalObject, VM& vm, TopExceptionScope& catchScope)
{
    MicrotaskCall microtaskCall(vm);

    while (true) {
        if (m_queue.isEmpty()) {
            // UNGIL §E.1/§E.4: splice any foreign-inbox tasks into the
            // owner's deque before concluding the drain (flag-off:
            // takeForeignTasks is a single predicted-false test). Splicing
            // only at the empty boundary keeps intra-drain ordering of the
            // owner's own enqueues untouched and bounds lock traffic to one
            // acquire per drain round.
            if (!takeForeignTasks())
                break;
        }
        auto& front = m_queue.front();

        if (!front.isJSMicrotaskDispatcher()) [[likely]] {
            auto* globalObject = uncheckedDowncast<JSGlobalObject>(front.dispatcher());
            auto result = globalObject->microtaskRunnability();
            if (result != QueuedTask::Result::Executed) [[unlikely]] {
                auto task = m_queue.dequeue();
                if (result == QueuedTask::Result::Suspended)
                    m_toKeep.enqueue(WTF::move(task));
                continue;
            }

            if (globalObject != currentGlobalObject) [[unlikely]]
                return { globalObject, false };

            auto task = m_queue.dequeue();
            if (!runMicrotask(globalObject, catchScope, vm, task, &microtaskCall)) [[unlikely]] {
                clear();
                return { nullptr, true };
            }
        } else {
            auto* jsMicrotaskDispatcher = uncheckedDowncast<JSMicrotaskDispatcher>(front.dispatcher());
            auto* globalObject = front.globalObject();

            if (globalObject != currentGlobalObject) [[unlikely]]
                return { globalObject, false };

            auto task = m_queue.dequeue();
            QueuedTask::Result result;
            {
                ScriptProfilingScope profilingScope(globalObject, ProfilingReason::Microtask);
                result = jsMicrotaskDispatcher->dispatcher()->run(task);
            }

            switch (result) {
            case QueuedTask::Result::Executed:
                break;
            case QueuedTask::Result::Discard:
                break;
            case QueuedTask::Result::Suspended:
                m_toKeep.enqueue(WTF::move(task));
                break;
            }

            if (!catchScope.clearExceptionExceptTermination()) [[unlikely]] {
                clear();
                return { nullptr, true };
            }
        }

        if constexpr (useCallOnEachMicrotask) {
            vm.callOnEachMicrotaskTick();
            if (!catchScope.clearExceptionExceptTermination()) [[unlikely]] {
                clear();
                return { nullptr, true };
            }
        }
    }

    return { nullptr, true };
}

std::pair<JSGlobalObject*, bool> MicrotaskQueue::drainWithoutUseCallOnEachMicrotask(JSGlobalObject* currentGlobalObject, VM& vm, TopExceptionScope& catchScope)
{
    return drainImpl</* useCallOnEachMicrotask */ false>(currentGlobalObject, vm, catchScope);
}

std::pair<JSGlobalObject*, bool> MicrotaskQueue::drainWithUseCallOnEachMicrotask(JSGlobalObject* currentGlobalObject, VM& vm, TopExceptionScope& catchScope)
{
    return drainImpl</* useCallOnEachMicrotask */ true>(currentGlobalObject, vm, catchScope);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
