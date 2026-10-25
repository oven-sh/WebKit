/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTIRTestSupport.h"
#include "AOTProgramFile.h"
#include "AOTStringArena.h"
#include "AOTValidationPlanText.h"
#include "AOTValidationRules.h"
#include "AOTVector.h"
#include "YarrInterpreter.h"
#include <wtf/BumpPointerAllocator.h>
#include <wtf/DataLog.h>
#include <wtf/JSONValues.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NodeText {
    ASCIILiteral shape;
    ASCIILiteral rules { ""_s };
    ASCIILiteral text { ""_s };
};

constexpr ASCIILiteral unknown = ""_s;
constexpr NodeText numberNode { "Primitive{primitive=Number}"_s };
constexpr NodeText stringNode { "Primitive{}"_s };

class Graph {
    WTF_MAKE_NONCOPYABLE(Graph);
public:
    Graph(std::initializer_list<NodeText> nodes)
        : m_build(makeDefaultCompileOptions())
    {
        StringBuilder text;
        text.append("module m0 path=\"main.js\" displayPath=\"main.js\"\nexpectedresult x0 type=anything validation=["_s);
        bool isFirst = true;
        for (const NodeText& node : nodes) {
            text.append(std::exchange(isFirst, false) ? "{"_s : ", {"_s);
            if (!node.shape.isEmpty())
                text.append("shape="_s, node.shape, ' ');
            text.append("type=anything"_s);
            if (!node.rules.isEmpty())
                text.append(" rules=["_s, node.rules, ']');
            if (!node.text.isEmpty())
                text.append(" text=\""_s, node.text, '"');
            text.append('}');
        }
        text.append("]\n"_s);
        m_program = parseProgram(text.toString(), IRForm::Prepared, m_build);
        if (!m_program) {
            m_build.diagnostics().finish(nullptr);
            printDiagnostics(m_build.diagnostics(), WTF::dataFile());
            dataLogLn(text.toString());
        }
        AOT_TEST_CHECK(m_program);
    }

    Program& program() { return *m_program; }

    String plan(uint32_t root = 0)
    {
        return String::fromUTF8(writeValidationPlan(program(), ExpectedResultID(0u), root).span());
    }

    Vector<uint8_t> planBytes() { return writeValidationPlan(program(), ExpectedResultID(0u), 0); }

    String rule(uint32_t index)
    {
        return String::fromUTF8(writeValidationRule(program(), program().at(ExpectedResultID(0u)).validation[0].rules[index]).span());
    }

private:
    BuildContext m_build;
    std::unique_ptr<Program> m_program;
};

bool hasText(const String& actual, const String& expected)
{
    if (actual == expected)
        return true;
    dataLogLn("the text is\n", actual, "\nexpected\n", expected);
    return false;
}

bool hasPlan(std::initializer_list<NodeText> nodes, const String& expected)
{
    Graph graph(nodes);
    return hasText(graph.plan(), expected);
}

} // anonymous namespace

static void testPlanListsNodesInOrder()
{
    AOT_TEST_CHECK(hasPlan({
        { "Object{members=[{key=\"id\" node=1}, {key=\"name\" node=2}, {key=\"tags\" node=3}]}"_s, ""_s, "ProbeOrder"_s },
        { "Primitive{primitive=Number}"_s, "Integer, Minimum{bound=1}"_s },
        stringNode,
        { "Array{element=2}"_s },
    }, makeString(
        R"([{"shape":"Object","members":[{"key":"id","node":1,"isOptional":false,"mustBeOwn":false},{"key":"name","node":2,"isOptional":false,"mustBeOwn":false},)"_s,
        R"({"key":"tags","node":3,"isOptional":false,"mustBeOwn":false}],"undeclared":"Allowed","text":"ProbeOrder"},)"_s,
        R"({"shape":"Primitive","primitive":"number","rules":[{"rule":"Integer"},{"rule":"Minimum","bound":1}]},)"_s,
        R"({"shape":"Primitive","primitive":"string"},)"_s,
        R"({"shape":"Array","element":2}])"_s)));
}

static void testPlanHoldsOnlyWhatIsBelowItsRoot()
{
    Graph graph({
        { "Object{members=[{key=\"a\" node=1}, {key=\"b\" node=4}]}"_s },
        { "Array{element=3}"_s },
        stringNode,
        { "Tuple{elements=[5, 2, 1] required=3}"_s },
        numberNode,
        { "Null"_s },
    });
    AOT_TEST_CHECK(hasText(graph.plan(3), R"([{"shape":"Tuple","elements":[3,2,1],"required":3},{"shape":"Array","element":0},{"shape":"Primitive","primitive":"string"},{"shape":"Null"}])"_s));
    AOT_TEST_CHECK(hasText(graph.plan(1), R"([{"shape":"Array","element":2},{"shape":"Primitive","primitive":"string"},{"shape":"Tuple","elements":[3,1,0],"required":3},{"shape":"Null"}])"_s));
    AOT_TEST_CHECK(hasText(graph.plan(4), R"([{"shape":"Primitive","primitive":"number"}])"_s));
}

