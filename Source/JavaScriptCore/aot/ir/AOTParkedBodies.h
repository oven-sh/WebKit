/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRFunction.h"
#include "AOTVector.h"
#include <atomic>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/UniqueArray.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

// The bodies of all functions, parked while few of them are read and none is edited. hasBody() answers as before.
class ParkedBodies {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ParkedBodies);
    WTF_MAKE_NONCOPYABLE(ParkedBodies);
public:
    JS_EXPORT_PRIVATE ParkedBodies(std::span<Function>, const ParallelLoops&);
    JS_EXPORT_PRIVATE ~ParkedBodies();

    // From any thread. The first read of a function takes its body back.
    const Function& at(FunctionID id)
    {
        if (m_states[id.index()].load(std::memory_order_acquire) >= IsParked) [[unlikely]]
            takeBack(id);
        return m_functions[id.index()];
    }

    // While nobody reads a body. Parks what was taken back before the call before this one.
    JS_EXPORT_PRIVATE void parkAgain();
    JS_EXPORT_PRIVATE void takeAllBack();

    size_t numberOfParkedFunctions() const { return m_numberOfParkedFunctions; }
    size_t numberTakenBackAtRead() const { return m_numberTakenBackAtRead.load(std::memory_order_relaxed); }

private:
    enum State : uint8_t { WasNeverParked, IsTakenBack, IsParked, IsBeingTakenBack };

    JS_EXPORT_PRIVATE void takeBack(FunctionID);

    std::span<Function> m_functions;
    const ParallelLoops& m_loops;
    Vector<ParkedBody> m_bodies;
    UniqueArray<Atomic<uint8_t>> m_states;
    Lock m_takenBackLock;
    Vector<uint32_t> m_takenBackLately WTF_GUARDED_BY_LOCK(m_takenBackLock);
    Vector<uint32_t> m_takenBackEarlier;
    size_t m_numberOfParkedFunctions { 0 };
    std::atomic<size_t> m_numberTakenBackAtRead { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
