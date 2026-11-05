/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTTypeArena.h"
#include <array>
#include <bit>
#include <cmath>
#include <wtf/StdLibExtras.h>

#include "AOTWalkNumberGuesses.h"

namespace JSC { namespace AOT { namespace Typed {

static Type typeOfBuiltInNumberBuiltins(Walker& walker, Reference reference)
{
    TypeID id = reference && reference.isRecord() ? walker.typeOf(reference) : TypeID();
    return walker.types().at(id ? id : TypeID(TypeTable::never));
}

static bool takesOnlyNumbersInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    for (Reference argument : call.arguments) {
        if (!typeOfBuiltInNumberBuiltins(walker, argument).isNumber())
            return false;
    }
    return true;
}

static std::span<const ValueID> argumentsWrittenInNumberBuiltins(const BodyValue& call)
{
    return call.opcode() == Opcode::Call ? call.argumentsOperands() : std::span<const ValueID>();
}

static Draft draftOfCodeInLineInNumberBuiltins(Walker& walker, const CallByOneRow& call, std::span<const Reference> arguments)
{
    const BuiltinOverload& overload = walker.program().builtins().at(call.overload);
    Vector<uint32_t, 4> parameters;
    for (uint8_t parameter : std::span(overload.code.implementation.inlineParameters).first(numberOfParametersOf(*call.codeInLine)))
        parameters.append(parameter);

    Draft draft(Opcode::CallInline, call.origin);
    walker.chooseConservativeFields(draft);
    draft.setInlineBuiltin(*call.codeInLine);
    draft.setInlineParameters(parameters.span());
    draft.setOverload(call.overload);
    draft.setArgumentsOperands(arguments);
    draft.inputValueGiven = call.call.id();
    return draft;
}

static Reference buildCodeInLineInNumberBuiltins(Walker& walker, const CallByOneRow& call, std::span<const Reference> arguments)
{
    Effects effects = Effects::none();
    return walker.build(draftOfCodeInLineInNumberBuiltins(walker, call, arguments), &effects);
}

static ValueID recordBehindNamesInNumberBuiltins(const Code& code, ValueID record)
{
    while (opcodesThatGiveSameValue.contains(code.at(record).opcode))
        record = code.at(record).operand(0);
    return record;
}

static bool isResultOfCodeInLineInNumberBuiltins(Walker& walker, Reference reference, InlineBuiltin builtin)
{
    if (!reference.isValue())
        return false;
    const Operation& record = walker.code().at(recordBehindNamesInNumberBuiltins(walker.code(), reference.id()));
    return record.opcode == Opcode::CallInline && record.inlineBuiltin() == builtin;
}

static std::optional<QuotientRounding> roundingOfBuiltinInNumberBuiltins(BuiltinID builtin)
{
    if (builtin == Builtins::MathFloor)
        return QuotientRounding::Floor;
    if (builtin == Builtins::MathCeil)
        return QuotientRounding::Ceil;
    if (builtin == Builtins::MathTrunc)
        return QuotientRounding::Truncate;
    if (builtin == Builtins::MathRound)
        return QuotientRounding::Round;
    return std::nullopt;
}

namespace {

struct RoundedQuotientInNumberBuiltins {
    QuotientRounding rounding;
    DivisionAsWritten division;
};

} // anonymous namespace

static std::optional<RoundedQuotientInNumberBuiltins> findRoundedQuotientInNumberBuiltins(Walker& walker, BuiltinID builtin, const BodyValue& call, const InputsOfTypedOperation& inputs)
{
    std::optional<QuotientRounding> rounding = roundingOfBuiltinInNumberBuiltins(builtin);
    std::span<const ValueID> written = argumentsWrittenInNumberBuiltins(call);
    if (!rounding || written.size() != 1)
        return std::nullopt;
    std::optional<DivisionAsWritten> division = findQuotientOfIntegers(walker, written[0], inputs);
    if (!division)
        return std::nullopt;
    return RoundedQuotientInNumberBuiltins { *rounding, *division };
}

static Reference buildRoundedQuotientInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    if (call.isForwarded)
        return { };
    std::optional<RoundedQuotientInNumberBuiltins> quotient = findRoundedQuotientInNumberBuiltins(walker, call.builtin, call.call, InputsOfTypedOperation(walker, nullptr));
    if (!quotient)
        return { };
    return buildDivisionOfIntegers(walker, call.origin, walker.referenceOf(quotient->division.dividend), walker.referenceOf(quotient->division.divisor), quotient->rounding);
}

TypeID typeOfCallOnNumbersAhead(Walker& walker, const BodyIndex& index, const BodyValue& call, const TypesOfInputs& types)
{
    if (!walker.has(Mechanism::BuiltinCalls) || !hasMechanismsOfNumberForms(walker))
        return { };
    if (&index != &walker.currentBodyIndex() || call.opcode() != Opcode::Call || call.isDoneInPlace())
        return { };
    TypeID typeOfCallee = types(call.calleeOperand());
    if (!typeOfCallee)
        return { };
    Type callee = walker.types().at(typeOfCallee);
    BuiltinID builtin = callee.isObject() ? singleBuiltinOf(walker.types(), callee) : BuiltinID();
    if (!builtin)
        return { };

    std::optional<RoundedQuotientInNumberBuiltins> quotient = findRoundedQuotientInNumberBuiltins(walker, walker.program().builtins().canonicalID(builtin), call, InputsOfTypedOperation(walker, &types));
    if (!quotient || wouldCallBuiltin(walker, call, types) != BuiltinCallAskedAbout::ByOneRow)
        return { };
    return typeOfDivisionOfIntegers(walker, quotient->rounding, types(quotient->division.dividend), types(quotient->division.divisor));
}

static Reference buildExtremumInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    bool asksForGreatest = *call.codeInLine == InlineBuiltin::MathMax;
    if (call.arguments.size() < 2 || !takesOnlyNumbersInNumberBuiltins(walker, call))
        return { };

