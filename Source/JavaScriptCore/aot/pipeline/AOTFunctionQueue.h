/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTProvisionalFunctions.h"
#include <array>
#include <atomic>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/UniqueArray.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

struct QueuedFunction {
    FunctionID id;
    uint32_t size { 0 };
};

// Functions that are each processed once, where processing one finds more. The threads share no list: a thread keeps what it
// finds, and hands half of what it has to the pool whenever the build has a thread to spare. A thread that has run out takes
// half of what another has, so nothing waits behind a function that takes long.
class FunctionQueue {
    WTF_MAKE_NONCOPYABLE(FunctionQueue);
public:
    using FoundFunctions = Vector<QueuedFunction, 128>;
    using Process = ScopedLambda<void(FunctionID, FoundFunctions&)>;

    FunctionQueue(const ParallelLoops&, unsigned numberOfFunctions);
    ~FunctionQueue();

    void processEach(std::span<const QueuedFunction>, NOESCAPE const Process&);

    bool wasQueued(FunctionID) const;
    BitVector takeQueuedProvisionalOrdinals();

private:
    static constexpr unsigned bitsPerWord = 64;
    static constexpr unsigned wordsPerSegment = 1 << 12;
    static constexpr unsigned numberOfSegments = 1 << 8;

    using Word = std::atomic<uint64_t>;
    using Segment = std::array<Word, wordsPerSegment>;

    struct Worker {
        void didChangeFunctions() WTF_REQUIRES_LOCK(lock) { numberOfFunctions.store(functions.size(), std::memory_order_relaxed); }

        FunctionQueue* queue { nullptr };
        Lock lock;
        Vector<QueuedFunction> functions WTF_GUARDED_BY_LOCK(lock);
        // For a thread that looks for a list to take from.
        std::atomic<size_t> numberOfFunctions { 0 };
        std::atomic<bool> isBusy { false };
        uint8_t restOfCacheLine[16];
    };

    Word& ensureWordOf(FunctionID);
    const Word* wordOf(FunctionID) const;
    bool markQueued(FunctionID);
    bool takeSpareThread();
    Worker& takeIdleWorker();
    void start(Vector<QueuedFunction>&&);
    bool takeHalfOfAnother(Worker&);
    void processShare(Worker&);

    const ParallelLoops& m_loops;
    const Process* m_process { nullptr };
    const UniqueArray<Worker> m_workers;
    Vector<Word> m_isQueued;
    std::array<std::atomic<Segment*>, numberOfSegments> m_isProvisionalQueued { };
    std::atomic<unsigned> m_numberOfSpareThreads { 0 };
    Atomic<uint32_t> m_numberOfBusyWorkers { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
