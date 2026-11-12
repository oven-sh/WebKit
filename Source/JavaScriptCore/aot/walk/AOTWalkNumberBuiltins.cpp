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
#include "ParseInt.h"
#include <array>
#include <bit>
#include <cmath>
#include <wtf/MathExtras.h>
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
    while (valuePreservingOpcodes.contains(code.at(record).opcode))
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

static bool isUnitFractionInNumberBuiltins(Walker& walker, Reference number)
{
    if (!number || !number.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& record = code.at(recordBehindNamesInNumberBuiltins(code, number.id()));
    if (record.opcode == Opcode::CallInline)
        return record.inlineBuiltin() == InlineBuiltin::MathRandom;
    if (record.opcode != Opcode::Div)
        return false;
    constexpr Range unsignedRange { 0, std::numeric_limits<uint32_t>::max() };
    auto referenceTo = [&](ValueID operand) {
        return code.isConstant(operand) ? Reference::constant(operand) : Reference::value(operand);
    };
    Type dividend = typeOfBuiltInNumberBuiltins(walker, referenceTo(record.operand(0)));
    Type divisor = typeOfBuiltInNumberBuiltins(walker, referenceTo(record.operand(1)));
    return dividend.isInteger() && dividend.integers.isSubsetOf(unsignedRange) && divisor.isInteger() && divisor.integers.isSubsetOf({ unsignedRange.maximum + 1, unsignedRange.maximum + 1 });
}

static Reference buildIntegerPartOfScaledFractionInNumberBuiltins(Walker& walker, Reference number, Origin origin)
{
    if (!number.isValue())
        return { };
    const Code& code = walker.code();
    const Operation& product = code.at(recordBehindNamesInNumberBuiltins(code, number.id()));
    if (product.opcode != Opcode::Mul)
        return { };
    auto referenceTo = [&](ValueID operand) {
        return code.isConstant(operand) ? Reference::constant(operand) : Reference::value(operand);
    };
    Reference operands[] { referenceTo(product.operand(0)), referenceTo(product.operand(1)) };
    for (unsigned place = 0; place < 2; ++place) {
        Type count = typeOfBuiltInNumberBuiltins(walker, operands[1 - place]);
        if (!count.isInteger() || !count.integers.isSubsetOf({ 0, int64_t { 1 } << 32 }) || !isUnitFractionInNumberBuiltins(walker, operands[place]))
            continue;
        bool isInt32 = count.integers.maximum <= int64_t { 1 } << 31;
        return buildOperatorInNumberForms(walker, isInt32 ? Opcode::BitOr : Opcode::URShift, number, integerInNumberForms(walker, 0), origin);
    }
    return { };
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
    return buildDivisionOfIntegers(walker, call.origin, quotient->division.dividend, quotient->division.divisor, quotient->rounding);
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
    case InlineBuiltin::MathTrunc:
        if (!mayHandOn || type.isNumberWithoutFraction())
            return mayHandOn ? number : Reference();
        return buildIntegerPartOfScaledFractionInNumberBuiltins(walker, number, call.origin);
    case InlineBuiltin::MathCeil:
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
    if (!walker.has(Mechanism::Folding) || walker.checksProofs() || call.arguments.empty() || !takesOnlyNumbersInNumberBuiltins(walker, call))
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

static Reference buildParsedIntegerOfFractionInNumberBuiltins(Walker& walker, const CallByOneRow& call, Reference number)
{
    auto holds = [&](Opcode relation, Reference left, Reference right) {
        return buildOperatorInNumberForms(walker, relation, left, right, call.origin);
    };
    auto written = [&](double value) {
        return walker.constant(PrimitiveKnownToFolding::number(value));
    };
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID inputBlock = index.blockOf(call.call.id());
    uint32_t place = call.call.id().index() - index.block(inputBlock).firstValue().index();

    BlockID truncating = walker.addBlock();
    BlockID parsing = walker.addBlock(BlockFrequency::Rare);
    BlockID notTiny = walker.addBlock();
    BlockID belowTiny = walker.addBlock();
    BlockID atMostMinusOne = walker.addBlock();
    BlockID aroundZero = walker.addBlock();
    walker.branch(holds(Opcode::LessEq, written(0.000001), number), notTiny, belowTiny);
    walker.continueIn(notTiny);
    walker.branch(holds(Opcode::LessEq, number, written(maxSafeInteger())), truncating, parsing);
    walker.continueIn(belowTiny);
    walker.branch(holds(Opcode::LessEq, number, written(-1)), atMostMinusOne, aroundZero);
    walker.continueIn(atMostMinusOne);
    walker.branch(holds(Opcode::LessEq, written(-maxSafeInteger()), number), truncating, parsing);
    walker.continueIn(aroundZero);
    walker.branch(holds(Opcode::StrictEq, number, written(0)), truncating, parsing);

    Vector<Walker::JoinArm, 2> arms;
    walker.continueIn(truncating);
    Draft truncation(Opcode::ToIntegerOrInfinity, call.origin);
    walker.chooseConservativeFields(truncation);
    truncation.setValueOperand(number);
    if (Reference truncated = walker.build(WTF::move(truncation)))
        arms.append(Walker::JoinArm { walker.currentBlock(), { truncated } });
    walker.continueIn(parsing);
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(inputBlock, place, place + 1);
        if (walker.hasFailed())
            return { };
        if (!walker.isUnreachable())
            arms.append(Walker::JoinArm { walker.currentBlock(), { walker.referenceOf(call.call.id()) } });
    }
    std::array<Reference, 1> met;
    return !arms.isEmpty() && walker.joinArms(arms.span(), walker.addBlock(), met) ? met[0] : Reference();
}

static Reference buildParseOfProvenNumber(Walker& walker, const CallByOneRow& call, Reference number, bool parsesInteger)
{
    Type type = typeOfBuiltInNumberBuiltins(walker, number);
    if (type.isInteger())
        return number;
    if (PrimitiveKnownToFolding known = primitiveKnownInNumberForms(walker, number); known.kind() == PrimitiveKnownToFolding::Kind::Number) {
        std::optional<double> parsed = parsesInteger ? parseIntDouble(known.asNumber()) : std::optional<double>(known.asNumber() + 0.0);
        return parsed ? walker.constant(PrimitiveKnownToFolding::number(*parsed)) : Reference();
    }
    if (!parsesInteger)
        return buildOperatorInNumberForms(walker, Opcode::Add, number, integerInNumberForms(walker, 0), call.origin);
    const BodyIndex& index = walker.currentBodyIndex();
    bool standsInLoop = walker.isInLoop() || index.loopDepthOf(index.blockOf(call.call.id()));
    return standsInLoop && walker.opensGuesses() && number.isValue() ? buildParsedIntegerOfFractionInNumberBuiltins(walker, call, number) : Reference();
}

static Reference buildParseAfterTest(Walker& walker, const CallByOneRow& call, Reference value, TypeID tested, bool parsesInteger)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID inputBlock = index.blockOf(call.call.id());
    uint32_t place = call.call.id().index() - index.block(inputBlock).firstValue().index();

    Draft question(Opcode::TypeTest, call.origin);
    question.setValueOperand(value);
    question.setTestedType(tested);
    Reference isNumber = walker.build(WTF::move(question));
    if (!isNumber)
        return { };
    BlockID holdsNumber = walker.addBlock();
    BlockID holdsOther = walker.addBlock(walker.types().at(walker.typeOf(value)).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull) ? BlockFrequency::Rare : BlockFrequency::Normal);
    walker.branch(isNumber, holdsNumber, holdsOther);

    Vector<Walker::JoinArm, 2> arms;
    auto walkAsWritten = [&] {
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(inputBlock, place, place + 1);
        if (!walker.isUnreachable() && !walker.hasFailed())
            arms.append(Walker::JoinArm { walker.currentBlock(), { walker.referenceOf(call.call.id()) } });
    };
    walker.continueIn(holdsNumber);
    if (!walker.isUnreachable()) {
        Draft narrowed(Opcode::Narrowed, call.origin);
        narrowed.setValueOperand(value);
        narrowed.setTestOperand(isNumber);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        Reference number = walker.build(WTF::move(narrowed));
        Reference parsed = number ? buildParseOfProvenNumber(walker, call, number, parsesInteger) : Reference();
        if (parsed && !walker.hasFailed() && !walker.isUnreachable())
            arms.append(Walker::JoinArm { walker.currentBlock(), { parsed } });
        else
            walkAsWritten();
    }
    walker.continueIn(holdsOther);
    walkAsWritten();
    if (walker.hasFailed())
        return { };
    std::array<Reference, 1> met;
    return !arms.isEmpty() && walker.joinArms(arms.span(), walker.addBlock(), met) ? met[0] : Reference();
}

