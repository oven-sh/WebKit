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
#include "AOTValidationRules.h"
#include "AOTValidatorBuilder.h"
#include "AOTVector.h"
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NodeText {
    ASCIILiteral shape;
    ASCIILiteral rules { ""_s };
};

constexpr ASCIILiteral unknown = ""_s;
constexpr ASCIILiteral defaultRule = "DefaultNumber{value=1}"_s;

void appendValidation(StringBuilder& text, std::span<const NodeText> nodes)
{
    text.append("validation=["_s);
    for (size_t index = 0; index < nodes.size(); ++index) {
        text.append(index ? ", {"_s : "{"_s);
        if (!nodes[index].shape.isEmpty())
            text.append("shape="_s, nodes[index].shape, ' ');
        text.append("type=anything"_s);
        if (!nodes[index].rules.isEmpty())
            text.append(" rules=["_s, nodes[index].rules, ']');
        text.append(" text=\"n"_s, index, "\"}"_s);
    }
    text.append(']');
}

class ParsedProgram {
    WTF_MAKE_NONCOPYABLE(ParsedProgram);
public:
    explicit ParsedProgram(const String& text, std::initializer_list<ASCIILiteral> optionWords = { })
        : m_build(optionsWith(optionWords))
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
    BuildContext& build() { return m_build; }

    ResultNodeRef ref(uint32_t node, uint32_t expectedResult = 0) { return ResultNodeRef::forValidation(ExpectedResultID(expectedResult), node); }
    ResultNode node(uint32_t node, uint32_t expectedResult = 0) { return describeNode(program(), ref(node, expectedResult)); }
    TypeID type(uint32_t expectedResult) { return program().at(ExpectedResultID(expectedResult)).type; }
    ResultNode typeNode(uint32_t expectedResult, uint32_t part = 0) { return describeNode(program(), ResultNodeRef::forType(type(expectedResult), part)); }

    Vector<ResultNodeRef, 4> refs(std::initializer_list<uint32_t> nodes)
    {
        Vector<ResultNodeRef, 4> result;
        for (uint32_t node : nodes)
            result.append(ref(node));
        return result;
    }

    NodeDecision decide(ResultNodeRef ref, NodeUse use = NodeUse::ParsedFromText)
    {
        return decideNodeCode(NodeDecisionInputs { program(), m_flow, m_build }, ref, use);
    }

    String textOf(StringID string) { return program().strings.toWTFString(string); }

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

String textOfGraph(std::initializer_list<NodeText> nodes)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\"\nexpectedresult x0 type=anything "_s);
    appendValidation(text, std::span(nodes.begin(), nodes.size()));
    text.append('\n');
    return text.toString();
}

class Graph final : public ParsedProgram {
public:
    Graph(std::initializer_list<NodeText> nodes, std::initializer_list<ASCIILiteral> optionWords = { })
        : ParsedProgram(textOfGraph(nodes), optionWords)
    {
    }
};

Vector<uint32_t> bitsOf(const BitVector& bits)
{
    Vector<uint32_t> result;
    for (size_t bit : bits)
        result.append(static_cast<uint32_t>(bit));
    return result;
}

constexpr OptionSet<JSONValueClass> everyValueClass { JSONValueClass::Null, JSONValueClass::Boolean, JSONValueClass::Number, JSONValueClass::String, JSONValueClass::Array, JSONValueClass::Object };
constexpr OptionSet<RuntimeClass> everyRuntimeClass { RuntimeClass::Undefined, RuntimeClass::Null, RuntimeClass::Boolean, RuntimeClass::Number, RuntimeClass::String, RuntimeClass::BigInt, RuntimeClass::Array, RuntimeClass::Object };

} // anonymous namespace

static void testShapes()
{
    Graph graph({
        { unknown }, { "Never"_s }, { "Undefined"_s }, { "Null"_s }, { "AnyObject"_s },
        { "Primitive{}"_s }, { "Primitive{primitive=Number}"_s }, { "Primitive{primitive=Boolean}"_s }, { "Primitive{primitive=BigInt}"_s },
        { "BooleanLiteral{value=true}"_s }, { "NumberLiteral{value=4.5}"_s }, { "StringLiteral{value=\"s\"}"_s },
        { "Array{element=5}"_s }, { "Tuple{elements=[6, 5, 6]}"_s }, { "Dictionary{value=7}"_s }, { "Union{arms=[3, 5]}"_s },
        { "Object{members=[{key=\"x\" node=6 isOptional=true}, {key=\"y\" node=5}] undeclared=Validated undeclaredValue=7}"_s },
        { "Object{undeclared=Rejected}"_s },
    });
    constexpr ResultShape expected[] = {
        ResultShape::Unknown, ResultShape::Never, ResultShape::Undefined, ResultShape::Null, ResultShape::AnyObject,
        ResultShape::String, ResultShape::Number, ResultShape::Boolean, ResultShape::BigInt,
        ResultShape::BooleanLiteral, ResultShape::NumberLiteral, ResultShape::StringLiteral,
        ResultShape::Array, ResultShape::Tuple, ResultShape::Dictionary, ResultShape::Union, ResultShape::Object, ResultShape::Object,
    };
    for (uint32_t index = 0; index < std::size(expected); ++index) {
        ResultNode node = graph.node(index);
        AOT_TEST_CHECK(node.shape == expected[index]);
        AOT_TEST_CHECK(node.isContainer() == (index >= 12));
        AOT_TEST_CHECK(node.type == TypeTable::anything && graph.textOf(node.text) == makeString('n', index));
        AOT_TEST_CHECK(node.rules.empty() && !node.defaultValue && !node.message);
    }

    AOT_TEST_CHECK(graph.node(9).literal == (ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = true }));
    AOT_TEST_CHECK(graph.node(10).literal == (ResultLiteral { .kind = ResultLiteral::Kind::Number, .number = 4.5 }));
    AOT_TEST_CHECK(graph.node(11).literal.kind == ResultLiteral::Kind::String && graph.textOf(graph.node(11).literal.string) == "s"_s);
    AOT_TEST_CHECK(hasElements(graph.node(12).children, { graph.ref(5) }));
    AOT_TEST_CHECK(hasElements(graph.node(13).children, { graph.ref(6), graph.ref(5), graph.ref(6) }));
    AOT_TEST_CHECK(hasElements(graph.node(14).children, { graph.ref(7) }));
    AOT_TEST_CHECK(hasElements(graph.node(15).children, { graph.ref(3), graph.ref(5) }));

    ResultNode object = graph.node(16);
    AOT_TEST_CHECK(!object.layout && object.undeclared == UndeclaredMembers::Validated && object.undeclaredValue == graph.ref(7) && object.children.isEmpty());
    AOT_TEST_CHECK(object.members.size() == 2);
    AOT_TEST_CHECK(object.members[0].node == graph.ref(6) && object.members[0].isOptional && !object.members[0].field && !object.members[0].mustBeOwn);
    AOT_TEST_CHECK(object.members[1].node == graph.ref(5) && !object.members[1].isOptional && object.members[0].key != object.members[1].key);
    ResultNode closed = graph.node(17);
    AOT_TEST_CHECK(closed.undeclared == UndeclaredMembers::Rejected && !closed.undeclaredValue && closed.members.isEmpty());
}

