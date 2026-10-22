/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayCapacities.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t notTakenTargetIndex = 1;

static constexpr Effects effectsOfConversion = declaredBoundOf(Opcode::ConvertToDoubleStorage);

static constexpr Effects effectsOnConvertedArray = [] {
    Effects effects = Effects::none();
    effects.reads = effectsOfConversion.reads.restrictedTo(perObjectHeaps);
    effects.writes = effectsOfConversion.writes.restrictedTo(perObjectHeaps);
    return effects;
}();

static constexpr Effects effectsBesideConvertedArray = [] {
    Effects effects = effectsOfConversion;
    effects.writes.remove(perObjectHeaps);
    return effects;
}();

namespace {

struct LocatedPlan {
    const CapacityPlan* plan;
    ProgramPoint point;
    Vector<ValueID, 2> repeatedLengthReads;
    // Those whose test for null and undefined is repeated with them.
    Vector<ValueID, 2> repeatedChecks;
};

struct FailedStorageTest {
    Edge edge;
    ValueID array;
    Origin origin;
};

struct PlacedConversion {
    Edge edge;
    Target targetBeforeSplit;
    ProgramPoint point;
    bool mayEndProof { false };
};

} // anonymous namespace

template<typename Plan, typename Functor>
static void forEachTerm(Plan& plan, NOESCAPE const Functor& functor)
{
    for (auto& term : plan.added)
        functor(term);
    for (auto& term : plan.subtracted)
        functor(term);
    for (auto& term : plan.roundedUp)
        functor(term);
}

static ValueID emitSum(const CapacityPlan& plan, NOESCAPE const Invocable<ValueID(Value&&)> auto& emitValue)
{
    ValueID sum = emitValue(makeInteger(plan.constant));
    for (ValueID term : plan.added)
        sum = emitValue(IRBuilder::make(Opcode::Add, { sum, term }));
    for (ValueID term : plan.subtracted)
        sum = emitValue(IRBuilder::make(Opcode::Sub, { sum, term }));
    for (ValueID term : plan.roundedUp) {
        ValueID zero = emitValue(makeInteger(0));
        ValueID truncated = emitValue(IRBuilder::make(Opcode::BitOr, { term, zero }));
        ValueID one = emitValue(makeInteger(1));
        ValueID roundedUp = emitValue(IRBuilder::make(Opcode::Add, { truncated, one }));
        sum = emitValue(IRBuilder::make(Opcode::Add, { sum, roundedUp }));
    }
    return sum;
}

// Every insertion invalidates the use lists, so all questions about dominance are answered before the first one.
static Vector<LocatedPlan, 4> locate(const Program& program, FunctionAnalyses& analyses, std::span<const CapacityPlan> plans)
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    Vector<LocatedPlan, 4> located;
    located.reserveInitialCapacity(plans.size());
    for (const CapacityPlan& plan : plans) {
        LocatedPlan entry { &plan, useLists.pointOf(plan.literal), { }, { } };
        forEachTerm(plan, [&](ValueID term) {
            std::optional<MeasuredStorage> measured = storageMeasuredBy(program, function.at(term));
            if (!measured || analyses.dominates(term, entry.point))
                return;
            entry.repeatedLengthReads.append(term);
            if (!analyses.dominates(measured->owner, entry.point))
                entry.repeatedChecks.append(term);
        });
        located.append(WTF::move(entry));
    }
    return located;
}

static ValueID materializeTerm(FunctionPassContext& context, const LocatedPlan& located, ValueID term, NOESCAPE const Invocable<ValueID(Value&&)> auto& insert, NOESCAPE const Invocable<ValueID(ValueID)> auto& repeatWhereItThrows)
{
    const Value& value = context.function.at(term);
    if (value.opcode == Opcode::AppendElements)
        return insert(IRBuilder::make(Opcode::GetArrayLength, { value.sourceOperand(), value.guardOperand() }));
    if (std::optional<Value> sizeRead = makeSizeReadOfWalkedCollection(context, value))
        return insert(WTF::move(*sizeRead));
    if (!located.repeatedLengthReads.contains(term))
        return term;
    Value read = value;
    if (located.repeatedChecks.contains(term)) {
        ValueID check = checkInFrontOf(context.function, *storageMeasuredBy(context.program, value));
        ValueID repeatedCheck = repeatWhereItThrows(check);
        read.forEachChild([&](ValueID& operand) {
            if (operand == check)
                operand = repeatedCheck;
        });
    }
    return insert(WTF::move(read));
}

