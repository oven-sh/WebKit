/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidationRules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTRegExpSummary.h"
#include "AOTStringArena.h"
#include "AOTValidationPlanText.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool hasSubject(ResultShape shape, RuleSubject subject)
{
    switch (shape) {
    case ResultShape::Number:
    case ResultShape::NumberLiteral:
        return subject == RuleSubject::Number;
    case ResultShape::String:
    case ResultShape::StringLiteral:
        return subject == RuleSubject::Length || subject == RuleSubject::Text;
    case ResultShape::Array:
    case ResultShape::Tuple:
        return subject == RuleSubject::Length;
    case ResultShape::Unknown:
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::Null:
    case ResultShape::AnyObject:
    case ResultShape::Boolean:
    case ResultShape::BigInt:
    case ResultShape::BooleanLiteral:
    case ResultShape::Dictionary:
    case ResultShape::Object:
    case ResultShape::Union:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasExpression(TextFormat format)
{
    switch (format) {
    case TextFormat::Email:
    case TextFormat::Uuid:
    case TextFormat::Ipv4:
    case TextFormat::Ipv6:
    case TextFormat::Date:
    case TextFormat::DateTime:
        return true;
    case TextFormat::Url:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StringView expressionOf(const Program& program, const ValidationRule& rule, Vector<char16_t>& bufferForNonASCII)
{
    if (const auto* pattern = rule.tryAs<ValidationRule::Pattern>())
        return program.strings.viewOf(pattern->source, bufferForNonASCII);
    return formatExpression(rule.as<ValidationRule::Format>().format);
}

static ValueID appendNumber(IRBuilder& builder, double number)
{
    if (isTrackedInteger(number))
        return builder.appendInteger(static_cast<int64_t>(number));
    return builder.appendDouble(number);
}

static ValueID appendMatchedBuiltinCall(Emission& emission, BuiltinID builtin, BuiltinCallMode mode, ValueID receiver, std::span<const ValueID> arguments)
{
    TypingContext typing { emission.program(), emission.builder().function(), emission.inputs().types };
    BuiltinOperandTypes operands = operandTypesOf(typing, receiver, arguments);
    OverloadMatch match = matchOverload({ typing, builtin, mode, operands, operands });
    RELEASE_ASSERT(match.state == BuiltinMatchState::Proven && match.requirements.isEmpty());

    return emission.builder().append(makeBuiltinCall(match.overload, BuiltinCallForm::Whole, { receiver, Vector<ValueID, 4>(arguments) }, { }));
}

RuleSubject subjectOf(const ValidationRule& rule)
{
    switch (rule.kind()) {
    case ValidationRule::Kind::Integer:
    case ValidationRule::Kind::Minimum:
    case ValidationRule::Kind::Maximum:
    case ValidationRule::Kind::ExclusiveMinimum:
    case ValidationRule::Kind::ExclusiveMaximum:
    case ValidationRule::Kind::MultipleOf:
        return RuleSubject::Number;
    case ValidationRule::Kind::MinimumLength:
    case ValidationRule::Kind::MaximumLength:
        return RuleSubject::Length;
    case ValidationRule::Kind::Pattern:
    case ValidationRule::Kind::Format:
    case ValidationRule::Kind::Template:
        return RuleSubject::Text;
    case ValidationRule::Kind::Message:
    case ValidationRule::Kind::DefaultNull:
    case ValidationRule::Kind::DefaultBoolean:
    case ValidationRule::Kind::DefaultNumber:
    case ValidationRule::Kind::DefaultString:
        return RuleSubject::Nothing;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool appliesTo(const ValidationRule& rule, ResultShape shape)
{
    RuleSubject subject = subjectOf(rule);
    return subject == RuleSubject::Nothing || hasSubject(shape, subject);
}

bool hasIntegerRule(const ResultNode& node)
{
    return std::ranges::any_of(node.rules, [](const ValidationRule& rule) {
        return rule.is<ValidationRule::Integer>();
    });
}

ASCIILiteral formatName(TextFormat format)
{
    switch (format) {
    case TextFormat::Email:
        return "email"_s;
    case TextFormat::Uuid:
        return "uuid"_s;
    case TextFormat::Ipv4:
        return "ipv4"_s;
    case TextFormat::Ipv6:
        return "ipv6"_s;
    case TextFormat::Date:
        return "date"_s;
    case TextFormat::DateTime:
        return "date-time"_s;
    case TextFormat::Url:
        return "url"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral formatExpression(TextFormat format)
{
    switch (format) {
    case TextFormat::Email:
        return R"(^(?!\.)(?!.*\.\.)([A-Za-z0-9_'+\-\.]*)[A-Za-z0-9_+-]@([A-Za-z0-9][A-Za-z0-9\-]*\.)+[A-Za-z]{2,}$)"_s;
    case TextFormat::Uuid:
        return R"(^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-8][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}|00000000-0000-0000-0000-000000000000|ffffffff-ffff-ffff-ffff-ffffffffffff)$)"_s;
    case TextFormat::Ipv4:
        return R"(^(?:(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])\.){3}(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])$)"_s;
    case TextFormat::Ipv6:
        return R"(^(([0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}|::|([0-9a-fA-F]{1,4})?::([0-9a-fA-F]{1,4}:?){0,6})$)"_s;
    case TextFormat::Date:
        return R"(^(?:(?:\d\d[2468][048]|\d\d[13579][26]|\d\d0[48]|[02468][048]00|[13579][26]00)-02-29|\d{4}-(?:(?:0[13578]|1[02])-(?:0[1-9]|[12]\d|3[01])|(?:0[469]|11)-(?:0[1-9]|[12]\d|30)|(?:02)-(?:0[1-9]|1\d|2[0-8])))$)"_s;
    case TextFormat::DateTime:
        return R"(^(?:(?:\d\d[2468][048]|\d\d[13579][26]|\d\d0[48]|[02468][048]00|[13579][26]00)-02-29|\d{4}-(?:(?:0[13578]|1[02])-(?:0[1-9]|[12]\d|3[01])|(?:0[469]|11)-(?:0[1-9]|[12]\d|30)|(?:02)-(?:0[1-9]|1\d|2[0-8])))T(?:[01]\d|2[0-3]):[0-5]\d(?::[0-5]\d(?:\.\d+)?)?Z$)"_s;
    case TextFormat::Url:
        return ""_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValidationRuleEmitter::ValidationRuleEmitter(Emission& emission)
    : m_emission(emission)
{
}

RuleCode ValidationRuleEmitter::codeFor(const ValidationRule& rule, ResultShape shape)
{
    if (!appliesTo(rule, shape))
        return RuleCode::ByHelper;

    switch (rule.kind()) {
    case ValidationRule::Kind::Integer:
    case ValidationRule::Kind::Minimum:
    case ValidationRule::Kind::Maximum:
    case ValidationRule::Kind::ExclusiveMinimum:
    case ValidationRule::Kind::ExclusiveMaximum:
    case ValidationRule::Kind::MultipleOf:
    case ValidationRule::Kind::MinimumLength:
    case ValidationRule::Kind::MaximumLength:
        return RuleCode::Inline;
    case ValidationRule::Kind::Pattern: {
        Vector<char16_t> bufferForNonASCII;
        return isValidExpression(expressionOf(m_emission.program(), rule, bufferForNonASCII)) ? RuleCode::RegularExpression : RuleCode::ByHelper;
    }
    case ValidationRule::Kind::Format:
        return hasExpression(rule.as<ValidationRule::Format>().format) ? RuleCode::RegularExpression : RuleCode::ByHelper;
    case ValidationRule::Kind::Template:
        return RuleCode::ByHelper;
    case ValidationRule::Kind::Message:
    case ValidationRule::Kind::DefaultNull:
    case ValidationRule::Kind::DefaultBoolean:
    case ValidationRule::Kind::DefaultNumber:
    case ValidationRule::Kind::DefaultString:
        return RuleCode::Nothing;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ValidationRuleEmitter::emitRules(const ResultNode& node, ValueID value, BlockID rejected)
{
    for (const ValidationRule& rule : node.rules)
        emitRule(rule, node, value, rejected);
}

void ValidationRuleEmitter::emitLengthRules(const ResultNode& node, ValueID length, BlockID rejected)
{
    for (const ValidationRule& rule : node.rules) {
        if (subjectOf(rule) == RuleSubject::Length)
            m_emission.emitJumpUnless(appendLengthTest(rule, length), rejected);
    }
}

void ValidationRuleEmitter::emitRequireFinite(ValueID number, BlockID rejected)
{
    if (m_emission.typeOf(number).isInteger())
        return;

    IRBuilder& builder = m_emission.builder();
    ValueID difference = builder.append(Opcode::Sub, { number, number });
    m_emission.emitJumpUnless(builder.append(Opcode::StrictEq, { difference, builder.appendInteger(0) }), rejected);
}

ValueID ValidationRuleEmitter::appendLiteral(const ResultLiteral& literal)
{
    switch (literal.kind) {
    case ResultLiteral::Kind::Null:
        return m_emission.appendNull();
    case ResultLiteral::Kind::Boolean:
        return m_emission.builder().appendBoolean(literal.boolean);
    case ResultLiteral::Kind::Number:
        return appendNumber(m_emission.builder(), literal.number);
    case ResultLiteral::Kind::String:
        return m_emission.appendString(literal.string);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID ValidationRuleEmitter::appendLength(const ResultNode& node, ValueID value)
{
    switch (node.shape) {
    case ResultShape::String:
    case ResultShape::StringLiteral:
        return m_emission.builder().append(Opcode::GetStringLength, { value });
    case ResultShape::Array:
    case ResultShape::Tuple:
        return appendMatchedBuiltinCall(m_emission, Builtins::ArrayPrototypeLength, BuiltinCallMode::Get, value, { });
    case ResultShape::Unknown:
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::Null:
    case ResultShape::AnyObject:
    case ResultShape::Number:
    case ResultShape::Boolean:
    case ResultShape::BigInt:
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::Dictionary:
    case ResultShape::Object:
    case ResultShape::Union:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ValidationRuleEmitter::emitRule(const ValidationRule& rule, const ResultNode& node, ValueID value, BlockID rejected)
{
    Vector<char16_t> bufferForNonASCII;
    ValueID test;
    switch (codeFor(rule, node.shape)) {
    case RuleCode::Nothing:
        return;
    case RuleCode::ByHelper:
        test = appendTestByHelper(rule, value);
        break;
    case RuleCode::Inline:
        if (subjectOf(rule) == RuleSubject::Length) {
            test = appendLengthTest(rule, appendLength(node, value));
            break;
        }
        if (rule.is<ValidationRule::Integer>())
            emitRequireFinite(value, rejected);
        test = appendNumberTest(rule, value);
        break;
    case RuleCode::RegularExpression:
        test = appendExpressionTest(expressionOf(m_emission.program(), rule, bufferForNonASCII), value);
        break;
    }
    m_emission.emitJumpUnless(test, rejected);
}

ValueID ValidationRuleEmitter::appendNumberTest(const ValidationRule& rule, ValueID number)
{
    IRBuilder& builder = m_emission.builder();
    switch (rule.kind()) {
    case ValidationRule::Kind::Integer:
        return builder.appendTypeTest(number, Type::fromPrimitives(Primitive::NumberWithoutFraction));
    case ValidationRule::Kind::Minimum:
        return builder.append(Opcode::GreaterEq, { number, appendNumber(builder, rule.as<ValidationRule::Minimum>().bound) });
    case ValidationRule::Kind::Maximum:
        return builder.append(Opcode::LessEq, { number, appendNumber(builder, rule.as<ValidationRule::Maximum>().bound) });
    case ValidationRule::Kind::ExclusiveMinimum:
        return builder.append(Opcode::Greater, { number, appendNumber(builder, rule.as<ValidationRule::ExclusiveMinimum>().bound) });
    case ValidationRule::Kind::ExclusiveMaximum:
        return builder.append(Opcode::Less, { number, appendNumber(builder, rule.as<ValidationRule::ExclusiveMaximum>().bound) });
    case ValidationRule::Kind::MultipleOf:
        return appendIsMultipleOf(number, rule.as<ValidationRule::MultipleOf>().divisor);
    case ValidationRule::Kind::MinimumLength:
    case ValidationRule::Kind::MaximumLength:
    case ValidationRule::Kind::Pattern:
    case ValidationRule::Kind::Format:
    case ValidationRule::Kind::Template:
    case ValidationRule::Kind::Message:
    case ValidationRule::Kind::DefaultNull:
    case ValidationRule::Kind::DefaultBoolean:
    case ValidationRule::Kind::DefaultNumber:
    case ValidationRule::Kind::DefaultString:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID ValidationRuleEmitter::appendIsMultipleOf(ValueID number, double divisor)
{
    IRBuilder& builder = m_emission.builder();
    ValueID remainder = builder.append(Opcode::Mod, { number, appendNumber(builder, divisor) });
    return builder.append(Opcode::StrictEq, { remainder, builder.appendInteger(0) });
}

ValueID ValidationRuleEmitter::appendLengthTest(const ValidationRule& rule, ValueID length)
{
    IRBuilder& builder = m_emission.builder();
    if (const auto* minimum = rule.tryAs<ValidationRule::MinimumLength>())
        return builder.append(Opcode::GreaterEq, { length, builder.appendInteger(minimum->length) });
    return builder.append(Opcode::LessEq, { length, builder.appendInteger(rule.as<ValidationRule::MaximumLength>().length) });
}

ValueID ValidationRuleEmitter::appendExpressionTest(StringView source, ValueID string)
{
    IRBuilder& builder = m_emission.builder();
    Value literal = IRBuilder::make(Opcode::SharedRegExpLiteral);
    literal.setPattern(m_emission.inputs().strings.intern(source));
    literal.setRegExpFlags({ });
    ValueID regExp = builder.append(WTF::move(literal));
    // Placement keys the cell of a literal by (function, position), and every other value of a derived function has the same position.
    builder.function().edit(regExp, ValueEdit::Definition).setOrigin({ regExp.index(), builder.origin().inlineFrame });
    return appendMatchedBuiltinCall(m_emission, Builtins::RegExpPrototypeTest, BuiltinCallMode::Call, regExp, singleElementSpan(string));
}

ValueID ValidationRuleEmitter::appendTestByHelper(const ValidationRule& rule, ValueID value)
{
    StringID text = validationRuleText(m_emission.program(), m_emission.inputs().strings, rule);
    return m_emission.appendHelperCall(Builtins::CompilerHelpersHoldsRule, { value, m_emission.appendString(text) });
}

bool ValidationRuleEmitter::isValidExpression(StringView source)
{
    const EmissionInputs& inputs = m_emission.inputs();
    const RegExpSummary& summary = inputs.build.regExpSummaries().summaryOf(inputs.strings.intern(source), { }, source);
    return summary.isValid() && summary.metadata == RegExpMetadata::Known;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
