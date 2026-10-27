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
#include "AOTProgramFile.h"
#include "AOTSelectTypedResultsNodes.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include "AOTVector.h"
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class ParsedProgram {
    WTF_MAKE_NONCOPYABLE(ParsedProgram);
public:
    explicit ParsedProgram(const String& text)
        : m_build(makeDefaultCompileOptions())
        , m_program(parseProgram(text, IRForm::Prepared, m_build))
    {
        if (!m_program) {
            m_build.diagnostics().finish(nullptr);
            printDiagnostics(m_build.diagnostics(), WTF::dataFile());
            dataLogLn(text);
        }
        AOT_TEST_CHECK(m_program);
    }

    Program& program() { return *m_program; }

    TypeID type(uint32_t expectedResult) { return program().at(ExpectedResultID(expectedResult)).type; }
    ResultNodeRef ref(uint32_t expectedResult, uint32_t part = 0) { return { type(expectedResult), part }; }
    ResultNode node(uint32_t expectedResult, uint32_t part = 0) { return describeNode(program(), ref(expectedResult, part)); }
    NodeDecision decide(uint32_t expectedResult) { return decideNodeCode(NodeDecisionInputs { program(), m_flow, m_build }, ref(expectedResult)); }
    std::optional<TaggedArms> taggedArmsOf(uint32_t expectedResult) { return findTaggedArms(program(), node(expectedResult).children.span()); }

    String textOf(StringID string) { return program().strings.toWTFString(string); }

private:
    BuildContext m_build;
    std::unique_ptr<Program> m_program;
    FlowResults m_flow;
};

} // anonymous namespace

static constexpr ASCIILiteral typesText = R"IR(module m0 path="main.js" displayPath="main.js"
expectedresult x0 type=null|string
expectedresult x1 type="a"|"b"
expectedresult x2 type=Array<L0>
expectedresult x3 type=L0|L1
expectedresult x4 type=anything
expectedresult x5 type=L2
expectedresult x6 type=undefined|symbol|bigint
expectedresult x7 type=null|boolean|number|string|Array<number>|L0
expectedresult x8 type=true
expectedresult x9 type=i32
expectedresult x10 type=object
expectedresult x11 type=L3
expectedresult x12 type=L0|L3
expectedresult x13 type=false|"x"
layout L0 name="User" fields=[{key="id" declared={type=number}}, {key="name" declared={type=undefined|string}}, {key=symbol("tag")@m0:120 declared={type=number}}, {key="friend" declared={type=undefined|L0}}, {key="guess" declared={type=undefined|number flags=[IsExpected]}}]
layout L1 name="UserLiteral" fields=[{key="id" declared={type=number}}, {key="name" declared={type=undefined|string}}] supertypes=[L0]
layout L2 kind=Instance name="Made"
layout L3 name="Other" fields=[{key="x" declared={type=number}}]
)IR"_s;

static Vector<ResultShape> shapesOf(const Vector<TypeAlternative, 4>& alternatives)
{
    return alternatives.map([](const TypeAlternative& alternative) {
        return alternative.shape;
    });
}

static void testAlternativesOfStringOrNull()
{
    ParsedProgram types { String(typesText) };
    AOT_TEST_CHECK(shapesOf(alternativesOf(types.program(), types.type(0))) == Vector<ResultShape>({ ResultShape::Null, ResultShape::String }));

    ResultNode whole = types.node(0);
    AOT_TEST_CHECK(whole.shape == ResultShape::Union && whole.type == types.type(0));
    AOT_TEST_CHECK(hasElements(whole.children, { types.ref(0, 1), types.ref(0, 2) }));
    AOT_TEST_CHECK(types.node(0, 1).shape == ResultShape::Null && types.node(0, 2).shape == ResultShape::String);
    AOT_TEST_CHECK(types.node(0, 1).type == types.type(0) && types.node(0, 2).type == types.type(0));
}

