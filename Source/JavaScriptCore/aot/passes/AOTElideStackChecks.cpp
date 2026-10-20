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

static BitVector findCandidates(const Program& program, const CallGraph& callGraph, const BuildContext& build)
{
    unsigned count = program.functions.size();
    BitVector isCandidate(count);
    auto candidates = findIndicesInParallel(build.parallelLoops(), count, [&](unsigned i) {
        FunctionID id(i);
        auto& function = program.at(id);
        return function.hasBody()
            && callGraph.entrySet(id) == EntrySet::DirectCallers
            && function.hasOrdinaryFrame()
            && !function.creator.is<CreatorRecord::Entrypoint>()
            && build.isEnabled(PassID::ElideStackChecks, id);
    });
    for (unsigned i : candidates)
        isCandidate.set(i);
    return isCandidate;
}

static void computeHeights(unsigned count, const CallGraph& callGraph, unsigned maxNested, BitVector& isCandidate, BuildContext& build)
{
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
                if (state[callee] == VisitState::InProgress)
                    isCandidate.clear(callee);
                else if (state[callee] == VisitState::NotSeen) {
                    state[callee] = VisitState::InProgress;
                    inProgress.append({ callee, 0 });
                }
                continue;
            }
            unsigned below = 0;
            for (uint32_t callee : callees[function]) {
                if (isCandidate.get(callee))
                    below = std::max<unsigned>(below, height[callee]);
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
            && function.creator.is<CreatorRecord::Entrypoint>()
            && build.isEnabled(PassID::ElideStackChecks, id)
            && canDeferStackCheck(context, function, isCandidate);
    });
}

StackChecks elideStackChecks(const Program& program, const CallGraph& callGraph, BuildContext& build)
{
    unsigned count = program.functions.size();
    unsigned maxNested = static_cast<unsigned>(std::min<uint64_t>(maxNestedFunctionsWithoutStackCheck, build.limit(LimitID::UncheckedStackBytes) / 2 / (maxFrameSizeWithoutStackCheck + 2 * sizeof(void*))));
    BitVector isCandidate = findCandidates(program, callGraph, build);
    computeHeights(count, callGraph, maxNested, isCandidate, build);

    EffectSummaries unknownSummaries;
    Vector<uint32_t> deferringEntrypoints = findDeferringEntrypoints(EffectContext { program, unknownSummaries }, isCandidate, build);

    Vector<StackCheck> byFunction(FillWith { }, count, StackCheck::Required);
    for (size_t i : isCandidate)
        byFunction[i] = StackCheck::CoveredByCallers;
    for (uint32_t i : deferringEntrypoints)
        byFunction[i] = StackCheck::DeferredToCallee;
    build.statistics().count(StatisticID::StackChecksCoveredByCallers, isCandidate.bitCount());
    build.statistics().count(StatisticID::StackChecksDeferredToCallee, deferringEntrypoints.size());
    return StackChecks(WTF::move(byFunction));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