static void testFirstDefaultAndFirstMessageCount()
{
    Graph graph({
        { "Primitive{primitive=Number}"_s, "Minimum{bound=1}, DefaultNumber{value=3}, Message{text=\"first\"}, DefaultNumber{value=4}, Message{text=\"second\"}"_s },
        { "Null"_s, "DefaultNull"_s },
        { "Primitive{primitive=Boolean}"_s, "DefaultBoolean{}"_s },
        { "Primitive{}"_s, "DefaultString{value=\"d\"}"_s },
        { "Primitive{}"_s, "MinimumLength{length=1}"_s },
    });
    ResultNode number = graph.node(0);
    AOT_TEST_CHECK(number.rules.size() == 5);
    AOT_TEST_CHECK(number.defaultValue == (ResultLiteral { .kind = ResultLiteral::Kind::Number, .number = 3 }));
    AOT_TEST_CHECK(graph.textOf(number.message) == "first"_s);
    AOT_TEST_CHECK(graph.node(1).defaultValue == ResultLiteral { });
    AOT_TEST_CHECK(graph.node(2).defaultValue == (ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = false }));
    AOT_TEST_CHECK(graph.node(3).defaultValue && graph.node(3).defaultValue->kind == ResultLiteral::Kind::String && graph.textOf(graph.node(3).defaultValue->string) == "d"_s);
    AOT_TEST_CHECK(!graph.node(4).defaultValue && !graph.node(4).message && graph.node(4).rules.size() == 1);
}

static bool isOnCycle(ParsedProgram& graph, uint32_t node)
{
    ResultNode described = graph.node(node);
    Vector<ResultNodeRef, 8> children;
    children.appendRange(described.children.begin(), described.children.end());
    for (const ResultMember& member : described.members)
        children.append(member.node);
    if (described.undeclaredValue)
        children.append(*described.undeclaredValue);
    return children.containsIf([&](ResultNodeRef child) {
        return nodesReachableFrom(graph.program(), ExpectedResultID(0u), child.part).get(node);
    });
}

static void testNodesOnCycles()
{
    Graph graph({
        { "Object{members=[{key=\"a\" node=1}, {key=\"b\" node=4}, {key=\"c\" node=5}]}"_s },
        { "Object{members=[{key=\"a\" node=2}, {key=\"b\" node=4}]}"_s },
        { "Array{element=3}"_s },
        { "Object{undeclared=Validated undeclaredValue=1}"_s },
        { "AnyObject"_s },
        { "Union{arms=[4, 5]}"_s },
    });
    auto reachableFrom = [&](uint32_t node) {
        return bitsOf(nodesReachableFrom(graph.program(), ExpectedResultID(0u), node));
    };
    AOT_TEST_CHECK(reachableFrom(0) == Vector<uint32_t>({ 0, 1, 2, 3, 4, 5 }));
    AOT_TEST_CHECK(reachableFrom(1) == Vector<uint32_t>({ 1, 2, 3, 4 }));
    AOT_TEST_CHECK(reachableFrom(2) == Vector<uint32_t>({ 1, 2, 3, 4 }));
    AOT_TEST_CHECK(reachableFrom(3) == Vector<uint32_t>({ 1, 2, 3, 4 }));
    AOT_TEST_CHECK(reachableFrom(4) == Vector<uint32_t>({ 4 }));
    AOT_TEST_CHECK(reachableFrom(5) == Vector<uint32_t>({ 4, 5 }));

    Vector<uint32_t> onCycles;
    for (uint32_t node = 0; node < 6; ++node) {
        if (isOnCycle(graph, node))
            onCycles.append(node);
    }
    AOT_TEST_CHECK(onCycles == Vector<uint32_t>({ 1, 2, 3, 5 }));
}

static void testLongChainNeedsNoStack()
{
    constexpr uint32_t length = 60000;
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\"\nexpectedresult x0 type=anything validation=["_s);
    for (uint32_t index = 0; index < length; ++index)
        text.append(index ? ", "_s : ""_s, "{shape=Array{element="_s, (index + 1) % length, "} type=anything}"_s);
    text.append("]\n"_s);
    ParsedProgram chain(text.toString());
    for (uint32_t start : { 0u, 1u, length - 1 })
        AOT_TEST_CHECK(nodesReachableFrom(chain.program(), ExpectedResultID(0u), start).bitCount() == length);
}

static void testUndefinedAcceptance()
{
    Graph graph({
        { unknown },
        { "Undefined"_s },
        { "AnyObject"_s },
        { "Union{arms=[2, 1]}"_s },
        { "Union{arms=[2, 3]}"_s },
        { "Union{arms=[2, 5]}"_s },
        { unknown, defaultRule },
        { "Union{arms=[2, 1]}"_s, defaultRule },
        { "Union{arms=[2, 6]}"_s },
        { "Union{arms=[2, 7]}"_s },
        { "Object{members=[{key=\"m0\"}, {key=\"m1\" node=1}, {key=\"m2\" node=2}, {key=\"m3\" node=3}, {key=\"m4\" node=4}, {key=\"m5\" node=5}, {key=\"m6\" node=6}, {key=\"m7\" node=7}, {key=\"m8\" node=8}, {key=\"m9\" node=9}]}"_s },
    });
    ResultNode object = graph.node(10);
    Vector<uint32_t> accepting;
    for (const ResultMember& member : object.members) {
        if (member.mustBeOwn)
            accepting.append(member.node.part);
    }
    AOT_TEST_CHECK(object.members.size() == 10 && accepting == Vector<uint32_t>({ 0, 1, 3, 4 }));
}

static void testAlternativesOfRecursiveUnion()
{
    Graph graph({
        { "Union{arms=[1, 2, 0]}"_s },
        { "AnyObject"_s },
        { "Union{arms=[0, 3, 1]}"_s },
        { "Undefined"_s, defaultRule },
    });
    auto alternativesOf = [&](uint32_t root) {
        Vector<String> visited;
        forEachAlternative(graph.program(), graph.ref(root), [&](const ResultNode& node) {
            visited.append(graph.textOf(node.text));
        });
        std::ranges::sort(visited, codePointCompareLessThan);
        return visited;
    };
    AOT_TEST_CHECK(alternativesOf(0) == Vector<String>({ "n0"_s, "n1"_s, "n2"_s, "n3"_s }));
    AOT_TEST_CHECK(alternativesOf(2) == Vector<String>({ "n0"_s, "n1"_s, "n2"_s, "n3"_s }));
    AOT_TEST_CHECK(alternativesOf(1) == Vector<String>({ "n1"_s }));
    AOT_TEST_CHECK(alternativesOf(3) == Vector<String>({ "n3"_s }));
}