static void testAlternativesOfLiteralUnion()
{
    ParsedProgram types { String(typesText) };
    Vector<TypeAlternative, 4> alternatives = alternativesOf(types.program(), types.type(1));
    AOT_TEST_CHECK(shapesOf(alternatives) == Vector<ResultShape>({ ResultShape::StringLiteral, ResultShape::StringLiteral }));
    const auto& literals = types.program().at(types.type(1)).stringLiterals;
    AOT_TEST_CHECK(literals.size() == 2);
    for (unsigned index = 0; index < 2; ++index) {
        AOT_TEST_CHECK(alternatives[index].literal == (ResultLiteral { .kind = ResultLiteral::Kind::String, .string = literals[index] }));
        AOT_TEST_CHECK(types.node(1, index + 1).literal == alternatives[index].literal);
    }
    AOT_TEST_CHECK(types.node(1).shape == ResultShape::Union && types.node(1).children.size() == 2);

    Vector<TypeAlternative, 4> mixed = alternativesOf(types.program(), types.type(13));
    AOT_TEST_CHECK(shapesOf(mixed) == Vector<ResultShape>({ ResultShape::BooleanLiteral, ResultShape::StringLiteral }));
    AOT_TEST_CHECK(mixed[0].literal == (ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = false }) && types.textOf(mixed[1].literal.string) == "x"_s);
}

static void testAlternativesOfArray()
{
    ParsedProgram types { String(typesText) };
    Vector<TypeAlternative, 4> alternatives = alternativesOf(types.program(), types.type(2));
    AOT_TEST_CHECK(shapesOf(alternatives) == Vector<ResultShape>({ ResultShape::Array }));
    ResultNode array = types.node(2);
    AOT_TEST_CHECK(array.shape == ResultShape::Array && hasElements(array.children, { ResultNodeRef { alternatives[0].contents } }));
    ResultNode element = describeNode(types.program(), array.children[0]);
    AOT_TEST_CHECK(element.shape == ResultShape::Object && element.layout == LayoutID(0u));
}

static void testHeadLayoutsOfSetWithLiteralMembers()
{
    ParsedProgram types { String(typesText) };
    Vector<TypeAlternative, 4> alternatives = alternativesOf(types.program(), types.type(3));
    AOT_TEST_CHECK(alternatives.size() == 1 && alternatives[0].shape == ResultShape::Object && alternatives[0].layout == LayoutID(0u));
    AOT_TEST_CHECK(types.node(3).shape == ResultShape::Object && types.node(3).layout == LayoutID(0u));

    Vector<TypeAlternative, 4> unrelated = alternativesOf(types.program(), types.type(12));
    AOT_TEST_CHECK(unrelated.size() == 2 && unrelated[0].layout == LayoutID(0u) && unrelated[1].layout == LayoutID(3u));
    AOT_TEST_CHECK(types.node(12).shape == ResultShape::Union);
}

static void testTypesThatTextCannotProduce()
{
    ParsedProgram types { String(typesText) };
    AOT_TEST_CHECK(types.node(4).shape == ResultShape::Unknown && types.node(4).type == TypeTable::anything);
    AOT_TEST_CHECK(types.node(10).shape == ResultShape::Unknown);
    AOT_TEST_CHECK(alternativesOf(types.program(), types.type(5)).isEmpty() && types.node(5).shape == ResultShape::Unknown);
    AOT_TEST_CHECK(alternativesOf(types.program(), types.type(6)).isEmpty() && types.node(6).shape == ResultShape::Unknown);
}

static void testOrderOfAlternatives()
{
    ParsedProgram types { String(typesText) };
    AOT_TEST_CHECK(shapesOf(alternativesOf(types.program(), types.type(7))) == Vector<ResultShape>({ ResultShape::Null, ResultShape::Boolean, ResultShape::Number, ResultShape::String, ResultShape::Array, ResultShape::Object }));
    AOT_TEST_CHECK(types.node(7).children.size() == 6);

    Vector<TypeAlternative, 4> onlyTrue = alternativesOf(types.program(), types.type(8));
    AOT_TEST_CHECK(onlyTrue.size() == 1 && onlyTrue[0].shape == ResultShape::BooleanLiteral && onlyTrue[0].literal.boolean);
    AOT_TEST_CHECK(types.node(8).shape == ResultShape::BooleanLiteral);
    AOT_TEST_CHECK(shapesOf(alternativesOf(types.program(), types.type(9))) == Vector<ResultShape>({ ResultShape::Number }));
}

