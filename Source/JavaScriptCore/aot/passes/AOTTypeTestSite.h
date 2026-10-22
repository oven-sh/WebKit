/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTTypeTestConditions.h"
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_AOT_TEST_SITE_RULE(v) \
    v(EnumeratedKeyUse, reportEnumeratedKeyTests, rewriteTestedEnumeratedKeyUse) \
    v(UnionReceiver, reportUnionReceiverTests, rewriteTestedUnionReceiver) \
    v(BuiltinUse, reportBuiltinUseTests, rewriteTestedBuiltinUse) \
    v(Call, reportCallTests, rewriteTestedCall) \
    v(PropertyAccess, reportPropertyAccessTests, rewriteTestedPropertyAccess) \
    v(ElementAccess, reportElementAccessTests, rewriteTestedElementAccess) \
    v(Operator, reportOperatorTests, rewriteTestedOperator) \
    v(RepeatedTest, reportRepeatedTests, rewriteRepeatedTest)

enum class TestSiteRuleID : uint8_t {
#define AOT_DECLARE_TEST_SITE_RULE_ID(id, report, rewrite) id,
    FOR_EACH_AOT_TEST_SITE_RULE(AOT_DECLARE_TEST_SITE_RULE_ID)
#undef AOT_DECLARE_TEST_SITE_RULE_ID
};

#define AOT_COUNT_TEST_SITE_RULE(id, report, rewrite) + 1
inline constexpr unsigned numberOfTestSiteRules = 0 FOR_EACH_AOT_TEST_SITE_RULE(AOT_COUNT_TEST_SITE_RULE);
#undef AOT_COUNT_TEST_SITE_RULE

inline constexpr uint32_t testedOperandIndex = 0;

inline Value makeTypeTest(ValueID value, TypeID testedType)
{
    Value test = IRBuilder::make(Opcode::TypeTest, { value });
    test.setTestedType(testedType);
    return test;
}

inline Value makeShapeTest(ValueID object, LayoutSetID layouts)
{
    Value test = IRBuilder::make(Opcode::ShapeTest, { object });
    test.setLayouts(layouts);
    return test;
}

inline Value makePristineTest(ValueID value, BuiltinClassID builtinClass, OptionSet<PristineAspects> aspects)
{
    Value test = IRBuilder::make(Opcode::IsPristine, { value });
    test.setBuiltinClass(builtinClass);
    test.setAspects(aspects);
    return test;
}

inline Value makeArrayStorageTest(ValueID array, ArrayStorage arrayStorage, bool isWritable = false)
{
    Value test = IRBuilder::make(Opcode::HasArrayStorage, { array });
    test.setArrayStorage(arrayStorage);
    test.setIsWritable(isWritable);
    return test;
}

inline Value makeBoundsTest(ValueID array, ValueID position)
{
    return IRBuilder::make(Opcode::IsInBounds, { array, position });
}

inline Value makePresenceTest(ValueID array, ValueID position, ArrayStorage arrayStorage, bool keyIsIntegral)
{
    Value test = IRBuilder::make(Opcode::IsElementPresent, { array, position });
    test.setArrayStorage(arrayStorage);
    test.setKeyIsIntegral(keyIsIntegral);
    return test;
}

inline Value makeViewBoundsTest(ValueID view, ValueID position, TypedArrayType typedArrayType, bool keyIsIntegral)
{
    Value test = IRBuilder::make(Opcode::IsInsideView, { view, position });
    test.setTypedArrayType(typedArrayType);
    test.setKeyIsIntegral(keyIsIntegral);
    return test;
}

inline Value makeClosureTest(ValueID value, FunctionID function)
{
    Value test = IRBuilder::make(Opcode::IsClosureOf, { value });
    test.setFunction(function);
    return test;
}

struct TestSite {
    TestSiteRuleID rule { TestSiteRuleID::Operator };
    ValueID use;
    ProgramPoint point;
    uint32_t firstTest { 0 };
    uint32_t numberOfTests { 0 };
    Effects effectsWhenPassed;
};

struct TestSites {
    Vector<TestSite> sites;
    Vector<Value> tests;

    std::span<const Value> testsOf(const TestSite& site) const LIFETIME_BOUND { return tests.subspan(site.firstTest, site.numberOfTests); }
    bool isEmpty() const { return sites.isEmpty(); }
};

class TestSiteReporter {
    WTF_MAKE_NONCOPYABLE(TestSiteReporter);
public:
    explicit TestSiteReporter(FunctionPassContext&);