unsigned applyCapacityPlans(FunctionPassContext& context, std::span<const CapacityPlan> plans)
{
    if (plans.empty())
        return 0;

    Function& function = context.function;
    Vector<LocatedPlan, 4> locatedPlans = locate(context.program, context.analyses, plans);
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    unsigned numberOfPlannedLiterals = 0;
    for (const LocatedPlan& located : locatedPlans) {
        CapacityPlan plan = *located.plan;
        builder.setOrigin(function.at(plan.literal).origin());
        auto insert = [&](Value&& value) {
            return builder.insertBefore(located.point, WTF::move(value));
        };
        // The error names the place of the test that the program has.
        auto repeatWhereItThrows = [&](ValueID check) {
            builder.setOrigin(function.at(check).origin());
            ValueID repeated = insert(Value(function.at(check)));
            builder.setOrigin(function.at(plan.literal).origin());
            return repeated;
        };
        forEachTerm(plan, [&](ValueID& term) {
            term = materializeTerm(context, located, term, insert, repeatWhereItThrows);
        });

        ValueID sum = emitSum(plan, insert);
        if (!context.program.typeOf(function, sum).isInteger())
            continue;
        function.edit(plan.literal, ValueEdit::Definition).setCapacityOperand(sum);
        ++numberOfPlannedLiterals;
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return numberOfPlannedLiterals;
}

static ValueID appendCopy(IRBuilder& builder, const Value& original)
{
    if (resultRuleOf(original.opcode) == ResultRule::Declared)
        return builder.appendWithType(Value(original), original.type);
    return builder.append(Value(original));
}

static ValueID appendTerm(IRBuilder& builder, const CapacityTerm& term)
{
    RELEASE_ASSERT(term.kind != CapacityTermKind::Unknown);
    return term.kind == CapacityTermKind::Constant ? builder.appendInteger(term.constant) : term.value;
}

static ValueID appendElementCount(IRBuilder& builder, const GrowthPlan& plan, const TripCount& count)
{
    ValueID iterations = builder.append(Opcode::Sub, { appendTerm(builder, count.added), appendTerm(builder, count.subtracted) });
    int64_t adjustment = count.adjustment;
    if (count.added.kind == CapacityTermKind::Number || count.subtracted.kind == CapacityTermKind::Number) {
        iterations = builder.append(Opcode::BitOr, { iterations, builder.appendInteger(0) });
        ++adjustment;
    }
    if (adjustment)
        iterations = builder.append(Opcode::Add, { iterations, builder.appendInteger(adjustment) });
    if (plan.elementsPerIteration > 1)
        iterations = builder.append(Opcode::Mul, { iterations, builder.appendInteger(plan.elementsPerIteration) });
    return iterations;
}

static ValueID appendRemainingElementCount(IRBuilder& builder, const GrowthPlan& plan)
{
    return appendElementCount(builder, plan, plan.remainingIterations);
}

// The append on the taken edge repeats the test, which B3 folds, so an array with spare capacity pays nothing.
unsigned applyGrowthPlans(FunctionPassContext& context, std::span<const GrowthPlan> plans)
{
    if (plans.empty())
        return 0;

    Function& function = context.function;
    Vector<std::pair<ProgramPoint, const GrowthPlan*>, 4> locatedPlans;
    for (const GrowthPlan& plan : plans)
        locatedPlans.append({ context.analyses.useLists().pointOf(plan.append), &plan });
    // An expansion moves only the values after its point.
    std::ranges::sort(locatedPlans, [](const auto& a, const auto& b) {
        return std::tie(a.first.block, a.first.index) > std::tie(b.first.block, b.first.index);
    });

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const auto& [point, plan] : locatedPlans) {
        Value append = function.at(plan->append);
        BlockFrequency frequency = function.at(point.block).frequency;
        builder.setOrigin(append.origin());
        IRBuilder::Expansion expansion = builder.beginExpansion(point);
        auto appendAndRejoin = [&] {
            ValueID result = appendCopy(builder, append);
            if (expansion.results.isEmpty())
                builder.endExpansion(expansion, std::span<const ValueID> { });
            else
                builder.endExpansion(expansion, result);
        };

        ValueID hasSpareCapacity = builder.append(Opcode::HasSpareArrayCapacity, { plan->array });
        IRBuilder::BranchBlocks blocks = builder.emitBranch(hasSpareCapacity, frequency, BlockFrequency::Rare);
        builder.moveToEndOf(blocks.taken);
        appendAndRejoin();

        builder.moveToEndOf(blocks.notTaken);
        builder.append(Opcode::ReserveArrayCapacity, { plan->array, appendRemainingElementCount(builder, *plan) });
        appendAndRejoin();
    }
    return plans.size();
}

