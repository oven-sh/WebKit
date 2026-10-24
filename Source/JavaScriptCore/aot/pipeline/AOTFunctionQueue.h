/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTProvisionalFunctions.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/UniqueArray.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

struct QueuedFunction {
    FunctionID id;
    uint32_t size { 0 };
};

// Functions that are each processed once, where processing one finds more.
class FunctionQueue {
    WTF_MAKE_NONCOPYABLE(FunctionQueue);
public:
    using FoundFunctions = Vector<QueuedFunction, 128>;
    using Process = ScopedLambda<void(FunctionID, FoundFunctions&)>;
    using DidConfirm = ScopedLambda<void(FunctionID)>;

    FunctionQueue(const ParallelLoops&, unsigned numberOfFunctions);
    ~FunctionQueue();

    void processEach(std::span<const QueuedFunction>, NOESCAPE const Process&);

    // A speculative function starts before anything has found it, so a long function does not wait for the long function that names it.
    void processEach(std::span<const QueuedFunction>, std::span<const QueuedFunction> speculativeFunctions, NOESCAPE const Process&, NOESCAPE const DidConfirm&);

    bool wasQueued(FunctionID) const;
    bool wasConfirmed(FunctionID) const;
    BitVector takeQueuedProvisionalOrdinals();

private:
    static constexpr unsigned bitsPerWord = 64;
    static constexpr unsigned wordsPerSegment = 1 << 12;
    static constexpr unsigned numberOfSegments = 1 << 8;

    using Word = std::atomic<uint64_t>;
    using Segment = std::array<Word, wordsPerSegment>;

    enum State : uint8_t {
        Queued = 1 << 0,
        Speculative = 1 << 1,
        Confirmed = 1 << 2,
        Done = 1 << 3,
    };

    enum class Confirmation : uint8_t { AlreadyQueued, FirstQueued, FinishedSpeculatively };

    struct SpeculativeFunction {
        FunctionID id;
        Vector<QueuedFunction> found;
    };

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

    Word& ensureProvisionalWord(FunctionID);
    const Word* provisionalWord(FunctionID) const;
    bool isSpeculative(FunctionID) const;
    SpeculativeFunction& speculativeFunction(FunctionID) LIFETIME_BOUND;
    Confirmation confirm(FunctionID);
    void confirmFoundFunctions(FoundFunctions&);
    bool finishSpeculativeFunction(FunctionID, const FoundFunctions&);
    bool takeSpareThread();
    Worker& takeIdleWorker();
    void start(Vector<QueuedFunction>&&);
    bool takeHalfOfAnother(Worker&);
    void processShare(Worker&);

    const ParallelLoops& m_loops;
    const Process* m_process { nullptr };
    const DidConfirm* m_didConfirm { nullptr };
    const UniqueArray<Worker> m_workers;
    Vector<std::atomic<uint8_t>> m_states;
    Vector<SpeculativeFunction> m_speculativeFunctions;
    std::array<std::atomic<Segment*>, numberOfSegments> m_isProvisionalQueued { };
    std::atomic<unsigned> m_numberOfSpareThreads { 0 };
    Atomic<uint32_t> m_numberOfBusyWorkers { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