    Vector<Reference, 8> contenders;
    for (Reference argument : call.arguments) {
        PrimitiveKnownToFolding known = primitiveKnownInNumberForms(walker, argument);
        if (known.kind() == PrimitiveKnownToFolding::Kind::Number && std::isnan(known.asNumber()))
            return walker.has(Mechanism::Folding) ? argument : Reference();
        if (!contenders.contains(argument))
            contenders.append(argument);
    }

    auto rangeOf = [&](Reference integer) {
        Range range = typeOfBuiltInNumberBuiltins(walker, integer).integers;
        return asksForGreatest ? range : Range { -range.maximum, -range.minimum };
    };
    Reference strongest;
    for (Reference contender : contenders) {
        if (!typeOfBuiltInNumberBuiltins(walker, contender).isInteger())
            continue;
        if (!strongest || rangeOf(contender).minimum > rangeOf(strongest).minimum)
            strongest = contender;
    }
    if (strongest) {
        int64_t least = rangeOf(strongest).minimum;
        contenders.removeAllMatching([&](Reference contender) {
            return contender != strongest && typeOfBuiltInNumberBuiltins(walker, contender).isInteger() && rangeOf(contender).maximum <= least;
        });
    }

    if (contenders.size() == call.arguments.size())
        return { };
    if (contenders.size() == 1)
        return walker.has(Mechanism::Folding) ? contenders[0] : Reference();
    return buildCodeInLineInNumberBuiltins(walker, call, contenders.span());
}

static Reference buildToInt32InNumberBuiltins(Walker& walker, Reference number, Origin origin)
{
    if (typeOfBuiltInNumberBuiltins(walker, number).isInt32())
        return number;
    Draft conversion(Opcode::ToInt32, origin);
    walker.chooseConservativeFields(conversion);
    conversion.setValueOperand(number);
    return walker.build(WTF::move(conversion));
}

static Reference buildImulInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    if (call.arguments.size() != 2 || !takesOnlyNumbersInNumberBuiltins(walker, call))
        return { };
    TypeArena& types = walker.types();
    bool takesOnlyInt32 = true;
    for (Reference factor : call.arguments) {
        if (integerKnownInNumberForms(walker, factor) == std::optional<int64_t>(0) && walker.has(Mechanism::Folding))
            return factor;
        takesOnlyInt32 &= typeOfBuiltInNumberBuiltins(walker, factor).isInt32();
    }
    if (takesOnlyInt32) {
        OperatorChoice product = chooseOperatorForm(walker.program(), types, Opcode::Mul, { walker.typeOf(call.arguments[0]), walker.typeOf(call.arguments[1]) });
        if (product.form != OperatorForm::Integers || !types.at(product.result).isInt32())
            return { };
        return buildOperatorInNumberForms(walker, Opcode::Mul, call.arguments[0], call.arguments[1], call.origin);
    }

    std::array<Reference, 2> factors;
    for (uint32_t index = 0; index < 2; ++index) {
        factors[index] = buildToInt32InNumberBuiltins(walker, call.arguments[index], call.origin);
        if (!factors[index])
            return { };
    }
    return buildCodeInLineInNumberBuiltins(walker, call, factors);
}

