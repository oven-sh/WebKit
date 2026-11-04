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

// What a thread of a build is at, for the text of a crash.
// Only its thread writes it. Any thread reads it at any moment, a signal handler too, so the fields need not fit each other.
struct alignas(128) CurrentWork {
    std::atomic<const Typed::Program*> program { nullptr };
    std::atomic<uint32_t> functionPlusOne { 0 };
    // "facts: scan", "facts: solve", "plan", "walk", "sweep", "emit", "allocate", "encode", "image". A literal, so that a signal handler may read it.
    std::atomic<const char*> step { nullptr };
    std::atomic<bool> isTaken { false };
};

namespace CurrentWorkInternal {
extern JS_EXPORT_PRIVATE CurrentWork sharedRecord;
extern JS_EXPORT_PRIVATE constinit thread_local CurrentWork* recordOfThread;
// The thread's own from then on, or the one that all threads share and nobody reads where more threads build than there are records.
JS_EXPORT_PRIVATE CurrentWork& takeRecord();
}

ALWAYS_INLINE CurrentWork& currentWorkOfThread()
{
    CurrentWork& work = *CurrentWorkInternal::recordOfThread;
    if (&work == &CurrentWorkInternal::sharedRecord) [[unlikely]]
        return CurrentWorkInternal::takeRecord();
    return work;
}

// While one is alive the thread is at what it names. Behind it the thread is at what it was at before. It costs four relaxed stores each way.
class CurrentWorkScope {
    WTF_MAKE_NONCOPYABLE(CurrentWorkScope);
public:
    // A stage that is at no one function.
    explicit CurrentWorkScope(ASCIILiteral step)
        : CurrentWorkScope(nullptr, 0, step)
    {
    }

    // Compile opens one per (function, role) it takes from its queue, and calls setStep as the function goes from walk to encode.
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

// Both are safe in a signal handler, allocate nothing, and return how many bytes they wrote. Bun's crash handler calls them through Bun__AOT__describe*.
// "Stopped while compiling parse (src/parser.ts:12) in walk.\n", or nothing where the thread is at no work.
JS_EXPORT_PRIVATE size_t describeWhereThisThreadStopped(std::span<char>);
// One line for each thread that is at a function.
JS_EXPORT_PRIVATE size_t describeWhatThreadsAreCompiling(std::span<char>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