static void testMembersOfLayout()
{
    ParsedProgram types { String(typesText) };
    const Layout& layout = types.program().at(LayoutID(0u));
    ResultNode object = types.node(3);
    AOT_TEST_CHECK(object.children.isEmpty() && object.members.size() == 4);
    constexpr uint32_t fields[] = { 0, 1, 3, 4 };
    constexpr bool isOptional[] = { false, true, true, true };
    for (unsigned index = 0; index < 4; ++index) {
        const ResultMember& member = object.members[index];
        const Field& field = layout.fields[fields[index]];
        AOT_TEST_CHECK(member.field == fields[index] && member.key == field.key);
        AOT_TEST_CHECK(member.isOptional == isOptional[index]);
        AOT_TEST_CHECK(member.node == ResultNodeRef { expectationOf(field.declared).predicted() });
    }
    AOT_TEST_CHECK(expectationOf(layout.fields[0].declared).predicted() == layout.fields[0].declared.written());
    AOT_TEST_CHECK(layout.fields[4].declared.isExpected() && expectationOf(layout.fields[4].declared).predicted() == layout.fields[4].declared.written());
    AOT_TEST_CHECK(describeNode(types.program(), object.members[2].node).layout == LayoutID(0u));
    AOT_TEST_CHECK(describeNode(types.program(), object.members[3].node).shape == ResultShape::Number);
}

static void testValueClasses()
{
    ParsedProgram types { String(typesText) };
    auto classesOf = [&](uint32_t expectedResult) {
        return valueClassesOf(types.program(), types.ref(expectedResult));
    };
    constexpr OptionSet<JSONValueClass> everyClass { JSONValueClass::Null, JSONValueClass::Boolean, JSONValueClass::Number, JSONValueClass::String, JSONValueClass::Array, JSONValueClass::Object };
    AOT_TEST_CHECK(classesOf(0) == OptionSet<JSONValueClass>({ JSONValueClass::Null, JSONValueClass::String }));
    AOT_TEST_CHECK(classesOf(1) == JSONValueClass::String && classesOf(2) == JSONValueClass::Array && classesOf(3) == JSONValueClass::Object);
    AOT_TEST_CHECK(classesOf(4) == everyClass && classesOf(7) == everyClass);
    AOT_TEST_CHECK(classesOf(8) == JSONValueClass::Boolean && classesOf(9) == JSONValueClass::Number);
    AOT_TEST_CHECK(classesOf(13) == OptionSet<JSONValueClass>({ JSONValueClass::Boolean, JSONValueClass::String }));
}

static void testKeysLeadBackToTheirNodes()
{
    ParsedProgram types { String(typesText) };
    for (uint32_t expectedResult : { 2u, 11u }) {
        DerivedFunctionKey key = parserKeyOf(types.ref(expectedResult));
        AOT_TEST_CHECK(key.role == FunctionRole::Parser && nodeOfKey(key) == types.ref(expectedResult));
    }
    AOT_TEST_CHECK(parserKeyOf(types.ref(2)) != parserKeyOf(types.ref(11)));
}

static constexpr ASCIILiteral taggedText = R"IR(module m0 path="main.js" displayPath="main.js"
expectedresult x0 type=L0|L1|L2
expectedresult x1 type=L0|L3
expectedresult x2 type=L0|L1|L4
expectedresult x3 type=L0|L4
expectedresult x4 type=L5|L6
expectedresult x5 type=L0|L7
layout L0 name="A" fields=[{key="kind" declared={type="a"}}]
layout L1 name="B" fields=[{key="kind" declared={type="b"}}]
layout L2 name="C" fields=[{key="kind" declared={type="c"}}]
layout L3 name="AlsoA" fields=[{key="kind" declared={type="a"}}, {key="more" declared={type=number}}]
layout L4 name="WithoutKind" fields=[{key="x" declared={type=number}}]
layout L5 name="Yes" fields=[{key="text" declared={type=string}}, {key="ok" declared={type=true}}]
layout L6 name="No" fields=[{key="text" declared={type=string}}, {key="ok" declared={type=false}}]
layout L7 name="AnyKind" fields=[{key="kind" declared={type=string}}]
)IR"_s;

