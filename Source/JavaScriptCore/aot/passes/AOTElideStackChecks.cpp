/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTElideStackChecks.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTConvention.h"
#include "AOTEffectSummaries.h"
#include "AOTEffects.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include <algorithm>
#include <limits>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class CalledCode : uint8_t { Unknown, None, FunctionWithStackCheck };
enum class VisitState : uint8_t { NotSeen, InProgress, Done };

} // anonymous namespace

ASCIILiteral nameOf(StackCheck check)
{
    switch (check) {
    case StackCheck::Required:
        return "Required"_s;
    case StackCheck::CoveredByCallers:
        return "CoveredByCallers"_s;
    case StackCheck::DeferredToCallee:
        return "DeferredToCallee"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StackChecks::dump(PrintStream& out, const Program& program) const
{
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (program.functions[i].hasBody())
            out.print("f", i, " stack check: ", nameOf(of(FunctionID(i))), "\n");
    }
}

// The entrypoint of the method table tests its arguments and goes on with a tail call. It needs no frame, unless it has to check the stack for what it enters.
// So what it enters that way is not covered by its callers: it checks where it sets up a frame, and the entrypoint leaves the check to it.
// The method table names the body itself where its entrypoint has nothing to test, and then nothing stands between the call and the body.
static BitVector findFunctionsThatTableEntrypointsTailCall(const Program& program, const ParallelLoops& loops)
{
    BitVector isTailCalled(program.functions.size());
    auto tableEntrypoints = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        return function.hasBody() && function.role == FunctionRole::TableEntrypoint;
    });
    for (uint32_t index : tableEntrypoints) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::ReturnCall)
                continue;
            const Value& call = function.at(block.terminator.callOperand());
            if (call.opcode == Opcode::CallDirect)
                isTailCalled.set(call.function().index());
        }
    }
    return isTailCalled;
}

// A closure names its generic entrypoint, and whatever enters code through the word of a closure tests the stack right in front of it: the stubs that call a value
// or an accessor, and the adapters through which the host comes in, a module's body too. So such an entry is covered like a direct call from a function with a check,
// and a function that closures are made of is itself entered by direct calls alone.
static bool isEnteredBehindStackCheck(const Function& function, EntrySet entrySet)
{
    switch (function.role) {
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::ResultWrapper:
    case FunctionRole::GenericEntrypoint:
        return true;
    default:
        return entrySet == EntrySet::DirectCallers && !function.creator.is<CreatorRecord::Entrypoint>();
    }
}

static BitVector findCandidates(const Program& program, const CallGraph& callGraph, const BuildContext& build)
{
    unsigned count = program.functions.size();
    BitVector isCandidate(count);
    BitVector isTailCalledByTableEntrypoint = findFunctionsThatTableEntrypointsTailCall(program, build.parallelLoops());
    auto candidates = findIndicesInParallel(build.parallelLoops(), count, [&](unsigned i) {
        FunctionID id(i);
        auto& function = program.at(id);
        return function.hasBody()
            && isEnteredBehindStackCheck(function, callGraph.entrySet(id))
            && function.hasOrdinaryFrame()
            && !isTailCalledByTableEntrypoint.get(i)
            && build.isEnabled(PassID::ElideStackChecks, id);
    });
    for (unsigned i : candidates)
        isCandidate.set(i);
    return isCandidate;
}

// The tests in front of a body: whichever way they end, the function goes on with a tail call, so no frame of its own is left on the stack.
static bool isEntranceThatLeavesByTailCallsAlone(const Function& function)
{
    if (function.role != FunctionRole::EntryChecks && function.role != FunctionRole::ParameterWrapper)
        return false;
    return std::ranges::all_of(function.blocks, [&](const Block& block) {
        return std::ranges::all_of(block.values, [&](ValueID id) {
            return function.at(id).opcode != Opcode::CallDirect || (block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.callOperand() == id);
        });
    });
}

