/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/ParallelHelperPool.h>

namespace TestWebKitAPI {

namespace {

// Counts the runs of a task. A test waits for a count, with a bound so that a failure is a failed expectation and not a hang.
struct RunCounter {
    void didRun()
    {
        Locker locker { lock };
        ++runs;
        condition.notifyAll();
    }

    bool waitForRuns(unsigned expected)
    {
        Locker locker { lock };
        return condition.waitFor(lock, 30_s, [&] {
            return runs >= expected;
        });
    }

    unsigned count()
    {
        Locker locker { lock };
        return runs;
    }

    Lock lock;
    Condition condition;
    unsigned runs { 0 };
};

} // anonymous namespace

TEST(WTF_ParallelHelperPool, TaskOnRequestRunsOncePerThreadThenOncePerRequest)
{
    Ref pool = ParallelHelperPool::create("Test Helper"_s);
    pool->ensureThreads(2);
    ParallelHelperClient client(pool.copyRef());
    RunCounter counter;

    client.setFunctionOnRequest([&] {
        counter.didRun();
    });
    EXPECT_TRUE(counter.waitForRuns(2));

    // A thread that returned from the task left it set.
    client.requestThreads(1);
    EXPECT_TRUE(counter.waitForRuns(3));

    client.requestThreads(2);
    EXPECT_TRUE(counter.waitForRuns(5));

    client.finish();
    EXPECT_EQ(5u, counter.count());
}

TEST(WTF_ParallelHelperPool, ThreadThatReturnsFromTaskOnRequestServesAnotherClient)
{
    Ref pool = ParallelHelperPool::create("Test Helper"_s);
    pool->ensureThreads(1);
    ParallelHelperClient idleClient(pool.copyRef());
    ParallelHelperClient otherClient(pool.copyRef());
    RunCounter idleCounter;
    RunCounter otherCounter;

    // The task of a parallel algorithm that has no work for the thread: the thread returns from it.
    idleClient.setFunctionOnRequest([&] {
        idleCounter.didRun();
    });
    EXPECT_TRUE(idleCounter.waitForRuns(1));

    // The only thread of the pool runs the task of the other client, while the first task is still set.
    otherClient.setFunction([&] {
        otherCounter.didRun();
    });
    EXPECT_TRUE(otherCounter.waitForRuns(1));
    otherClient.finish();

    // And the first algorithm gets the thread again when it has work.
    idleClient.requestThreads(1);
    EXPECT_TRUE(idleCounter.waitForRuns(2));
    idleClient.finish();
    EXPECT_EQ(2u, idleCounter.count());
    EXPECT_EQ(1u, otherCounter.count());
}

TEST(WTF_ParallelHelperPool, FinishDropsRequestsAndTask)
{
    Ref pool = ParallelHelperPool::create("Test Helper"_s);
    pool->ensureThreads(1);
    ParallelHelperClient client(pool.copyRef());
    RunCounter counter;

    client.setFunctionOnRequest([&] {
        counter.didRun();
    });
    EXPECT_TRUE(counter.waitForRuns(1));
    client.finish();

    // No task is set: the request brings no thread.
    client.requestThreads(1);

    // The client takes a task that is not on request as before: it is cleared when a thread returns from it.
    RunCounter second;
    client.setFunction([&] {
        second.didRun();
    });
    EXPECT_TRUE(second.waitForRuns(1));
    client.finish();
    EXPECT_EQ(1u, counter.count());
    EXPECT_EQ(1u, second.count());
}

} // namespace TestWebKitAPI