static ArrayStorage storageHolding(FunctionPassContext& context, const Value& allocation, std::span<const ValueID> stores)
{
    const Function& function = context.function;
    TypeID stored = function.at(function.at(stores.front()).valueOperand()).type;
    for (ValueID store : stores.subspan(1))
        stored = context.types.unionOf(stored, function.at(function.at(store).valueOperand()).type);
    ArrayStorage kind = arrayStorageAtAllocation(context.types.at(stored));
    if (kind != ArrayStorage::Int32 || !allocation.expected())
        return kind;
    // The readers test for the kind that the declared elements give the array.
    ArrayStorage declared = arrayStorageDeclaredBy(context.program, context.types.at(allocation.expected()));
    return declared == ArrayStorage::Double ? declared : kind;
}

static bool namesStorageOfArray(Opcode opcode)
{
    switch (opcode) {
    case Opcode::GetArrayElement:
    case Opcode::HasArrayElement:
    case Opcode::IsElementPresent:
        return true;
    default:
        return false;
    }
}

static BitVector findAllocationsReadAsDoubles(FunctionPassContext& context)
{
    const Function& function = context.function;
    BitVector allocations(function.values.size());
    const EscapeAnalysis& escape = context.analyses.escape();
    if (escape.gaveUp())
        return allocations;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& test = function.at(id);
            if (test.opcode != Opcode::HasArrayStorage || test.arrayStorage() != ArrayStorage::Double)
                continue;
            for (ObjectID object : escape.pointsTo(test.arrayOperand()).objects) {
                if (escape.at(object).kind == ObjectKind::Allocation)
                    allocations.quickSet(escape.at(object).definition.index());
            }
        }
    }
    return allocations;
}

unsigned chooseStorageOfConstructedArrays(FunctionPassContext& context)
{
    struct Uses {
        Vector<ValueID, 2> directStores;
        Vector<ValueID, 2> testedStores;
        Vector<ValueID, 2> tests;
        bool hasOtherUseOfStorage { false };
    };

    Function& function = context.function;
    Vector<ValueID, 2> allocations;
    HashMap<ValueID, Uses> usesByAllocation;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (constructionAllocations.contains(value.opcode)) {
                allocations.append(id);
                continue;
            }
            bool isTestedUse = value.opcode == Opcode::PutArrayElement || value.opcode == Opcode::HasArrayStorage || namesStorageOfArray(value.opcode);
            if (value.opcode != Opcode::TryPutByValDirect && !isTestedUse)
                continue;
            ValueID array = context.analyses.sameObjectThroughCasts(value.arrayOperand());
            if (!constructionAllocations.contains(function.at(array).opcode))
                continue;
            Uses& uses = usesByAllocation.add(array, Uses { }).iterator->value;
            if (value.opcode == Opcode::TryPutByValDirect)
                uses.directStores.append(id);
            else if (value.opcode != Opcode::PutArrayElement && value.arrayStorage() == ArrayStorage::Double)
                continue;
            else if (namesStorageOfArray(value.opcode) || value.arrayStorage() != ArrayStorage::Contiguous)
                uses.hasOtherUseOfStorage = true;
            else if (value.opcode == Opcode::PutArrayElement)
                uses.testedStores.append(id);
            else
                uses.tests.append(id);
        }
    }

    std::optional<BitVector> readAsDoubles;
    unsigned numberOfChangedAllocations = 0;
    for (ValueID allocation : allocations) {
        auto entry = usesByAllocation.find(allocation);
        if (entry == usesByAllocation.end() || function.at(allocation).arrayStorage() != ArrayStorage::Other)
            continue;
        Uses& uses = entry->value;
        if (uses.directStores.isEmpty()) {
            if (uses.testedStores.isEmpty() || uses.hasOtherUseOfStorage)
                continue;
            bool holdsDoubles = std::ranges::all_of(uses.testedStores, [&](ValueID store) {
                return canStoreWithoutConversion(ArrayStorage::Double, context.types.at(function.at(function.at(store).valueOperand()).type));
            });
            if (!holdsDoubles)
                continue;
            if (!readAsDoubles)
                readAsDoubles = findAllocationsReadAsDoubles(context);
            if (!readAsDoubles->quickGet(allocation.index()))
                continue;
            function.edit(allocation, ValueEdit::Definition).setArrayStorage(ArrayStorage::Double);
            for (ValueID store : uses.testedStores)
                function.edit(store, ValueEdit::Definition).setArrayStorage(ArrayStorage::Double);
            for (ValueID test : uses.tests)
                function.edit(test, ValueEdit::Definition).setArrayStorage(ArrayStorage::Double);
            ++numberOfChangedAllocations;
            continue;
        }
        ArrayStorage kind = storageHolding(context, function.at(allocation), uses.directStores.span());
        if (kind == ArrayStorage::Contiguous)
            continue;

        function.edit(allocation, ValueEdit::Definition).setArrayStorage(kind);
        for (ValueID store : uses.directStores)
            function.edit(store, ValueEdit::Definition).setArrayStorage(kind);
        ++numberOfChangedAllocations;
    }
    return numberOfChangedAllocations;
}

