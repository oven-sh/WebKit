/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTVector.h"
#include <atomic>
#include <initializer_list>
#include <wtf/Condition.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC { namespace AOT {

// Runs on a thread of the pool once its dependencies have finished. finish() runs it on the calling thread if no thread has taken it yet.
class BackgroundTask final : public ThreadSafeRefCounted<BackgroundTask> {
public:
    static Ref<BackgroundTask> start(const ParallelLoops& loops, WTF::Function<void()>&& run)
    {
        return start(loops, TaskPriority::Background, { }, WTF::move(run));
    }

    static Ref<BackgroundTask> start(const ParallelLoops& loops, TaskPriority priority, std::initializer_list<BackgroundTask*> dependencies, WTF::Function<void()>&& run)
    {
        Ref<BackgroundTask> task = adoptRef(*new BackgroundTask(loops, priority, dependencies, WTF::move(run)));
        for (auto& dependency : task->m_dependencies) {
            task->m_numberOfUnfinishedDependencies.fetch_add(1, std::memory_order_relaxed);
            if (!dependency->addDependent(task.get()))
                task->m_numberOfUnfinishedDependencies.fetch_sub(1, std::memory_order_relaxed);
        }
        task->dependencyDidFinish();
        return task;
    }

    void finish()
    {
        for (auto& dependency : m_dependencies)
            dependency->finish();
        runUnlessTaken();
        Locker locker { m_lock };
        if (m_state == State::Finished)
            return;
        BusyThreads::WaitScope waitScope;
        m_condition.wait(m_lock, [this] {
            assertIsHeld(m_lock);
            return m_state == State::Finished;
        });
    }

private:
    enum class State : uint8_t { Waiting, Running, StartingDependents, Finished };

    BackgroundTask(const ParallelLoops& loops, TaskPriority priority, std::initializer_list<BackgroundTask*> dependencies, WTF::Function<void()>&& run)
        : m_loops(loops)
        , m_priority(priority)
        , m_dependencies(dependencies.size(), [&](size_t index) {
            return Ref<BackgroundTask> { *dependencies.begin()[index] };
        })
        , m_run(WTF::move(run))
    {
    }

    bool addDependent(BackgroundTask& dependent)
    {
        Locker locker { m_lock };
        if (m_state >= State::StartingDependents)
            return false;
        m_dependents.append(dependent);
        return true;
    }

    void dependencyDidFinish()
    {
        if (m_numberOfUnfinishedDependencies.fetch_sub(1, std::memory_order_acq_rel) != 1 || m_loops.width() == 1)
            return;
        ref();
        m_loops.schedule(m_priority, [](void* task) {
            adoptRef(*static_cast<BackgroundTask*>(task))->runUnlessTaken();
        }, this);
    }

    void runUnlessTaken()
    {
        {
            Locker locker { m_lock };
            if (m_state != State::Waiting)
                return;
            m_state = State::Running;
        }
        m_run();
        Vector<Ref<BackgroundTask>> dependents;
        {
            Locker locker { m_lock };
            m_state = State::StartingDependents;
            dependents = std::exchange(m_dependents, { });
        }
        // Whoever waits in finish() may destroy the pool as soon as the task has finished.
        for (auto& dependent : dependents)
            dependent->dependencyDidFinish();
        Locker locker { m_lock };
        m_state = State::Finished;
        m_condition.notifyAll();
    }

    const ParallelLoops& m_loops;
    const TaskPriority m_priority;
    const Vector<Ref<BackgroundTask>> m_dependencies;
    WTF::Function<void()> m_run;
    std::atomic<unsigned> m_numberOfUnfinishedDependencies { 1 };
    Lock m_lock;
    Condition m_condition;
    State m_state WTF_GUARDED_BY_LOCK(m_lock) { State::Waiting };
    Vector<Ref<BackgroundTask>> m_dependents WTF_GUARDED_BY_LOCK(m_lock);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