// An entrance needs no frame but for a check of its own: the frame, the check, and a jump to the body where it would fall into it. A body has its frame anyway.
// So where a cycle of calls, or the number of functions without a check, asks an entrance to check, what it enters checks in its place.
static void computeHeights(const Program& program, const CallGraph& callGraph, unsigned maxNested, BitVector& isCandidate, BuildContext& build)
{
    unsigned count = program.functions.size();
    Vector<VisitState> state(FillWith { }, count, VisitState::NotSeen);
    Vector<uint16_t> height(FillWith { }, count, static_cast<uint16_t>(0));
    static_assert(maxNestedFunctionsWithoutStackCheck < std::numeric_limits<uint16_t>::max());
    Vector<Vector<uint32_t, 4>> callees(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned i) {
        if (!isCandidate.get(i))
            return;
        for (FunctionID callee : callGraph.calleesOf(FunctionID(i))) {
            if (isCandidate.get(callee.index()))
                callees[i].append(callee.index());
        }
        if (callees[i].isEmpty() && maxNested) {
            state[i] = VisitState::Done;
            height[i] = 1;
        }
    });
    Vector<std::pair<uint32_t, uint32_t>> inProgress;
    for (unsigned root = 0; root < count; ++root) {
        if (!isCandidate.get(root) || state[root] != VisitState::NotSeen)
            continue;
        state[root] = VisitState::InProgress;
        inProgress.append({ root, 0 });
        while (!inProgress.isEmpty()) {
            auto [function, next] = inProgress.last();
            if (next < callees[function].size()) {
                ++inProgress.last().second;
                uint32_t callee = callees[function][next];
                if (state[callee] == VisitState::InProgress) {
                    if (isEntranceThatLeavesByTailCallsAlone(program.functions[callee])) {
                        for (uint32_t entered : callees[callee])
                            isCandidate.clear(entered);
                    } else
                        isCandidate.clear(callee);
                } else if (state[callee] == VisitState::NotSeen) {
                    state[callee] = VisitState::InProgress;
                    inProgress.append({ callee, 0 });
                }
                continue;
            }
            unsigned below = 0;
            for (uint32_t callee : callees[function]) {
                // What is still without a check while it is being visited is an entrance in a cycle: all that it enters checks, so it is one function and nothing below it.
                if (isCandidate.get(callee))
                    below = std::max<unsigned>(below, state[callee] == VisitState::InProgress ? 1 : height[callee]);
            }
            if (isCandidate.get(function) && below >= maxNested && isEntranceThatLeavesByTailCallsAlone(program.functions[function])) {
                below = 0;
                for (uint32_t callee : callees[function]) {
                    if (!isCandidate.get(callee))
                        continue;
                    if (height[callee] >= maxNested)
                        isCandidate.clear(callee);
                    else
                        below = std::max<unsigned>(below, height[callee]);
                }
            }
            if (isCandidate.get(function) && below >= maxNested) {
                build.gaveUp(LimitID::UncheckedStackBytes, Subject { .function = FunctionID(function) });
                isCandidate.clear(function);
            }
            height[function] = isCandidate.get(function) ? static_cast<uint16_t>(below + 1) : 0;
            state[function] = VisitState::Done;
            inProgress.removeLast();
        }
    }
    build.destroyLater(WTF::move(callees));
}

// An entrance that lies in a cycle of calls has a check of its own. On the way into a function with a check it is one load, the poll of the cycle;
// on the way into a function that leaves its check to its callers it is a frame, the check, and a call where the check fails.
// Where the body that the entrance mostly enters checks for itself, what it rarely enters does so too, and then the entrance can leave all of it to them.
static void checkStackInWhatEntrancesRarelyEnter(const Program& program, BitVector& isCandidate, const BuildContext& build)
{
    auto entrances = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned i) {
        const Function& function = program.functions[i];
        return function.hasBody() && isEntranceThatLeavesByTailCallsAlone(function) && build.isEnabled(PassID::ElideStackChecks, FunctionID(i));
    });
    for (uint32_t i : entrances) {
        const Function& function = program.functions[i];
        if (isCandidate.get(i))
            continue;
        Vector<uint32_t, 2> rarelyEntered;
        bool mostlyEntersFunctionWithCheck = true;
        for (const Block& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::ReturnCall)
                continue;
            const Value& call = function.at(block.terminator.callOperand());
            if (call.opcode != Opcode::CallDirect || !isCandidate.get(call.function().index()))
                continue;
            if (block.frequency == BlockFrequency::Normal)
                mostlyEntersFunctionWithCheck = false;
            rarelyEntered.append(call.function().index());
        }
        if (!mostlyEntersFunctionWithCheck)
            continue;
        for (uint32_t callee : rarelyEntered)
            isCandidate.clear(callee);
    }
}

static bool hasRequiredStackCheck(const Program& program, FunctionID id, const BitVector& isCandidate)
{
    auto& function = program.at(id);
    return function.hasBody() && !isCandidate.get(id.index()) && !function.creator.is<CreatorRecord::Entrypoint>();
}

