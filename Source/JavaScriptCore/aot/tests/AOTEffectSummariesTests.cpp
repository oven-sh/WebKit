/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTEffectSummariesInternal.h"
#include "AOTNativeTest.h"
#include "AOTVector.h"
#include "AOTBitVector.h"

namespace JSC { namespace AOT { namespace Typed {

static ParameterSummary retainedInOperand(uint32_t operand)
{
    return { { OperandUses::all(), RetainedIn::Operand, operand }, OperandMemory::none() };
}

static FunctionSummary summaryOfFunctionThatWritesNothing()
{
    return { .elsewhere = EffectsOnEdges::none(), .throughParameters = OperandMemory::none() };
}

static void testSummaryWithoutParameters()
{
    ParameterSummary retainedInResult { { OperandUses::all(), RetainedIn::Result, 0 }, OperandMemory::none() };
    FunctionSummary summary = summaryOfFunctionThatWritesNothing();
    summary.parameters = { ParameterSummary { }, retainedInOperand(3), retainedInOperand(0), retainedInResult };
    AOT_TEST_CHECK(summary.withoutParameters({ }) == summary);

    BitVector removed;
    removed.set(0);
    FunctionSummary reduced = summary.withoutParameters(removed);
    AOT_TEST_CHECK(reduced.elsewhere == summary.elsewhere && reduced.throughParameters == summary.throughParameters && reduced.parameters.size() == 3);
    AOT_TEST_CHECK(reduced.parameters[0] == retainedInOperand(2));
    AOT_TEST_CHECK(reduced.parameters[1].operand.retainedIn == RetainedIn::UnknownMemory && !reduced.parameters[1].operand.retainingOperand);
    AOT_TEST_CHECK(reduced.parameters[2] == retainedInResult);

    removed.set(7);
    AOT_TEST_CHECK(summary.withoutParameters(removed) == reduced);

    summary.parameters = { retainedInOperand(9) };
    AOT_TEST_CHECK(summary.withoutParameters({ }).parameters[0].operand.retainedIn == RetainedIn::UnknownMemory);
}

static void testSummariesOfAddedFunctions()
{
    FunctionSummary writesNothing = summaryOfFunctionThatWritesNothing();
    FunctionSummary unknown;
    AOT_TEST_CHECK(writesNothing != unknown);

    EffectSummaries none;
    EffectSummaries first(none, FunctionID(2u), { writesNothing });
    EffectSummaries second(first, FunctionID(5u), { unknown, writesNothing });

    for (unsigned index : { 2u, 5u, 6u })
        AOT_TEST_CHECK(second.contains(FunctionID(index)));
    for (unsigned index : { 0u, 1u, 3u, 4u, 7u })
        AOT_TEST_CHECK(!second.contains(FunctionID(index)) && second.of(FunctionID(index)) == unknown);
    AOT_TEST_CHECK(!second.contains(FunctionID()) && second.of(FunctionID()) == unknown);
    AOT_TEST_CHECK(second.of(FunctionID(2u)) == writesNothing && second.of(FunctionID(5u)) == unknown && second.of(FunctionID(6u)) == writesNothing);
    AOT_TEST_CHECK(!first.contains(FunctionID(6u)) && first.of(FunctionID(6u)) == unknown);
    AOT_TEST_CHECK(second.unknownCode() == none.unknownCode());
}

static LocationMasks masksOfObjectFields(uint64_t mask)
{
    LocationMasks masks;
    masks.add(AbstractHeap::ObjectFields, mask);
    return masks;
}

static void testInternedLocationMasks()
{
    constexpr uint32_t numberOfMasks = 300000;
    Ref<InternedLocationMasks> table = InternedLocationMasks::create();
    AOT_TEST_CHECK(table->intern(LocationMasks()) == InternedLocationMasks::empty && table->at(InternedLocationMasks::empty).isEmpty());

    Vector<uint32_t> ids(numberOfMasks, [&](size_t index) {
        return table->intern(masksOfObjectFields(index + 1));
    });
    AOT_TEST_CHECK(table->size() == numberOfMasks + 1);
    bool areKept = true;
    for (uint32_t index = 0; index < numberOfMasks; ++index)
        areKept &= table->at(ids[index]) == masksOfObjectFields(index + 1) && table->intern(masksOfObjectFields(index + 1)) == ids[index];
    AOT_TEST_CHECK(areKept && table->size() == numberOfMasks + 1);

    AOT_TEST_CHECK(table->merge(ids[0], InternedLocationMasks::empty) == ids[0] && table->merge(InternedLocationMasks::empty, ids[0]) == ids[0]);
    AOT_TEST_CHECK(table->merge(ids[0], ids[1]) == ids[2] && table->merge(ids[2], ids[0]) == ids[2]);
}

static void testCompactEffects()
{
    Ref<InternedLocationMasks> table = InternedLocationMasks::create();
    Effects onlyForLackOfMemory = Effects::none();
    onlyForLackOfMemory.mayThrow = ThrowEffect::onlyForLackOfMemory();
    onlyForLackOfMemory.reads = masksOfObjectFields(5);
    Effects suspends = Effects::none();
    suspends.maySuspend = true;
    suspends.writes = LocationMasks::top();
    for (const Effects& effects : { Effects::none(), Effects::unknownCode(), Effects(), onlyForLackOfMemory, suspends }) {
        CompactEffects compact = table->compress(effects);
        AOT_TEST_CHECK(table->expand(compact) == effects && table->compress(effects) == compact);
        AOT_TEST_CHECK(table->expand(compact.withoutSuspension()) == effects.withoutSuspension());
        Effects merged = onlyForLackOfMemory;
        table->mergeInto(merged, compact);
        Effects expected = onlyForLackOfMemory;
        expected.merge(effects);
        AOT_TEST_CHECK(merged == expected);
    }
    AOT_TEST_CHECK(table->compress(Effects::none()) == CompactEffects());
}

static void testStoredSummaries()
{
    FunctionSummary reads = summaryOfFunctionThatWritesNothing();
    reads.elsewhere.normal.reads = masksOfObjectFields(1);
    reads.elsewhere.normal.mayThrow = true;
    reads.elsewhere.handler.reads = masksOfObjectFields(2);
    reads.elsewhere.handler.mayThrow = true;
    reads.throughParameters.writes = masksOfObjectFields(4);
    reads.parameters = { ParameterSummary { }, retainedInOperand(0), ParameterSummary { { { OperandUse::Inspects }, RetainedIn::Result, 0 }, { LocationMasks(), masksOfObjectFields(4) } } };
    FunctionSummary summaries[] = { FunctionSummary(), reads, summaryOfFunctionThatWritesNothing() };

    EffectSummaries stored(std::span<const FunctionSummary> { summaries }, Effects::none());
    AOT_TEST_CHECK(stored.size() == 3 && stored.unknownCode() == Effects::none());
    for (unsigned index = 0; index < 3; ++index) {
        FunctionSummaryView view = stored.of(FunctionID(index));
        const FunctionSummary& summary = summaries[index];
        AOT_TEST_CHECK(view == summary && view.onCall() == summary.onCall() && view.elsewhere() == summary.elsewhere && view.throughParameters() == summary.throughParameters);
        AOT_TEST_CHECK(view.numberOfParameters() == summary.parameters.size());
        for (uint32_t parameter = 0; parameter < 4; ++parameter)
            AOT_TEST_CHECK(view.operand(parameter) == summary.parameter(parameter).operand && view.memory(parameter) == summary.parameter(parameter).memory);
    }
    AOT_TEST_CHECK(!stored.contains(FunctionID(3u)) && stored.of(FunctionID(3u)) == FunctionSummary());
}

static void testStoredBodySummary(uint32_t largestParameter)
{
    Ref<InternedLocationMasks> table = InternedLocationMasks::create();
    FunctionID function(7u);
    BodySummary body;
    body.onReturn.reads = masksOfObjectFields(1);
    body.onThrow.maySuspend = true;
    body.parameters = { ParameterSummary { }, retainedInOperand(0) };
    body.unknownCodeWitness = { function, ValueID(3u) };
    body.followsFieldsOfOperands = true;
    body.dependencies = {
        { SummaryNode::onReturn(FunctionID(2u)), SummaryNode::onReturn(function) },
        { SummaryNode::onReturn(function), SummaryNode::onThrow(function) },
        { SummaryNode::onReturn(FunctionID(300000u)), SummaryNode::onReturn(function) },
        { SummaryNode::onThrow(FunctionID(2u)), SummaryNode::onThrow(function) },
        { SummaryNode::parameterOf(FunctionID(2u), 200), SummaryNode::parameterOf(function, 1) },
        { SummaryNode::parameterOf(FunctionID(2u), largestParameter), SummaryNode::onReturn(function) },
        { SummaryNode::unknownCode(), SummaryNode::onReturn(function) },
        { SummaryNode::unknownCode(), SummaryNode::parameterOf(function, 0) },
    };

    StoredBodySummary stored(table, function, body);
    AOT_TEST_CHECK(table->expand(stored.onReturn()) == body.onReturn && table->expand(stored.onThrow()) == body.onThrow);
    AOT_TEST_CHECK(stored.unknownCodeWitness(function) == body.unknownCodeWitness && stored.followsFieldsOfOperands());
    AOT_TEST_CHECK(stored.numberOfParameters() == 2 && stored.parameters()[1].operand() == body.parameters[1].operand && table->expandMemory(stored.parameters()[0].memory) == body.parameters[0].memory);

    auto nodeOf = [](FunctionID owner, uint32_t node) {
        if (!owner)
            return SummaryNode::unknownCode();
        if (!node)
            return SummaryNode::onReturn(owner);
        return node == 1 ? SummaryNode::onThrow(owner) : SummaryNode::parameterOf(owner, node - 2);
    };
    Vector<SummaryDependency> dependencies;
    stored.forEachGroupOfDependencies([&](FunctionID source, const StoredBodySummary::DependencyGroup& group) {
        group.forEach([&](uint32_t sourceNode, uint32_t targetNode) {
            dependencies.append(SummaryDependency { nodeOf(source, sourceNode), nodeOf(function, targetNode) });
        });
    });
    AOT_TEST_CHECK(dependencies.size() == body.dependencies.size() && stored.numberOfDependencies() == body.dependencies.size());
    for (const SummaryDependency& dependency : body.dependencies)
        AOT_TEST_CHECK(dependencies.contains(dependency));

    Vector<FunctionID> functions;
    stored.forEachFunctionInDependencies(function, [&](FunctionID named) {
        functions.append(named);
    });
    AOT_TEST_CHECK(functions.size() == 2 && functions[0] == FunctionID(2u) && functions[1] == FunctionID(300000u));

    AOT_TEST_CHECK(stored == StoredBodySummary(table, function, body));
    std::ranges::reverse(body.dependencies);
    AOT_TEST_CHECK(stored == StoredBodySummary(table, function, body));
    body.dependencies.last().target = SummaryNode::onThrow(function);
    AOT_TEST_CHECK(!(stored == StoredBodySummary(table, function, body)));

    StoredBodySummary empty(table, function, BodySummary());
    AOT_TEST_CHECK(!empty.numberOfDependencies() && !empty.numberOfParameters() && empty == StoredBodySummary(table, function, BodySummary()));
}

static void testStoredBodySummaries()
{
    testStoredBodySummary(20000);
    testStoredBodySummary(70000);
}

std::span<const NativeTest> effectSummariesTests()
{
    static constexpr NativeTest tests[] = {
        { "effect-summaries/without-parameters"_s, testSummaryWithoutParameters },
        { "effect-summaries/of-added-functions"_s, testSummariesOfAddedFunctions },
        { "effect-summaries/interned-location-masks"_s, testInternedLocationMasks },
        { "effect-summaries/compact-effects"_s, testCompactEffects },
        { "effect-summaries/stored-summaries"_s, testStoredSummaries },
        { "effect-summaries/stored-body-summary"_s, testStoredBodySummaries },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
