/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTExpectedTypesByUse.h"
#include "AOTFlowResults.h"
#include "AOTHeat.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResolveAccesses.h"
#include "AOTSplitUnions.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

TestPlacer::TestPlacer(FunctionPassContext& context, const TestPlacementPlan& plan, const ExpectedTypesByUse& expectations)
    : m_context(context)
    , m_function(context.function)
    , m_plan(plan)
    , m_expectations(expectations)
    , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
    , m_definitions(plan.conditions->size())
{
    // In a very large program the copies of the loops of cold functions are megabytes, and no run shows the time that they save. In any other program a kernel that is taken for cold would lose half its speed.
    const Heat& heat = context.inputs.heat;
    if (isCold(heat.of(context.program, context.id)))
        m_stretchesInFrontOfLoops = heat.isOfVeryLargeProgram() ? BitVector(plan.stretches.size()) : findStretchesInFrontOfLoops();
}

// Those that lie in a loop, and those from which a loop can still be reached.
BitVector TestPlacer::findStretchesInFrontOfLoops() const
{
    const CFG& cfg = m_context.analyses.cfg();
    BitVector stretches(m_plan.stretches.size());
    if (!cfg.hasCycle())
        return stretches;
    const NaturalLoops& loops = m_context.analyses.naturalLoops();
    if (!loops.numLoops())
        return stretches;

    BitVector reachesLoop(m_function.blocks.size());
    Vector<BlockID, 16> worklist;
    for (BlockID block : cfg.reversePostOrder()) {
        if (loops.headerOf(block) && reachesLoop.add(block.index()))
            worklist.append(block);
    }
    while (!worklist.isEmpty()) {
        for (Edge edge : cfg.incomingEdges(worklist.takeLast())) {
            if (reachesLoop.add(edge.from.index()))
                worklist.append(edge.from);
        }
    }
    for (const StretchRange& range : m_plan.ranges) {
        if (reachesLoop.get(range.block.index()))
            stretches.quickSet(range.stretch);
    }
    return stretches;
}

PassOutcome TestPlacer::run()
{
    collectSubjectOperands();
    m_stretchesThatGuessLayouts = findStretchesThatGuessLayouts();

    auto blocksBeforeSplitting = m_function.blocks;
    size_t numberOfValuesBeforeSplitting = m_function.values.size();

    // A proof adopted on an edge that also carries tests has to become a parameter of the block the tests stand in.
    splitTestedEdges();
    adoptExistingProofs();
    collectCuts();
    splitBlocks();
    findRejoinBlocks();
    checkStretches();
    if (!numberOfGuardedStretches()) {
        // Left split, the function would be simplified, planned, split and rejected again in every round of the fixpoint.
        m_function.replaceBlocks(WTF::move(blocksBeforeSplitting));
        m_function.editAllValues().shrink(numberOfValuesBeforeSplitting);
        m_context.analyses.didChangeControlFlow();
        return PassOutcome::Unchanged;
    }

    cloneGenericCopies();
    redirectSlowPaths();
    emitTests();
    rewriteUses();
    if (m_copy)
        m_copy->addWaysBack(m_changedValues.span(), GenericCopy::TypesTestedOnWayBack::Expected);
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
    TypeID expected;
    ValueID read;

    auto placeRead() const { return std::tuple { object, key, expected }; }
};

} // anonymous namespace

// What nobody could follow is in an expected type beside whatever else reached the place.
static TypeID narrowExpectedTypeByUses(FunctionPassContext& context, std::span<const ValueID> values, TypeID expected)
{
    const Type& expectedType = context.types.at(expected);
    if (maxNumberOfUnionParts(expectedType) < 2)
        return expected;
    UnionParts parts = unionPartsOf(expectedType);
    // Only a part that has objects can go, and never the last one. undefined | null stands beside every reference type.
    auto hasObjects = [](const UnionPart& part) {
        return part.type.hasObjects();
    };
    if (std::ranges::count_if(parts, hasObjects) < 2)
        return expected;

    // One access alone contradicts nothing.
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(context, values, parts);
    if (evidence.numberOfResolvableAccesses < 2)
        return expected;
    Type narrowed = Type::never();
    bool expectsObject = false;
    for (size_t part = 0; part < parts.size(); ++part) {
        bool servesEveryAccess = evidence.countOfPart[part] == evidence.numberOfResolvableAccesses;
        if (!parts[part].type.hasObjects() || servesEveryAccess)
            narrowed = narrowed.unionWith(parts[part].type, context.types);
        expectsObject |= parts[part].type.hasObjects() && servesEveryAccess;
    }
    return expectsObject ? context.types.intern(narrowed) : expected;
}