static bool maySuspend(const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return maySuspend(function.at(id).opcode);
        });
    });
}

static bool allowsOnEveryExit(const FunctionSummaryView& summary, const Effects& effects)
{
    EffectsOnEdges allowed = summary.elsewhere();
    return effects.isSubsetOf(allowed.normal) && (!allowed.mayThrow() || effects.isSubsetOf(allowed.handler));
}

// A caller keeps what it has proven about the storage of an array across a call by the summary of the callee, and the summaries were made of the bodies without the conversions.
static bool summaryAllowsConversion(FunctionPassContext& context, const FunctionSummaryView& summary, bool functionMaySuspend, ValueID array)
{
    if (!allowsOnEveryExit(summary, effectsBesideConvertedArray))
        return false;

    bool isAllowed = true;
    forEachBaseObject(context.analyses, array, [&](const BaseObject& object) {
        switch (object.kind) {
        case BaseObject::Kind::OwnAllocation:
            // A saved frame holds the array, and the function goes on when its caller has long gone on.
            isAllowed = isAllowed && (!functionMaySuspend || allowsOnEveryExit(summary, effectsOnConvertedArray));
            return;
        case BaseObject::Kind::EntryParameter: {
            OperandMemory memory = summary.memory(object.entryParameter);
            isAllowed = isAllowed && effectsOnConvertedArray.reads.isSubsetOf(memory.reads) && effectsOnConvertedArray.writes.isSubsetOf(memory.writes);
            return;
        }
        case BaseObject::Kind::Untracked:
            isAllowed = isAllowed && allowsOnEveryExit(summary, effectsOnConvertedArray);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    return isAllowed;
}

static bool convertsAtHead(const Function& function, BlockID block, ValueID array)
{
    const BlockValues& values = function.at(block).values;
    if (values.isEmpty())
        return false;
    const Value& first = function.at(values.first());
    return first.opcode == Opcode::ConvertToDoubleStorage && first.arrayOperand() == array;
}

// Whoever goes on to ask for another kind reads the array as it is.
static bool asksForStorageOf(const Function& function, BlockID block, ValueID array)
{
    const Terminator& terminator = function.at(block).terminator;
    if (!terminator.isConditionalBranch())
        return false;
    const Value& test = function.at(terminator.conditionOperand());
    return test.opcode == Opcode::HasArrayStorage && test.arrayOperand() == array;
}

static Vector<FailedStorageTest, 4> collectFailedTestsForDoubleStorage(FunctionPassContext& context)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    CalleeSummary summary(analyses.effectContext(), context.id);
    bool functionMaySuspend = maySuspend(function);

    Vector<FailedStorageTest, 4> failedTests;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        if (test.opcode != Opcode::HasArrayStorage || test.arrayStorage() != ArrayStorage::Double)
            continue;
        ValueID array = test.arrayOperand();
        if (convertsAtHead(function, terminator.notTaken().block, array) || asksForStorageOf(function, terminator.notTaken().block, array) || !summaryAllowsConversion(context, *summary, functionMaySuspend, array))
            continue;
        failedTests.append(FailedStorageTest { Edge { block, notTakenTargetIndex }, array, test.origin() });
    }
    return failedTests;
}

// The code behind a failing edge is generic as a rule and relies on no proof about an array.
static void findConversionsEndingProofs(FunctionPassContext& context, Vector<PlacedConversion, 4>& conversions)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    const GuardAvailability& guards = analyses.guards();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        std::optional<GuardAvailability::Cursor> cursor;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& user = function.at(values[index]);
            if (reliesOf(user.opcode).isEmpty() || proofsNeededFromGuard(context.program, function, user).isEmpty())
                continue;

            for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                ValueID guard = user.operand(operandIndex);
                if (user.operandKind(operandIndex) != OperandKind::Guard || !guard)
                    continue;
                if (!cursor)
                    cursor.emplace(guards, block);
                cursor->advanceTo(index);
                if (cursor->stateOf(guard, user) == GuardState::Available)
                    continue;
                for (PlacedConversion& conversion : conversions)
                    conversion.mayEndProof = conversion.mayEndProof || guards.mayBeEndedAt(guard, user, conversion.point);
            }
        }
    }
}

