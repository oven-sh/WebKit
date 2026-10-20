/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTPropertyAccessElements.h"
#include "AOTSplitUnions.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

TestPlacer::TestPlacer(FunctionPassContext& context, const TestPlacementPlan& plan)
    : m_context(context)
    , m_function(context.function)
    , m_plan(plan)
    , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
    , m_definitions(plan.conditions->size())
{
}

PassOutcome TestPlacer::run()
{
    collectSubjectOperands();

    auto blocksBeforeSplitting = m_function.blocks;
    size_t numberOfValuesBeforeSplitting = m_function.values.size();

    // A proof adopted on an edge that also carries tests has to become a parameter of the block the tests stand in.
    splitTestedEdges();
    adoptExistingProofs();
    collectCuts();
    splitBlocks();
    checkStretches();
    if (!numberOfGuardedStretches()) {
        // Left split, the function would be simplified, planned, split and rejected again in every round of the fixpoint.
        m_function.blocks = WTF::move(blocksBeforeSplitting);
        m_function.values.shrink(numberOfValuesBeforeSplitting);
        m_context.analyses.didChangeControlFlow();
        return PassOutcome::Unchanged;
    }

    cloneGenericCopies();
    redirectSlowPaths();
    emitTests();
    rewriteUses();
    repairProofs();
    replaceDominatedUses();
    retypeChangedValues();
    return PassOutcome::Changed;
}

unsigned TestPlacer::numberOfGuardedStretches() const
{
    unsigned numberOfStretches = 0;
    for (unsigned stretch = 0; stretch < m_plan.stretches.size(); ++stretch) {
        if (isPlaced(stretch) && m_plan.stretches[stretch].numberOfUses)
            ++numberOfStretches;
    }
    return numberOfStretches;
}

namespace {

struct PropertyRead {
    ValueID object;
    PropertyKeyID key;
    TypeID prediction;
    ValueID read;

    auto placeRead() const { return std::tuple { object, key, prediction }; }
};

} // anonymous namespace

// What flow could not follow is in a prediction beside whatever else reached the place. A part on which another access of the same value is not resolved is not
// expected: an index finds nothing in the slots of a layout.
static TypeID narrowPredictionByUses(FunctionPassContext& context, std::span<const ValueID> values, TypeID prediction)
{
    UnionParts parts = unionPartsOf(context.types.at(prediction));
    if (parts.size() < 2)
        return prediction;

    // One access alone contradicts nothing.
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(context, values, parts);
    if (evidence.numberOfResolvableAccesses < 2)
        return prediction;
    Type narrowed = Type::never();
    bool expectsObject = false;
    for (size_t part = 0; part < parts.size(); ++part) {
        bool servesEveryAccess = evidence.countOfPart[part] == evidence.numberOfResolvableAccesses;
        if (!parts[part].type.hasObjects() || servesEveryAccess)
            narrowed = narrowed.unionWith(parts[part].type, context.types);
        expectsObject |= parts[part].type.hasObjects() && servesEveryAccess;
    }
    return expectsObject ? context.types.intern(narrowed) : prediction;
}

// Of a value that nobody could follow, its uses are all there is to go by. What is read or stored under a number, and asked for nothing that an array does not have, is expected to be an array.
static TypeID predictArrayByUses(FunctionPassContext& context, std::span<const ValueID> values)
{
    const Function& function = context.function;
    bool isAccessedByKey = std::ranges::any_of(values, [&](ValueID id) {
        return std::ranges::any_of(context.analyses.useLists().usesOf(id), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && elementAccessOf(function.at(use.user).opcode);
        });
    });
    if (!isAccessedByKey)
        return { };
    UnionParts arrayAlone = unionPartsOf(Type::builtinClass(BuiltinClasses::Array).withoutClassProofs());
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(context, values, arrayAlone);
    if (!evidence.numberOfResolvableElementAccesses || evidence.numberOfResolvableAccesses < evidence.numberOfAccesses)
        return { };
    return context.types.intern(arrayAlone[0].type);
}

static bool settlePredictionOf(FunctionPassContext& context, std::span<const ValueID> values)
{
    Function& function = context.function;
    TypeID prediction = function.at(values[0]).prediction();
    TypeID settled = prediction ? narrowPredictionByUses(context, values, prediction) : predictArrayByUses(context, values);
    if (settled == prediction)
        return false;
    for (ValueID id : values)
        function.at(id).setPrediction(settled);
    return true;
}

static bool hasOpenPrediction(FunctionPassContext& context, ValueID id)
{
    const Value& value = context.function.at(id);
    if (!context.analyses.useLists().useCount(id) || value.prediction() == value.type)
        return false;
    return value.prediction() || context.types.at(value.type).anyObject;
}

// Every rule then tests a value for the same parts. A program reads `row.tags` again wherever it uses it: what is read from one object under one name is used as one value.
static PassOutcome settlePredictionsByUses(FunctionPassContext& context)
{
    if (!context.build.recordedOptions().usesPredictions())
        return PassOutcome::Unchanged;

    const Function& function = context.function;
    bool changed = predictLayoutsByNames(context);
    Vector<PropertyRead> reads;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        ValueID id(index);
        if (!hasOpenPrediction(context, id))
            continue;
        const Value& value = function.at(id);
        if (value.opcode == Opcode::PropGet)
            reads.append(PropertyRead { value.objectOperand(), value.key(), value.prediction(), id });
        else
            changed |= settlePredictionOf(context, singleElementSpan(id));
    }

    std::ranges::sort(reads, { }, [](const PropertyRead& read) {
        return std::tuple { read.object, read.key, read.prediction, read.read };
    });
    Vector<ValueID, 4> readsOfOnePlace;
    for (size_t first = 0; first < reads.size();) {
        readsOfOnePlace.shrink(0);
        size_t next = first;
        for (; next < reads.size() && reads[next].placeRead() == reads[first].placeRead(); ++next)
            readsOfOnePlace.append(reads[next].read);
        changed |= settlePredictionOf(context, readsOfOnePlace.span());
        first = next;
    }

    if (!changed)
        return PassOutcome::Unchanged;
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

PassOutcome placeTypeTests(FunctionPassContext& context)
{
    PassOutcome outcome = settlePredictionsByUses(context);
    TestPlacementPlan plan = planTestPlacement(context);
    if (!plan.hasGuardedStretch())
        return outcome;

    TestPlacer placer(context, plan);
    outcome |= placer.run();

    Statistics& statistics = context.build.statistics();
    statistics.count(StatisticID::StretchesGuarded, placer.numberOfGuardedStretches());
    statistics.count(StatisticID::StretchesNotClonable, placer.numberOfUnclonableStretches());
    statistics.count(StatisticID::EpochTestsInserted, placer.numberOfRenewals());
    statistics.count(StatisticID::TypeTestsPlaced, placer.numberOfTests());
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