static void testShapesWithoutDetail()
{
    AOT_TEST_CHECK(hasPlan({ { unknown } }, R"([{"shape":"Unknown"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Never"_s } }, R"([{"shape":"Never"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "AnyObject"_s } }, R"([{"shape":"AnyObject"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Undefined"_s } }, R"([{"shape":"Undefined"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Null"_s } }, R"([{"shape":"Null"}])"_s));
}

static void testPrimitiveIsTypeofWord()
{
    AOT_TEST_CHECK(hasPlan({ { "Primitive{}"_s } }, R"([{"shape":"Primitive","primitive":"string"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Primitive{primitive=Number}"_s } }, R"([{"shape":"Primitive","primitive":"number"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Primitive{primitive=Boolean}"_s } }, R"([{"shape":"Primitive","primitive":"boolean"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Primitive{primitive=BigInt}"_s } }, R"([{"shape":"Primitive","primitive":"bigint"}])"_s));
}

static void testLiteralIsJSONValue()
{
    AOT_TEST_CHECK(hasPlan({ { "StringLiteral{value=\"a\"}"_s } }, R"([{"shape":"StringLiteral","value":"a"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "StringLiteral{value=\"\"}"_s } }, R"([{"shape":"StringLiteral","value":""}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "NumberLiteral{value=1.5}"_s } }, R"([{"shape":"NumberLiteral","value":1.5}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "NumberLiteral{}"_s } }, R"([{"shape":"NumberLiteral","value":0}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "BooleanLiteral{value=true}"_s } }, R"([{"shape":"BooleanLiteral","value":true}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "BooleanLiteral{}"_s } }, R"([{"shape":"BooleanLiteral","value":false}])"_s));
}

static void testContainersNameTheirNodes()
{
    constexpr ASCIILiteral number = R"({"shape":"Primitive","primitive":"number"})"_s;
    constexpr ASCIILiteral string = R"({"shape":"Primitive","primitive":"string"})"_s;
    AOT_TEST_CHECK(hasPlan({ { "Array{element=1}"_s }, numberNode, stringNode }, makeString(R"([{"shape":"Array","element":1},)"_s, number, ']')));
    AOT_TEST_CHECK(hasPlan({ { "Tuple{elements=[1, 2, 1] required=3}"_s }, numberNode, stringNode }, makeString(R"([{"shape":"Tuple","elements":[1,2,1],"required":3},)"_s, number, ',', string, ']')));
    AOT_TEST_CHECK(hasPlan({ { "Tuple{}"_s }, numberNode, stringNode }, R"([{"shape":"Tuple","required":0}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Tuple{elements=[1, 2] required=1 rest=1}"_s }, numberNode, stringNode }, makeString(R"([{"shape":"Tuple","elements":[1,2],"required":1,"rest":1},)"_s, number, ',', string, ']')));
    AOT_TEST_CHECK(hasPlan({ { "Tuple{elements=[1]}"_s }, numberNode, stringNode }, makeString(R"([{"shape":"Tuple","elements":[1],"required":0},)"_s, number, ']')));
    AOT_TEST_CHECK(hasPlan({ { "Tuple{rest=2}"_s }, numberNode, stringNode }, makeString(R"([{"shape":"Tuple","required":0,"rest":1},)"_s, string, ']')));
    AOT_TEST_CHECK(hasPlan({ { "Dictionary{value=2}"_s }, numberNode, stringNode }, makeString(R"([{"shape":"Dictionary","element":1},)"_s, string, ']')));
    AOT_TEST_CHECK(hasPlan({ { "Union{arms=[1, 2]}"_s }, numberNode, stringNode }, makeString(R"([{"shape":"Union","arms":[1,2]},)"_s, number, ',', string, ']')));
    AOT_TEST_CHECK(hasPlan({ { "Union{}"_s }, numberNode, stringNode }, R"([{"shape":"Union"}])"_s));
}

static void testEveryMemberHasFourMembers()
{
    AOT_TEST_CHECK(hasPlan({
        { "Object{members=[{key=\"plain\" node=1}, {key=\"note\" node=1 isOptional=true}, {key=\"own\" node=2}, {key=\"maybe\" node=2 isOptional=true}]}"_s },
        numberNode,
        { unknown },
    }, makeString(
        R"([{"shape":"Object","members":[{"key":"plain","node":1,"isOptional":false,"mustBeOwn":false},{"key":"note","node":1,"isOptional":true,"mustBeOwn":false},)"_s,
        R"({"key":"own","node":2,"isOptional":false,"mustBeOwn":true},{"key":"maybe","node":2,"isOptional":true,"mustBeOwn":false}],"undeclared":"Allowed"},)"_s,
        R"({"shape":"Primitive","primitive":"number"},{"shape":"Unknown"}])"_s)));
}