static void testTagInThreeArms()
{
    ParsedProgram types { String(taggedText) };
    std::optional<TaggedArms> arms = types.taggedArmsOf(0);
    AOT_TEST_CHECK(arms && arms->tagged == types.node(0).children && arms->untagged.isEmpty());
    AOT_TEST_CHECK(arms->discriminant.key == types.program().at(LayoutID(0u)).fields[0].key && arms->discriminant.tagOfArm.size() == 3);
    AOT_TEST_CHECK(arms->discriminant.tagOfArm[1].kind == ResultLiteral::Kind::String && types.textOf(arms->discriminant.tagOfArm[1].string) == "b"_s);
}

static void testEqualTagsTellNothingApart()
{
    ParsedProgram types { String(taggedText) };
    AOT_TEST_CHECK(!types.taggedArmsOf(1));
}

static void testArmWithoutTheMemberIsUntagged()
{
    ParsedProgram types { String(taggedText) };
    std::optional<TaggedArms> arms = types.taggedArmsOf(2);
    AOT_TEST_CHECK(arms && arms->tagged.size() == 2 && hasElements(arms->untagged, { types.ref(2, 3) }));
}

static void testOneTagTellsNothingApart()
{
    ParsedProgram types { String(taggedText) };
    AOT_TEST_CHECK(!types.taggedArmsOf(3));
}

static void testBooleanTagsBehindMemberThatIsNoLiteral()
{
    ParsedProgram types { String(taggedText) };
    std::optional<TaggedArms> arms = types.taggedArmsOf(4);
    AOT_TEST_CHECK(arms && arms->untagged.isEmpty() && arms->discriminant.key == types.program().at(LayoutID(5u)).fields[1].key);
    AOT_TEST_CHECK(arms->discriminant.tagOfArm[0] == (ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = true }));
    AOT_TEST_CHECK(arms->discriminant.tagOfArm[1] == (ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = false }));
    AOT_TEST_CHECK(!types.taggedArmsOf(5));
}

static bool isCompiled(NodeDecision decision)
{
    return decision.code == NodeCode::Compiled && decision.cause == InterpretedCause::None;
}

static bool isInterpretedFor(NodeDecision decision, InterpretedCause cause)
{
    return decision.code == NodeCode::Interpreted && decision.cause == cause;
}

static void testUnionThatRepeatsRecursionIsInterpreted()
{
    ParsedProgram types { String(R"IR(module m0 path="main.js" displayPath="main.js"
expectedresult x0 type=L0|L1|L2
expectedresult x1 type=L0|L2
expectedresult x2 type=L3|L4|L2
expectedresult x3 type=L5|Array<L5|number>|number
layout L0 name="Left" fields=[{key="below" declared={type=L0|L1|L2}}, {key="a" declared={type=number}}]
layout L1 name="Right" fields=[{key="below" declared={type=L0|L1|L2}}, {key="b" declared={type=number}}]
layout L2 name="Leaf" fields=[{key="leaf" declared={type=number}}]
layout L3 name="TaggedLeft" fields=[{key="k" declared={type="a"}}, {key="below" declared={type=L3|L4|L2}}]
layout L4 name="TaggedRight" fields=[{key="k" declared={type="b"}}, {key="below" declared={type=L3|L4|L2}}]
layout L5 name="Holder" fields=[{key="below" declared={type=L5|Array<L5|number>|number}}]
)IR"_s) };
    AOT_TEST_CHECK(isInterpretedFor(types.decide(0), InterpretedCause::RepeatedRecursion));
    AOT_TEST_CHECK(isCompiled(types.decide(1)));
    // The tag chooses one of the two arms that recur.
    AOT_TEST_CHECK(isCompiled(types.decide(2)));
    // The first byte of the text chooses between an object and an array.
    AOT_TEST_CHECK(isCompiled(types.decide(3)));
}

// Level n is a union of two objects, and each holds level n - 1 under one name. Result n has the type of level n.
static String textOfUnionsBelowUnions(uint32_t levels, bool isTagged)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\"\n"_s);
    for (uint32_t level = 0; level < levels; ++level)
        text.append("expectedresult x"_s, level, " type=L"_s, 2 * level, "|L"_s, 2 * level + 1, '\n');
    for (uint32_t level = 0; level < levels; ++level) {
        for (uint32_t arm = 0; arm < 2; ++arm) {
            text.append("layout L"_s, 2 * level + arm, " name=\"Level\" fields=[{key=\"left\" declared={type="_s);
            if (level)
                text.append('L', 2 * (level - 1), "|L"_s, 2 * (level - 1) + 1);
            else
                text.append("number"_s);
            text.append("}}, {key=\"v\" declared={type="_s, isTagged ? (arm ? "\"b\""_s : "\"a\""_s) : (arm ? "string"_s : "number"_s), "}}]\n"_s);
        }
    }
    return text.toString();
}