PassOutcome convertIntegerArraysBehindFailedTests(FunctionPassContext& context)
{
    Function& function = context.function;
    if (!context.analyses.opcodesOfValues().contains(Opcode::HasArrayStorage))
        return PassOutcome::Unchanged;

    // Splitting an edge invalidates the order of the blocks and what is known about the arrays, so every test is found before the first split.
    Vector<FailedStorageTest, 4> failedTests = collectFailedTestsForDoubleStorage(context);
    if (failedTests.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<PlacedConversion, 4> conversions;
    conversions.reserveInitialCapacity(failedTests.size());
    for (const FailedStorageTest& failed : failedTests) {
        Target targetBeforeSplit = targetOf(function, failed.edge);
        BlockID repair = builder.splitEdge(failed.edge);
        Block& block = function.edit(repair);
        block.frequency = rarerOf(block.frequency, BlockFrequency::Unlikely);
        context.analyses.invalidate(FunctionAnalysis::Frequencies);
        conversions.append(PlacedConversion { failed.edge, WTF::move(targetBeforeSplit), ProgramPoint::atHeadOf(repair) });
    }
    for (size_t index = 0; index < failedTests.size(); ++index) {
        builder.setOrigin(failedTests[index].origin);
        builder.insertBefore(conversions[index].point, IRBuilder::make(Opcode::ConvertToDoubleStorage, { failedTests[index].array }));
    }
    builder.applyInsertions();

    findConversionsEndingProofs(context, conversions);
    unsigned numberOfConversions = 0;
    for (PlacedConversion& conversion : conversions) {
        if (conversion.mayEndProof)
            builder.redirectEdge(conversion.edge, WTF::move(conversion.targetBeforeSplit));
        else
            ++numberOfConversions;
    }
    if (numberOfConversions < conversions.size())
        builder.emptyUnreachableBlocks();
    if (numberOfConversions)
        context.build.statistics().count(StatisticID::IntegerArrayConversionsPlaced, numberOfConversions);
    // An edge that got its target back has left an empty block behind.
    return PassOutcome::Changed;
}

unsigned applyCollectionGrowthPlans(FunctionPassContext& context, std::span<const GrowthPlan> plans)
{
    if (plans.empty())
        return 0;

    struct LocatedGrowth {
        ProgramPoint point;
        const GrowthPlan* plan;
        ValueID collection;
        ValueID sizeAtEntry;
    };
    Function& function = context.function;
    Vector<LocatedGrowth, 4> located;
    for (const GrowthPlan& plan : plans)
        located.append({ context.analyses.useLists().pointOf(plan.append), &plan, context.analyses.identityOf(plan.array), { } });
    // An expansion moves only the values after its point.
    std::ranges::sort(located, [](const LocatedGrowth& a, const LocatedGrowth& b) {
        return std::tie(a.point.block, a.point.index) > std::tie(b.point.block, b.point.index);
    });

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    // One read for each loop and collection, at the end of the block in front of the loop, where it moves no point.
    for (size_t index = 0; index < located.size(); ++index) {
        LocatedGrowth& growth = located[index];
        for (size_t earlier = 0; earlier < index && !growth.sizeAtEntry; ++earlier) {
            if (located[earlier].plan->loopHeader == growth.plan->loopHeader && located[earlier].collection == growth.collection)
                growth.sizeAtEntry = located[earlier].sizeAtEntry;
        }
        if (growth.sizeAtEntry)
            continue;
        bool isMap = inlineImplementationOf(context.program, function.at(growth.plan->append)) == InlineBuiltin::MapSet;
        std::optional<Value> sizeRead = makeSizeRead(context, growth.plan->array, isMap ? Builtins::MapPrototypeSize : Builtins::SetPrototypeSize);
        BlockID preheader = growth.plan->preheader;
        builder.setOrigin(function.at(preheader).terminator.origin());
        growth.sizeAtEntry = builder.insertBefore({ preheader, static_cast<uint32_t>(function.at(preheader).values.size()) }, WTF::move(*sizeRead));
    }
    builder.applyInsertions();

    for (const LocatedGrowth& growth : located) {
        const GrowthPlan& plan = *growth.plan;
        Value addition = function.at(plan.append);
        BlockFrequency frequency = function.at(growth.point.block).frequency;
        builder.setOrigin(addition.origin());
        IRBuilder::Expansion expansion = builder.beginExpansion(growth.point);
        BlockID adds = builder.addBlock(frequency);
        BlockID grows = builder.addBlock(BlockFrequency::Unlikely);
        builder.emitBranch(builder.append(Opcode::IsCollectionTableFull, { plan.array }), Target { grows, { }, { } }, Target { adds, { }, { } });

        builder.moveToEndOf(grows);
        ValueID remaining = appendElementCount(builder, plan, plan.remainingIterations);
        ValueID finished = appendElementCount(builder, plan, plan.finishedIterations);
        builder.append(Opcode::ReserveCollectionCapacity, { plan.array, remaining, finished, growth.sizeAtEntry });
        builder.emitJump(adds);

        builder.moveToEndOf(adds);
        ValueID result = appendCopy(builder, addition);
        if (expansion.results.isEmpty())
            builder.endExpansion(expansion, std::span<const ValueID> { });
        else
            builder.endExpansion(expansion, result);
    }
    return plans.size();
}

PassOutcome planCollectionCapacities(FunctionPassContext& context)
{
    Vector<GrowthPlan, 4> plans = ArrayCapacityPlanner(context).collectCollectionGrowthPlans();
    unsigned planned = applyCollectionGrowthPlans(context, plans.span());
    if (!planned)
        return PassOutcome::Unchanged;
    context.build.statistics().count(StatisticID::CollectionGrowthsPlanned, planned);
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

PassOutcome planArrayCapacities(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().intersects(arrayCapacityOpcodes))
        return PassOutcome::Unchanged;

    PassOutcome converted = convertIntegerArraysBehindFailedTests(context);
    unsigned narrowed = narrowStorageOfLocalArrays(context);
    unsigned constructed = chooseStorageOfConstructedArrays(context);
    Vector<CapacityPlan, 4> plans = ArrayCapacityPlanner(context).collectPlans();
    unsigned planned = applyCapacityPlans(context, plans.span());
    if (planned)
        context.build.statistics().count(StatisticID::ArrayCapacitiesPlanned, planned);
    bool didInsertValues = !plans.isEmpty();

    Vector<GrowthPlan, 4> growthPlans = ArrayCapacityPlanner(context).collectGrowthPlans();
    unsigned grown = applyGrowthPlans(context, growthPlans.span());
    if (grown)
        context.build.statistics().count(StatisticID::ArrayGrowthsPlanned, grown);
    if (converted == PassOutcome::Unchanged && !narrowed && !constructed && !didInsertValues && !grown)
        return PassOutcome::Unchanged;

    if (constructed)
        context.analyses.invalidate(valueAnalyses - analysesThatReadNoTypes);
    context.keptAnalyses = allAnalyses - FunctionAnalysis::Effects;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