static Reference buildClz32InNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    if (call.arguments.size() != 1)
        return { };
    Reference number = call.arguments[0];
    Type type = typeOfBuiltInNumberBuiltins(walker, number);
    if (!type.isNumber())
        return { };
    if (!type.isInt32()) {
        Reference bits = buildToInt32InNumberBuiltins(walker, number, call.origin);
        return bits ? buildCodeInLineInNumberBuiltins(walker, call, singleElementSpan(bits)) : Reference();
    }
    if (!walker.has(Mechanism::Folding))
        return { };
    if (type.integers.maximum < 0)
        return integerInNumberForms(walker, 0);
    if (type.integers.minimum < 0)
        return { };
    int leadingZeros = std::countl_zero(static_cast<uint32_t>(type.integers.minimum));
    return leadingZeros == std::countl_zero(static_cast<uint32_t>(type.integers.maximum)) ? integerInNumberForms(walker, leadingZeros) : Reference();
}

static Reference buildPowerInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    if (call.arguments.size() != 2 || !takesOnlyNumbersInNumberBuiltins(walker, call))
        return { };
    PrimitiveKnownToFolding exponent = primitiveKnownInNumberForms(walker, call.arguments[1]);
    if (exponent.kind() != PrimitiveKnownToFolding::Kind::Number)
        return { };
    if (exponent.asNumber() == 2)
        return buildOperatorInNumberForms(walker, Opcode::Mul, call.arguments[0], call.arguments[0], call.origin);
    if (!walker.has(Mechanism::Folding))
        return { };
    if (!exponent.asNumber())
        return integerInNumberForms(walker, 1);
    return exponent.asNumber() == 1 ? call.arguments[0] : Reference();
}

static Reference buildFunctionOfOneNumberInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    if (call.arguments.size() != 1)
        return { };
    Reference number = call.arguments[0];
    Type type = typeOfBuiltInNumberBuiltins(walker, number);
    if (!type.isNumber())
        return { };
    bool mayHandOn = walker.has(Mechanism::Folding);
    auto liesWithin = [&](int64_t limit) {
        return type.isInteger() && type.integers.isSubsetOf({ -limit, limit });
    };

    switch (*call.codeInLine) {
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathRound:
        return mayHandOn && type.isNumberWithoutFraction() ? number : Reference();
    case InlineBuiltin::MathAbs:
        if (mayHandOn && (isResultOfCodeInLineInNumberBuiltins(walker, number, InlineBuiltin::MathAbs) || (type.isInteger() && type.integers.minimum >= 0)))
            return number;
        if (type.isInteger() && type.integers.maximum < 0)
            return buildOperatorInNumberForms(walker, Opcode::Negate, number, { }, call.origin);
        return { };
    case InlineBuiltin::MathHypot:
        return mayHandOn && type.isInteger() && type.integers.minimum >= 0 ? number : Reference();
    case InlineBuiltin::MathSign:
        return mayHandOn && (isResultOfCodeInLineInNumberBuiltins(walker, number, InlineBuiltin::MathSign) || liesWithin(1)) ? number : Reference();
    case InlineBuiltin::MathFround:
        return mayHandOn && liesWithin(int64_t { 1 } << 24) ? number : Reference();
    case InlineBuiltin::MathF16Round:
        return mayHandOn && liesWithin(int64_t { 1 } << 11) ? number : Reference();
    default:
        return { };
    }
}

static Reference constantWhereRowLeavesOneIntegerInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    if (!walker.has(Mechanism::Folding) || call.arguments.empty() || !takesOnlyNumbersInNumberBuiltins(walker, call))
        return { };
    TypeID result = walker.resultTypeOf(draftOfCodeInLineInNumberBuiltins(walker, call, call.arguments));
    if (!result)
        return { };
    Type type = walker.types().at(result);
    return type.isInteger() && type.integers.isSingleValue() ? integerInNumberForms(walker, type.integers.minimum) : Reference();
}