static void testUnionsBelowUnionsMultiply()
{
    // Level n looks at the number of level 0 twice as often as level n - 1 does: 2, 4, 8 ..
    ParsedProgram untagged(textOfUnionsBelowUnions(40, false));
    for (uint32_t level = 0; level < 40; ++level)
        AOT_TEST_CHECK(isCompiled(untagged.decide(level)) == (level < 6));

    ParsedProgram tagged(textOfUnionsBelowUnions(40, true));
    for (uint32_t level = 0; level < 40; ++level)
        AOT_TEST_CHECK(isCompiled(tagged.decide(level)));

    // Where the type is too deep to count, the union is taken for one that multiplies.
    ParsedProgram deep(textOfUnionsBelowUnions(400, true));
    AOT_TEST_CHECK(isCompiled(deep.decide(20)) && isInterpretedFor(deep.decide(399), InterpretedCause::RepeatedRecursion));
}

static void testLayoutThatIsNeverAllocatedIsInterpreted()
{
    ParsedProgram types { String(R"IR(module m0 path="main.js" displayPath="main.js"
expectedresult x0 type=L0
expectedresult x1 type=L1
expectedresult x2 type=Array<L0>
expectedresult x3 type=number
layout L0 name="NeverMade" fields=[{key="id" declared={type=number}}]
layout L1 name="Made" fields=[{key="id" declared={type=number}}]
)IR"_s) };
    types.program().layouts[0].flow.allocation = AllocationState::NeverAllocated;
    AOT_TEST_CHECK(types.program().layouts[1].flow.allocation == AllocationState::Unknown);

    AOT_TEST_CHECK(isInterpretedFor(types.decide(0), InterpretedCause::LayoutIsNotModeled));
    AOT_TEST_CHECK(isCompiled(types.decide(1)) && isCompiled(types.decide(2)) && isCompiled(types.decide(3)));
}

std::span<const NativeTest> resultNodeTests()
{
    static constexpr NativeTest tests[] = {
        { "result-nodes/alternatives-of-string-or-null"_s, testAlternativesOfStringOrNull },
        { "result-nodes/alternatives-of-literal-union"_s, testAlternativesOfLiteralUnion },
        { "result-nodes/alternatives-of-array"_s, testAlternativesOfArray },
        { "result-nodes/head-layouts-of-set-with-literal-members"_s, testHeadLayoutsOfSetWithLiteralMembers },
        { "result-nodes/types-that-text-cannot-produce"_s, testTypesThatTextCannotProduce },
        { "result-nodes/order-of-alternatives"_s, testOrderOfAlternatives },
        { "result-nodes/members-of-layout"_s, testMembersOfLayout },
        { "result-nodes/value-classes"_s, testValueClasses },
        { "result-nodes/keys-lead-back-to-their-nodes"_s, testKeysLeadBackToTheirNodes },
        { "result-nodes/tags/tag-in-three-arms"_s, testTagInThreeArms },
        { "result-nodes/tags/equal-tags-tell-nothing-apart"_s, testEqualTagsTellNothingApart },
        { "result-nodes/tags/arm-without-the-member-is-untagged"_s, testArmWithoutTheMemberIsUntagged },
        { "result-nodes/tags/one-tag-tells-nothing-apart"_s, testOneTagTellsNothingApart },
        { "result-nodes/tags/boolean-tags-behind-member-that-is-no-literal"_s, testBooleanTagsBehindMemberThatIsNoLiteral },
        { "result-nodes/decisions/union-that-repeats-recursion-is-interpreted"_s, testUnionThatRepeatsRecursionIsInterpreted },
        { "result-nodes/decisions/unions-below-unions-multiply"_s, testUnionsBelowUnionsMultiply },
        { "result-nodes/decisions/layout-that-is-never-allocated-is-interpreted"_s, testLayoutThatIsNeverAllocatedIsInterpreted },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