// Of a value that nobody could follow, its uses are all there is to go by. What is read or stored under a number, and asked for nothing that an array does not have, is expected to be an array.
static TypeID expectArrayByUses(FunctionPassContext& context, std::span<const ValueID> values)
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

static void settleExpectedTypeOf(FunctionPassContext& context, std::span<const ValueID> values, ExpectedTypesByUse& expectations)
{
    TypeID expected = context.function.at(values[0]).expected();
    TypeID settled = expected ? narrowExpectedTypeByUses(context, values, expected) : expectArrayByUses(context, values);
    if (settled == expected)
        return;
    for (ValueID id : values)
        expectations.expect(id, settled);
}

static bool hasOpenExpectedType(FunctionPassContext& context, ValueID id)
{
    const Value& value = context.function.at(id);
    if (value.expected() == value.type)
        return false;
    if (!value.expected() && !context.types.at(value.type).anyObject)
        return false;
    return !!context.analyses.useLists().useCount(id);
}

// Every rule then tests a value for the same parts. A program reads `row.tags` again wherever it uses it: what is read from one object under one name is used as one value.
static void settleExpectedTypesByUses(FunctionPassContext& context, ExpectedTypesByUse& expectations)
{
    const Function& function = context.function;
    Vector<PropertyRead> reads;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        ValueID id(index);
        if (!hasOpenExpectedType(context, id))
            continue;
        const Value& value = function.at(id);
        if (value.opcode == Opcode::PropGet)
            reads.append(PropertyRead { value.objectOperand(), value.key(), value.expected(), id });
        else
            settleExpectedTypeOf(context, singleElementSpan(id), expectations);
    }

    std::ranges::sort(reads, { }, [](const PropertyRead& read) {
        return std::tuple { read.object, read.key, read.expected, read.read };
    });
    Vector<ValueID, 4> readsOfOnePlace;
    for (size_t first = 0; first < reads.size();) {
        readsOfOnePlace.shrink(0);
        size_t next = first;
        for (; next < reads.size() && reads[next].placeRead() == reads[first].placeRead(); ++next)
            readsOfOnePlace.append(reads[next].read);
        settleExpectedTypeOf(context, readsOfOnePlace.span(), expectations);
        first = next;
    }
}

static PassOutcome expectTypesByUses(FunctionPassContext& context, ExpectedTypesByUse& expectations)
{
    if (!context.build.recordedOptions().usesExpectedTypes())
        return PassOutcome::Unchanged;

    bool changedDeclaredType = expectLayoutsByNames(context, expectations);
    settleExpectedTypesByUses(context, expectations);
    if (!expectations.isEmpty())
        context.analyses.didChangeTypes();
    return changedDeclaredType ? PassOutcome::Changed : PassOutcome::Unchanged;
}

namespace {

// What flow has seen arrive at the value that a test looks at.
enum class ArrivalsAtTestedValue : uint8_t {
    // Flow keeps a record for the receiver of an access and for the callee of a call. The test looks at another value or at none, or a pass has made the access.
    NotRecorded,
    // No placeholder: the analysis has followed every object and every function there.
    Closed,
    // The unknown object or the untracked object.
    Open,
};

} // anonymous namespace