static void testEveryObjectSaysWhatUndeclaredMembersDo()
{
    AOT_TEST_CHECK(hasPlan({ { "Object{}"_s }, numberNode }, R"([{"shape":"Object","undeclared":"Allowed"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Object{undeclared=Rejected}"_s }, numberNode }, R"([{"shape":"Object","undeclared":"Rejected"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Object{undeclared=Validated undeclaredValue=1}"_s }, numberNode }, R"([{"shape":"Object","undeclared":"Validated","undeclaredValue":1},{"shape":"Primitive","primitive":"number"}])"_s));
}

static void testUnionOnCycleRemembers()
{
    AOT_TEST_CHECK(hasPlan({
        { "Union{arms=[1, 2]}"_s },
        { "Object{members=[{key=\"next\"}]}"_s },
        { "Null"_s },
    }, makeString(
        R"([{"shape":"Union","arms":[1,2],"remembers":true},)"_s,
        R"({"shape":"Object","members":[{"key":"next","node":0,"isOptional":false,"mustBeOwn":false}],"undeclared":"Allowed"},)"_s,
        R"({"shape":"Null"}])"_s)));

    AOT_TEST_CHECK(hasPlan({
        { "Union{arms=[1, 2]}"_s },
        { "Object{members=[{key=\"next\" node=2}]}"_s },
        { "Null"_s },
    }, makeString(
        R"([{"shape":"Union","arms":[1,2]},)"_s,
        R"({"shape":"Object","members":[{"key":"next","node":2,"isOptional":false,"mustBeOwn":false}],"undeclared":"Allowed"},)"_s,
        R"({"shape":"Null"}])"_s)));
}

static void testOtherShapesOnCyclesRememberNothing()
{
    AOT_TEST_CHECK(hasPlan({ { "Array{}"_s } }, R"([{"shape":"Array","element":0}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Dictionary{}"_s } }, R"([{"shape":"Dictionary","element":0}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Tuple{elements=[0] required=1}"_s } }, R"([{"shape":"Tuple","elements":[0],"required":1}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Tuple{rest=0}"_s } }, R"([{"shape":"Tuple","required":0,"rest":0}])"_s));
    AOT_TEST_CHECK(hasPlan({ { unknown, "Not{node=0}"_s } }, R"([{"shape":"Unknown","rules":[{"rule":"Not","node":0}]}])"_s));
}

static void testEveryDiscriminantHasFiveMembers()
{
    constexpr ASCIILiteral tail =
        R"({"shape":"Object","members":[{"key":"kind","node":4,"isOptional":false,"mustBeOwn":false}],"undeclared":"Allowed"},)"
        R"({"shape":"Object","members":[{"key":"kind","node":5,"isOptional":false,"mustBeOwn":false}],"undeclared":"Allowed"},)"
        R"({"shape":"Object","undeclared":"Allowed"},)"
        R"({"shape":"StringLiteral","value":"a"},{"shape":"StringLiteral","value":"b"}])"_s;
    AOT_TEST_CHECK(hasPlan({
        { "Union{arms=[1, 2, 3]}"_s },
        { "Object{members=[{key=\"kind\" node=4}]}"_s },
        { "Object{members=[{key=\"kind\" node=5}]}"_s },
        { "Object{}"_s },
        { "StringLiteral{value=\"a\"}"_s },
        { "StringLiteral{value=\"b\"}"_s },
    }, makeString(R"([{"shape":"Union","arms":[1,2,3],"discriminant":{"key":"kind","tags":["a","b"],"tagged":[1,2],"untagged":[3],"expected":"\"a\" | \"b\""}},)"_s, tail)));

    AOT_TEST_CHECK(hasPlan({
        { "Union{arms=[1, 2]}"_s },
        { "Object{members=[{key=\"v\" node=3}]}"_s },
        { "Object{members=[{key=\"v\" node=4}]}"_s },
        { "NumberLiteral{value=1}"_s },
        { "BooleanLiteral{}"_s },
    }, makeString(
        R"([{"shape":"Union","arms":[1,2],"discriminant":{"key":"v","tags":[1,false],"tagged":[1,2],"untagged":[],"expected":"1 | false"}},)"_s,
        R"({"shape":"Object","members":[{"key":"v","node":3,"isOptional":false,"mustBeOwn":false}],"undeclared":"Allowed"},)"_s,
        R"({"shape":"Object","members":[{"key":"v","node":4,"isOptional":false,"mustBeOwn":false}],"undeclared":"Allowed"},)"_s,
        R"({"shape":"NumberLiteral","value":1},{"shape":"BooleanLiteral","value":false}])"_s)));
}

