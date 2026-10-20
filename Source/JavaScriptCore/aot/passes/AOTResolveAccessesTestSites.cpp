/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTResolveAccesses.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TestedAccess {
    PropertyAccessPlan plan;
    SlotStamp stamp;
    AccessTests tests;
};

} // anonymous namespace

static PropertyAccessPlan planWithoutCalls(FunctionPassContext& context, PropertyAccessQuery& query)
{
    query.isOnlyCalled = false;
    query.maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    return planPropertyAccess({ context.program, &context.inputs.flow }, query);
}

static const Value* findFilledSlotTest(std::span<const Value> tests)
{
    auto found = std::ranges::find(tests, Opcode::IsSlotFilled, &Value::opcode);
    return found == tests.end() ? nullptr : &*found;
}

static std::optional<TestedAccess> planAccessUnderTests(TestSiteReporter& reporter, const Value& access, PropertyAccessQuery& query)
{
    FunctionPassContext& context = reporter.context();
    AccessOperands operands = operandsOfNamedAccess(access);
    while (true) {
        TestedAccess tested;
        tested.plan = planWithoutCalls(context, query);
        if (!tested.plan.isResolved() || tested.plan.needsNoTest())
            return std::nullopt;

        tested.stamp = createSlotStamp(context, access, tested.plan);
        tested.tests = createAccessTests(context, tested.plan, { operands.receiver, operands.stored, tested.stamp });
        if (tested.tests.tests.isEmpty())
            return std::nullopt;

        const Value* receiverTest = tested.tests.findReceiverLayoutTest();
        if (receiverTest && reporter.findEarlierTest(*receiverTest) == EarlierTest::Failed) {
            size_t numberOfPredictedMembers = query.receivers.predicted.size();
            query.receivers.predicted.subtract(tested.plan.receivers);
            RELEASE_ASSERT(query.receivers.predicted.size() < numberOfPredictedMembers);
            continue;
        }

        const Value* filledSlotTest = findFilledSlotTest(tested.tests.tests.span());
        if (filledSlotTest && query.access == Access::Store && !query.expectsEmptySlot && reporter.findEarlierTest(*filledSlotTest) == EarlierTest::Failed) {
            query.expectsEmptySlot = true;
            continue;
        }

        if (tested.plan.form == AccessForm::Call)
            return std::nullopt;
        return tested;
    }
}

void reportPropertyAccessTests(TestSiteReporter& reporter)
{
    reporter.forEachCandidate([&](const Value& access) {
        std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(reporter.program(), reporter.function(), access);
        if (!query)
            return;
        std::optional<TestedAccess> tested = planAccessUnderTests(reporter, access, *query);
        if (!tested)
            return;
        reporter.report(tested->tests.tests.span(), effectsAfterRewrite(reporter.context(), tested->plan, tested->stamp));
    });
}

// A test of the stored value may name layouts too, and it says nothing about the receiver.
static std::span<const Value> testsWithoutStoredValueTest(const TestSiteRewriter& rewriter, ValueID stored)
{
    std::span<const Value> tests = rewriter.tests();
    if (!stored || tests.empty())
        return tests;
    uint32_t last = static_cast<uint32_t>(tests.size() - 1);
    bool lastTestsStoredValue = tests[last].opcode == Opcode::TypeTest && rewriter.proofOf(last) == stored;
    return lastTestsStoredValue ? tests.first(last) : tests;
}

#if ASSERT_ENABLED
static bool isObjectTestOf(const Value& test, Proof proof, PropertyKeyID key)
{
    if (provesOf(test.opcode) != proof || !OperandPosition::object(test.opcode))
        return false;
    return proof != Proof::SlotFilled || test.key() == key;
}

static bool isConditionTestOf(const Value& test, const MemberCondition& condition)
{
    Value expected = makeConditionTest(condition);
    return test.opcode == expected.opcode && test.hasSamePayloadAs(expected);
}