static ArrivalsAtTestedValue arrivalsBehind(UnprovenCause cause)
{
    switch (cause) {
    case UnprovenCause::NonLayoutObject:
    case UnprovenCause::LayoutOutsideFamily:
    case UnprovenCause::OtherNewTarget:
    case UnprovenCause::UnknownCallee:
        return ArrivalsAtTestedValue::Closed;
    case UnprovenCause::UnknownObject:
    case UnprovenCause::UndescribedNativeResult:
    case UnprovenCause::UnseenCode:
    case UnprovenCause::UncheckedConversionOfUntracked:
    case UnprovenCause::DataFromOutside:
    case UnprovenCause::ThrownByRunTime:
    case UnprovenCause::ParameterOfEscapedFunction:
    case UnprovenCause::ContentsOfEscapedObject:
    case UnprovenCause::ResultOfUnknownCallee:
    case UnprovenCause::ObjectWithoutLayout:
    case UnprovenCause::Untracked:
        return ArrivalsAtTestedValue::Open;
    default:
        return ArrivalsAtTestedValue::NotRecorded;
    }
}

static ArrivalsAtTestedValue arrivalsAtValueTestedBy(FunctionPassContext& context, const TestPlacementPlan& plan, const PlannedTest& planned)
{
    const TestRequirement& requirement = plan.requirements[planned.requirement];
    const Value& test = plan.sites.tests[requirement.test];
    const Value& use = context.function.at(plan.sites.sites[requirement.site].use);
    if (!test.numberOfOperands() || !PayloadWord::accessSite(use.opcode) || !use.accessSite())
        return ArrivalsAtTestedValue::NotRecorded;

    std::optional<unsigned> recordedOperand = OperandPosition::callee(use.opcode);
    if (std::optional<uint8_t> receiver = receiverOf(use.opcode))
        recordedOperand = *receiver;
    if (!recordedOperand || context.analyses.identityOf(test.operand(testedOperandIndex)) != context.analyses.identityOf(use.operand(*recordedOperand)))
        return ArrivalsAtTestedValue::NotRecorded;

    // Without a loss the stamp is complete.
    UnprovenCause cause = context.inputs.flow.lossOf(use.accessSite()).cause;
    if (cause != UnprovenCause::Unknown)
        return arrivalsBehind(cause);
    bool hasCompleteStamp = (PayloadWord::receivers(use.opcode) && use.receivers() && context.types.at(use.receivers()).isComplete)
        || (PayloadWord::callees(use.opcode) && use.callees() && context.types.at(use.callees()).isComplete);
    return hasCompleteStamp ? ArrivalsAtTestedValue::Closed : ArrivalsAtTestedValue::NotRecorded;
}

// No block runs more often than the function is entered unless it is in a cycle.
static bool everyBlockRunsAtMostOnce(FunctionPassContext& context)
{
    return context.inputs.heat.of(context.program, context.id) == HeatClass::RunsOnce && !context.analyses.cfg().hasCycle();
}

static PassOutcome placePlannedTests(FunctionPassContext& context, const ExpectedTypesByUse& expectations)
{
    TestPlacementPlan plan = planTestPlacement(context);
    if (!plan.hasGuardedStretch())
        return PassOutcome::Unchanged;
    if (context.build.options().logging.dumpedPass == PassID::PlaceTypeTests && context.build.shouldDump(context.subject().function)) [[unlikely]] {
        WTF::dataFile().println("AOT plan of PlaceTypeTests, in the numbers of the IR in front of the pass:");
        plan.dump(WTF::dataFile(), context.program);
    }

    // The placer changes the uses.
    Vector<ArrivalsAtTestedValue> arrivals(plan.tests.size(), [&](size_t index) {
        return arrivalsAtValueTestedBy(context, plan, plan.tests[index]);
    });
    TestPlacer placer(context, plan, expectations);
    PassOutcome outcome = placer.run();
    std::array<unsigned, 3> numberOfTestsByArrivals { };
    placer.forEachEmittedTest([&](uint32_t plannedTest) {
        ++numberOfTestsByArrivals[static_cast<size_t>(arrivals[plannedTest])];
    });

    Statistics& statistics = context.build.statistics();
    statistics.count(StatisticID::StretchesGuarded, placer.numberOfGuardedStretches());
    statistics.count(StatisticID::StretchesNotClonable, placer.numberOfUnclonableStretches());
    statistics.count(StatisticID::WaysBackFromStretchCopies, placer.numberOfWaysBack());
    statistics.count(StatisticID::TypeTestsPlaced, placer.numberOfTests());
    statistics.count(StatisticID::TypeTestsPlacedOnClosedSets, numberOfTestsByArrivals[static_cast<size_t>(ArrivalsAtTestedValue::Closed)]);
    statistics.count(StatisticID::TypeTestsPlacedOnOpenSets, numberOfTestsByArrivals[static_cast<size_t>(ArrivalsAtTestedValue::Open)]);
    return outcome;
}