static void testMessageAndDefaultAreNoRules()
{
    AOT_TEST_CHECK(hasPlan({ { "Primitive{primitive=Number}"_s, "Message{text=\"say \\\"when\\\"\"}, DefaultNumber{value=1}"_s } },
        R"([{"shape":"Primitive","primitive":"number","message":"say \"when\"","default":1}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Null"_s, "DefaultNull"_s } }, R"([{"shape":"Null","default":null}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Primitive{primitive=Boolean}"_s, "DefaultBoolean{}"_s } }, R"([{"shape":"Primitive","primitive":"boolean","default":false}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Primitive{}"_s, "DefaultString{value=\"\"}"_s } }, R"([{"shape":"Primitive","primitive":"string","default":""}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Primitive{}"_s, "DefaultString{value=\"d\"}, MinimumLength{length=1}, Message{text=\"m\"}"_s, "T"_s } },
        R"([{"shape":"Primitive","primitive":"string","rules":[{"rule":"MinimumLength","length":1}],"message":"m","default":"d","text":"T"}])"_s));
}

static void testRules()
{
    Graph graph({ { unknown,
        "Integer, Minimum{bound=1}, Maximum{bound=2.5}, ExclusiveMinimum{bound=-1}, ExclusiveMaximum{bound=1e21}, MultipleOf{divisor=0.25}, MinimumLength{length=2}, MaximumLength{length=4294967295}, "
        "Pattern{source=\"^\\\\d+$\"}, Pattern{source=\"^.$\" isUnicode=true}, Format{format=Url}, Format{format=Ipv4}, "
        "Template{head=\"id-\" parts=[{placeholder=Number text=\"/\"}, {text=\"#\"}, {placeholder=BigInt text=\"\"}]}, Template{head=\"\"}"_s } });
    const ASCIILiteral expected[] = {
        R"({"rule":"Integer"})"_s,
        R"({"rule":"Minimum","bound":1})"_s,
        R"({"rule":"Maximum","bound":2.5})"_s,
        R"({"rule":"ExclusiveMinimum","bound":-1})"_s,
        R"({"rule":"ExclusiveMaximum","bound":1e+21})"_s,
        R"({"rule":"MultipleOf","divisor":0.25})"_s,
        R"({"rule":"MinimumLength","length":2})"_s,
        R"({"rule":"MaximumLength","length":4294967295})"_s,
        R"({"rule":"Pattern","source":"^\\d+$"})"_s,
        R"({"rule":"Pattern","source":"^.$","unicode":true})"_s,
        R"({"rule":"Format","format":"url"})"_s,
        R"({"rule":"Format","format":"ipv4"})"_s,
        R"({"rule":"Template","texts":["id-","/","#",""],"placeholders":"nsb"})"_s,
        R"({"rule":"Template","texts":[""],"placeholders":""})"_s,
    };
    StringBuilder plan;
    plan.append(R"([{"shape":"Unknown","rules":[)"_s);
    for (uint32_t index = 0; index < std::size(expected); ++index) {
        AOT_TEST_CHECK(hasText(graph.rule(index), expected[index]));
        plan.append(index ? ","_s : ""_s, expected[index]);
    }
    plan.append("]}]"_s);
    AOT_TEST_CHECK(hasText(graph.plan(), plan.toString()));
}

static bool hasStringText(ASCIILiteral spelledInProgramText, std::span<const char> expectedBytes)
{
    Graph graph({ { unknown, ""_s, spelledInProgramText } });
    Vector<uint8_t> expected;
    expected.append(byteCast<uint8_t>(R"([{"shape":"Unknown","text":)"_span));
    expected.append(byteCast<uint8_t>(expectedBytes));
    expected.append(byteCast<uint8_t>("}]"_span));
    return graph.planBytes() == expected;
}

static void testStringsEscapeQuotesAndControls()
{
    AOT_TEST_CHECK(hasStringText("plain"_s, R"("plain")"_span));
    AOT_TEST_CHECK(hasStringText("\\\""_s, R"("\"")"_span));
    AOT_TEST_CHECK(hasStringText("\\\\"_s, R"("\\")"_span));
    AOT_TEST_CHECK(hasStringText("\\x01"_s, R"("\u0001")"_span));
    AOT_TEST_CHECK(hasStringText("\\n\\x1f "_s, R"("\n\u001f ")"_span));
    AOT_TEST_CHECK(hasStringText("\\x08\\x09\\x0c\\x0d\\x7f"_s, "\"\\b\\t\\f\\r\x7f\""_span));
}

// ""_span takes ASCII only.
template<size_t size>
static std::span<const char> utf8(const char (&text)[size])
{
    return std::span<const char>(text).first(size - 1);
}

static void testStringsKeepScalarValues()
{
    AOT_TEST_CHECK(hasStringText("\\u{e9}"_s, utf8("\"\xc3\xa9\"")));
    AOT_TEST_CHECK(hasStringText("\\u{d7ff}"_s, utf8("\"\xed\x9f\xbf\"")));
    AOT_TEST_CHECK(hasStringText("\\u{e000}"_s, utf8("\"\xee\x80\x80\"")));
    AOT_TEST_CHECK(hasStringText("\\u{1f600}"_s, utf8("\"\xf0\x9f\x98\x80\"")));
}

static void testStringsEscapeLoneSurrogates()
{
    AOT_TEST_CHECK(hasStringText("\\xed\\xa0\\xbd"_s, R"("\ud83d")"_span));
    AOT_TEST_CHECK(hasStringText("a\\xed\\xb0\\x80b"_s, R"("a\udc00b")"_span));
}

static bool hasNumberText(ASCIILiteral minimumRule, ASCIILiteral expected)
{
    Graph graph({ { unknown, minimumRule } });
    return hasText(graph.rule(0), makeString(R"({"rule":"Minimum","bound":)"_s, expected, '}'));
}

static void testNumbersRoundTrip()
{
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=0}"_s, "0"_s));
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=-0}"_s, "-0"_s));
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=1.5}"_s, "1.5"_s));
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=-2}"_s, "-2"_s));
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=0.1}"_s, "0.1"_s));
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=1e21}"_s, "1e+21"_s));
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=5e-324}"_s, "5e-324"_s));
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=1.7976931348623157e308}"_s, "1.7976931348623157e+308"_s));
    AOT_TEST_CHECK(hasPlan({ { "NumberLiteral{value=-0}"_s, "DefaultNumber{value=-0}"_s } }, R"([{"shape":"NumberLiteral","value":-0,"default":-0}])"_s));
}

