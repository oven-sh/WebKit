/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "WriteBarrier.h"
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>

// Telling a program of what is run: sys.monitoring (PEP 669), and sys.settrace() and sys.setprofile(), which are made out of it as they are in CPython.
//
// There is one version of the code, which has in it a place for each thing that can be told of: op_py_enter, op_py_line, op_py_call, op_py_called, op_py_branch, op_py_jump, op_py_leave and
// op_py_ret. Each does nothing but look at VM::m_pythonLimitUnlessWatched, which is 0 while there is anything to tell anyone. What is raised is told of by the unwinder.

namespace JSC { namespace Python {

// The numbers are those of Include/cpython/monitoring.h, and 1 << event is what a program has for it in sys.monitoring.events.
enum class MonitoringEvent : uint8_t {
    // Those that can be asked for of one code object.
    PyStart,
    PyResume,
    PyReturn,
    PyYield,
    Call,
    Line,
    Instruction,
    Jump,
    BranchLeft,
    BranchRight,
    StopIteration,
    // Those that cannot, nor be disabled at a place.
    Raise,
    ExceptionHandled,
    PyUnwind,
    PyThrow,
    Reraise,
    // Those that go with another: the first two with Call, and the last is both kinds of branch.
    CReturn,
    CRaise,
    Branch,
};

static constexpr unsigned numberOfLocalMonitoringEvents = 11;
static constexpr unsigned numberOfUngroupedMonitoringEvents = 16;
static constexpr unsigned numberOfMonitoringEvents = 19;
static constexpr unsigned numberOfMonitoringTools = 8;
// The last two are not for a program to use.
static constexpr unsigned profileTool = 6; // sys.setprofile()
static constexpr unsigned traceTool = 7; // sys.settrace()

// What is asked for of one code object. A FunctionExecutable has it, if anything has been.
struct CodeMonitor {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CodeMonitor);

    // Which tools are told of each event.
    std::array<uint8_t, numberOfLocalMonitoringEvents> tools { };
    // What MonitoringState::toolVersions had when each tool last asked. If it has moved on, the tool has been cleared since and asks for nothing.
    std::array<uint32_t, numberOfMonitoringTools> toolVersions { };
    // For each event, by where in the code plus one: the tools that have said that they want no more of it there, by returning sys.monitoring.DISABLE. It holds until
    // sys.monitoring.restart_events(), or until the tool stops asking for the event and asks for it again.
    std::array<UncheckedKeyHashMap<unsigned, uint8_t>, numberOfLocalMonitoringEvents> disabledTools;
    uint32_t restartVersion { 0 };
    // What MonitoringState::timesAskedFor had when what a tool has disabled was last looked at.
    std::array<std::array<uint32_t, numberOfLocalMonitoringEvents>, numberOfMonitoringTools> timesAskedFor { };

    void forgetDisabled(unsigned tool, unsigned event)
    {
        for (auto& tools : disabledTools[event].values())
            tools &= ~(1u << tool);
    }
};

// What a realm has, sys being a realm's.
struct MonitoringState {
    // Which tools are told of each event, wherever it happens.
    std::array<uint8_t, numberOfUngroupedMonitoringEvents> tools { };
    WriteBarrier<Unknown> callbacks[numberOfMonitoringTools][numberOfMonitoringEvents];
    // Which tools were given what they have for BRANCH_LEFT, and for BRANCH_RIGHT, as what to tell of BRANCH, which was the two as one.
    uint8_t toolsToldOfBranchLeftAsBranch { 0 };
    uint8_t toolsToldOfBranchRightAsBranch { 0 };
    WriteBarrier<Unknown> toolNames[numberOfMonitoringTools];
    std::array<uint32_t, numberOfMonitoringTools> toolVersions { };
    uint32_t restartVersion { 1 };
    // How many times each tool has begun to ask for each event, wherever it happens.
    std::array<std::array<uint32_t, numberOfLocalMonitoringEvents>, numberOfMonitoringTools> timesAskedFor { };
    // Nothing is told of what is done by what is being told.
    unsigned callbackDepth { 0 };
    // What is being told, if anything is, of which frame, and where that has got to. It is for frame.f_lineno = n, which can be done only by what is being told of a line.
    std::optional<MonitoringEvent> eventBeingTold;
    CallFrame* frameBeingToldOf { nullptr };
    unsigned offsetBeingToldOf { 0 };
    bool isToldFromLine { false }; // By op_py_line, which is what can go on from somewhere else.
    WriteBarrier<Unknown> traceFunction;
    WriteBarrier<Unknown> profileFunction;
    WriteBarrier<Unknown> disable; // sys.monitoring.DISABLE
    WriteBarrier<Unknown> missing; // sys.monitoring.MISSING
    // A StopIteration that was raised to say that there is no more, and caught by what was asking, which is about to say so.
    WriteBarrier<Unknown> caughtStopIteration;
    unsigned codesWithLocalEvents { 0 };
    bool isWatching { false };

    template<typename Visitor>
    void visit(Visitor& visitor)
    {
        for (auto& row : callbacks) {
            for (auto& callback : row)
                visitor.append(callback);
        }
        for (auto& name : toolNames)
            visitor.append(name);
        visitor.append(traceFunction);
        visitor.append(profileFunction);
        visitor.append(disable);
        visitor.append(missing);
        visitor.append(caughtStopIteration);
    }
};

} } // namespace JSC::Python