static Reference buildParseOfNumberInNumberBuiltins(Walker& walker, const CallByOneRow& call)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID builtin = table.canonicalID(call.builtin);
    bool parsesInteger = builtin == table.canonicalID(Builtins::ParseInt);
    if ((!parsesInteger && builtin != table.canonicalID(Builtins::ParseFloat)) || call.call.opcode() != Opcode::Call || call.isForwarded || !walker.has(Mechanism::Folding))
        return { };
    std::span<const ValueID> arguments = argumentsWrittenInNumberBuiltins(call.call);
    if (arguments.empty() || !arguments[0])
        return { };
    Reference value = walker.referenceOf(arguments[0]);
    Type type = typeOfBuiltInNumberBuiltins(walker, value);
    bool isNumber = type.isNumber();
    const BodyIndex& index = walker.currentBodyIndex();
    bool standsInLoop = walker.isInLoop() || index.loopDepthOf(index.blockOf(call.call.id()));
    bool mayBeNumber = !isNumber && !index.isUnused(call.call.id()) && walker.opensGuesses() && value && value.isValue() && type.isJavaScriptValue() && walker.types().overlap(walker.typeOf(value), TypeTable::number);
    bool asksWhole = standsInLoop && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    TypeID tested = asksWhole ? TypeTable::number : TypeTable::int32;
    if (!isNumber && (!mayBeNumber || !walker.types().overlap(walker.typeOf(value), tested)))
        return { };
    if (parsesInteger && arguments.size() > 1) {
        PrimitiveKnownToFolding radix = arguments[1] ? primitiveKnownInNumberForms(walker, walker.referenceOf(arguments[1])) : PrimitiveKnownToFolding();
        bool isDecimal = radix.kind() == PrimitiveKnownToFolding::Kind::Undefined || (radix.kind() == PrimitiveKnownToFolding::Kind::Number && (!radix.asNumber() || radix.asNumber() == 10));
        if (!isDecimal)
            return { };
    }
    return isNumber ? buildParseOfProvenNumber(walker, call, value, parsesInteger) : buildParseAfterTest(walker, call, value, tested, parsesInteger);
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
    if (Reference parsed = buildParseOfNumberInNumberBuiltins(walker, call))
        return parsed;
    if (Reference charCode = buildCharCode(walker, call))
        return charCode;
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