static void testOwnMembers()
{
    Graph graph({
        { "Object{members=[{key=\"a\" node=1}, {key=\"b\" node=3}, {key=\"c\" node=4}, {key=\"d\" node=5}, {key=\"e\" node=3 isOptional=true}]}"_s },
        { "AnyObject"_s },
        { "Undefined"_s },
        { "Union{arms=[1, 2]}"_s },
        { unknown },
        { unknown, defaultRule },
    });
    Vector<bool> mustBeOwn;
    for (const ResultMember& member : graph.node(0).members)
        mustBeOwn.append(member.mustBeOwn);
    AOT_TEST_CHECK(mustBeOwn == Vector<bool>({ false, true, true, false, false }));
}

static void testValueClassesPerKind()
{
    struct Row {
        ASCIILiteral shape;
        OptionSet<JSONValueClass> classes;
    };
    const Row rows[] = {
        { unknown, everyValueClass },
        { "Never"_s, { } },
        { "Undefined"_s, { } },
        { "Primitive{primitive=BigInt}"_s, { } },
        { "Primitive{}"_s, JSONValueClass::String },
        { "StringLiteral{value=\"a\"}"_s, JSONValueClass::String },
        { "Primitive{primitive=Number}"_s, JSONValueClass::Number },
        { "NumberLiteral{value=1}"_s, JSONValueClass::Number },
        { "Primitive{primitive=Boolean}"_s, JSONValueClass::Boolean },
        { "BooleanLiteral{value=true}"_s, JSONValueClass::Boolean },
        { "BooleanLiteral{}"_s, JSONValueClass::Boolean },
        { "Null"_s, JSONValueClass::Null },
        { "AnyObject"_s, { JSONValueClass::Array, JSONValueClass::Object } },
        { "Array{}"_s, JSONValueClass::Array },
        { "Tuple{}"_s, JSONValueClass::Array },
        { "Dictionary{}"_s, JSONValueClass::Object },
        { "Object{}"_s, JSONValueClass::Object },
    };
    for (const Row& row : rows) {
        Graph graph({ { row.shape } });
        AOT_TEST_CHECK(valueClassesOf(graph.program(), graph.ref(0)) == row.classes);
    }
}

static void testValueClassesOfRecursiveUnion()
{
    Graph graph({
        { "Union{arms=[1, 2, 0]}"_s },
        { "Primitive{}"_s },
        { "Union{arms=[0, 3]}"_s },
        { "Null"_s },
    });
    OptionSet<JSONValueClass> expected { JSONValueClass::String, JSONValueClass::Null };
    AOT_TEST_CHECK(valueClassesOf(graph.program(), graph.ref(0)) == expected);
    AOT_TEST_CHECK(valueClassesOf(graph.program(), graph.ref(2)) == expected);
    AOT_TEST_CHECK(valueClassesOf(graph.program(), graph.ref(3)) == JSONValueClass::Null);
}

static bool hasNoDiscriminant(Graph& graph, std::initializer_list<uint32_t> arms)
{
    Vector<ResultNodeRef, 4> refs = graph.refs(arms);
    return !findDiscriminant(graph.program(), refs.span()) && !findTaggedArms(graph.program(), refs.span());
}

static void testDiscriminantInThreeArms()
{
    Graph graph({
        { "StringLiteral{value=\"a\"}"_s },
        { "StringLiteral{value=\"b\"}"_s },
        { "StringLiteral{value=\"c\"}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{members=[{key=\"kind\" node=1}]}"_s },
        { "Object{members=[{key=\"kind\" node=2}]}"_s },
    });
    Vector<ResultNodeRef, 4> arms = graph.refs({ 3, 4, 5 });
    std::optional<ResultDiscriminant> found = findDiscriminant(graph.program(), arms.span());
    AOT_TEST_CHECK(found && found->key == graph.node(3).members[0].key && found->tagOfArm.size() == 3);
    AOT_TEST_CHECK(found->tagOfArm[1].kind == ResultLiteral::Kind::String && graph.textOf(found->tagOfArm[1].string) == "b"_s);

    std::optional<TaggedArms> tagged = findTaggedArms(graph.program(), arms.span());
    AOT_TEST_CHECK(tagged && tagged->tagged == arms && tagged->untagged.isEmpty() && tagged->discriminant.key == found->key);
}

static void testEqualTagsHaveNoDiscriminant()
{
    Graph graph({
        { "StringLiteral{value=\"a\"}"_s },
        { "StringLiteral{value=\"a\"}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{members=[{key=\"kind\" node=1}]}"_s },
    });
    AOT_TEST_CHECK(hasNoDiscriminant(graph, { 2, 3 }));
}

static void testZeroAndMinusZeroAreEqualTags()
{
    Graph graph({
        { "NumberLiteral{}"_s },
        { "NumberLiteral{value=-0}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{members=[{key=\"kind\" node=1}]}"_s },
    });
    AOT_TEST_CHECK(std::signbit(graph.node(1).literal.number) && !graph.node(1).literal.number);
    AOT_TEST_CHECK(hasNoDiscriminant(graph, { 2, 3 }));
}

static void testOptionalMemberIsNotTag()
{
    Graph graph({
        { "StringLiteral{value=\"a\"}"_s },
        { "StringLiteral{value=\"b\"}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{members=[{key=\"kind\" node=1 isOptional=true}]}"_s },
    });
    AOT_TEST_CHECK(hasNoDiscriminant(graph, { 2, 3 }));
}

static void testDefaultedMemberIsNotTag()
{
    Graph graph({
        { "StringLiteral{value=\"a\"}"_s },
        { "StringLiteral{value=\"b\"}"_s, "DefaultString{value=\"b\"}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{members=[{key=\"kind\" node=1}]}"_s },
    });
    AOT_TEST_CHECK(hasNoDiscriminant(graph, { 2, 3 }));
}

static void testConstrainedMemberIsNotTag()
{
    Graph graph({
        { "StringLiteral{value=\"a\"}"_s },
        { "StringLiteral{value=\"b\"}"_s, "MinimumLength{length=1}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{members=[{key=\"kind\" node=1}]}"_s },
    });
    AOT_TEST_CHECK(hasNoDiscriminant(graph, { 2, 3 }));
}

static void testMemberThatIsNoLiteralIsNotTag()
{
    Graph graph({
        { "StringLiteral{value=\"a\"}"_s },
        { "Primitive{}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{members=[{key=\"kind\" node=1}]}"_s },
    });
    AOT_TEST_CHECK(hasNoDiscriminant(graph, { 2, 3 }));
}

static void testArmWithoutMemberIsUntagged()
{
    Graph graph({
        { "StringLiteral{value=\"a\"}"_s },
        { "StringLiteral{value=\"b\"}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{}"_s },
        { "Object{members=[{key=\"kind\" node=1}]}"_s },
    });
    Vector<ResultNodeRef, 4> arms = graph.refs({ 2, 3, 4 });
    std::optional<TaggedArms> found = findTaggedArms(graph.program(), arms.span());
    AOT_TEST_CHECK(found && found->tagged == graph.refs({ 2, 4 }) && found->untagged == graph.refs({ 3 }));
    AOT_TEST_CHECK(found->discriminant.tagOfArm.size() == 2);
    AOT_TEST_CHECK(!findDiscriminant(graph.program(), arms.span()));
}