static void testInfinitiesAreNumbersBeyondDoubleRange()
{
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=Infinity}"_s, "1e999"_s));
    AOT_TEST_CHECK(hasNumberText("Minimum{bound=-Infinity}"_s, "-1e999"_s));
    AOT_TEST_CHECK(hasPlan({ { "Primitive{primitive=Number}"_s, "DefaultNumber{value=Infinity}"_s } }, R"([{"shape":"Primitive","primitive":"number","default":1e999}])"_s));
}

static void testPlanWithEveryCaseIsJSON()
{
    Graph graph({
        { "Object{members=[{key=\"a\" node=1}, {key=\"b\" node=2 isOptional=true}, {key=\"c\" node=3}, {key=\"d\" node=4}, {key=\"e\" node=5}, {key=\"f\" node=6}, {key=\"g\" node=7}, {key=\"h\" node=8}, {key=\"i\" node=9}, {key=\"j\" node=10}, {key=\"k\" node=11}, {key=\"l\" node=12}, {key=\"m\" node=13}, {key=\"n\" node=14}, {key=\"o\" node=15}, {key=\"p\" node=16}, {key=\"q\" node=17}] undeclared=Validated undeclaredValue=1}"_s, "Message{text=\"bad\"}"_s, "T"_s },
        { unknown }, { "Never"_s }, { "Undefined"_s }, { "Null"_s, "DefaultNull"_s }, { "AnyObject"_s },
        { "Primitive{}"_s, "MinimumLength{length=1}, MaximumLength{length=8}, Pattern{source=\"^a\"}, Format{format=Email}, Format{format=Uuid}, Format{format=Ipv4}, Format{format=Ipv6}, Format{format=Date}, Format{format=DateTime}, Format{format=Url}, Template{head=\"id-\" parts=[{placeholder=Number text=\"-\"}, {text=\"!\"}, {placeholder=BigInt text=\"\"}]}, DefaultString{value=\"d\"}"_s },
        { "Primitive{primitive=Number}"_s, "Integer, Minimum{bound=1}, Maximum{bound=9}, ExclusiveMinimum{bound=0.5}, ExclusiveMaximum{bound=Infinity}, MultipleOf{divisor=2}, DefaultNumber{value=3}"_s },
        { "Primitive{primitive=Boolean}"_s, "DefaultBoolean{value=true}"_s }, { "Primitive{primitive=BigInt}"_s },
        { "BooleanLiteral{value=true}"_s }, { "NumberLiteral{value=4}"_s }, { "StringLiteral{value=\"s\"}"_s },
        { "Array{element=7}"_s }, { "Tuple{elements=[6, 7] required=2}"_s }, { "Dictionary{value=7}"_s },
        { "Union{arms=[17, 18, 0]}"_s },
        { "Object{members=[{key=\"kind\" node=12}] undeclared=Rejected}"_s },
        { "Object{members=[{key=\"kind\" node=11}]}"_s },
    });
    String text = graph.plan();
    RefPtr<JSON::Value> value = JSON::Value::parseJSON(text);
    AOT_TEST_CHECK(value);
    RefPtr<JSON::Array> nodes = value->asArray();
    AOT_TEST_CHECK(nodes && nodes->length() == 19);

    const ASCIILiteral shapes[] = {
        "Object"_s, "Unknown"_s, "Never"_s, "Undefined"_s, "Null"_s, "AnyObject"_s, "Primitive"_s, "Primitive"_s, "Primitive"_s, "Primitive"_s,
        "BooleanLiteral"_s, "NumberLiteral"_s, "StringLiteral"_s, "Array"_s, "Tuple"_s, "Dictionary"_s, "Union"_s, "Object"_s, "Object"_s,
    };
    for (unsigned index = 0; index < std::size(shapes); ++index) {
        RefPtr<JSON::Object> node = nodes->get(index)->asObject();
        AOT_TEST_CHECK(node && node->getString("shape"_s) == shapes[index]);
        AOT_TEST_CHECK(!!node->getValue("undeclared"_s) == (shapes[index] == "Object"_s));
        AOT_TEST_CHECK(!!node->getValue("remembers"_s) == (shapes[index] == "Union"_s));
        if (RefPtr<JSON::Array> rules = node->getArray("rules"_s)) {
            AOT_TEST_CHECK(rules->length());
            for (unsigned rule = 0; rule < rules->length(); ++rule) {
                String word = rules->get(rule)->asObject()->getString("rule"_s);
                AOT_TEST_CHECK(!word.isEmpty() && word != "Message"_s && !word.startsWith("Default"_s));
            }
        }
    }
    AOT_TEST_CHECK(nodes->get(6)->asObject()->getArray("rules"_s)->length() == 11 && nodes->get(7)->asObject()->getArray("rules"_s)->length() == 6);
    AOT_TEST_CHECK(!nodes->get(0)->asObject()->getValue("rules"_s) && nodes->get(0)->asObject()->getString("message"_s) == "bad"_s && nodes->get(0)->asObject()->getString("text"_s) == "T"_s);
    AOT_TEST_CHECK(nodes->get(4)->asObject()->getValue("default"_s)->isNull() && !nodes->get(3)->asObject()->getValue("default"_s));

    RefPtr<JSON::Object> discriminant = nodes->get(16)->asObject()->getObject("discriminant"_s);
    AOT_TEST_CHECK(discriminant && discriminant->size() == 5);
    AOT_TEST_CHECK(discriminant->getString("key"_s) == "kind"_s && discriminant->getString("expected"_s) == "\"s\" | 4"_s);
    AOT_TEST_CHECK(discriminant->getArray("tags"_s)->length() == 2 && discriminant->getArray("tagged"_s)->length() == 2 && discriminant->getArray("untagged"_s)->length() == 1);
    RefPtr<JSON::Array> members = nodes->get(0)->asObject()->getArray("members"_s);
    AOT_TEST_CHECK(members->length() == 17);
    for (unsigned index = 0; index < members->length(); ++index)
        AOT_TEST_CHECK(members->get(index)->asObject()->size() == 4);
}