    FunctionPassContext& context() const { return m_context; }
    const Program& program() const { return m_context.program; }
    const Function& function() const { return m_context.function; }
    FunctionAnalyses& analyses() const { return m_context.analyses; }
    TypeArena& types() const { return m_context.types; }

    ValueID use() const { return m_use; }
    const Value& useValue() const LIFETIME_BOUND { return m_context.function.at(m_use); }
    ProgramPoint point() const { return m_point; }

    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_context.types.at(m_context.function.at(value).type); }
    std::optional<Type> expectedPartOf(ValueID operand, uint16_t droppedPrimitives = Primitive::UndefinedOrNull) const;
    EarlierTest findEarlierTest(const Value& test) const;

    void report(std::span<const Value> tests, const Effects& effectsWhenPassed);
    void report(std::initializer_list<Value> tests, const Effects& effectsWhenPassed) { report(std::span<const Value> { tests.begin(), tests.size() }, effectsWhenPassed); }

    void forEachCandidate(NOESCAPE const Invocable<void(const Value&)> auto& functor)
    {
        const CFG& cfg = m_context.analyses.cfg();
        const BlockFrequencies& frequencies = m_context.analyses.blockFrequencies();
        for (BlockID block : cfg.reversePostOrder()) {
            if (frequencies.classOf(block) == BlockFrequency::Rare)
                continue;
            const BlockValues& values = m_context.function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (m_isClaimed.get(values[index].index()) || m_awaitsExpansion.get(values[index].index()))
                    continue;
                moveTo(m_rule, values[index], { block, index });
                functor(m_context.function.at(values[index]));
            }
        }
    }

    void beginRule(TestSiteRuleID rule) { m_rule = rule; }
    void moveTo(TestSiteRuleID, ValueID use, ProgramPoint);
    bool didReport() const { return m_didReport; }
    TestSites takeSites() { return WTF::move(m_sites); }

private:
    bool canPlace(const Value& test) const;
    bool hasFailedTest(std::span<const Value> tests) const;
    bool isAboutSubjectOfFailedTest(std::span<const Value> tests) const;
    bool refusesObjectsThatAreExpected(std::span<const Value> tests) const;
    bool asksForBagOfOptions(std::span<const Value> tests) const;
    void findUsesAwaitingExpansion();

    FunctionPassContext& m_context;
    TestSiteRuleID m_rule { TestSiteRuleID::Operator };
    ValueID m_use;
    ProgramPoint m_point;
    bool m_didReport { false };
    mutable bool m_hasFailedEarlierTest { false };
    BitVector m_isClaimed;
    BitVector m_awaitsExpansion;
    TestSites m_sites;
    mutable std::optional<FailedConditions> m_failedConditions;
};

class TestSiteRewriter {
    WTF_MAKE_NONCOPYABLE(TestSiteRewriter);
public:
    struct Inputs {
        FunctionPassContext& context;
        IRBuilder& builder;
        const TestSite& site;
        std::span<const Value> tests;
        std::span<const ValueID> proofs;
        ValueID guard;
        ProgramPoint point;
        Vector<ValueID>& changedValues;
    };

    explicit TestSiteRewriter(const Inputs& inputs)
        : m_inputs(inputs)
    {
    }

    FunctionPassContext& context() const { return m_inputs.context; }
    const Program& program() const { return m_inputs.context.program; }
    Function& function() const { return m_inputs.context.function; }
    TypeArena& types() const { return m_inputs.context.types; }
    IRBuilder& builder() const { return m_inputs.builder; }

    ValueID use() const { return m_inputs.site.use; }
    Value& useValue() const LIFETIME_BOUND { return m_inputs.context.function.edit(m_inputs.site.use); }
    ProgramPoint point() const { return m_inputs.point; }

    std::span<const Value> tests() const { return m_inputs.tests; }
    ValueID proofOf(uint32_t testIndex) const { return m_inputs.proofs[testIndex]; }
    ValueID guard() const { return m_inputs.guard; }

    ValueID insertBeforeUse(Value&&);

private:
    Inputs m_inputs;
};

struct TestSiteRule {
    TestSiteRuleID id;
    ASCIILiteral name;
    void (*report)(TestSiteReporter&);
    void (*rewrite)(TestSiteRewriter&);
};

#define AOT_DECLARE_TEST_SITE_RULE(id, report, rewrite) \
    void report(TestSiteReporter&); \
    void rewrite(TestSiteRewriter&);
FOR_EACH_AOT_TEST_SITE_RULE(AOT_DECLARE_TEST_SITE_RULE)
#undef AOT_DECLARE_TEST_SITE_RULE

std::span<const TestSiteRule> testSiteRules();
const TestSiteRule& ruleOf(TestSiteRuleID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