static void testOneTagHasNoDiscriminant()
{
    Graph graph({
        { "StringLiteral{value=\"a\"}"_s },
        { "Object{members=[{key=\"kind\"}]}"_s },
        { "Object{}"_s },
    });
    AOT_TEST_CHECK(hasNoDiscriminant(graph, { 1, 2 }));
    AOT_TEST_CHECK(hasNoDiscriminant(graph, { 1 }));
}

static void testFirstWorkingNameWins()
{
    Graph graph({
        { "StringLiteral{value=\"same\"}"_s },
        { "StringLiteral{value=\"a\"}"_s },
        { "StringLiteral{value=\"b\"}"_s },
        { "StringLiteral{value=\"x\"}"_s },
        { "StringLiteral{value=\"y\"}"_s },
        { "Object{members=[{key=\"shared\"}, {key=\"first\" node=1}, {key=\"second\" node=3}]}"_s },
        { "Object{members=[{key=\"shared\"}, {key=\"first\" node=2}, {key=\"second\" node=4}]}"_s },
    });
    Vector<ResultNodeRef, 4> arms = graph.refs({ 5, 6 });
    std::optional<ResultDiscriminant> found = findDiscriminant(graph.program(), arms.span());
    AOT_TEST_CHECK(found && found->key == graph.node(5).members[1].key);
}

static void testNumberAndBooleanTags()
{
    Graph numbers({
        { "NumberLiteral{value=1}"_s },
        { "NumberLiteral{value=2}"_s },
        { "Object{members=[{key=\"v\"}]}"_s },
        { "Object{members=[{key=\"v\" node=1}]}"_s },
    });
    std::optional<ResultDiscriminant> number = findDiscriminant(numbers.program(), numbers.refs({ 2, 3 }).span());
    AOT_TEST_CHECK(number && number->tagOfArm[1] == (ResultLiteral { .kind = ResultLiteral::Kind::Number, .number = 2 }));

    Graph booleans({
        { "BooleanLiteral{value=true}"_s },
        { "BooleanLiteral{}"_s },
        { "Object{members=[{key=\"ok\"}]}"_s },
        { "Object{members=[{key=\"ok\" node=1}]}"_s },
    });
    std::optional<ResultDiscriminant> boolean = findDiscriminant(booleans.program(), booleans.refs({ 2, 3 }).span());
    AOT_TEST_CHECK(boolean && boolean->tagOfArm[0] == (ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = true }));
}

static void testRuntimeClassesPerKind()
{
    struct Row {
        ASCIILiteral shape;
        OptionSet<RuntimeClass> classes;
    };
    const Row rows[] = {
        { unknown, everyRuntimeClass },
        { "Never"_s, { } },
        { "Undefined"_s, RuntimeClass::Undefined },
        { "Null"_s, RuntimeClass::Null },
        { "Primitive{primitive=Boolean}"_s, RuntimeClass::Boolean },
        { "BooleanLiteral{value=true}"_s, RuntimeClass::Boolean },
        { "Primitive{primitive=Number}"_s, RuntimeClass::Number },
        { "NumberLiteral{value=1}"_s, RuntimeClass::Number },
        { "Primitive{}"_s, RuntimeClass::String },
        { "StringLiteral{value=\"a\"}"_s, RuntimeClass::String },
        { "Primitive{primitive=BigInt}"_s, RuntimeClass::BigInt },
        { "AnyObject"_s, { RuntimeClass::Array, RuntimeClass::Object } },
        { "Array{}"_s, RuntimeClass::Array },
        { "Tuple{}"_s, RuntimeClass::Array },
        { "Dictionary{}"_s, RuntimeClass::Object },
        { "Object{}"_s, RuntimeClass::Object },
    };
    for (const Row& row : rows) {
        Graph graph({ { row.shape } });
        AOT_TEST_CHECK(runtimeClassesOf(graph.program(), graph.ref(0)) == row.classes);
        AOT_TEST_CHECK(!appliesDefault(graph.program(), graph.ref(0)));
    }
}

static void testRuntimeClassesOfRecursiveUnion()
{
    Graph graph({
        { "Union{arms=[1, 2, 0]}"_s },
        { "Primitive{primitive=Number}"_s },
        { "Union{arms=[0, 3, 4]}"_s },
        { "Null"_s },
        { "Array{}"_s },
        { "Union{arms=[1, 5]}"_s },
    });
    OptionSet<RuntimeClass> accepted { RuntimeClass::Number, RuntimeClass::Null, RuntimeClass::Array };
    AOT_TEST_CHECK(runtimeClassesOf(graph.program(), graph.ref(0)) == accepted);
    AOT_TEST_CHECK(runtimeClassesOf(graph.program(), graph.ref(2)) == accepted);
    AOT_TEST_CHECK(runtimeClassesOf(graph.program(), graph.ref(5)) == RuntimeClass::Number);
}

static void testNodeWithDefaultAcceptsUndefined()
{
    Graph graph({
        { "Union{arms=[1, 2]}"_s },
        { "Primitive{primitive=Number}"_s, defaultRule },
        { "Primitive{}"_s },
    });
    OptionSet<RuntimeClass> withUndefined { RuntimeClass::Number, RuntimeClass::Undefined };
    AOT_TEST_CHECK(runtimeClassesOf(graph.program(), graph.ref(1)) == withUndefined);
    AOT_TEST_CHECK(runtimeClassesOf(graph.program(), graph.ref(2)) == RuntimeClass::String);
    AOT_TEST_CHECK(runtimeClassesOf(graph.program(), graph.ref(0)) == (withUndefined | RuntimeClass::String));
}

static void testDefaultIsFoundThroughUnions()
{
    Graph graph({
        { "Union{arms=[1, 2]}"_s },
        { "Primitive{}"_s },
        { "Union{arms=[0, 3]}"_s },
        { "Primitive{primitive=Number}"_s, defaultRule },
        { "Array{element=3}"_s },
    });
    AOT_TEST_CHECK(appliesDefault(graph.program(), graph.ref(0)));
    AOT_TEST_CHECK(appliesDefault(graph.program(), graph.ref(2)));
    AOT_TEST_CHECK(appliesDefault(graph.program(), graph.ref(3)));
    AOT_TEST_CHECK(!appliesDefault(graph.program(), graph.ref(1)));
    AOT_TEST_CHECK(!appliesDefault(graph.program(), graph.ref(4)));
}

static constexpr NodeText belowNode { "Array{}"_s };
static constexpr NodeText numberNode { "Primitive{primitive=Number}"_s };
static constexpr NodeText tagANode { "StringLiteral{value=\"a\"}"_s };
static constexpr NodeText tagBNode { "StringLiteral{value=\"b\"}"_s };
static constexpr NodeText unionOfThreeArms { "Union{arms=[1, 2, 3]}"_s };
static constexpr NodeText leafArm { "Object{members=[{key=\"leaf\" node=5}]}"_s };