static void testEqualPlansInternToOneString()
{
    Graph graph({
        { "Tuple{elements=[1, 2, 3] required=3}"_s },
        { "Array{element=4}"_s },
        { "Array{element=4}"_s },
        { "Array{element=5}"_s },
        numberNode,
        stringNode,
    });
    StringArena strings(graph.program().strings);
    ExpectedResultID expectedResult(0u);
    StringID first = validationPlanText(graph.program(), strings, expectedResult, 1);
    AOT_TEST_CHECK(first && validationPlanText(graph.program(), strings, expectedResult, 1) == first);
    AOT_TEST_CHECK(validationPlanText(graph.program(), strings, expectedResult, 2) == first);
    AOT_TEST_CHECK(validationPlanText(graph.program(), strings, expectedResult, 3) != first);
    AOT_TEST_CHECK(validationPlanText(graph.program(), strings, expectedResult, 0) != first);

    Graph rules({ { unknown, "Minimum{bound=1}, Minimum{bound=1}, Minimum{bound=2}, Maximum{bound=1}"_s } });
    StringArena ruleStrings(rules.program().strings);
    const Vector<ValidationRule>& list = rules.program().at(expectedResult).validation[0].rules;
    StringID minimum = validationRuleText(rules.program(), ruleStrings, list[0]);
    AOT_TEST_CHECK(minimum && validationRuleText(rules.program(), ruleStrings, list[1]) == minimum);
    AOT_TEST_CHECK(validationRuleText(rules.program(), ruleStrings, list[2]) != minimum && validationRuleText(rules.program(), ruleStrings, list[3]) != minimum);
}

static constexpr TextFormat formats[] = {
    TextFormat::Email, TextFormat::Uuid, TextFormat::Ipv4, TextFormat::Ipv6, TextFormat::Date, TextFormat::DateTime, TextFormat::Url, TextFormat::Time,
    TextFormat::Duration, TextFormat::Hostname, TextFormat::Uri, TextFormat::UriReference, TextFormat::Regex, TextFormat::JsonPointer, TextFormat::RelativeJsonPointer,
};

static void testFormatNames()
{
    const ASCIILiteral names[] = {
        "email"_s, "uuid"_s, "ipv4"_s, "ipv6"_s, "date"_s, "date-time"_s, "url"_s, "time"_s,
        "duration"_s, "hostname"_s, "uri"_s, "uri-reference"_s, "regex"_s, "json-pointer"_s, "relative-json-pointer"_s,
    };
    AOT_TEST_CHECK(std::size(names) == std::size(formats));
    for (unsigned index = 0; index < std::size(formats); ++index)
        AOT_TEST_CHECK(formatName(formats[index]) == names[index]);
}