static Reference buildSimplerMathCallInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    if (!call.codeInLine || call.receiver)
        return { };
    switch (*call.codeInLine) {
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax:
        if (Reference constant = constantWhereRowLeavesOneIntegerInNumberBuiltins(walker, call))
            return constant;
        return buildExtremumInNumberBuiltins(walker, call);
    case InlineBuiltin::MathImul:
        return buildImulInNumberBuiltins(walker, call);
    case InlineBuiltin::MathClz32:
        return buildClz32InNumberBuiltins(walker, call);
    case InlineBuiltin::MathPow:
        return buildPowerInNumberBuiltins(walker, call);
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathRound:
    case InlineBuiltin::MathAbs:
    case InlineBuiltin::MathHypot:
    case InlineBuiltin::MathSign:
    case InlineBuiltin::MathFround:
    case InlineBuiltin::MathF16Round:
        if (Reference constant = constantWhereRowLeavesOneIntegerInNumberBuiltins(walker, call))
            return constant;
        return buildFunctionOfOneNumberInNumberBuiltins(walker, call);
    default:
        return { };
    }
}

static Reference buildConversionByCallInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    BuiltinID builtin = call.builtin;
    bool isNumber = builtin == Builtins::Number;
    bool isString = builtin == Builtins::String;
    bool isBoolean = builtin == Builtins::Boolean;
    if (!isNumber && !isString && !isBoolean && builtin != Builtins::Object && builtin != Builtins::BigInt)
        return { };
    if (call.call.opcode() != Opcode::Call || call.isForwarded)
        return { };
    bool mayHandOn = walker.has(Mechanism::Folding);

    if (argumentsWrittenInNumberBuiltins(call.call).empty()) {
        if (!mayHandOn)
            return { };
        if (isNumber)
            return integerInNumberForms(walker, 0);
        if (isString)
            return walker.constant(PrimitiveKnownToFolding::text(walker.program().strings.emptyString()));
        return isBoolean ? booleanInNumberForms(walker, false) : Reference();
    }
    if (call.arguments.empty())
        return { };

    Reference value = call.arguments[0];
    Type type = typeOfBuiltInNumberBuiltins(walker, value);
    if (type.isNever() || !type.isJavaScriptValue() || type.mayBeUnspecified() || type.primitives & Primitive::Absent)
        return { };

    bool isAlreadyConverted = false;
    if (isNumber)
        isAlreadyConverted = type.isNumber();
    else if (isString)
        isAlreadyConverted = type.isAnyString();
    else if (isBoolean)
        isAlreadyConverted = type.isBoolean();
    else if (builtin == Builtins::Object)
        isAlreadyConverted = type.isObject();
    else
        isAlreadyConverted = type.hasOnlyPrimitives(Primitive::BigInt);
    if (isAlreadyConverted)
        return mayHandOn ? value : Reference();

    if (isNumber && type.isNumberLike())
        return buildOperatorInNumberForms(walker, Opcode::ToNumber, value, { }, call.origin);
    if (isBoolean)
        return buildOperatorInNumberForms(walker, Opcode::ToBoolean, value, { }, call.origin);
    return { };
}

Reference buildCallOnNumbers(Walker& walker, const CallByOneRow& call)
{
    if (!hasMechanismsOfNumberForms(walker))
        return { };

    if (walker.has(Mechanism::Folding)) {
        if (std::optional<PrimitiveKnownToFolding> folded = foldExactly(walker, call)) {
            Reference constant = walker.constant(*folded);
            TypeID byRow = walker.resultTypeOf(draftOfCodeInLineInNumberBuiltins(walker, call, call.arguments));
            if (constant && byRow && walker.types().isSubset(walker.typeOf(constant), byRow))
                return constant;
        }
    }
    if (Reference quotient = buildRoundedQuotientInNumberBuiltins(walker, call))
        return quotient;
    if (Reference simpler = buildSimplerMathCallInNumberBuiltins(walker, call))
        return simpler;
    if (Reference converted = buildConversionByCallInNumberBuiltins(walker, call))
        return converted;
    if (Reference charCode = buildCharCode(walker, call))
        return charCode;
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
