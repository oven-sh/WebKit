/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayCapacities.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include <algorithm>
#include <tuple>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constructionAllocations { Opcode::NewArrayLike, Opcode::NewArrayWithCapacity };

namespace {

struct LocatedPlan {
    const CapacityPlan* plan;
    ProgramPoint point;
    Vector<ValueID, 2> repeatedLengthReads;
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
        LocatedPlan entry { &plan, useLists.pointOf(plan.literal), { } };
        forEachTerm(plan, [&](ValueID term) {
            if (storageMeasuredBy(program, function.at(term)) && !analyses.dominates(term, entry.point))
                entry.repeatedLengthReads.append(term);
        });
        located.append(WTF::move(entry));
    }
    return located;
}

static ValueID materializeTerm(FunctionPassContext& context, const LocatedPlan& located, ValueID term, NOESCAPE const Invocable<ValueID(Value&&)> auto& insert)
{
    const Value& value = context.function.at(term);
    if (value.opcode == Opcode::AppendElements)
        return insert(IRBuilder::make(Opcode::GetArrayLength, { value.sourceOperand(), value.guardOperand() }));
    if (std::optional<Value> sizeRead = makeSizeReadOfWalkedCollection(context, value))
        return insert(WTF::move(*sizeRead));
    if (located.repeatedLengthReads.contains(term))
        return insert(Value(value));
    return term;
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
        forEachTerm(plan, [&](ValueID& term) {
            term = materializeTerm(context, located, term, insert);
        });

        ValueID sum = emitSum(plan, insert);
        if (!context.program.typeOf(function, sum).isInteger())
            continue;
        function.at(plan.literal).setCapacityOperand(sum);
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

static ValueID appendRemainingElementCount(IRBuilder& builder, const GrowthPlan& plan)
{
    const TripCount& count = plan.remainingIterations;
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

static ArrayStorage storageHolding(FunctionPassContext& context, std::span<const ValueID> stores)
{
    const Function& function = context.function;
    TypeID stored = function.at(function.at(stores.front()).valueOperand()).type;
    for (ValueID store : stores.subspan(1))
        stored = context.types.unionOf(stored, function.at(function.at(store).valueOperand()).type);
    return arrayStorageAtAllocation(context.types.at(stored));
}

unsigned chooseStorageOfConstructedArrays(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<ValueID, 2> allocations;
    HashMap<ValueID, Vector<ValueID, 2>> storesByAllocation;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (constructionAllocations.contains(value.opcode)) {
                allocations.append(id);
                continue;
            }
            if (value.opcode != Opcode::TryPutByValDirect)
                continue;
            ValueID array = context.analyses.sameObjectThroughCasts(value.arrayOperand());
            if (constructionAllocations.contains(function.at(array).opcode))
                storesByAllocation.add(array, Vector<ValueID, 2> { }).iterator->value.append(id);
        }
    }

    unsigned numberOfChangedAllocations = 0;
    for (ValueID allocation : allocations) {
        auto entry = storesByAllocation.find(allocation);
        if (entry == storesByAllocation.end() || function.at(allocation).arrayStorage() != ArrayStorage::Other)
            continue;
        ArrayStorage kind = storageHolding(context, entry->value.span());
        if (kind == ArrayStorage::Contiguous)
            continue;

        function.at(allocation).setArrayStorage(kind);
        for (ValueID store : entry->value)
            function.at(store).setArrayStorage(kind);
        ++numberOfChangedAllocations;
    }
    return numberOfChangedAllocations;
}

PassOutcome planArrayCapacities(FunctionPassContext& context)
{
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
    return narrowed || constructed || didInsertValues || grown ? PassOutcome::Changed : PassOutcome::Unchanged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
