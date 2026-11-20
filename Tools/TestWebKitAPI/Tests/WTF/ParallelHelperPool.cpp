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
    unsigned didRun()
    {
        Locker locker { lock };
        condition.notifyAll();
        return ++runs;
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

// The pools are never destroyed, like the pool of the marking helpers.
ParallelHelperPool& createPool(unsigned numberOfThreads)
{
    ParallelHelperPool& pool = ParallelHelperPool::create("Test Helper"_s).leakRef();
    pool.ensureThreads(numberOfThreads);
    return pool;
}

} // anonymous namespace

TEST(WTF_ParallelHelperPool, TaskOnRequestRunsOncePerRequest)
{
    ParallelHelperPool& pool = createPool(2);
    ParallelHelperClient client(&pool);
    RunCounter counter;

    // The task starts with one request for each thread of the pool.
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

TEST(WTF_ParallelHelperPool, RequestsAreNotMoreThanTheThreadsOfThePool)
{
    ParallelHelperPool& pool = createPool(2);
    ParallelHelperClient client(&pool);
    RunCounter counter;

    client.setFunctionOnRequest([&] {
        counter.didRun();
    });
    EXPECT_TRUE(counter.waitForRuns(2));

    client.requestThreads(5);
    EXPECT_TRUE(counter.waitForRuns(4));

    client.finish();
    EXPECT_EQ(4u, counter.count());
}

TEST(WTF_ParallelHelperPool, OneThreadTakesAllRequestsWhileTheOtherIsBusy)
{
    ParallelHelperPool& pool = createPool(2);
    ParallelHelperClient busyClient(&pool);
    ParallelHelperClient client(&pool);
    RunCounter busyCounter;
    RunCounter counter;
    Lock releaseLock;
    Condition releaseCondition;
    bool released = false;

    // One of the two threads stays in the task of another client. The other thread returns from it.
    busyClient.setFunctionOnRequest([&] {
        if (busyCounter.didRun() != 1)
            return;
        Locker locker { releaseLock };
        releaseCondition.wait(releaseLock, [&] {
            return released;
        });
    });
    EXPECT_TRUE(busyCounter.waitForRuns(2));

    // The other thread takes both requests, one after the other.
    client.setFunctionOnRequest([&] {
        counter.didRun();
    });
    EXPECT_TRUE(counter.waitForRuns(2));
    client.finish();
    EXPECT_EQ(2u, counter.count());

    {
        Locker locker { releaseLock };
        released = true;
        releaseCondition.notifyAll();
    }
    busyClient.finish();
    EXPECT_EQ(2u, busyCounter.count());
}

TEST(WTF_ParallelHelperPool, TaskOnRequestCanRequestAThread)
{
    ParallelHelperPool& pool = createPool(1);
    ParallelHelperClient client(&pool);
    RunCounter counter;

    client.setFunctionOnRequest([&] {
        if (counter.didRun() == 1)
            client.requestThreads(1);
    });
    EXPECT_TRUE(counter.waitForRuns(2));

    client.finish();
    EXPECT_EQ(2u, counter.count());
}

TEST(WTF_ParallelHelperPool, ThreadThatReturnsFromTaskOnRequestServesAnotherClient)
{
    ParallelHelperPool& pool = createPool(1);
    ParallelHelperClient idleClient(&pool);
    ParallelHelperClient otherClient(&pool);
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
    ParallelHelperPool& pool = createPool(1);
    ParallelHelperClient client(&pool);
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
