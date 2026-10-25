/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidationRules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFlowResults.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTStringArena.h"
#include "AOTValidationPlanText.h"
#include <algorithm>
#include <cmath>

namespace JSC { namespace AOT { namespace Typed {

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
    case TextFormat::Time:
        return "time"_s;
    case TextFormat::Duration:
        return "duration"_s;
    case TextFormat::Hostname:
        return "hostname"_s;
    case TextFormat::Uri:
        return "uri"_s;
    case TextFormat::UriReference:
        return "uri-reference"_s;
    case TextFormat::Regex:
        return "regex"_s;
    case TextFormat::JsonPointer:
        return "json-pointer"_s;
    case TextFormat::RelativeJsonPointer:
        return "relative-json-pointer"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    case ValidationRule::Kind::UniqueItems:
    case ValidationRule::Kind::Contains:
    case ValidationRule::Kind::Not:
    case ValidationRule::Kind::If:
    case ValidationRule::Kind::ExactlyOne:
    case ValidationRule::Kind::Every:
    case ValidationRule::Kind::PatternMembers:
    case ValidationRule::Kind::MemberNames:
    case ValidationRule::Kind::MinimumMembers:
    case ValidationRule::Kind::MaximumMembers:
    case ValidationRule::Kind::DependentMembers:
    case ValidationRule::Kind::DependentNode:
        return RuleSubject::GivenValue;
    case ValidationRule::Kind::Message:
    case ValidationRule::Kind::DefaultNull:
    case ValidationRule::Kind::DefaultBoolean:
    case ValidationRule::Kind::DefaultNumber:
    case ValidationRule::Kind::DefaultString:
        return RuleSubject::Nothing;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isText(ResultShape shape)
{
    return shape == ResultShape::String || shape == ResultShape::StringLiteral;
}

bool appliesTo(const ValidationRule& rule, ResultShape shape)
{
    switch (subjectOf(rule)) {
    case RuleSubject::Number:
        return shape == ResultShape::Number || shape == ResultShape::NumberLiteral;
    case RuleSubject::Length:
        return isText(shape) || shape == ResultShape::Array || shape == ResultShape::Tuple;
    case RuleSubject::Text:
        return isText(shape);
    case RuleSubject::GivenValue:
    case RuleSubject::Nothing:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasIntegerRule(const ResultNode& node)
{
    return std::ranges::any_of(node.rules, [](const ValidationRule& rule) {
        return rule.is<ValidationRule::Integer>();
    });
}

bool hasRuleOnGivenValue(const ResultNode& node)
{
    return std::ranges::any_of(node.rules, [](const ValidationRule& rule) {
        return subjectOf(rule) == RuleSubject::GivenValue;
    });
}

static OptionSet<RegExpFlags> flagsOf(const ValidationRule::Pattern& pattern)
{
    return pattern.isUnicode ? OptionSet<RegExpFlags> { RegExpFlags::Unicode } : OptionSet<RegExpFlags> { };
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
        return RuleCode::Inline;
    case ValidationRule::Kind::MultipleOf: {
        // The remainder by a whole divisor is exact. That by a fraction is not what the decimal spelling of the two numbers says.
        double divisor = rule.as<ValidationRule::MultipleOf>().divisor;
        return std::trunc(divisor) == divisor ? RuleCode::Inline : RuleCode::ByHelper;
    }
    case ValidationRule::Kind::MinimumLength:
        // One unit is one character, whatever it is.
        return isText(shape) && rule.as<ValidationRule::MinimumLength>().length > 1 ? RuleCode::CountedCharacters : RuleCode::Inline;
    case ValidationRule::Kind::MaximumLength:
        return isText(shape) ? RuleCode::CountedCharacters : RuleCode::Inline;
    case ValidationRule::Kind::Pattern: {
        const EmissionInputs& inputs = m_emission.inputs();
        Vector<char16_t> bufferForNonASCII;
        const auto& pattern = rule.as<ValidationRule::Pattern>();
        StringView source = inputs.program.strings.viewOf(pattern.source, bufferForNonASCII);
        const RegExpSummary& summary = inputs.build.regExpSummaries().summaryOf(inputs.strings.intern(source), toYarrFlags(flagsOf(pattern)), source);
        return summary.isValid() && summary.metadata == RegExpMetadata::Known ? RuleCode::RegularExpression : RuleCode::ByHelper;
    }
    case ValidationRule::Kind::Format:
    case ValidationRule::Kind::Template:
    case ValidationRule::Kind::UniqueItems:
    case ValidationRule::Kind::Contains:
    case ValidationRule::Kind::Not:
    case ValidationRule::Kind::If:
    case ValidationRule::Kind::ExactlyOne:
    case ValidationRule::Kind::Every:
    case ValidationRule::Kind::PatternMembers:
    case ValidationRule::Kind::MemberNames:
    case ValidationRule::Kind::MinimumMembers:
    case ValidationRule::Kind::MaximumMembers:
    case ValidationRule::Kind::DependentMembers:
    case ValidationRule::Kind::DependentNode:
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

static ValueID appendNumberTest(IRBuilder& builder, const ValidationRule& rule, ValueID number)
{
    auto appendWithOperand = [&](Opcode opcode, double operand) {
        return builder.append(opcode, { number, appendNumber(builder, operand) });
    };
    switch (rule.kind()) {
    case ValidationRule::Kind::Integer:
        return builder.appendTypeTest(number, Type::fromPrimitives(Primitive::NumberWithoutFraction));
    case ValidationRule::Kind::Minimum:
        return appendWithOperand(Opcode::GreaterEq, rule.as<ValidationRule::Minimum>().bound);
    case ValidationRule::Kind::Maximum:
        return appendWithOperand(Opcode::LessEq, rule.as<ValidationRule::Maximum>().bound);
    case ValidationRule::Kind::ExclusiveMinimum:
        return appendWithOperand(Opcode::Greater, rule.as<ValidationRule::ExclusiveMinimum>().bound);
    case ValidationRule::Kind::ExclusiveMaximum:
        return appendWithOperand(Opcode::Less, rule.as<ValidationRule::ExclusiveMaximum>().bound);
    case ValidationRule::Kind::MultipleOf:
        return builder.append(Opcode::StrictEq, { appendWithOperand(Opcode::Mod, rule.as<ValidationRule::MultipleOf>().divisor), builder.appendInteger(0) });
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
    case ValidationRule::Kind::UniqueItems:
    case ValidationRule::Kind::Contains:
    case ValidationRule::Kind::Not:
    case ValidationRule::Kind::If:
    case ValidationRule::Kind::ExactlyOne:
    case ValidationRule::Kind::Every:
    case ValidationRule::Kind::PatternMembers:
    case ValidationRule::Kind::MemberNames:
    case ValidationRule::Kind::MinimumMembers:
    case ValidationRule::Kind::MaximumMembers:
    case ValidationRule::Kind::DependentMembers:
    case ValidationRule::Kind::DependentNode:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID appendLength(Emission& emission, ResultShape shape, ValueID value)
{
    if (isText(shape))
        return emission.builder().append(Opcode::GetStringLength, { value });
    RELEASE_ASSERT(shape == ResultShape::Array || shape == ResultShape::Tuple);
    return appendMatchedBuiltinCall(emission, Builtins::ArrayPrototypeLength, BuiltinCallMode::Get, value, { });
}

static ValueID appendLengthTest(IRBuilder& builder, const ValidationRule& rule, ValueID length)
{
    if (const auto* minimum = rule.tryAs<ValidationRule::MinimumLength>())
        return builder.append(Opcode::GreaterEq, { length, builder.appendInteger(minimum->length) });
    return builder.append(Opcode::LessEq, { length, builder.appendInteger(rule.as<ValidationRule::MaximumLength>().length) });
}

// The length of a string is counted in code points. It has at most as many of them as UTF-16 units, and at least half as many: only between the two are they counted.
static void emitCharacterCountRule(Emission& emission, const ValidationRule& rule, ValueID string, BlockID rejected)
{
    IRBuilder& builder = emission.builder();
    ValueID units = builder.append(Opcode::GetStringLength, { string });
    ValueID unitsDecide = appendLengthTest(builder, rule, units);
    BlockFrequency whenCounted = BlockFrequency::Rare;
    if (const auto* minimum = rule.tryAs<ValidationRule::MinimumLength>()) {
        emission.emitJumpUnless(unitsDecide, rejected);
        unitsDecide = builder.append(Opcode::GreaterEq, { units, builder.appendInteger(2 * static_cast<int64_t>(minimum->length) - 1) });
        whenCounted = BlockFrequency::Normal;
    }

    BlockID after = builder.addBlock();
    IRBuilder::BranchBlocks blocks = builder.emitBranch(unitsDecide, BlockFrequency::Normal, whenCounted);
    builder.moveToEndOf(blocks.taken);
    builder.emitJump(after);

    builder.moveToEndOf(blocks.notTaken);
    ValueID characters = emission.appendHelperCall(Builtins::CompilerHelpersCountCodePoints, { string });
    emission.emitJumpUnless(appendLengthTest(builder, rule, characters), rejected);
    builder.emitJump(after);

    builder.moveToEndOf(after);
}

static ValueID appendExpressionTest(Emission& emission, StringView source, OptionSet<RegExpFlags> flags, ValueID string)
{
    IRBuilder& builder = emission.builder();
    Value literal = IRBuilder::make(Opcode::SharedRegExpLiteral);
    literal.setPattern(emission.inputs().strings.intern(source));
    literal.setRegExpFlags(flags);
    ValueID regExp = builder.append(WTF::move(literal));
    // Placement keys the cell of a literal by (function, position), and every other value of a derived function has the same position.
    builder.function().edit(regExp, ValueEdit::Definition).setOrigin({ regExp.index(), builder.origin().inlineFrame });
    // What test matches is the last match that RegExp.$1 and its like tell of.
    if (emission.inputs().flow.mayReadLegacyRegExpStatics())
        return builder.append(Opcode::MatchesUnrecorded, { regExp, string });
    return appendMatchedBuiltinCall(emission, Builtins::RegExpPrototypeTest, BuiltinCallMode::Call, regExp, singleElementSpan(string));
}

void ValidationRuleEmitter::emitRules(const ResultNode& node, ValueID built, ValueID given, BlockID rejected)
{
    IRBuilder& builder = m_emission.builder();
    for (const ValidationRule& rule : node.rules) {
        ValueID value = subjectOf(rule) == RuleSubject::GivenValue ? given : built;
        Vector<char16_t> bufferForNonASCII;
        ValueID test;
        switch (codeFor(rule, node.shape)) {
        case RuleCode::Nothing:
            continue;
        case RuleCode::CountedCharacters:
            emitCharacterCountRule(m_emission, rule, value, rejected);
            continue;
        case RuleCode::ByHelper: {
            StringID text = validationRuleText(m_emission.program(), m_emission.inputs().strings, rule);
            test = m_emission.appendHelperCall(Builtins::CompilerHelpersHoldsRule, { value, m_emission.appendString(text), builder.append(Opcode::GetPreparedPlans, { }) });
            break;
        }
        case RuleCode::Inline:
            if (subjectOf(rule) == RuleSubject::Length) {
                test = appendLengthTest(builder, rule, appendLength(m_emission, node.shape, value));
                break;
            }
            if (rule.is<ValidationRule::Integer>())
                emitRequireFinite(value, rejected);
            test = appendNumberTest(builder, rule, value);
            break;
        case RuleCode::RegularExpression: {
            const auto& pattern = rule.as<ValidationRule::Pattern>();
            test = appendExpressionTest(m_emission, m_emission.program().strings.viewOf(pattern.source, bufferForNonASCII), flagsOf(pattern), value);
            break;
        }
        }
        m_emission.emitJumpUnless(test, rejected);
    }
}

void ValidationRuleEmitter::emitLengthRules(const ResultNode& node, ValueID length, BlockID rejected)
{
    for (const ValidationRule& rule : node.rules) {
        if (subjectOf(rule) == RuleSubject::Length)
            m_emission.emitJumpUnless(appendLengthTest(m_emission.builder(), rule, length), rejected);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
