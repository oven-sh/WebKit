/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidationPlanText.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTSelectTypedResultsNodes.h"
#include "AOTStringArena.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTValidationRules.h"
#include <cmath>
#include <limits>
#include <ranges>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

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

namespace {

enum class NumberSpelling : uint8_t { RoundTrip, Stringified };

struct ValidationTextWriter {
    static constexpr uint32_t notInPlan = std::numeric_limits<uint32_t>::max();

    const Program& program;
    // By the index of a node in its list. Empty for a rule that is written alone: it has no plan whose nodes it could name.
    Vector<uint32_t> numbers { };
    StringBuilder text { };

    // Punctuation and names go into the text as they are, every other part as the JSON text of its value.
    void append(const auto&... parts) { (appendPart(parts), ...); }

    void appendPart(ASCIILiteral punctuation) { text.append(punctuation); }
    void appendPart(char punctuation) { text.append(punctuation); }
    void appendPart(uint32_t integer) { text.append(integer); }
    void appendPart(bool boolean) { text.append(boolean ? "true"_s : "false"_s); }
    void appendPart(const String& string) { text.appendQuotedJSONString(string); }
    void appendPart(StringID string) { appendPart(program.strings.toWTFString(string)); }
    void appendPart(double number) { appendNumber(number, NumberSpelling::RoundTrip); }
    void appendPart(const ResultLiteral& literal) { appendLiteral(literal, NumberSpelling::RoundTrip); }
    void appendPart(PropertyKeyID id)
    {
        const PropertyKey& key = program.at(id);
        if (key.kind == PropertyKeyKind::Text)
            appendPart(key.text);
        else
            text.append("null"_s);
    }

    void appendNumber(double number, NumberSpelling spelling)
    {
        bool roundTrips = spelling == NumberSpelling::RoundTrip;
        if (roundTrips && std::isinf(number))
            text.append(number > 0 ? "1e999"_s : "-1e999"_s);
        else if (roundTrips && !number && std::signbit(number))
            text.append("-0"_s);
        else if (!std::isfinite(number))
            text.append("null"_s);
        else
            text.append(number);
    }