static void testTwoRecursiveArmsRepeatRecursion()
{
    Graph two({
        unionOfThreeArms,
        { "Object{members=[{key=\"below\" node=4}, {key=\"a\" node=5}]}"_s },
        { "Object{members=[{key=\"below\" node=4}, {key=\"b\" node=5}]}"_s },
        leafArm, belowNode, numberNode, tagANode, tagBNode,
    });
    AOT_TEST_CHECK(canRepeatRecursion(two.program(), two.ref(0)));

    Graph one({
        unionOfThreeArms,
        leafArm,
        { "Object{members=[{key=\"below\" node=4}, {key=\"a\" node=5}]}"_s },
        leafArm, belowNode, numberNode, tagANode, tagBNode,
    });
    AOT_TEST_CHECK(!canRepeatRecursion(one.program(), one.ref(0)));
}

// Level n is a union of two objects that no tag tells apart, and each holds level n - 1 under one name. Node 3 * n is the union of level n.
static String textOfUnionsBelowUnions(uint32_t levels, bool isTagged)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\"\nexpectedresult x0 type=anything validation=["_s);
    uint32_t number = 3 * levels;
    for (uint32_t level = 0; level < levels; ++level) {
        uint32_t below = level ? 3 * (level - 1) : number;
        text.append(level ? ", "_s : ""_s, "{shape=Union{arms=["_s, 3 * level + 1, ", "_s, 3 * level + 2, "]} type=anything}"_s);
        for (uint32_t arm = 0; arm < 2; ++arm)
            text.append(", {shape=Object{members=[{key=\"left\" node="_s, below, "}, {key=\"v\" node="_s, number + (isTagged ? 2 : 0) + arm, "}]} type=anything}"_s);
    }
    text.append(", {shape=Primitive{primitive=Number} type=anything}, {shape=Primitive{} type=anything}"_s);
    text.append(", {shape=StringLiteral{value=\"a\"} type=anything}, {shape=StringLiteral{value=\"b\"} type=anything}]\n"_s);
    return text.toString();
}

static void testUnionsBelowUnionsMultiply()
{
    auto repeats = [](ParsedProgram& parsed, uint32_t level) {
        return canRepeatRecursion(parsed.program(), ResultNodeRef::forValidation(ExpectedResultID(0u), 3 * level));
    };
    // Level n looks at the number of level 0 twice as often as level n - 1 does: 2, 4, 8 ..
    ParsedProgram untagged(textOfUnionsBelowUnions(40, false));
    for (uint32_t level = 0; level < 40; ++level)
        AOT_TEST_CHECK(repeats(untagged, level) == (level >= 6));

    ParsedProgram tagged(textOfUnionsBelowUnions(40, true));
    for (uint32_t level = 0; level < 40; ++level)
        AOT_TEST_CHECK(!repeats(tagged, level));

    // Where the type is too deep to count, the union is taken for one that multiplies.
    ParsedProgram deep(textOfUnionsBelowUnions(400, true));
    AOT_TEST_CHECK(!repeats(deep, 20) && repeats(deep, 399));
}

static void testArmsOutsideUnionCycleRepeatNothing()
{
    Graph graph({
        unionOfThreeArms,
        { "Object{members=[{key=\"a\" node=4}]}"_s },
        { "Object{members=[{key=\"b\" node=5}]}"_s },
        { "Object{members=[{key=\"c\" node=6}]}"_s },
        { "Array{element=1}"_s },
        { "Array{element=2}"_s },
        { "Array{element=3}"_s },
    });
    for (uint32_t arm : { 1u, 2u, 3u })
        AOT_TEST_CHECK(isOnCycle(graph, arm));
    AOT_TEST_CHECK(!isOnCycle(graph, 0) && !canRepeatRecursion(graph.program(), graph.ref(0)));
}

static void testOneTaggedArmIsTried()
{
    Graph withLeaf({
        unionOfThreeArms,
        { "Object{members=[{key=\"k\" node=6}, {key=\"below\" node=4}]}"_s },
        { "Object{members=[{key=\"k\" node=7}, {key=\"below\" node=4}]}"_s },
        leafArm, belowNode, numberNode, tagANode, tagBNode,
    });
    AOT_TEST_CHECK(!canRepeatRecursion(withLeaf.program(), withLeaf.ref(0)));

    Graph withRecursiveArm({
        unionOfThreeArms,
        { "Object{members=[{key=\"k\" node=6}, {key=\"below\" node=4}]}"_s },
        { "Object{members=[{key=\"k\" node=7}, {key=\"below\" node=4}]}"_s },
        { "Object{members=[{key=\"below\" node=4}]}"_s },
        belowNode, numberNode, tagANode, tagBNode,
    });
    AOT_TEST_CHECK(canRepeatRecursion(withRecursiveArm.program(), withRecursiveArm.ref(0)));
}

static void testRecursiveArmsOfDifferentClassesRepeatNothing()
{
    Graph graph({
        { "Union{arms=[1, 2, 3]}"_s },
        { "Object{members=[{key=\"below\"}]}"_s },
        { "Array{}"_s },
        { "Primitive{primitive=Number}"_s },
    });
    AOT_TEST_CHECK(isOnCycle(graph, 0) && !canRepeatRecursion(graph.program(), graph.ref(0)));
}

static constexpr ASCIILiteral numberRules = "Integer, Minimum{bound=1}, Maximum{bound=9}, ExclusiveMinimum{bound=0.5}, ExclusiveMaximum{bound=9.5}, MultipleOf{divisor=2}"_s;
static constexpr ASCIILiteral lengthRules = "MinimumLength{length=1}, MaximumLength{length=8}"_s;
static constexpr ASCIILiteral textRules = "Pattern{source=\"^a\"}, Format{format=Uuid}, Template{head=\"id-\" parts=[{placeholder=Number text=\"-\"}]}"_s;
static constexpr ASCIILiteral rulesThatTestNothing = "Message{text=\"bad\"}, DefaultNull, DefaultBoolean{value=true}, DefaultNumber{value=3}, DefaultString{value=\"d\"}"_s;

static constexpr ResultShape everyShape[] = {
    ResultShape::Unknown, ResultShape::Never, ResultShape::Undefined, ResultShape::Null, ResultShape::AnyObject, ResultShape::String, ResultShape::Number,
    ResultShape::Boolean, ResultShape::BigInt, ResultShape::BooleanLiteral, ResultShape::NumberLiteral, ResultShape::StringLiteral, ResultShape::Array,
    ResultShape::Tuple, ResultShape::Dictionary, ResultShape::Object, ResultShape::Union,
};