static CalledCode classifyCalledCode(const EffectContext& context, const Function& function, const Value& value, const BitVector& isCandidate)
{
    Effects effects = effectsOf(context, function, value).onAnyEdge();
    if (effects.maySuspend)
        return CalledCode::Unknown;
    if (!effects.mayCallUnknownCode)
        return CalledCode::None;
    if (effectRuleOf(value.opcode) != EffectRule::Call)
        return CalledCode::Unknown;

    bool hasCallee = false;
    bool hasUncheckedCallee = false;
    forEachFunctionReference(value, [&](FunctionID target, FunctionReferenceKind kind) {
        if (kind != FunctionReferenceKind::Call) {
            hasUncheckedCallee = true;
            return;
        }
        hasCallee = true;
        hasUncheckedCallee |= !hasRequiredStackCheck(context.program, target, isCandidate);
    });
    return hasCallee && !hasUncheckedCallee ? CalledCode::FunctionWithStackCheck : CalledCode::Unknown;
}

static bool canDeferStackCheck(const EffectContext& context, const Function& function, const BitVector& isCandidate)
{
    bool hasDirectCall = false;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            switch (classifyCalledCode(context, function, function.at(id), isCandidate)) {
            case CalledCode::Unknown:
                return false;
            case CalledCode::None:
                break;
            case CalledCode::FunctionWithStackCheck:
                hasDirectCall = true;
                break;
            }
        }
        Effects effects = effectsOf(context, function, block.terminator).onAnyEdge();
        if (effects.mayCallUnknownCode || effects.maySuspend)
            return false;
    }
    return hasDirectCall;
}

static Vector<uint32_t> findDeferringEntrypoints(const EffectContext& context, const BitVector& isCandidate, const BuildContext& build)
{
    const Program& program = context.program;
    return findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned i) {
        FunctionID id(i);
        auto& function = program.at(id);
        return function.hasBody()
            && (function.creator.is<CreatorRecord::Entrypoint>() || isEntranceThatLeavesByTailCallsAlone(function))
            && !isCandidate.get(i)
            && build.isEnabled(PassID::ElideStackChecks, id)
            && canDeferStackCheck(context, function, isCandidate);
    });
}

static void removeEntrancesThatDeferInCycle(const Program& program, Vector<uint32_t>& deferringEntrypoints)
{
    BitVector mayLieInCycle(program.functions.size());
    for (uint32_t i : deferringEntrypoints)
        mayLieInCycle.set(i);
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t i : deferringEntrypoints) {
            if (!mayLieInCycle.get(i))
                continue;
            const Function& function = program.functions[i];
            bool entersOneThatMay = std::ranges::any_of(function.blocks, [&](const Block& block) {
                return std::ranges::any_of(block.values, [&](ValueID id) {
                    const Value& value = function.at(id);
                    return value.opcode == Opcode::CallDirect && mayLieInCycle.get(value.function().index());
                });
            });
            if (!entersOneThatMay) {
                mayLieInCycle.clear(i);
                changed = true;
            }
        }
    }
    deferringEntrypoints.removeAllMatching([&](uint32_t i) {
        return mayLieInCycle.get(i);
    });
}

StackChecks elideStackChecks(const Program& program, const CallGraph& callGraph, BuildContext& build)
{
    PhaseTimes times(build, PassID::ElideStackChecks);
    unsigned count = program.functions.size();
    unsigned maxNested = static_cast<unsigned>(std::min<uint64_t>(maxNestedFunctionsWithoutStackCheck, build.limit(LimitID::UncheckedStackBytes) / 2 / (maxFrameSizeWithoutStackCheck + 2 * sizeof(void*))));
    BitVector isCandidate = findCandidates(program, callGraph, build);
    computeHeights(program, callGraph, maxNested, isCandidate, build);
    checkStackInWhatEntrancesRarelyEnter(program, isCandidate, build);

    EffectSummaries unknownSummaries;
    Vector<uint32_t> deferringEntrypoints = findDeferringEntrypoints(EffectContext { program, unknownSummaries }, isCandidate, build);
    removeEntrancesThatDeferInCycle(program, deferringEntrypoints);

    Vector<StackCheck> byFunction(FillWith { }, count, StackCheck::Required);
    for (size_t i : isCandidate)
        byFunction[i] = StackCheck::CoveredByCallers;
    for (uint32_t i : deferringEntrypoints)
        byFunction[i] = StackCheck::DeferredToCallee;
    build.statistics().count(StatisticID::StackChecksCoveredByCallers, isCandidate.bitCount());
    build.statistics().count(StatisticID::StackChecksDeferredToCallee, deferringEntrypoints.size());
    times.didFinish("elideStackChecks"_s);
    return StackChecks(WTF::move(byFunction));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