    void appendLiteral(const ResultLiteral& literal, NumberSpelling spelling)
    {
        switch (literal.kind) {
        case ResultLiteral::Kind::Null:
            return text.append("null"_s);
        case ResultLiteral::Kind::Boolean:
            return appendPart(literal.boolean);
        case ResultLiteral::Kind::Number:
            return appendNumber(literal.number, spelling);
        case ResultLiteral::Kind::String:
            return appendPart(literal.string);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void appendList(auto&& items, NOESCAPE const auto& appendItem)
    {
        text.append('[');
        bool isFirst = true;
        for (const auto& item : items) {
            if (!std::exchange(isFirst, false))
                text.append(',');
            appendItem(item);
        }
        text.append(']');
    }

    uint32_t numberOf(uint32_t indexInList) const
    {
        RELEASE_ASSERT(indexInList < numbers.size() && numbers[indexInList] != notInPlan);
        return numbers[indexInList];
    }

    void appendNumbers(const auto& indicesInList)
    {
        appendList(indicesInList, [&](uint32_t indexInList) {
            append(numberOf(indexInList));
        });
    }

    void appendStrings(const auto& strings)
    {
        appendList(strings, [&](StringID string) {
            append(string);
        });
    }

    void appendRule(const ValidationRule& rule)
    {
        append("{\"rule\":\""_s, nameOf(rule.kind()), '"');
        switch (rule.kind()) {
        case ValidationRule::Kind::Integer:
        case ValidationRule::Kind::UniqueItems:
        case ValidationRule::Kind::Message:
        case ValidationRule::Kind::DefaultNull:
        case ValidationRule::Kind::DefaultBoolean:
        case ValidationRule::Kind::DefaultNumber:
        case ValidationRule::Kind::DefaultString:
            break;
        case ValidationRule::Kind::Minimum:
            append(",\"bound\":"_s, rule.as<ValidationRule::Minimum>().bound);
            break;
        case ValidationRule::Kind::Maximum:
            append(",\"bound\":"_s, rule.as<ValidationRule::Maximum>().bound);
            break;
        case ValidationRule::Kind::ExclusiveMinimum:
            append(",\"bound\":"_s, rule.as<ValidationRule::ExclusiveMinimum>().bound);
            break;
        case ValidationRule::Kind::ExclusiveMaximum:
            append(",\"bound\":"_s, rule.as<ValidationRule::ExclusiveMaximum>().bound);
            break;
        case ValidationRule::Kind::MultipleOf:
            append(",\"divisor\":"_s, rule.as<ValidationRule::MultipleOf>().divisor);
            break;
        case ValidationRule::Kind::MinimumLength:
            append(",\"length\":"_s, rule.as<ValidationRule::MinimumLength>().length);
            break;
        case ValidationRule::Kind::MaximumLength:
            append(",\"length\":"_s, rule.as<ValidationRule::MaximumLength>().length);
            break;
        case ValidationRule::Kind::Pattern: {
            const auto& pattern = rule.as<ValidationRule::Pattern>();
            append(",\"source\":"_s, pattern.source);
            if (pattern.isUnicode)
                append(",\"unicode\":true"_s);
            break;
        }
        case ValidationRule::Kind::Format:
            append(",\"format\":"_s, String(formatName(rule.as<ValidationRule::Format>().format)));
            break;
        case ValidationRule::Kind::Template: {
            const auto& operands = rule.as<ValidationRule::Template>();
            append(",\"texts\":["_s, operands.head);
            for (const TemplatePart& part : operands.parts)
                append(',', part.text);
            append("],\"placeholders\":\""_s);
            for (const TemplatePart& part : operands.parts)
                append(placeholderLetter(part.placeholder));
            append('"');
            break;
        }
        case ValidationRule::Kind::Contains: {
            const auto& contains = rule.as<ValidationRule::Contains>();
            append(",\"node\":"_s, numberOf(contains.node), ",\"minimum\":"_s, contains.minimum);
            if (contains.maximum)
                append(",\"maximum\":"_s, *contains.maximum);
            break;
        }
        case ValidationRule::Kind::Not:
            append(",\"node\":"_s, numberOf(rule.as<ValidationRule::Not>().node));
            break;
        case ValidationRule::Kind::If: {
            const auto& choice = rule.as<ValidationRule::If>();
            append(",\"condition\":"_s, numberOf(choice.condition), ",\"then\":"_s, numberOf(choice.then), ",\"otherwise\":"_s, numberOf(choice.otherwise));
            break;
        }
        case ValidationRule::Kind::ExactlyOne:
            append(",\"nodes\":"_s);
            appendNumbers(rule.as<ValidationRule::ExactlyOne>().nodes);
            break;
        case ValidationRule::Kind::Every:
            append(",\"nodes\":"_s);
            appendNumbers(rule.as<ValidationRule::Every>().nodes);
            break;
        case ValidationRule::Kind::PatternMembers: {
            const auto& members = rule.as<ValidationRule::PatternMembers>();
            RELEASE_ASSERT(members.sources.size() == members.nodes.size());
            append(",\"sources\":"_s);
            appendStrings(members.sources);
            append(",\"nodes\":"_s);
            appendNumbers(members.nodes);
            break;
        }
        case ValidationRule::Kind::MemberNames:
            append(",\"node\":"_s, numberOf(rule.as<ValidationRule::MemberNames>().node));
            break;
        case ValidationRule::Kind::MinimumMembers:
            append(",\"count\":"_s, rule.as<ValidationRule::MinimumMembers>().count);
            break;
        case ValidationRule::Kind::MaximumMembers:
            append(",\"count\":"_s, rule.as<ValidationRule::MaximumMembers>().count);
            break;
        case ValidationRule::Kind::DependentMembers: {
            const auto& dependent = rule.as<ValidationRule::DependentMembers>();
            append(",\"key\":"_s, dependent.key, ",\"names\":"_s);
            appendStrings(dependent.names);
            break;
        }
        case ValidationRule::Kind::DependentNode: {
            const auto& dependent = rule.as<ValidationRule::DependentNode>();
            append(",\"key\":"_s, dependent.key, ",\"node\":"_s, numberOf(dependent.node));
            break;
        }
        }
        append('}');
    }
};

} // anonymous namespace

Vector<uint8_t> writeValidationPlan(const Program& program, ExpectedResultID expectedResult, uint32_t root)
{
    static constexpr size_t typicalNodeLength = 64;

    // The plan numbers the nodes that the root reaches: the root first, the others in their order.
    const Vector<ValidationNode>& list = program.at(expectedResult).validation;
    Vector<uint32_t> parts { root };
    for (size_t part : nodesReachableFrom(program, expectedResult, root)) {
        if (part != root)
            parts.append(static_cast<uint32_t>(part));
    }
    ValidationTextWriter writer { program };
    writer.numbers.fill(ValidationTextWriter::notInPlan, list.size());
    for (uint32_t number = 0; number < parts.size(); ++number)
        writer.numbers[parts[number]] = number;
    auto numberOf = [&](ResultNodeRef node) {
        return writer.numberOf(node.part);
    };

    Vector<ResultNode> nodes = parts.map([&](uint32_t part) {
        return describeNode(program, ResultNodeRef::forValidation(expectedResult, part));
    });
    Vector<Vector<uint32_t, 4>> children = nodes.map([&](const ResultNode& node) {
        Vector<uint32_t, 4> numbersOfChildren;
        node.forEachChild([&](ResultNodeRef child) {
            numbersOfChildren.append(numberOf(child));
        });
        return numbersOfChildren;
    });
    BitVector isOnCycle = computeStronglyConnectedComponents(static_cast<unsigned>(children.size()), [&](unsigned number) -> const Vector<uint32_t, 4>& {
        return children[number];
    }).isInCycle;

    writer.text.reserveCapacity(static_cast<unsigned>(std::min<size_t>(nodes.size() * typicalNodeLength, String::MaxLength)));
    auto appendNumbers = [&](ASCIILiteral member, const Vector<ResultNodeRef, 4>& refs) {
        writer.append(member);
        writer.appendList(refs, [&](ResultNodeRef ref) {
            writer.append(numberOf(ref));
        });
    };
    auto appendDiscriminant = [&](const TaggedArms& arms) {
        ValidationTextWriter expected { program };
        for (const ResultLiteral& tag : arms.discriminant.tagOfArm) {
            if (!expected.text.isEmpty())
                expected.append(" | "_s);
            expected.appendLiteral(tag, NumberSpelling::Stringified);
        }
        writer.append(",\"discriminant\":{\"key\":"_s, arms.discriminant.key, ",\"tags\":"_s);
        writer.appendList(arms.discriminant.tagOfArm, [&](const ResultLiteral& tag) {
            writer.append(tag);
        });
        appendNumbers(",\"tagged\":"_s, arms.tagged);
        appendNumbers(",\"untagged\":"_s, arms.untagged);
        writer.append(",\"expected\":"_s, expected.text.toString(), '}');
    };

    writer.appendList(std::views::iota(size_t { 0 }, nodes.size()), [&](size_t number) {
        const ResultNode& node = nodes[number];
        const ValidationShape& shape = list[parts[number]].shape;
        writer.append("{\"shape\":\""_s, nameOf(shape.kind()), '"');
        switch (shape.kind()) {
        case ValidationShape::Kind::Primitive:
            writer.append(",\"primitive\":\""_s, typeofName(shape.as<ValidationShape::Primitive>().primitive), '"');
            break;
        case ValidationShape::Kind::BooleanLiteral:
        case ValidationShape::Kind::NumberLiteral:
        case ValidationShape::Kind::StringLiteral:
            writer.append(",\"value\":"_s, node.literal);
            break;
        case ValidationShape::Kind::Array:
        case ValidationShape::Kind::Dictionary:
            writer.append(",\"element\":"_s, numberOf(node.children[0]));
            break;
        case ValidationShape::Kind::Tuple:
            if (!node.children.isEmpty())
                appendNumbers(",\"elements\":"_s, node.children);
            writer.append(",\"required\":"_s, node.requiredElements);
            if (node.restElement)
                writer.append(",\"rest\":"_s, numberOf(*node.restElement));
            break;
        case ValidationShape::Kind::Object:
            if (!node.members.isEmpty()) {
                writer.append(",\"members\":"_s);
                writer.appendList(node.members, [&](const ResultMember& member) {
                    writer.append("{\"key\":"_s, member.key, ",\"node\":"_s, numberOf(member.node), ",\"isOptional\":"_s, member.isOptional, ",\"mustBeOwn\":"_s, member.mustBeOwn, '}');
                });
            }
            writer.append(",\"undeclared\":\""_s, nameOf(node.undeclared), '"');
            if (node.undeclaredValue)
                writer.append(",\"undeclaredValue\":"_s, numberOf(*node.undeclaredValue));
            break;
        case ValidationShape::Kind::Union:
            if (!node.children.isEmpty())
                appendNumbers(",\"arms\":"_s, node.children);
            if (std::optional<TaggedArms> arms = findTaggedArms(program, node.children.span()))
                appendDiscriminant(*arms);
            if (isOnCycle.get(number) || canRepeatRecursion(program, ResultNodeRef::forValidation(expectedResult, parts[number])))
                writer.append(",\"remembers\":true"_s);
            break;
        case ValidationShape::Kind::Unknown:
        case ValidationShape::Kind::Never:
        case ValidationShape::Kind::Undefined:
        case ValidationShape::Kind::Null:
        case ValidationShape::Kind::AnyObject:
            break;
        }

        auto tests = node.rules | std::views::filter([](const ValidationRule& rule) {
            return subjectOf(rule) != RuleSubject::Nothing;
        });
        if (!tests.empty()) {
            writer.append(",\"rules\":"_s);
            writer.appendList(tests, [&](const ValidationRule& rule) {
                writer.appendRule(rule);
            });
        }
        if (node.message)
            writer.append(",\"message\":"_s, node.message);
        if (node.defaultValue)
            writer.append(",\"default\":"_s, *node.defaultValue);
        if (node.text)
            writer.append(",\"text\":"_s, node.text);
        writer.append('}');
    });
    return encodeProgramString(StringView(writer.text));
}

Vector<uint8_t> writeValidationRule(const Program& program, const ValidationRule& rule)
{
    RELEASE_ASSERT(!namesNodes(rule));
    ValidationTextWriter writer { program };
    writer.appendRule(rule);
    return encodeProgramString(StringView(writer.text));
}

StringID validationPlanText(const Program& program, StringArena& strings, ExpectedResultID expectedResult, uint32_t rootNode)
{
    return strings.intern(writeValidationPlan(program, expectedResult, rootNode).span());
}

StringID validationRuleText(const Program& program, StringArena& strings, const ValidationRule& rule)
{
    return strings.intern(writeValidationRule(program, rule).span());
}

} } } // namespace JSC::AOT::Typed

namespace JSC { namespace AOT {

Vector<uint8_t> writeValidationPlans(std::span<const uint8_t> bytes, const Wire::CompileOptions& options)
{
    BuildContext build(options, nullptr);
    std::unique_ptr<Typed::Program> program = Typed::Program::read(bytes, build);
    Vector<uint8_t> plans;
    if (!program)
        return plans;
    for (uint32_t index = 0; index < program->expectedResults.size(); ++index) {
        Vector<uint8_t> plan = Typed::writeValidationPlan(*program, Typed::ExpectedResultID(index), 0);
        uint32_t length = static_cast<uint32_t>(plan.size());
        for (unsigned shift = 0; shift < 32; shift += 8)
            plans.append(static_cast<uint8_t>(length >> shift));
        plans.appendVector(plan);
    }
    return plans;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