static void testRulesWithoutNodesAreWrittenAlone()
{
    Graph graph({ { unknown,
        "UniqueItems, MinimumMembers{count=1}, MaximumMembers{count=4294967295}, DependentMembers{key=\"a\" names=[\"b\", \"__proto__\"]}, DependentMembers{key=\"\"}"_s } });
    const ASCIILiteral expected[] = {
        R"({"rule":"UniqueItems"})"_s,
        R"({"rule":"MinimumMembers","count":1})"_s,
        R"({"rule":"MaximumMembers","count":4294967295})"_s,
        R"({"rule":"DependentMembers","key":"a","names":["b","__proto__"]})"_s,
        R"({"rule":"DependentMembers","key":"","names":[]})"_s,
    };
    const Vector<ValidationRule>& rules = graph.program().at(ExpectedResultID(0u)).validation[0].rules;
    for (uint32_t index = 0; index < std::size(expected); ++index) {
        AOT_TEST_CHECK(!namesNodes(rules[index]));
        AOT_TEST_CHECK(hasText(graph.rule(index), expected[index]));
    }
}

static void testRulesNameNodesByTheirNumberInThePlan()
{
    Graph graph({
        numberNode,
        { unknown,
            "Not{node=3}, Contains{node=0 minimum=2}, Contains{node=0 maximum=5}, If{condition=3 then=0 otherwise=4}, ExactlyOne{nodes=[4, 3]}, Every{nodes=[0]}, "
            "PatternMembers{sources=[\"^a\", \"b$\"] nodes=[3, 0]}, MemberNames{node=3}, DependentNode{key=\"k\" node=4}, ExactlyOne{}"_s },
        { "Never"_s },
        stringNode,
        { "Null"_s },
    });
    for (const ValidationRule& rule : graph.program().at(ExpectedResultID(0u)).validation[1].rules)
        AOT_TEST_CHECK(namesNodes(rule));
    AOT_TEST_CHECK(hasText(graph.plan(1), makeString(
        R"([{"shape":"Unknown","rules":[{"rule":"Not","node":2},{"rule":"Contains","node":1,"minimum":2},{"rule":"Contains","node":1,"minimum":0,"maximum":5},)"_s,
        R"({"rule":"If","condition":2,"then":1,"otherwise":3},{"rule":"ExactlyOne","nodes":[3,2]},{"rule":"Every","nodes":[1]},)"_s,
        R"({"rule":"PatternMembers","sources":["^a","b$"],"nodes":[2,1]},{"rule":"MemberNames","node":2},{"rule":"DependentNode","key":"k","node":3},{"rule":"ExactlyOne","nodes":[]}]},)"_s,
        R"({"shape":"Primitive","primitive":"number"},{"shape":"Primitive","primitive":"string"},{"shape":"Null"}])"_s)));
}

static void testUnionOnCycleThroughRuleOrRestRemembers()
{
    AOT_TEST_CHECK(hasPlan({ { "Union{arms=[1, 2]}"_s }, { unknown, "Not{node=0}"_s }, { "Null"_s } },
        R"([{"shape":"Union","arms":[1,2],"remembers":true},{"shape":"Unknown","rules":[{"rule":"Not","node":0}]},{"shape":"Null"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Union{arms=[1, 2]}"_s }, { "Tuple{rest=0}"_s }, { "Null"_s } },
        R"([{"shape":"Union","arms":[1,2],"remembers":true},{"shape":"Tuple","required":0,"rest":0},{"shape":"Null"}])"_s));
    AOT_TEST_CHECK(hasPlan({ { "Union{arms=[1, 2]}"_s }, { unknown, "Not{node=2}"_s }, { "Null"_s } },
        R"([{"shape":"Union","arms":[1,2]},{"shape":"Unknown","rules":[{"rule":"Not","node":2}]},{"shape":"Null"}])"_s));
}