static void checkRules(ASCIILiteral rules, size_t numberOfRules, RuleSubject subject, std::initializer_list<ResultShape> shapes)
{
    Graph graph({ { unknown, rules } });
    ResultNode node = graph.node(0);
    AOT_TEST_CHECK(node.rules.size() == numberOfRules);
    for (const ValidationRule& rule : node.rules) {
        AOT_TEST_CHECK(subjectOf(rule) == subject);
        for (ResultShape shape : everyShape)
            AOT_TEST_CHECK(appliesTo(rule, shape) == (std::ranges::find(shapes, shape) != shapes.end()));
    }
}

static void testNumberRulesApplyToNumbers()
{
    checkRules(numberRules, 6, RuleSubject::Number, { ResultShape::Number, ResultShape::NumberLiteral });
}

static void testLengthRulesApplyToStringsAndArrays()
{
    checkRules(lengthRules, 2, RuleSubject::Length, { ResultShape::String, ResultShape::StringLiteral, ResultShape::Array, ResultShape::Tuple });
}

static void testTextRulesApplyToStrings()
{
    checkRules(textRules, 3, RuleSubject::Text, { ResultShape::String, ResultShape::StringLiteral });
}

static void testRulesThatTestNothingApplyEverywhere()
{
    Graph graph({ { unknown, rulesThatTestNothing } });
    ResultNode node = graph.node(0);
    AOT_TEST_CHECK(node.rules.size() == 5);
    for (const ValidationRule& rule : node.rules) {
        AOT_TEST_CHECK(subjectOf(rule) == RuleSubject::Nothing);
        for (ResultShape shape : everyShape)
            AOT_TEST_CHECK(appliesTo(rule, shape));
    }
}

static void testIntegerRuleIsFoundAmongOtherRules()
{
    Graph graph({
        { "Primitive{primitive=Number}"_s, "Minimum{bound=1}, Integer"_s },
        { "Primitive{primitive=Number}"_s, "Minimum{bound=1}"_s },
        { "Primitive{primitive=Number}"_s },
    });
    AOT_TEST_CHECK(hasIntegerRule(graph.node(0)) && !hasIntegerRule(graph.node(1)) && !hasIntegerRule(graph.node(2)));
}

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

    ResultNode whole = types.typeNode(0);
    AOT_TEST_CHECK(whole.shape == ResultShape::Union && whole.type == types.type(0) && whole.rules.empty() && !whole.defaultValue);
    AOT_TEST_CHECK(hasElements(whole.children, { ResultNodeRef::forType(types.type(0), 1), ResultNodeRef::forType(types.type(0), 2) }));
    AOT_TEST_CHECK(types.typeNode(0, 1).shape == ResultShape::Null && types.typeNode(0, 2).shape == ResultShape::String);
    AOT_TEST_CHECK(types.typeNode(0, 1).type == types.type(0) && types.typeNode(0, 2).type == types.type(0));
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
        AOT_TEST_CHECK(types.typeNode(1, index + 1).literal == alternatives[index].literal);
    }
    AOT_TEST_CHECK(types.typeNode(1).shape == ResultShape::Union && types.typeNode(1).children.size() == 2);

    Vector<TypeAlternative, 4> mixed = alternativesOf(types.program(), types.type(13));
    AOT_TEST_CHECK(shapesOf(mixed) == Vector<ResultShape>({ ResultShape::BooleanLiteral, ResultShape::StringLiteral }));
    AOT_TEST_CHECK(mixed[0].literal == (ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = false }) && types.textOf(mixed[1].literal.string) == "x"_s);
}

static void testAlternativesOfArray()
{
    ParsedProgram types { String(typesText) };
    Vector<TypeAlternative, 4> alternatives = alternativesOf(types.program(), types.type(2));
    AOT_TEST_CHECK(shapesOf(alternatives) == Vector<ResultShape>({ ResultShape::Array }));
    ResultNode array = types.typeNode(2);
    AOT_TEST_CHECK(array.shape == ResultShape::Array && hasElements(array.children, { ResultNodeRef::forType(alternatives[0].contents) }));
    ResultNode element = describeNode(types.program(), array.children[0]);
    AOT_TEST_CHECK(element.shape == ResultShape::Object && element.layout == LayoutID(0u));
}

static void testHeadLayoutsOfSetWithLiteralMembers()
{
    ParsedProgram types { String(typesText) };
    Vector<TypeAlternative, 4> alternatives = alternativesOf(types.program(), types.type(3));
    AOT_TEST_CHECK(alternatives.size() == 1 && alternatives[0].shape == ResultShape::Object && alternatives[0].layout == LayoutID(0u));
    AOT_TEST_CHECK(types.typeNode(3).shape == ResultShape::Object && types.typeNode(3).layout == LayoutID(0u));

    Vector<TypeAlternative, 4> unrelated = alternativesOf(types.program(), types.type(12));
    AOT_TEST_CHECK(unrelated.size() == 2 && unrelated[0].layout == LayoutID(0u) && unrelated[1].layout == LayoutID(3u));
    AOT_TEST_CHECK(types.typeNode(12).shape == ResultShape::Union);
}

static void testTypesThatTextCannotProduce()
{
    ParsedProgram types { String(typesText) };
    AOT_TEST_CHECK(types.typeNode(4).shape == ResultShape::Unknown && types.typeNode(4).type == TypeTable::anything);
    AOT_TEST_CHECK(types.typeNode(10).shape == ResultShape::Unknown);
    AOT_TEST_CHECK(alternativesOf(types.program(), types.type(5)).isEmpty() && types.typeNode(5).shape == ResultShape::Unknown);
    AOT_TEST_CHECK(alternativesOf(types.program(), types.type(6)).isEmpty() && types.typeNode(6).shape == ResultShape::Unknown);
}

static void testOrderOfAlternatives()
{
    ParsedProgram types { String(typesText) };
    AOT_TEST_CHECK(shapesOf(alternativesOf(types.program(), types.type(7))) == Vector<ResultShape>({ ResultShape::Null, ResultShape::Boolean, ResultShape::Number, ResultShape::String, ResultShape::Array, ResultShape::Object }));
    AOT_TEST_CHECK(types.typeNode(7).children.size() == 6);

    Vector<TypeAlternative, 4> onlyTrue = alternativesOf(types.program(), types.type(8));
    AOT_TEST_CHECK(onlyTrue.size() == 1 && onlyTrue[0].shape == ResultShape::BooleanLiteral && onlyTrue[0].literal.boolean);
    AOT_TEST_CHECK(types.typeNode(8).shape == ResultShape::BooleanLiteral);
    AOT_TEST_CHECK(shapesOf(alternativesOf(types.program(), types.type(9))) == Vector<ResultShape>({ ResultShape::Number }));
}

