/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidationPlanText.h"

#if ENABLE(AOT)

#include "AOTSelectTypedResultsNodes.h"
#include "AOTStringArena.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTValidationRules.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t typicalNodeLength = 64;
static constexpr ASCIILiteral numberAboveDoubleRange = "1e999"_s;
static constexpr ASCIILiteral numberBelowDoubleRange = "-1e999"_s;

enum class NumberSpelling : uint8_t {
    RoundTrip,
    Stringified,
};

static ASCIILiteral typeofName(ValidatedPrimitive primitive)
{
    switch (primitive) {
    case ValidatedPrimitive::String:
        return "string"_s;
    case ValidatedPrimitive::Number:
        return "number"_s;
    case ValidatedPrimitive::Boolean:
        return "boolean"_s;
    case ValidatedPrimitive::BigInt:
        return "bigint"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static char placeholderLetter(TemplatePlaceholder placeholder)
{
    switch (placeholder) {
    case TemplatePlaceholder::String:
        return 's';
    case TemplatePlaceholder::Number:
        return 'n';
    case TemplatePlaceholder::BigInt:
        return 'b';
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class ValidationTextWriter {
    WTF_MAKE_NONCOPYABLE(ValidationTextWriter);
public:
    explicit ValidationTextWriter(const Program& program)
        : m_program(program)
    {
    }

    Vector<uint8_t> toBytes() const
    {
        StringView text = m_text;
        return encodeProgramString(text);
    }

    String toString() { return m_text.toString(); }

    void beginObject(ASCIILiteral firstMember) { m_text.append("{\""_s, firstMember, "\":"_s); }
    void beginMember(ASCIILiteral name) { m_text.append(",\""_s, name, "\":"_s); }
    void endObject() { m_text.append('}'); }

    void appendText(ASCIILiteral text) { m_text.append(text); }
    void appendWord(ASCIILiteral word) { m_text.append('"', word, '"'); }
    void appendBoolean(bool value) { m_text.append(value ? "true"_s : "false"_s); }
    void appendInteger(uint32_t value) { m_text.append(value); }
    void appendString(const String& string) { m_text.appendQuotedJSONString(string); }
    void appendString(StringID string) { appendString(m_program.strings.toWTFString(string)); }
    void appendNumber(double, NumberSpelling);
    void appendLiteral(const ResultLiteral&, NumberSpelling);
    void appendRule(const ValidationRule&);

    void appendList(const auto& items, NOESCAPE const auto& appendItem)
    {
        m_text.append('[');
        bool isFirst = true;
        for (const auto& item : items) {
            if (!std::exchange(isFirst, false))
                m_text.append(',');
            appendItem(item);
        }
        m_text.append(']');
    }

protected:
    const Program& m_program;
    StringBuilder m_text;

private:
    void appendTemplate(const ValidationRule::Template&);
};

void ValidationTextWriter::appendNumber(double number, NumberSpelling spelling)
{
    if (spelling == NumberSpelling::RoundTrip) {
        if (std::isinf(number)) {
            m_text.append(number > 0 ? numberAboveDoubleRange : numberBelowDoubleRange);
            return;
        }
        if (!number && std::signbit(number)) {
            m_text.append("-0"_s);
            return;
        }
    }
    if (!std::isfinite(number)) {
        m_text.append("null"_s);
        return;
    }
    m_text.append(number);
}

void ValidationTextWriter::appendLiteral(const ResultLiteral& literal, NumberSpelling spelling)
{
    switch (literal.kind) {
    case ResultLiteral::Kind::Null:
        m_text.append("null"_s);
        return;
    case ResultLiteral::Kind::Boolean:
        appendBoolean(literal.boolean);
        return;
    case ResultLiteral::Kind::Number:
        appendNumber(literal.number, spelling);
        return;
    case ResultLiteral::Kind::String:
        appendString(literal.string);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ValidationTextWriter::appendTemplate(const ValidationRule::Template& rule)
{
    beginMember("texts"_s);
    m_text.append('[');
    appendString(rule.head);
    for (const TemplatePart& part : rule.parts) {
        m_text.append(',');
        appendString(part.text);
    }
    m_text.append(']');
    beginMember("placeholders"_s);
    m_text.append('"');
    for (const TemplatePart& part : rule.parts)
        m_text.append(placeholderLetter(part.placeholder));
    m_text.append('"');
}

void ValidationTextWriter::appendRule(const ValidationRule& rule)
{
    auto appendOperand = [&](ASCIILiteral name, double operand) {
        beginMember(name);
        appendNumber(operand, NumberSpelling::RoundTrip);
    };
    auto appendLength = [&](uint32_t length) {
        beginMember("length"_s);
        appendInteger(length);
    };

    beginObject("rule"_s);
    appendWord(nameOf(rule.kind()));
    switch (rule.kind()) {
    case ValidationRule::Kind::Integer:
    case ValidationRule::Kind::Message:
    case ValidationRule::Kind::DefaultNull:
    case ValidationRule::Kind::DefaultBoolean:
    case ValidationRule::Kind::DefaultNumber:
    case ValidationRule::Kind::DefaultString:
        break;
    case ValidationRule::Kind::Minimum:
        appendOperand("bound"_s, rule.as<ValidationRule::Minimum>().bound);
        break;
    case ValidationRule::Kind::Maximum:
        appendOperand("bound"_s, rule.as<ValidationRule::Maximum>().bound);
        break;
    case ValidationRule::Kind::ExclusiveMinimum:
        appendOperand("bound"_s, rule.as<ValidationRule::ExclusiveMinimum>().bound);
        break;
    case ValidationRule::Kind::ExclusiveMaximum:
        appendOperand("bound"_s, rule.as<ValidationRule::ExclusiveMaximum>().bound);
        break;
    case ValidationRule::Kind::MultipleOf:
        appendOperand("divisor"_s, rule.as<ValidationRule::MultipleOf>().divisor);
        break;
    case ValidationRule::Kind::MinimumLength:
        appendLength(rule.as<ValidationRule::MinimumLength>().length);
        break;
    case ValidationRule::Kind::MaximumLength:
        appendLength(rule.as<ValidationRule::MaximumLength>().length);
        break;
    case ValidationRule::Kind::Pattern:
        beginMember("source"_s);
        appendString(rule.as<ValidationRule::Pattern>().source);
        break;
    case ValidationRule::Kind::Format: {
        TextFormat format = rule.as<ValidationRule::Format>().format;
        beginMember("format"_s);
        appendString(formatName(format));
        beginMember("source"_s);
        appendString(formatExpression(format));
        break;
    }
    case ValidationRule::Kind::Template:
        appendTemplate(rule.as<ValidationRule::Template>());
        break;
    }
    endObject();
}

static String describeExpectedTags(const Program& program, std::span<const ResultLiteral> tags)
{
    ValidationTextWriter expected(program);
    bool isFirst = true;
    for (const ResultLiteral& tag : tags) {
        if (!std::exchange(isFirst, false))
            expected.appendText(" | "_s);
        expected.appendLiteral(tag, NumberSpelling::Stringified);
    }
    return expected.toString();
}

class ValidationPlanWriter final : public ValidationTextWriter {
public:
    ValidationPlanWriter(const Program&, ExpectedResultID, uint32_t root);

    void appendPlan();

private:
    struct NumberedNode {
        const ValidationShape* shape { nullptr };
        ResultNode node;
        bool isOnCycle { false };
    };

    uint32_t numberOf(ResultNodeRef node) const { return m_numbers[node.part]; }
    void markNodesOnCycles();

    void appendNode(const NumberedNode&);
    void appendShapeMembers(const NumberedNode&);
    void appendNodeNumbers(ASCIILiteral member, std::span<const ResultNodeRef>);
    void appendObjectMembers(const ResultNode&);
    void appendKey(PropertyKeyID);
    void appendDiscriminant(const ResultNode& unionNode);
    void appendRules(const ResultNode&);

    Vector<NumberedNode> m_nodes;
    Vector<uint32_t> m_numbers;
};

ValidationPlanWriter::ValidationPlanWriter(const Program& program, ExpectedResultID expectedResult, uint32_t root)
    : ValidationTextWriter(program)
{
    const Vector<ValidationNode>& list = program.at(expectedResult).validation;
    m_numbers.fill(0, list.size());
    auto addNode = [&](size_t index) {
        m_numbers[index] = static_cast<uint32_t>(m_nodes.size());
        m_nodes.append(NumberedNode { &list[index].shape, describeNode(program, ResultNodeRef::forValidation(expectedResult, static_cast<uint32_t>(index))) });
    };
    addNode(root);
    for (size_t index : nodesReachableFrom(program, expectedResult, root)) {
        if (index != root)
            addNode(index);
    }
    markNodesOnCycles();
}

void ValidationPlanWriter::markNodesOnCycles()
{
    Vector<Vector<uint32_t, 4>> children = m_nodes.map([&](const NumberedNode& numbered) {
        Vector<uint32_t, 4> numbers;
        for (ResultNodeRef child : numbered.node.children)
            numbers.append(numberOf(child));
        for (const ResultMember& member : numbered.node.members)
            numbers.append(numberOf(member.node));
        if (numbered.node.undeclaredValue)
            numbers.append(numberOf(*numbered.node.undeclaredValue));
        return numbers;
    });
    StronglyConnectedComponents components = computeStronglyConnectedComponents(static_cast<unsigned>(children.size()), [&](unsigned number) -> const Vector<uint32_t, 4>& {
        return children[number];
    });
    for (size_t number : components.isInCycle)
        m_nodes[number].isOnCycle = true;
}

void ValidationPlanWriter::appendPlan()
{
    m_text.reserveCapacity(static_cast<unsigned>(std::min<size_t>(m_nodes.size() * typicalNodeLength, String::MaxLength)));
    appendList(m_nodes, [&](const NumberedNode& numbered) {
        appendNode(numbered);
    });
}

void ValidationPlanWriter::appendNode(const NumberedNode& numbered)
{
    const ResultNode& node = numbered.node;
    beginObject("shape"_s);
    appendWord(nameOf(numbered.shape->kind()));
    appendShapeMembers(numbered);
    appendRules(node);
    if (node.message) {
        beginMember("message"_s);
        appendString(node.message);
    }
    if (node.defaultValue) {
        beginMember("default"_s);
        appendLiteral(*node.defaultValue, NumberSpelling::RoundTrip);
    }
    if (node.text) {
        beginMember("text"_s);
        appendString(node.text);
    }
    endObject();
}

void ValidationPlanWriter::appendShapeMembers(const NumberedNode& numbered)
{
    const ResultNode& node = numbered.node;
    switch (numbered.shape->kind()) {
    case ValidationShape::Kind::Unknown:
    case ValidationShape::Kind::Never:
    case ValidationShape::Kind::Undefined:
    case ValidationShape::Kind::Null:
    case ValidationShape::Kind::AnyObject:
        return;
    case ValidationShape::Kind::Primitive:
        beginMember("primitive"_s);
        appendWord(typeofName(numbered.shape->as<ValidationShape::Primitive>().primitive));
        return;
    case ValidationShape::Kind::BooleanLiteral:
    case ValidationShape::Kind::NumberLiteral:
    case ValidationShape::Kind::StringLiteral:
        beginMember("value"_s);
        appendLiteral(node.literal, NumberSpelling::RoundTrip);
        return;
    case ValidationShape::Kind::Array:
    case ValidationShape::Kind::Dictionary:
        beginMember("element"_s);
        appendInteger(numberOf(node.children[0]));
        return;
    case ValidationShape::Kind::Tuple:
        appendNodeNumbers("elements"_s, node.children.span());
        return;
    case ValidationShape::Kind::Object:
        appendObjectMembers(node);
        return;
    case ValidationShape::Kind::Union:
        appendNodeNumbers("arms"_s, node.children.span());
        appendDiscriminant(node);
        if (numbered.isOnCycle) {
            beginMember("remembers"_s);
            appendBoolean(true);
        }
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ValidationPlanWriter::appendNodeNumbers(ASCIILiteral member, std::span<const ResultNodeRef> nodes)
{
    if (nodes.empty())
        return;
    beginMember(member);
    appendList(nodes, [&](ResultNodeRef node) {
        appendInteger(numberOf(node));
    });
}

void ValidationPlanWriter::appendObjectMembers(const ResultNode& node)
{
    if (!node.members.isEmpty()) {
        beginMember("members"_s);
        appendList(node.members, [&](const ResultMember& member) {
            beginObject("key"_s);
            appendKey(member.key);
            beginMember("node"_s);
            appendInteger(numberOf(member.node));
            beginMember("isOptional"_s);
            appendBoolean(member.isOptional);
            beginMember("mustBeOwn"_s);
            appendBoolean(member.mustBeOwn);
            endObject();
        });
    }
    beginMember("undeclared"_s);
    appendWord(nameOf(node.undeclared));
    if (node.undeclaredValue) {
        beginMember("undeclaredValue"_s);
        appendInteger(numberOf(*node.undeclaredValue));
    }
}

void ValidationPlanWriter::appendKey(PropertyKeyID id)
{
    const PropertyKey& key = m_program.at(id);
    if (key.kind != PropertyKeyKind::Text) {
        appendText("null"_s);
        return;
    }
    appendString(key.text);
}

void ValidationPlanWriter::appendDiscriminant(const ResultNode& unionNode)
{
    std::optional<TaggedArms> arms = findTaggedArms(m_program, unionNode.children.span());
    if (!arms)
        return;
    auto appendNumberOf = [&](ResultNodeRef arm) {
        appendInteger(numberOf(arm));
    };
    beginMember("discriminant"_s);
    beginObject("key"_s);
    appendKey(arms->discriminant.key);
    beginMember("tags"_s);
    appendList(arms->discriminant.tagOfArm, [&](const ResultLiteral& tag) {
        appendLiteral(tag, NumberSpelling::RoundTrip);
    });
    beginMember("tagged"_s);
    appendList(arms->tagged, appendNumberOf);
    beginMember("untagged"_s);
    appendList(arms->untagged, appendNumberOf);
    beginMember("expected"_s);
    appendString(describeExpectedTags(m_program, arms->discriminant.tagOfArm.span()));
    endObject();
}

void ValidationPlanWriter::appendRules(const ResultNode& node)
{
    Vector<const ValidationRule*, 4> tests;
    for (const ValidationRule& rule : node.rules) {
        if (subjectOf(rule) != RuleSubject::Nothing)
            tests.append(&rule);
    }
    if (tests.isEmpty())
        return;
    beginMember("rules"_s);
    appendList(tests, [&](const ValidationRule* rule) {
        appendRule(*rule);
    });
}

Vector<uint8_t> writeValidationPlan(const Program& program, ExpectedResultID expectedResult, uint32_t rootNode)
{
    ValidationPlanWriter writer(program, expectedResult, rootNode);
    writer.appendPlan();
    return writer.toBytes();
}

Vector<uint8_t> writeValidationRule(const Program& program, const ValidationRule& rule)
{
    ValidationTextWriter writer(program);
    writer.appendRule(rule);
    return writer.toBytes();
}

StringID validationPlanText(const Program& program, StringArena& strings, ExpectedResultID expectedResult, uint32_t rootNode)
{
    Vector<uint8_t> text = writeValidationPlan(program, expectedResult, rootNode);
    return strings.intern(text.span());
}

StringID validationRuleText(const Program& program, StringArena& strings, const ValidationRule& rule)
{
    Vector<uint8_t> text = writeValidationRule(program, rule);
    return strings.intern(text.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