static bool asksForElementOfArray(const FunctionPassContext& context, const Value& test)
{
    if (test.opcode == Opcode::HasArrayElement)
        return true;
    if (test.opcode != Opcode::ElemHas || !test.keyIsIntegral())
        return false;
    const ClassPart* receiver = context.program.typeOf(context.function, test.objectOperand()).singleBuiltinClass();
    return receiver && receiver->builtinClass == BuiltinClasses::Array;
}

// Arrays seldom have holes. The arm that a hole takes, in the loop of map() for one, is a slow path: what holds in front of the question holds behind it on the typed path, and the arm comes back through the tests.
static PassOutcome markArmsOfAbsentElements(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    const CFG& cfg = context.analyses.cfg();
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = context.function.at(id);
        if (block.frequency != BlockFrequency::Normal || !block.terminator.isConditionalBranch())
            continue;
        if (!asksForElementOfArray(context, context.function.at(block.terminator.conditionOperand())))
            continue;
        // An arm with code of its own is code of the program, which gets its tests as any other.
        const Block& absent = context.function.at(block.terminator.notTaken().block);
        if (absent.frequency != BlockFrequency::Normal || !absent.values.isEmpty() || absent.terminator.kind != TerminatorKind::Jump || cfg.incomingEdges(block.terminator.notTaken().block).size() != 1)
            continue;
        context.function.edit(block.terminator.notTaken().block, BlockEdit::Rest).frequency = BlockFrequency::Unlikely;
        outcome = PassOutcome::Changed;
    }
    if (outcome == PassOutcome::Changed)
        context.analyses.invalidate(FunctionAnalysis::Frequencies);
    return outcome;
}

// What a test in a cold function buys, it buys at run time: the passes of the rounds gain little from it, and they would carry its arm and its join: a twentieth of the build of a very large program.
static bool placesTestsBehindRounds(FunctionPassContext& context)
{
    const Heat& heat = context.inputs.heat;
    return heat.isOfVeryLargeProgram() && isCold(heat.of(context.program, context.id));
}

PassOutcome placeTypeTests(FunctionPassContext& context)
{
    bool hasUsesLeftByLastRound = std::exchange(context.hasUsesAwaitingExpansion, false);
    if (context.isBehindRounds ? !hasUsesLeftByLastRound && !placesTestsBehindRounds(context) : context.isInRounds && placesTestsBehindRounds(context))
        return PassOutcome::Unchanged;
    if (everyBlockRunsAtMostOnce(context))
        return PassOutcome::Unchanged;
    // Where a test at the entrance has failed. A test in here would guess again what has just proved wrong, and cost a copy.
    if (context.function.runsOnlyBehindFailedTest())
        return PassOutcome::Unchanged;

    PassOutcome marked = markArmsOfAbsentElements(context);
    ExpectedTypesByUse expectations(context.function);
    PassOutcome outcome = expectTypesByUses(context, expectations);
    outcome |= placePlannedTests(context, expectations);
    outcome |= addEmptySlotCases(context);
    if (outcome == PassOutcome::Unchanged && !expectations.isEmpty()) {
        expectations.takeBack();
        context.analyses.didChangeTypes();
    }
    // Whoever edits blocks, operands or edges here drops what that makes stale.
    context.keptAnalyses = outcome == PassOutcome::Unchanged ? allAnalyses - FunctionAnalysis::Effects : controlFlowAnalyses | analysesThatReadNoTypes;
    return outcome | marked;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