static bool testsCoverPlan(std::span<const Value> tests, const PropertyAccessPlan& plan, PropertyKeyID key)
{
    bool coverProofs = std::ranges::all_of(allMembersOf<Proof>, [&](Proof proof) {
        return !plan.requiredProofs.contains(proof) || std::ranges::any_of(tests, [&](const Value& test) {
            return isObjectTestOf(test, proof, key);
        });
    });
    bool coverConditions = std::ranges::all_of(plan.conditions, [&](const MemberCondition& condition) {
        return std::ranges::any_of(tests, [&](const Value& test) {
            return isConditionTestOf(test, condition);
        });
    });
    return coverProofs && coverConditions;
}
#endif

static TypeID writtenTypeOfEntryParameter(const Function& function, ValueID value)
{
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    unsigned count = std::min<unsigned>(function.numberOfEntryParameters(), parameters.size());
    for (unsigned index = 0; index < count; ++index) {
        if (parameters[index] != value)
            continue;
        const DeclaredType* declared = function.declaredEntryParameter(index);
        return declared ? declared->written : TypeID();
    }
    return { };
}

// The values of a function keep no written types once its requests are resolved. The position a value was read from still has one.
static bool areWrittenLayoutsOf(FunctionPassContext& context, ValueID receiver, const LayoutList& layouts)
{
    const Program& program = context.program;
    const Function& function = context.function;
    auto writtenLayoutsAt = [&](ValueID id) -> std::optional<std::span<const LayoutID>> {
        const Value& value = function.at(id);
        TypeID written = value.opcode == Opcode::Parameter ? writtenTypeOfEntryParameter(function, id) : writtenTypeOf(program, function, value, context.types);
        if (!written || !context.types.at(written).layouts)
            return std::nullopt;
        return membersOf(program, context.types.at(written));
    };
    auto areAmong = [&](std::span<const LayoutID> writtenLayouts) {
        return std::ranges::all_of(layouts, [&](LayoutID layout) {
            return std::ranges::binary_search(writtenLayouts, layout);
        });
    };

    for (ValueID id : CastChain(function, receiver)) {
        if (auto writtenLayouts = writtenLayoutsAt(id))
            return areAmong(*writtenLayouts);
    }
    // The receiver behind a test is what the test was asked about.
    auto writtenLayouts = writtenLayoutsAt(context.analyses.identities().identityOf(receiver));
    return writtenLayouts && areAmong(*writtenLayouts);
}

// A permanent test gives the receiver its layouts for good. Where the program wrote those layouts, the access is as the written type says.
static KnowledgeLevel levelUnderTests(FunctionPassContext& context, const AccessOperands& operands, const PropertyAccessPlan& plan)
{
    if (operands.guard || !areWrittenLayoutsOf(context, operands.receiver, plan.receivers))
        return KnowledgeLevel::Predicted;
    return KnowledgeLevel::Written;
}

void rewriteTestedPropertyAccess(TestSiteRewriter& rewriter)
{
    FunctionPassContext& context = rewriter.context();
    const Program& program = rewriter.program();
    const Value& access = rewriter.useValue();

    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(program, rewriter.function(), access);
    RELEASE_ASSERT(query);

    AccessOperands operands = operandsOfNamedAccess(access);
    operands.guard = rewriter.guard();

    if (std::optional<ReceiverCandidates> established = receiversEstablishedBy(program, testsWithoutStoredValueTest(rewriter, operands.stored)))
        query->receivers = WTF::move(*established);
    query->expectsEmptySlot = query->access == Access::Store && !findFilledSlotTest(rewriter.tests());

    PropertyAccessPlan plan = planWithoutCalls(context, *query);
    RELEASE_ASSERT(plan.isResolved());
    RELEASE_ASSERT(plan.receiverTest == ReceiverTest::None);
    RELEASE_ASSERT(!plan.stores() || plan.storedValueFits);
    ASSERT(testsCoverPlan(rewriter.tests(), plan, query->key));
    plan.level = levelUnderTests(context, operands, plan);

    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeUse = [&](auto&& value) -> ValueID {
        return rewriter.insertBeforeUse(WTF::move(value));
    };
    InsertValue insert(insertBeforeUse);
    ProgramPoint point = rewriter.point();
    NamedAccessSite site { { point.block, point.index, rewriter.use() }, WTF::move(*query), WTF::move(plan) };
    AccessRewriter(context, insert).rewrite(site, operands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