static void testMembersOfLayout()
{
    ParsedProgram types { String(typesText) };
    const Layout& layout = types.program().at(LayoutID(0u));
    ResultNode object = types.typeNode(3);
    AOT_TEST_CHECK(object.undeclared == UndeclaredMembers::Allowed && !object.undeclaredValue && object.children.isEmpty());
    AOT_TEST_CHECK(object.members.size() == 4);
    constexpr uint32_t fields[] = { 0, 1, 3, 4 };
    constexpr bool isOptional[] = { false, true, true, true };
    for (unsigned index = 0; index < 4; ++index) {
        const ResultMember& member = object.members[index];
        const Field& field = layout.fields[fields[index]];
        AOT_TEST_CHECK(member.field == fields[index] && member.key == field.key && !member.mustBeOwn);
        AOT_TEST_CHECK(member.isOptional == isOptional[index]);
        AOT_TEST_CHECK(member.node == ResultNodeRef::forType(expectationOf(field.declared)));
    }
    AOT_TEST_CHECK(expectationOf(layout.fields[0].declared) == layout.fields[0].declared.type);
    AOT_TEST_CHECK(layout.fields[4].declared.flags.contains(PositionFlags::IsExpected) && expectationOf(layout.fields[4].declared) == layout.fields[4].declared.type);
    AOT_TEST_CHECK(describeNode(types.program(), object.members[2].node).layout == LayoutID(0u));
    AOT_TEST_CHECK(describeNode(types.program(), object.members[3].node).shape == ResultShape::Number);
}

static constexpr ASCIILiteral recordsText = R"IR(module m0 path="main.js" displayPath="main.js"
expectedresult x0 type=number validation=[{shape=Array{element=1} type=Array<number>}, {shape=Primitive{primitive=Number} type=number rules=[Minimum{bound=1}]}]
expectedresult x1 type=number validation=[{shape=Array{element=1} type=Array<number>}, {shape=Primitive{primitive=Number} type=number rules=[Minimum{bound=2}]}]
expectedresult x2 type=number validation=[{shape=Array{element=1} type=Array<number>}, {shape=Primitive{primitive=Number} type=number rules=[Minimum{bound=1}]}]
expectedresult x3 type=anything
expectedresult x4 type=number
expectedresult x5 type=L0
expectedresult x6 type=Array<L0>
layout L0 name="User" fields=[{key="id" declared={type=number}}]
)IR"_s;

static void testKeysLeadBackToTheirNodes()
{
    ParsedProgram records { String(recordsText) };
    for (ResultNodeRef ref : { ResultNodeRef::forType(records.type(5)), ResultNodeRef::forType(records.type(6)), records.ref(0, 0), records.ref(1, 0), records.ref(1, 2) }) {
        DerivedFunctionKey key = parserKeyOf(ref);
        AOT_TEST_CHECK(key.role == (ref.isValidated() ? FunctionRole::ValidatingParser : FunctionRole::Parser));
        AOT_TEST_CHECK(nodeOfKey(key) == ref);
    }
    for (ResultNodeRef ref : { records.ref(0, 0), records.ref(1, 0), records.ref(0, 1) }) {
        DerivedFunctionKey key = validatorKeyOf(ref);
        AOT_TEST_CHECK(key.role == FunctionRole::Validator && nodeOfKey(key) == ref);
        AOT_TEST_CHECK(key != parserKeyOf(ref));
    }
    AOT_TEST_CHECK(parserKeyOf(records.ref(0, 0)) != parserKeyOf(records.ref(1, 0)) && parserKeyOf(records.ref(0, 0)) != parserKeyOf(records.ref(0, 1)));
    AOT_TEST_CHECK(parserKeyOf(ResultNodeRef::forType(records.type(5))) != parserKeyOf(ResultNodeRef::forType(records.type(6))));
}

static void testCanonicalValidation()
{
    ParsedProgram records { String(recordsText) };
    auto canonicalOf = [&](uint32_t index) {
        return canonicalValidationOf(records.program(), ExpectedResultID(index)).index();
    };
    AOT_TEST_CHECK(canonicalOf(0) == 0 && canonicalOf(1) == 1 && canonicalOf(2) == 0);
    AOT_TEST_CHECK(canonicalOf(3) == 3 && canonicalOf(4) == 3);
}

static bool isCompiled(NodeDecision decision)
{
    return decision.code == NodeCode::Compiled && decision.cause == InterpretedCause::None;
}

static bool isInterpretedFor(NodeDecision decision, InterpretedCause cause)
{
    return decision.code == NodeCode::Interpreted && decision.cause == cause;
}

static void testNodesFromTheLimitOnAreInterpreted()
{
    std::initializer_list<NodeText> nodes {
        { "Object{members=[{key=\"a\" node=1}, {key=\"b\" node=2}, {key=\"c\" node=3}]}"_s },
        { "Primitive{primitive=Number}"_s },
        { "Array{element=1}"_s },
        { "Primitive{}"_s },
    };
    Graph limited(nodes, { "limit=MaxParserPlanNodes:2"_s });
    for (NodeUse use : { NodeUse::ParsedFromText, NodeUse::CopiedFromValue }) {
        AOT_TEST_CHECK(isCompiled(limited.decide(limited.ref(0), use)) && isCompiled(limited.decide(limited.ref(1), use)));
        AOT_TEST_CHECK(isInterpretedFor(limited.decide(limited.ref(2), use), InterpretedCause::TooManyNodes));
        AOT_TEST_CHECK(isInterpretedFor(limited.decide(limited.ref(3), use), InterpretedCause::TooManyNodes));
    }
    Graph unlimited(nodes);
    for (uint32_t node = 0; node < 4; ++node)
        AOT_TEST_CHECK(isCompiled(unlimited.decide(unlimited.ref(node))));
}

static void testUnionThatRepeatsRecursionIsInterpreted()
{
    Graph graph({
        unionOfThreeArms,
        { "Object{members=[{key=\"below\" node=4}, {key=\"a\" node=5}]}"_s },
        { "Object{members=[{key=\"below\" node=4}, {key=\"b\" node=5}]}"_s },
        leafArm, belowNode, numberNode,
        { "Union{arms=[3, 5]}"_s },
    });
    for (NodeUse use : { NodeUse::ParsedFromText, NodeUse::CopiedFromValue }) {
        AOT_TEST_CHECK(isInterpretedFor(graph.decide(graph.ref(0), use), InterpretedCause::RepeatedRecursion));
        for (uint32_t neighbor = 1; neighbor < 7; ++neighbor)
            AOT_TEST_CHECK(isCompiled(graph.decide(graph.ref(neighbor), use)));
    }
}

static void testObjectWithSymbolKeyIsInterpreted()
{
    Graph graph({
        { "Object{members=[{key=\"inner\" node=1}, {key=\"n\" node=2}]}"_s },
        { "Object{members=[{key=\"n\" node=2}, {key=symbol(\"tag\")@m0:120 node=2}]}"_s },
        { "Primitive{primitive=Number}"_s },
    });
    for (NodeUse use : { NodeUse::ParsedFromText, NodeUse::CopiedFromValue }) {
        AOT_TEST_CHECK(isInterpretedFor(graph.decide(graph.ref(1), use), InterpretedCause::MemberKeyIsNotText));
        AOT_TEST_CHECK(isCompiled(graph.decide(graph.ref(0), use)) && isCompiled(graph.decide(graph.ref(2), use)));
    }
}