// A rule under Not, a pattern for members and a test of names turn "no match" into "accepted". So a search without an answer has to be told from one that found nothing.
static void testSearchThatIsGivenUpIsToldFromNoMatch()
{
    Yarr::ErrorCode error { Yarr::ErrorCode::NoError };
    Yarr::YarrPattern pattern("(a+)+$"_s, { }, error);
    AOT_TEST_CHECK(!Yarr::hasError(error));
    BumpPointerAllocator allocator;
    std::unique_ptr<Yarr::BytecodePattern> bytecode = Yarr::byteCompile(pattern, &allocator, error);
    AOT_TEST_CHECK(bytecode && !bytecode->searchesGivenUp());

    Vector<unsigned, 8> output(FillWith { }, bytecode->m_offsetsSize, Yarr::offsetNoMatch);
    AOT_TEST_CHECK(!Yarr::interpret(bytecode.get(), "aaa"_s, 0, output.mutableSpan().data()));
    AOT_TEST_CHECK(Yarr::interpret(bytecode.get(), "bbb"_s, 0, output.mutableSpan().data()) == Yarr::offsetNoMatch);
    AOT_TEST_CHECK(!bytecode->searchesGivenUp());

    AOT_TEST_CHECK(Yarr::interpret(bytecode.get(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!"_s, 0, output.mutableSpan().data()) == Yarr::offsetNoMatch);
    AOT_TEST_CHECK(bytecode->searchesGivenUp() == 1);
    AOT_TEST_CHECK(Yarr::interpret(bytecode.get(), "bbb"_s, 0, output.mutableSpan().data()) == Yarr::offsetNoMatch);
    AOT_TEST_CHECK(bytecode->searchesGivenUp() == 1);
}

static void testMessageOfInvalidPatternQuotesItsSource()
{
    Graph graph({ { "Primitive{}"_s, "Pattern{source=\"{count}(\"}"_s } });
    StringID source = graph.program().at(ExpectedResultID(0u)).validation[0].rules[0].as<ValidationRule::Pattern>().source;

    Diagnostics diagnostics;
    diagnostics.add(UserDiagnostic { .kind = UserDiagnosticKind::InvalidValidationPattern, .text = source });
    diagnostics.add(UserDiagnostic { .kind = UserDiagnosticKind::InvalidUnicodeValidationPattern, .text = source });
    diagnostics.finish(&graph.program());
    AOT_TEST_CHECK(diagnostics.hasErrors());
    AOT_TEST_CHECK(hasText(diagnostics.messageOfUserDiagnostic(0), "the type that is validated here has a pattern that is not a valid regular expression: /{count}(/"_s));
    AOT_TEST_CHECK(hasText(diagnostics.messageOfUserDiagnostic(1), "the type that is validated here has a pattern that is not a valid regular expression with the 'u' flag: /{count}(/u"_s));
}

std::span<const NativeTest> validationPlanTextTests()
{
    static constexpr NativeTest tests[] = {
        { "validation-plan-text/plan-lists-nodes-in-order"_s, testPlanListsNodesInOrder },
        { "validation-plan-text/plan-holds-only-what-is-below-its-root"_s, testPlanHoldsOnlyWhatIsBelowItsRoot },
        { "validation-plan-text/shapes-without-detail"_s, testShapesWithoutDetail },
        { "validation-plan-text/primitive-is-typeof-word"_s, testPrimitiveIsTypeofWord },
        { "validation-plan-text/literal-is-json-value"_s, testLiteralIsJSONValue },
        { "validation-plan-text/containers-name-their-nodes"_s, testContainersNameTheirNodes },
        { "validation-plan-text/every-member-has-four-members"_s, testEveryMemberHasFourMembers },
        { "validation-plan-text/every-object-says-what-undeclared-members-do"_s, testEveryObjectSaysWhatUndeclaredMembersDo },
        { "validation-plan-text/union-on-cycle-remembers"_s, testUnionOnCycleRemembers },
        { "validation-plan-text/other-shapes-on-cycles-remember-nothing"_s, testOtherShapesOnCyclesRememberNothing },
        { "validation-plan-text/every-discriminant-has-five-members"_s, testEveryDiscriminantHasFiveMembers },
        { "validation-plan-text/message-and-default-are-no-rules"_s, testMessageAndDefaultAreNoRules },
        { "validation-plan-text/rules"_s, testRules },
        { "validation-plan-text/strings-escape-quotes-and-controls"_s, testStringsEscapeQuotesAndControls },
        { "validation-plan-text/strings-keep-scalar-values"_s, testStringsKeepScalarValues },
        { "validation-plan-text/strings-escape-lone-surrogates"_s, testStringsEscapeLoneSurrogates },
        { "validation-plan-text/numbers-round-trip"_s, testNumbersRoundTrip },
        { "validation-plan-text/infinities-are-numbers-beyond-double-range"_s, testInfinitiesAreNumbersBeyondDoubleRange },
        { "validation-plan-text/plan-with-every-case-is-json"_s, testPlanWithEveryCaseIsJSON },
        { "validation-plan-text/equal-plans-intern-to-one-string"_s, testEqualPlansInternToOneString },
        { "validation-plan-text/format-names"_s, testFormatNames },
        { "validation-plan-text/rules-without-nodes-are-written-alone"_s, testRulesWithoutNodesAreWrittenAlone },
        { "validation-plan-text/rules-name-nodes-by-their-number-in-the-plan"_s, testRulesNameNodesByTheirNumberInThePlan },
        { "validation-plan-text/union-on-cycle-through-rule-or-rest-remembers"_s, testUnionOnCycleThroughRuleOrRestRemembers },
        { "validation-plan-text/search-that-is-given-up-is-told-from-no-match"_s, testSearchThatIsGivenUpIsToldFromNoMatch },
        { "validation-plan-text/message-of-invalid-pattern-quotes-its-source"_s, testMessageOfInvalidPatternQuotesItsSource },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
