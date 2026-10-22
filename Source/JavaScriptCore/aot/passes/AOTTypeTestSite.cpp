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
    m_context.hasUsesAwaitingExpansion = !m_awaitsExpansion.isEmpty();
}

std::optional<Type> TestSiteReporter::expectedPartOf(ValueID operand, uint16_t droppedPrimitives) const
{
    const Value& value = function().at(operand);
    TypeID expectedType = value.expectedOrType();
    if (expectedType == value.type)
        return std::nullopt;

    const Type& type = typeOf(operand);
    Type expected = types().at(expectedType).withoutPrimitives(droppedPrimitives).intersectWith(type, types());
    if (expected.isNever() || type.isSubsetOf(expected, program()))
        return std::nullopt;
    return expected;
}

EarlierTest TestSiteReporter::findEarlierTest(const Value& test) const
{
    if (analyses().findEarlierTest(test, m_point.block) == EarlierTest::Failed) {
        m_hasFailedEarlierTest = true;
        return EarlierTest::Failed;
    }

    // Every failing edge of a chain of tests enters one generic copy, so no single failed edge dominates the copy.
    if (function().at(m_point.block).frequency == BlockFrequency::Normal)
        return EarlierTest::Unknown;
    if (!m_failedConditions)
        m_failedConditions.emplace(analyses());
    if (!m_failedConditions->mayHaveFailed(test, m_point.block))
        return EarlierTest::Unknown;
    m_hasFailedEarlierTest = true;
    return EarlierTest::Failed;
}

bool TestSiteReporter::canPlace(const Value& test) const
{
    return placeabilityOf(analyses(), test) == TestPlaceability::CanBePlaced;
}

// The tests of a site are made in order. A test that reads only its subject failed on an object that had passed the test of its type in front of it.
// Behind a test for another type, one that has not failed, it is made on another object: the second layout of a site is tested in the copy that the tests of the first one enter.
bool TestSiteReporter::hasFailedTest(std::span<const Value> tests) const
{
    Vector<ValueID, 2> subjectsTestedForOtherType;
    for (const Value& test : tests) {
        Proof proof = provesOf(test.opcode);
        ValueID subject = test.numberOfOperands() ? analyses().identityOf(test.operand(testedOperandIndex)) : ValueID();
        bool isMadeOnOtherObject = readsOnlySubjectOf(proof) && subjectsTestedForOtherType.contains(subject);
        EarlierTest earlier = isMadeOnOtherObject ? analyses().findEarlierTest(test, m_point.block) : findEarlierTest(test);
        if (earlier == EarlierTest::Failed)
            return true;
        if (proof == Proof::TestedType)
            subjectsTestedForOtherType.append(subject);
    }
    return false;
}

bool TestSiteReporter::isAboutSubjectOfFailedTest(std::span<const Value> tests) const
{
    if (!m_failedConditions)
        m_failedConditions.emplace(analyses());
    return std::ranges::any_of(tests, [&](const Value& test) {
        return test.numberOfOperands() && m_failedConditions->mayHaveFailedTestOf(analyses().identityOf(test.operand(testedOperandIndex)), m_point.block);
    });
}

// `handle.flags`, `handle.end()`, where the handle is an object of the host or a record that stands in for one: no test of layouts lets the object of the host pass, and it is as expected as the record.
// It would fail the test every time and take the rest of the function into its copy. Without the test the one operation goes by name where it stands.
bool TestSiteReporter::refusesObjectsThatAreExpected(std::span<const Value> tests) const
{
    return std::ranges::any_of(tests, [&](const Value& test) {
        if (test.opcode != Opcode::TypeTest)
            return false;
        const Type& tested = types().at(test.testedType());
        return tested.layouts && tested.classes.isEmpty() && !program().expectedOrTypeOf(function(), test.operand(testedOperandIndex)).classes.isEmpty();
    });
}

// `compilerOptions.strict`: a record type of dozens of fields, every one of them optional, describes a bag of options. Such bags are merged, copied name by name and read from files,
// so what arrives is mostly an ordinary object: it fails the test for the layout every time. Without the test the operation goes by name where it stands.
static constexpr size_t minFieldsOfBagOfOptions = 24;

bool TestSiteReporter::asksForBagOfOptions(std::span<const Value> tests) const
{
    return std::ranges::any_of(tests, [&](const Value& test) {
        if (test.opcode != Opcode::TypeTest)
            return false;
        const Type& tested = types().at(test.testedType());
        return tested.layouts && std::ranges::all_of(program().at(tested.layouts).members, [&](LayoutID id) {
            const Layout& layout = program().at(id);
            return layout.kind == LayoutKind::Record && layout.fields.size() >= minFieldsOfBagOfOptions && std::ranges::none_of(layout.fields, [](const Field& field) {
                return field.flags.contains(FieldFlags::IsRequired);
            });
        });
    });
}

void TestSiteReporter::report(std::span<const Value> tests, const Effects& effectsWhenPassed)
{
    RELEASE_ASSERT(m_use && !m_didReport && !tests.empty());
    for (const Value& test : tests)
        RELEASE_ASSERT(canPlace(test));
    if (refusesObjectsThatAreExpected(tests) || asksForBagOfOptions(tests))
        return;

    // A block that is marked is a slow path: a generic copy, the arm behind a failed test. A block that only the estimate of a branch calls unlikely is typed code like the rest, and the estimate may be wrong.
    bool isInTypedCode = function().at(m_point.block).frequency == BlockFrequency::Normal;
    if (!isInTypedCode && !m_hasFailedEarlierTest && !isAboutSubjectOfFailedTest(tests))
        return;
    if (hasFailedTest(tests))
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
    m_hasFailedEarlierTest = false;
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
