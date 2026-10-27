/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTLimits.h"
#include "AOTPassID.h"
#include "JSExportMacros.h"
#include <atomic>
#include <optional>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {
class Program;
struct Function;
}

// What a thread of a build is at, for whoever says where a build has stopped or what it is still busy with.
// Only its thread writes it. Any thread reads it at any moment, a signal handler too, so the fields need not fit each other.
struct alignas(128) CurrentWork {
    // The thread's own, or one that all threads share and nobody reads where more threads build than there are records.
    static CurrentWork& ofCurrentThread();

    void setPass(PassID id) { pass.store(id, std::memory_order_relaxed); }
    void setFunction(Typed::FunctionID asRead) { functionPlusOne.store(asRead.index() + 1, std::memory_order_relaxed); }
    // The back end takes the functions of a batch through each of its phases in turn.
    void enterBackEndPhase(uint32_t functionAsReadPlusOne, ASCIILiteral name)
    {
        functionPlusOne.store(functionAsReadPlusOne, std::memory_order_relaxed);
        backEndPhase.store(name.characters(), std::memory_order_relaxed);
    }
    void leaveBackEnd() { backEndPhase.store(nullptr, std::memory_order_relaxed); }
    void setFinishedPart(ASCIILiteral name) { finishedPart.store(name.characters(), std::memory_order_relaxed); }

    std::atomic<const Typed::Program*> program { nullptr };
    std::atomic<uint32_t> functionPlusOne { 0 };
    std::atomic<PassID> pass { PassID::None };
    std::atomic<Stage> stage { Stage::Any };
    std::atomic<bool> isTaken { false };
    std::atomic<const char*> backEndPhase { nullptr };
    std::atomic<const char*> finishedPart { nullptr };
    std::atomic<double> startInSeconds { 0 };
};

static_assert(!static_cast<uint32_t>(PassID::None) && !static_cast<uint32_t>(Stage::Any));

namespace CurrentWorkInternal {
extern JS_EXPORT_PRIVATE CurrentWork sharedRecord;
extern JS_EXPORT_PRIVATE constinit thread_local CurrentWork* recordOfThread;
JS_EXPORT_PRIVATE CurrentWork& takeRecord();
}

// For what runs under a CurrentWorkScope, which has taken the record.
ALWAYS_INLINE CurrentWork& currentWork() { return *CurrentWorkInternal::recordOfThread; }

inline CurrentWork& CurrentWork::ofCurrentThread()
{
    CurrentWork& work = currentWork();
    if (&work == &CurrentWorkInternal::sharedRecord) [[unlikely]]
        return CurrentWorkInternal::takeRecord();
    return work;
}

// What a task takes along from the thread that starts it.
struct InheritedWork {
    static InheritedWork ofCurrentThread()
    {
        const CurrentWork& work = currentWork();
        return { work.program.load(std::memory_order_relaxed), work.functionPlusOne.load(std::memory_order_relaxed), work.pass.load(std::memory_order_relaxed), work.stage.load(std::memory_order_relaxed) };
    }

    const Typed::Program* program { nullptr };
    uint32_t functionPlusOne { 0 };
    PassID pass { PassID::None };
    Stage stage { Stage::Any };
};

// While one is alive the thread is at what it names. Behind it the thread is at what it was at before.
class CurrentWorkScope {
    WTF_MAKE_NONCOPYABLE(CurrentWorkScope);
public:
    // A stage, or a pass of the stage that the thread is at.
    CurrentWorkScope(std::optional<Stage> stage, PassID pass, MonotonicTime start)
        : CurrentWorkScope(start)
    {
        if (stage)
            m_work.stage.store(*stage, std::memory_order_relaxed);
        m_work.setPass(pass);
    }

    CurrentWorkScope(const Typed::Program& program, Typed::FunctionID functionAsRead)
        : CurrentWorkScope(MonotonicTime::now())
    {
        m_work.program.store(&program, std::memory_order_relaxed);
        m_work.setFunction(functionAsRead);
    }

    explicit CurrentWorkScope(const InheritedWork& inherited)
        : CurrentWorkScope(MonotonicTime::now())
    {
        m_work.program.store(inherited.program, std::memory_order_relaxed);
        m_work.functionPlusOne.store(inherited.functionPlusOne, std::memory_order_relaxed);
        m_work.stage.store(inherited.stage, std::memory_order_relaxed);
        m_work.setPass(inherited.pass);
    }

    ~CurrentWorkScope()
    {
        m_work.functionPlusOne.store(m_functionPlusOne, std::memory_order_relaxed);
        m_work.program.store(m_program, std::memory_order_relaxed);
        m_work.pass.store(m_pass, std::memory_order_relaxed);
        m_work.stage.store(m_stage, std::memory_order_relaxed);
        m_work.backEndPhase.store(m_backEndPhase, std::memory_order_relaxed);
        m_work.finishedPart.store(m_finishedPart, std::memory_order_relaxed);
        m_work.startInSeconds.store(m_startInSeconds, std::memory_order_relaxed);
    }

private:
    explicit CurrentWorkScope(MonotonicTime start)
        : m_work(CurrentWork::ofCurrentThread())
        , m_program(m_work.program.load(std::memory_order_relaxed))
        , m_functionPlusOne(m_work.functionPlusOne.load(std::memory_order_relaxed))
        , m_pass(m_work.pass.load(std::memory_order_relaxed))
        , m_stage(m_work.stage.load(std::memory_order_relaxed))
        , m_backEndPhase(m_work.backEndPhase.load(std::memory_order_relaxed))
        , m_finishedPart(m_work.finishedPart.load(std::memory_order_relaxed))
        , m_startInSeconds(m_work.startInSeconds.load(std::memory_order_relaxed))
    {
        m_work.functionPlusOne.store(0, std::memory_order_relaxed);
        m_work.backEndPhase.store(nullptr, std::memory_order_relaxed);
        m_work.finishedPart.store(nullptr, std::memory_order_relaxed);
        m_work.startInSeconds.store(start.secondsSinceEpoch().value(), std::memory_order_relaxed);
    }

    CurrentWork& m_work;
    const Typed::Program* const m_program;
    const uint32_t m_functionPlusOne;
    const PassID m_pass;
    const Stage m_stage;
    const char* const m_backEndPhase;
    const char* const m_finishedPart;
    const double m_startInSeconds;
};

// --aot-sabotage: the build stops at a step, or in a function named stopsTheCompiler, for the tests of what a build that stops says. For a build with a sabotage.
void stopWhereSabotaged(const BuildContext&, std::optional<Stage>, PassID);
void stopWhereSabotaged(const BuildContext&, const Typed::Program&, const Typed::Function&, PassID);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
