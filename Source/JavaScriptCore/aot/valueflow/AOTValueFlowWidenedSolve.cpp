/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowWidenedSolve.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include <array>
#include <wtf/HashSet.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static ReporterOfWhatIsFoundOnlyFromLess s_reporter;

void reportWhatIsFoundOnlyFromLessBy(ReporterOfWhatIsFoundOnlyFromLess reporter)
{
    s_reporter = reporter;
}

ValueSetID objectsOfProgramThatNoNodeHolds(const ValueFlowSolver& solver, const ValueFlowUniverse& universe, ValueSetMaker& maker)
{
    HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen;
    Vector<ValueSetID> held;
    for (ValueSetID value : solver.graph().values()) {
        if (value && seen.add(value.index()).isNewEntry)
            held.append(value);
    }
    return maker.intersect(ValueSets::complement(maker.uniteAll(held.span())), maker.intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects())));
}

void reportWhatIsFoundOnlyFromLess(ValueFlowSolution& solution, std::span<const UnseenCodeRecord> unseenCodeOfProgramAsItIs, BuildContext& build)
{
    if (s_reporter) {
        s_reporter(solution, unseenCodeOfProgramAsItIs, build);
        return;
    }

    enum Sort : unsigned { Functions, Builtins, Records, Objects, Nodes, numberOfSorts };
    const ValueFlowSolver& asItIs = *solution.solverOfProgramAsItIs();
    const ValueFlowSolver& givenMore = solution.solver();
    const ValueSets& sets = solution.sets();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    const Program& program = solution.program();
    ValueSetMakers makers(solution.makerPool(), build.parallelLoops());
    ValueSetMaker& maker = makers.of(0);
    std::array<uint32_t, numberOfSorts> lacking { };
    std::array<uint32_t, numberOfSorts> first { };
    auto note = [&](Sort sort, uint32_t index) {
        if (!lacking[sort]++)
            first[sort] = index;
    };

    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (asItIs.isReached(FunctionID(index)) && !givenMore.isReached(FunctionID(index)))
            note(Functions, index);
    }
    for (uint32_t index = 0; index < program.builtins().builtinCount(); ++index) {
        if (!(asItIs.usesOf(BuiltinID(index)) - givenMore.usesOf(BuiltinID(index))).isEmpty())
            note(Builtins, index);
    }
    Vector<UnseenCodeRecord> unseenCodeGivenMore = unseenCodeRecordsOf(solution);
    for (uint32_t index = 0; index < unseenCodeOfProgramAsItIs.size(); ++index) {
        const UnseenCodeRecord& record = unseenCodeOfProgramAsItIs[index];
        bool isFound = false;
        for (const UnseenCodeRecord& other : unseenCodeGivenMore)
            isFound |= other.cause == record.cause && other.builtin == record.builtin;
        if (!isFound)
            note(Records, index);
    }
    ValueSetID neverMade = objectsOfProgramThatNoNodeHolds(givenMore, universe, maker);
    ValueSetID outOfSight = maker.unite(neverMade, maker.unite(givenMore.possibleValuesOf(ValueFlowProgramNodes::escaped()), givenMore.possibleValuesOf(ValueFlowProgramNodes::lost())));
    for (uint32_t index = 0; index < universe.numberOfObjects(); ++index) {
        if (!(asItIs.marksOf(ValueFlowObjectID(index)) - givenMore.marksOf(ValueFlowObjectID(index))).isEmpty() && !sets.contains(outOfSight, ValueFlowObjectID(index)))
            note(Objects, index);
    }
    for (uint32_t index = 0; index < asItIs.input().numberOfNodes; ++index) {
        ValueSetID less = asItIs.possibleValuesOf(ValueFlowNodeID(index));
        ValueSetID more = givenMore.possibleValuesOf(ValueFlowNodeID(index));
        if (less == more || sets.isSubset(less, more))
            continue;
        ValueSetID all = givenMore.withWhatMembersCover(more, maker);
        if (sets.contains(all, ValueFlowUniverse::unknownObject()))
            all = maker.unite(all, neverMade);
        if (!sets.isSubset(less, all))
            note(Nodes, index);
    }

    for (unsigned sort = 0; sort < numberOfSorts; ++sort) {
        if (!lacking[sort])
            continue;
        build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::FlowFindsLessFromMore,
            .pass = PassID::AnalyzeFlow,
            .detail = makeString(sort, ' ', lacking[sort], ' ', first[sort]),
        });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
