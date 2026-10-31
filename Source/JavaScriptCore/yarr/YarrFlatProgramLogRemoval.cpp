/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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

#include "config.h"
#include "YarrFlatProgram.h"

#include <array>
#include <utility>

namespace JSC { namespace Yarr {

namespace {

using Kind = FlatOp::Kind;

enum class StackState : uint8_t { Unreached, Empty, MayHoldFrames };

struct StackStates {
    Vector<StackState> atOp;
    Vector<StackState> atLookaheadBegin;
};

} // anonymous namespace

static bool propagateStackState(const FlatProgram& program, unsigned index, StackStates& states)
{
    const FlatOp& op = program.ops[index];
    StackState state = states.atOp[index];
    if (state == StackState::Unreached)
        return false;

    bool changed = false;
    auto join = [&](StackState& into, StackState from) {
        if (into >= from)
            return;
        into = from;
        changed = true;
    };
    auto& atOp = states.atOp;
    unsigned next = index + 1;

    switch (op.kind) {
    case Kind::Jump:
    case Kind::LoopBack:
    case Kind::LoopBackUnlessEmpty:
        join(atOp[op.target], state);
        break;
    case Kind::JumpIfBelow:
    case Kind::JumpIfNotBelow:
    case Kind::NeedsInputElseJump:
        join(atOp[next], state);
        join(atOp[op.target], state);
        break;
    case Kind::Choice:
    case Kind::ChoiceByFirst:
    case Kind::ChoiceByTable:
    case Kind::GreedyLoopChoice:
    case Kind::NegativeLookaheadBegin:
        join(atOp[next], StackState::MayHoldFrames);
        join(atOp[op.target], state);
        break;
    case Kind::GreedyRun:
    case Kind::LazyRun:
    case Kind::GreedyRunBeforeKnownCharacters:
    case Kind::LazyRunBeforeKnownCharacters:
        join(atOp[next], StackState::MayHoldFrames);
        break;
    case Kind::LookaheadBegin:
        join(atOp[next], state);
        join(states.atLookaheadBegin[op.registerIndex], state);
        break;
    case Kind::LookaheadEnd:
    case Kind::LookaheadEndKeepingCaptures:
        join(atOp[next], states.atLookaheadBegin[op.registerIndex]);
        break;
    case Kind::Match:
    case Kind::DotStarEnclosure:
    case Kind::NegativeLookaheadEnd:
    case Kind::RecordFailure:
        break;
    case Kind::LoggedCaptureStart:
    case Kind::LoggedCaptureEnd:
    case Kind::LoggedCaptureEmpty:
    case Kind::LoggedCapturedRun:
    case Kind::LoggedClearCaptures:
    case Kind::LoggedSavePosition:
    case Kind::LoggedSetRegister:
    case Kind::LoggedIncrement:
    case Kind::Character:
    case Kind::Text:
    case Kind::CasedText:
    case Kind::WideText:
    case Kind::Set:
    case Kind::SetCount:
    case Kind::PossessiveRun:
    case Kind::CapturedRun:
    case Kind::CaptureStart:
    case Kind::CaptureEnd:
    case Kind::CaptureEmpty:
    case Kind::ClearCaptures:
    case Kind::SavePosition:
    case Kind::SetRegister:
    case Kind::Increment:
    case Kind::FailIfEmpty:
    case Kind::NeedsInput:
    case Kind::StepBack:
    case Kind::CountTurn:
    case Kind::InputStart:
    case Kind::LineStart:
    case Kind::InputEnd:
    case Kind::LineEnd:
    case Kind::WordBoundary:
    case Kind::NotWordBoundary:
    case Kind::BackReference:
    case Kind::BackReferenceIgnoringCase:
        join(atOp[next], state);
        break;
    }
    return changed;
}

static StackStates computeStackStates(const FlatProgram& program)
{
    unsigned opCount = program.ops.size();
    StackStates states;
    states.atOp.fill(StackState::Unreached, opCount);
    states.atLookaheadBegin.fill(StackState::Unreached, program.numRegisters);
    states.atOp[0] = StackState::Empty;
    bool changed = true;
    while (changed) {
        changed = false;
        for (unsigned index = 0; index < opCount; ++index)
            changed |= propagateStackState(program, index, states);
    }
    return states;
}

static constexpr std::array<std::pair<Kind, Kind>, 9> formsWithoutLog { {
    { Kind::LoggedCaptureStart, Kind::CaptureStart },
    { Kind::LoggedCaptureEnd, Kind::CaptureEnd },
    { Kind::LoggedCaptureEmpty, Kind::CaptureEmpty },
    { Kind::LoggedCapturedRun, Kind::CapturedRun },
    { Kind::LoggedClearCaptures, Kind::ClearCaptures },
    { Kind::LoggedSavePosition, Kind::SavePosition },
    { Kind::LoggedSetRegister, Kind::SetRegister },
    { Kind::LoggedIncrement, Kind::Increment },
    { Kind::LookaheadEndKeepingCaptures, Kind::LookaheadEnd },
} };

void removeLogging(std::span<FlatOp> ops)
{
    for (FlatOp& op : ops) {
        for (auto [kind, kindWithoutLog] : formsWithoutLog) {
            if (op.kind == kind) {
                op.kind = kindWithoutLog;
                break;
            }
        }
    }
}

// Up to its first branch every attempt takes one path, and the only frames on the stack are those of runs on that path. A run that is resumed goes on along the path, and an attempt whose
// last run fails is over. So a capture that is set on the path is set again before anything reads it, unless a back reference in front of it on the path does.
static void removeLoggingBeforeFirstBranch(FlatProgram& program)
{
    Vector<bool, 32> isRead(FillWith { }, (program.numSubpatterns + 1) * 2, false);
    for (FlatOp& op : program.ops) {
        unsigned slotsWritten = 0;
        switch (op.kind) {
        case Kind::Character:
        case Kind::Text:
        case Kind::CasedText:
        case Kind::WideText:
        case Kind::Set:
        case Kind::SetCount:
        case Kind::NeedsInput:
        case Kind::InputStart:
        case Kind::LineStart:
        case Kind::InputEnd:
        case Kind::LineEnd:
        case Kind::WordBoundary:
        case Kind::NotWordBoundary:
        case Kind::PossessiveRun:
        case Kind::GreedyRun:
        case Kind::LazyRun:
        case Kind::GreedyRunBeforeKnownCharacters:
        case Kind::LazyRunBeforeKnownCharacters:
        case Kind::CaptureStart:
        case Kind::CaptureEnd:
        case Kind::CaptureEmpty:
        case Kind::CapturedRun:
            break;
        case Kind::BackReference:
        case Kind::BackReferenceIgnoringCase:
            isRead[op.argument] = true;
            isRead[op.argument + 1] = true;
            break;
        case Kind::LoggedCaptureStart:
        case Kind::LoggedCaptureEnd:
            slotsWritten = 1;
            break;
        case Kind::LoggedCaptureEmpty:
        case Kind::LoggedCapturedRun:
            slotsWritten = 2;
            break;
        default:
            return;
        }
        if (slotsWritten && !isRead[op.argument] && !isRead[op.argument + slotsWritten - 1])
            removeLogging({ &op, 1 });
    }
}

// A logged op pushes its undo frame only on a stack that holds a frame, so on a stack proven empty the unlogged op does the same.
void removeUnneededLogging(FlatProgram& program)
{
    if (!program.countsTurns)
        removeLoggingBeforeFirstBranch(program);

    StackStates states = computeStackStates(program);
    auto ops = program.ops.mutableSpan();
    for (unsigned index = 0; index < ops.size(); ++index) {
        StackState state = ops[index].kind == Kind::LookaheadEndKeepingCaptures ? states.atLookaheadBegin[ops[index].registerIndex] : states.atOp[index];
        if (state == StackState::Empty)
            removeLogging(ops.subspan(index, 1));
    }
}

} } // namespace JSC::Yarr
