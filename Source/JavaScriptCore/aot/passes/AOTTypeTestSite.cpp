/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTTypeTestConditions.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::TypeTest::value == testedOperandIndex);
static_assert(OperandIndex::ShapeTest::object == testedOperandIndex);
static_assert(OperandIndex::IsPristine::value == testedOperandIndex);
static_assert(OperandIndex::HasArrayStorage::array == testedOperandIndex);
static_assert(OperandIndex::IsClosureOf::value == testedOperandIndex);
static_assert(OperandIndex::IsEnumeratorCurrent::enumerator == testedOperandIndex);

TestSiteReporter::TestSiteReporter(FunctionPassContext& context)
    : m_context(context)
    , m_isClaimed(context.function.values.size())
    , m_awaitsExpansion(context.function.values.size())
{
    findUsesAwaitingExpansion();
}

static bool isUnexpanded(const Value& value)
{
    return removedByOf(value.opcode) == PassID::ExpandIteration;
}

// A test stays where it was placed, and an operation that ExpandIteration has yet to expand, later in the same round, is unknown code: in a cycle with one, no proof for a stretch lasts a turn.
// The uses in such a cycle are planned in the round after the expansion. What the operation reads is planned now: the expansion goes by the tests of the call that made it.
void TestSiteReporter::findUsesAwaitingExpansion()
{
    const Function& function = this->function();
    if (!function.values.containsIf(isUnexpanded))
        return;

    const CFG& cfg = analyses().cfg();
    const BlockCycles& cycles = analyses().blockCycles();
    BitVector cyclesWithUnexpandedOperation;
    BitVector isReadByUnexpandedOperation(function.values.size());
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (!isUnexpanded(value))
                continue;
            if (std::optional<unsigned> cycle = cycles.outermostCycleOf(block))
                cyclesWithUnexpandedOperation.set(*cycle);
            value.forEachOperand([&](ValueID operand, uint32_t) {
                for (ValueID read : CastChain(function, operand))
                    isReadByUnexpandedOperation.set(read.index());
            });
        }
    }

    for (BlockID block : cfg.reversePostOrder()) {
        std::optional<unsigned> cycle = cycles.outermostCycleOf(block);
        if (!cycle || !cyclesWithUnexpandedOperation.get(*cycle))
            continue;
        for (ValueID id : function.at(block).values) {
            if (!isReadByUnexpandedOperation.get(id.index()))
                m_awaitsExpansion.set(id.index());
        }
    }
}

std::optional<Type> TestSiteReporter::predictedPartOf(ValueID operand, uint16_t droppedPrimitives) const
{
    const Value& value = function().at(operand);
    TypeID prediction = value.predictionOrType();
    if (prediction == value.type)
        return std::nullopt;

    const Type& type = typeOf(operand);
    Type expected = types().at(prediction).withoutPrimitives(droppedPrimitives).intersectWith(type, types());
    if (expected.isNever() || type.isSubsetOf(expected, program()))
        return std::nullopt;
    return expected;
}

EarlierTest TestSiteReporter::findEarlierTest(const Value& test) const
{
    if (analyses().findEarlierTest(test, m_point.block) == EarlierTest::Failed)
        return EarlierTest::Failed;

    // Every failing edge of a chain of tests enters one generic copy, so no single failed edge dominates the copy.
    if (analyses().blockFrequencies().classOf(m_point.block) == BlockFrequency::Normal)
        return EarlierTest::Unknown;
    if (!m_failedConditions)
        m_failedConditions.emplace(analyses());
    return m_failedConditions->mayHaveFailed(test, m_point.block) ? EarlierTest::Failed : EarlierTest::Unknown;
}

bool TestSiteReporter::canPlace(const Value& test) const
{
    return placeabilityOf(analyses(), test) == TestPlaceability::CanBePlaced;
}

void TestSiteReporter::report(std::span<const Value> tests, const Effects& effectsWhenPassed)
{
    RELEASE_ASSERT(m_use && !m_didReport && !tests.empty());
    for (const Value& test : tests)
        RELEASE_ASSERT(canPlace(test));

    bool hasFailedTest = std::ranges::any_of(tests, [&](const Value& test) {
        return findEarlierTest(test) == EarlierTest::Failed;
    });
    if (hasFailedTest)
        return;

    m_sites.sites.append(TestSite { m_rule, m_use, m_point, static_cast<uint32_t>(m_sites.tests.size()), static_cast<uint32_t>(tests.size()), effectsWhenPassed });
    m_sites.tests.append(tests);
    m_isClaimed.set(m_use.index());
    m_didReport = true;
}

void TestSiteReporter::moveTo(TestSiteRuleID rule, ValueID use, ProgramPoint point)
{
    m_rule = rule;
    m_use = use;
    m_point = point;
    m_didReport = false;
}

ValueID TestSiteRewriter::insertBeforeUse(Value&& value)
{
    builder().setOrigin(useValue().origin());
    ValueID id = builder().insertBefore(point(), WTF::move(value));
    m_inputs.changedValues.append(id);
    return id;
}

std::span<const TestSiteRule> testSiteRules()
{
    static constexpr std::array<TestSiteRule, numberOfTestSiteRules> rules { {
#define AOT_DEFINE_TEST_SITE_RULE(id, report, rewrite) { TestSiteRuleID::id, #id ""_s, report, rewrite },
        FOR_EACH_AOT_TEST_SITE_RULE(AOT_DEFINE_TEST_SITE_RULE)
#undef AOT_DEFINE_TEST_SITE_RULE
    } };
    return rules;
}

const TestSiteRule& ruleOf(TestSiteRuleID id)
{
    return testSiteRules()[static_cast<unsigned>(id)];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
