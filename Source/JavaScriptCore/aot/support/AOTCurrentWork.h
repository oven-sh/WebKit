/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "JSExportMacros.h"
#include <atomic>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

namespace Typed {
class Program;
}

struct alignas(128) CurrentWork {
    std::atomic<const Typed::Program*> program { nullptr };
    std::atomic<uint32_t> functionPlusOne { 0 };
    std::atomic<const char*> step { nullptr };
    std::atomic<bool> isTaken { false };
};

namespace CurrentWorkInternal {
extern JS_EXPORT_PRIVATE CurrentWork sharedRecord;
extern JS_EXPORT_PRIVATE constinit thread_local CurrentWork* recordOfThread;
JS_EXPORT_PRIVATE CurrentWork& takeRecord();
}

ALWAYS_INLINE CurrentWork& currentWorkOfThread()
{
    CurrentWork& work = *CurrentWorkInternal::recordOfThread;
    if (&work == &CurrentWorkInternal::sharedRecord) [[unlikely]]
        return CurrentWorkInternal::takeRecord();
    return work;
}

class CurrentWorkScope {
    WTF_MAKE_NONCOPYABLE(CurrentWorkScope);
public:
    explicit CurrentWorkScope(ASCIILiteral step)
        : CurrentWorkScope(nullptr, 0, step)
    {
    }

    CurrentWorkScope(const Typed::Program& program, FunctionID function, ASCIILiteral step)
        : CurrentWorkScope(&program, function.index() + 1, step)
    {
    }

    ~CurrentWorkScope()
    {
        m_work.program.store(m_program, std::memory_order_relaxed);
        m_work.functionPlusOne.store(m_functionPlusOne, std::memory_order_relaxed);
        m_work.step.store(m_step, std::memory_order_relaxed);
    }

    void setStep(ASCIILiteral step) { m_work.step.store(step.characters(), std::memory_order_relaxed); }

private:
    CurrentWorkScope(const Typed::Program* program, uint32_t functionPlusOne, ASCIILiteral step)
        : m_work(currentWorkOfThread())
        , m_program(m_work.program.load(std::memory_order_relaxed))
        , m_functionPlusOne(m_work.functionPlusOne.load(std::memory_order_relaxed))
        , m_step(m_work.step.load(std::memory_order_relaxed))
    {
        m_work.program.store(program ? program : m_program, std::memory_order_relaxed);
        m_work.functionPlusOne.store(functionPlusOne, std::memory_order_relaxed);
        m_work.step.store(step.characters(), std::memory_order_relaxed);
    }

    CurrentWork& m_work;
    const Typed::Program* const m_program;
    const uint32_t m_functionPlusOne;
    const char* const m_step;
};

JS_EXPORT_PRIVATE size_t describeCurrentThreadWork(std::span<char>);
JS_EXPORT_PRIVATE size_t describeAllThreadsWork(std::span<char>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
