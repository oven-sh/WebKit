/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTFlowResults.h"
#include "AOTIRParser.h"
#include "AOTIRTestSupport.h"
#include "AOTSelectTypedResultsNodes.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr ASCIILiteral declaredArmsText = R"IR(module m0 path="main.js" displayPath="main.js"
expectedresult x0 type=L0|L1 layouts=[L0, L1] validation=[{shape=Union{arms=[2, 1]} type=L0|L1}, {shape=Object{layout=L0 members=[{key="a" node=3 field=0}]} type=L0}, {shape=Object{layout=L1 members=[{key="b" node=3 field=0}]} type=L1}, {shape=Primitive{primitive=Number} type=number}]
layout L0 name="First" module=m0 position=40 fields=[{key="a" declared={type=number}}]
layout L1 name="Second" module=m0 position=70 fields=[{key="b" declared={type=number}}]
)IR"_s;

namespace {

class DeclaredArms {
    WTF_MAKE_NONCOPYABLE(DeclaredArms);
public:
    explicit DeclaredArms(std::initializer_list<ASCIILiteral> optionWords = { })
        : m_build(optionsWith(optionWords))
        , m_program(parseProgram(String(declaredArmsText), IRForm::Prepared, m_build))
    {
        AOT_TEST_CHECK(m_program);
    }

    Program& program() { return *m_program; }
    BuildContext& build() { return m_build; }
    NodeDecisionInputs decisionInputs() { return { *m_program, m_flow, m_build }; }
    ResultNodeRef ref(uint32_t node) { return ResultNodeRef::forValidation(ExpectedResultID(0u), node); }

private:
    static Wire::CompileOptions optionsWith(std::initializer_list<ASCIILiteral> optionWords)
    {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        for (ASCIILiteral word : optionWords)
            AOT_TEST_CHECK(!applyOptionWord(options, word));
        return options;
    }

    BuildContext m_build;
    std::unique_ptr<Program> m_program;
    FlowResults m_flow;
};

} // anonymous namespace

static bool isDeclineAt(const Wire::DeclineRecord& decline, InterpretedCause cause, SourceLocation location)
{
    return decline.kind == DeclineKind::ResultNodeInterpreted && decline.reason == static_cast<uint32_t>(cause) && decline.limit == LimitID::None && !decline.count
        && decline.witness.function == Wire::none && decline.witness.value == Wire::none && decline.witness.location == location;
}

static void testFirstLayoutIsTheLowestAmongTheAlternatives()
{
    DeclaredArms arms;
    AOT_TEST_CHECK(firstLayoutOf(arms.program(), arms.ref(0)) == LayoutID(0u));
    AOT_TEST_CHECK(firstLayoutOf(arms.program(), arms.ref(2)) == LayoutID(1u));
    AOT_TEST_CHECK(!firstLayoutOf(arms.program(), arms.ref(3)));
}

static Vector<Wire::DeclineRecord> addDeclines(DeclaredArms& arms)
{
    NodeDecisionInputs inputs = arms.decisionInputs();
    addInterpretedNodeDecline(inputs, LayoutID(1u), InterpretedCause::RepeatedRecursion);
    addInterpretedNodeDecline(inputs, LayoutID(1u), InterpretedCause::RepeatedRecursion);
    addInterpretedNodeDecline(inputs, LayoutID(1u), InterpretedCause::MemberKeyIsNotText);
    addInterpretedNodeDecline(inputs, LayoutID(0u), InterpretedCause::FieldTypeIsNotTestable);
    addInterpretedNodeDecline(inputs, LayoutID(), InterpretedCause::RepeatedRecursion);
    addInterpretedNodeDecline(inputs, LayoutID(0u), InterpretedCause::TooManyNodes);
    return arms.build().takeDeclines(&arms.program());
}

static void testDeclinePointsAtTheDeclaration()
{
    DeclaredArms reporting { "report=Declines"_s };
    Vector<Wire::DeclineRecord> declines = addDeclines(reporting);
    AOT_TEST_CHECK(declines.size() == 4);
    AOT_TEST_CHECK(isDeclineAt(declines[0], InterpretedCause::FieldTypeIsNotTestable, { 0, 40 }));
    AOT_TEST_CHECK(isDeclineAt(declines[1], InterpretedCause::RepeatedRecursion, { 0, 70 }));
    AOT_TEST_CHECK(isDeclineAt(declines[2], InterpretedCause::MemberKeyIsNotText, { 0, 70 }));
    AOT_TEST_CHECK(isDeclineAt(declines[3], InterpretedCause::RepeatedRecursion, noSourceLocation));
}

static void testBuildWithoutTheReportKeepsNoDecline()
{
    DeclaredArms silent;
    AOT_TEST_CHECK(addDeclines(silent).isEmpty());
}

std::span<const NativeTest> resultNodeDeclineTests()
{
    static constexpr NativeTest tests[] = {
        { "result-node-declines/first-layout-is-the-lowest-among-the-alternatives"_s, testFirstLayoutIsTheLowestAmongTheAlternatives },
        { "result-node-declines/decline-points-at-the-declaration"_s, testDeclinePointsAtTheDeclaration },
        { "result-node-declines/build-without-the-report-keeps-no-decline"_s, testBuildWithoutTheReportKeepsNoDecline },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