static void testLayoutThatIsNeverAllocatedIsInterpreted()
{
    ParsedProgram program { String(R"IR(module m0 path="main.js" displayPath="main.js"
expectedresult x0 type=L0 layouts=[L0] validation=[{shape=Object{layout=L0 members=[{key="id" node=1 field=0}]} type=L0}, {shape=Primitive{primitive=Number} type=number}, {shape=Object{members=[{key="id" node=1}]} type=object}]
expectedresult x1 type=L1
expectedresult x2 type=Array<L0>
expectedresult x3 type=number
layout L0 name="NeverMade" fields=[{key="id" declared={type=number}}]
layout L1 name="Made" fields=[{key="id" declared={type=number}}]
)IR"_s) };
    program.program().layouts[0].flow.allocation = AllocationState::NeverAllocated;
    AOT_TEST_CHECK(program.program().layouts[1].flow.allocation == AllocationState::Unknown);

    AOT_TEST_CHECK(isInterpretedFor(program.decide(ResultNodeRef::forType(program.type(0))), InterpretedCause::LayoutIsNotModeled));
    AOT_TEST_CHECK(isCompiled(program.decide(ResultNodeRef::forType(program.type(1)))));
    AOT_TEST_CHECK(isCompiled(program.decide(ResultNodeRef::forType(program.type(2)))));
    AOT_TEST_CHECK(isCompiled(program.decide(ResultNodeRef::forType(program.type(3)))));
    for (NodeUse use : { NodeUse::ParsedFromText, NodeUse::CopiedFromValue }) {
        for (uint32_t node = 0; node < 3; ++node)
            AOT_TEST_CHECK(isCompiled(program.decide(program.ref(node), use)));
    }
}

std::span<const NativeTest> resultNodeTests()
{
    static constexpr NativeTest tests[] = {
        { "result-nodes/shapes"_s, testShapes },
        { "result-nodes/first-default-and-first-message-count"_s, testFirstDefaultAndFirstMessageCount },
        { "result-nodes/nodes-on-cycles"_s, testNodesOnCycles },
        { "result-nodes/long-chain-needs-no-stack"_s, testLongChainNeedsNoStack },
        { "result-nodes/undefined-acceptance"_s, testUndefinedAcceptance },
        { "result-nodes/alternatives-of-recursive-union"_s, testAlternativesOfRecursiveUnion },
        { "result-nodes/own-members"_s, testOwnMembers },
        { "result-nodes/value-classes-per-kind"_s, testValueClassesPerKind },
        { "result-nodes/value-classes-of-recursive-union"_s, testValueClassesOfRecursiveUnion },
        { "result-nodes/discriminant-in-three-arms"_s, testDiscriminantInThreeArms },
        { "result-nodes/equal-tags-have-no-discriminant"_s, testEqualTagsHaveNoDiscriminant },
        { "result-nodes/zero-and-minus-zero-are-equal-tags"_s, testZeroAndMinusZeroAreEqualTags },
        { "result-nodes/optional-member-is-not-tag"_s, testOptionalMemberIsNotTag },
        { "result-nodes/defaulted-member-is-not-tag"_s, testDefaultedMemberIsNotTag },
        { "result-nodes/constrained-member-is-not-tag"_s, testConstrainedMemberIsNotTag },
        { "result-nodes/member-that-is-no-literal-is-not-tag"_s, testMemberThatIsNoLiteralIsNotTag },
        { "result-nodes/arm-without-member-is-untagged"_s, testArmWithoutMemberIsUntagged },
        { "result-nodes/one-tag-has-no-discriminant"_s, testOneTagHasNoDiscriminant },
        { "result-nodes/first-working-name-wins"_s, testFirstWorkingNameWins },
        { "result-nodes/number-and-boolean-tags"_s, testNumberAndBooleanTags },
        { "result-nodes/runtime-classes-per-kind"_s, testRuntimeClassesPerKind },
        { "result-nodes/runtime-classes-of-recursive-union"_s, testRuntimeClassesOfRecursiveUnion },
        { "result-nodes/node-with-default-accepts-undefined"_s, testNodeWithDefaultAcceptsUndefined },
        { "result-nodes/default-is-found-through-unions"_s, testDefaultIsFoundThroughUnions },
        { "result-nodes/two-recursive-arms-repeat-recursion"_s, testTwoRecursiveArmsRepeatRecursion },
        { "result-nodes/unions-below-unions-multiply"_s, testUnionsBelowUnionsMultiply },
        { "result-nodes/arms-outside-union-cycle-repeat-nothing"_s, testArmsOutsideUnionCycleRepeatNothing },
        { "result-nodes/one-tagged-arm-is-tried"_s, testOneTaggedArmIsTried },
        { "result-nodes/recursive-arms-of-different-classes-repeat-nothing"_s, testRecursiveArmsOfDifferentClassesRepeatNothing },
        { "result-nodes/number-rules-apply-to-numbers"_s, testNumberRulesApplyToNumbers },
        { "result-nodes/length-rules-apply-to-strings-and-arrays"_s, testLengthRulesApplyToStringsAndArrays },
        { "result-nodes/text-rules-apply-to-strings"_s, testTextRulesApplyToStrings },
        { "result-nodes/rules-that-test-nothing-apply-everywhere"_s, testRulesThatTestNothingApplyEverywhere },
        { "result-nodes/integer-rule-is-found-among-other-rules"_s, testIntegerRuleIsFoundAmongOtherRules },
        { "result-nodes/type-nodes/alternatives-of-string-or-null"_s, testAlternativesOfStringOrNull },
        { "result-nodes/type-nodes/alternatives-of-literal-union"_s, testAlternativesOfLiteralUnion },
        { "result-nodes/type-nodes/alternatives-of-array"_s, testAlternativesOfArray },
        { "result-nodes/type-nodes/head-layouts-of-set-with-literal-members"_s, testHeadLayoutsOfSetWithLiteralMembers },
        { "result-nodes/type-nodes/types-that-text-cannot-produce"_s, testTypesThatTextCannotProduce },
        { "result-nodes/type-nodes/order-of-alternatives"_s, testOrderOfAlternatives },
        { "result-nodes/type-nodes/members-of-layout"_s, testMembersOfLayout },
        { "result-nodes/keys/keys-lead-back-to-their-nodes"_s, testKeysLeadBackToTheirNodes },
        { "result-nodes/keys/canonical-validation"_s, testCanonicalValidation },
        { "result-nodes/decisions/nodes-from-the-limit-on-are-interpreted"_s, testNodesFromTheLimitOnAreInterpreted },
        { "result-nodes/decisions/union-that-repeats-recursion-is-interpreted"_s, testUnionThatRepeatsRecursionIsInterpreted },
        { "result-nodes/decisions/object-with-symbol-key-is-interpreted"_s, testObjectWithSymbolKeyIsInterpreted },
        { "result-nodes/decisions/layout-that-is-never-allocated-is-interpreted"_s, testLayoutThatIsNeverAllocatedIsInterpreted },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
