/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTWalkStrings.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);

static constexpr OpcodeSet readersOfLengthInTextBuilderReads { Opcode::PropGet, Opcode::ToBoolean, Opcode::StrictEq, Opcode::Eq };
static_assert(OperandIndex::StrictEq::left == OperandIndex::Eq::left && OperandIndex::StrictEq::right == OperandIndex::Eq::right);

static bool isEmptyTextInTextBuilderReads(const Walker& walker, Reference reference)
{
    if (!reference || !reference.isConstant())
        return false;
    const Operation& constant = walker.code().at(reference.id());
    return constant.opcode == Opcode::String && constant.string() == walker.program().strings.emptyString();
}

static Reference lengthOfTextInTextBuilderReads(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    if (text.blockOfState != walker.currentBlock()) {
        text.stringOfState = { };
        text.lengthOfState = { };
        text.blockOfState = walker.currentBlock();
    }
    if (text.lengthOfState)
        return text.lengthOfState;

    Draft length(text.stringOfState ? Opcode::GetStringLength : Opcode::StringBuilderLength, origin);
    walker.chooseConservativeFields(length);
    if (text.stringOfState)
        length.setStringOperand(text.stringOfState);
    else {
        length.setBuilderOperand(name);
        becomeOperationOfStringBuilderInFrame(walker, text, name, length, Opcode::StackStringBuilderLength);
        if (walker.isUnreachable())
            return { };
    }
    Reference numberOfCodeUnits = walker.build(WTF::move(length));
    if (!numberOfCodeUnits)
        return { };
    text.lengthOfState = boxNumberIfNeeded(walker, numberOfCodeUnits, origin);
    return text.lengthOfState;
}

Reference lengthOfStringInBuilder(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    return lengthOfTextInTextBuilderReads(walker, text, name, origin);
}

static Reference buildTestOfLengthInTextBuilderReads(Walker& walker, Draft&& test)
{
    walker.chooseConservativeFields(test);
    test.setForm(operatorFormFor(walker, test));
    return walker.build(WTF::move(test));
}

Reference buildTruthinessOfStringInBuilder(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin);
    if (!length)
        return { };
    Draft hasCodeUnits(Opcode::ToBoolean, origin);
    hasCodeUnits.setValueOperand(length);
    return buildTestOfLengthInTextBuilderReads(walker, WTF::move(hasCodeUnits));
}

bool canCompareBuilderWithLiteral(const Walker& walker, const StringInBuilder& text, StringID literal)
{
    static constexpr unsigned maxLengthCompared = 24;
    unsigned length = lengthOfTextForStrings(walker, literal);
    if (!text.liesInFrame || !length || length > maxLengthCompared)
        return false;
    Vector<char16_t> buffer;
    StringView characters = walker.types().viewOf(literal, buffer);
    return characters.is8Bit() || WTF::charactersAreAllLatin1(characters.span16());
}

Reference buildBuilderIsOnStack(Walker& walker, Reference name, Origin origin)
{
    Draft isOnStack(Opcode::StackStringBuilderIsOnStack, origin);
    walker.chooseConservativeFields(isOnStack);
    isOnStack.setBuilderOperand(name);
    return walker.build(WTF::move(isOnStack));
}

Reference buildBuilderEqualsLiteral(Walker& walker, Reference name, StringID literal, Origin origin)
{
    walker.count(StatisticID::BuilderLiteralComparisons);
    Draft comparison(Opcode::StackStringBuilderEqualsLiteral, origin);
    walker.chooseConservativeFields(comparison);
    comparison.setBuilderOperand(name);
    comparison.setLiteral(literal);
    return walker.build(WTF::move(comparison));
}

static bool buildEqualityAfterLengthCheck(Walker& walker, const BodyValue& reader, StringInBuilder& text, Reference name, Reference written, Origin origin)
{
    StringID known = written ? textKnownForStrings(walker, written) : StringID();
    if (!known || operandsNarrowedBehindTestForStrings(walker, reader.id()) != std::optional<uint32_t>(0))
        return false;
    Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin);
    if (!length)
        return true;
    Draft hasLength(Opcode::StrictEq, origin);
    hasLength.setLeftOperand(length);
    hasLength.setRightOperand(integerForStrings(walker, lengthOfTextForStrings(walker, known)));
    Reference isOfSameLength = buildTestOfLengthInTextBuilderReads(walker, WTF::move(hasLength));
    std::array<Reference, 1> isEqual;
    auto compareString = [&](std::span<Reference> results) {
        Reference string = materializeStringInBuilder(walker, text, name, origin);
        results[0] = string ? buildOperatorForStrings(walker, Opcode::StrictEq, string, written, origin) : Reference();
    };
    auto compare = [&](std::span<Reference> results) {
        if (!canCompareBuilderWithLiteral(walker, text, known)) {
            compareString(results);
            return;
        }
        buildArmsForStrings(walker, buildBuilderIsOnStack(walker, name, origin), BlockFrequency::Unlikely, ArmForStrings([&](std::span<Reference> compared) {
            compared[0] = buildBuilderEqualsLiteral(walker, name, known, origin);
        }), ArmForStrings(compareString), results);
    };
    auto giveNo = [&](std::span<Reference> results) {
        results[0] = walker.constant(PrimitiveKnownToFolding::boolean(false));
    };
    bool isBuilt = buildArmsForStrings(walker, isOfSameLength, BlockFrequency::Normal, ArmForStrings(compare), ArmForStrings(giveNo), isEqual);
    text.stringOfState = { };
    text.lengthOfState = { };
    if (isBuilt)
        walker.bind(reader.id(), isEqual[0]);
    return true;
}

static Reference buildNumberOffBuilder(Walker& walker, StringInBuilder& text, Reference name, Reference length, Origin origin, std::optional<uint32_t> radix)
{
    std::array<Reference, 1> number;
    bool isBuilt = buildArmsForStrings(walker, buildBuilderIsOnStack(walker, name, origin), BlockFrequency::Unlikely, ArmForStrings([&](std::span<Reference> met) {
        Draft read(radix ? Opcode::StackStringBuilderParseInt : Opcode::StackStringBuilderToNumber, origin);
        walker.chooseConservativeFields(read);
        read.setBuilderOperand(name);
        read.setLengthOperand(length);
        if (radix)
            read.setRadix(*radix);
        Reference spelled = walker.build(WTF::move(read));
        met[0] = spelled ? boxNumberIfNeeded(walker, spelled, origin) : Reference();
    }), ArmForStrings([&](std::span<Reference> met) {
        Reference string = materializeStringInBuilder(walker, text, name, origin);
        if (!string)
            return;
        PieceOfString piece { string, integerForStrings(walker, 0), length };
        Reference spelled = radix ? buildParsedIntegerOfRange(walker, piece, *radix ? integerForStrings(walker, *radix) : Reference(), origin) : buildNumberOfRange(walker, piece, origin);
        met[0] = spelled ? boxNumberIfNeeded(walker, spelled, origin) : Reference();
    }), number);
    text.stringOfState = { };
    text.lengthOfState = { };
    return isBuilt ? number[0] : Reference();
}

static bool buildNumberOfBuilder(Walker& walker, const BodyValue& reader, StringInBuilder& text, Reference name)
{
    if (!text.liesInFrame || walker.nameLastBoundTo(reader.operand(0)) != name || walker.isUnreachable())
        return false;
    Origin origin = walker.originOf(reader);
    origin.isTailCall = false;
    Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin);
    if (!length)
        return true;
    if (text.stringOfState && text.blockOfState == walker.currentBlock())
        return false;
    walker.count(StatisticID::NumbersReadInBuilders);
    if (Reference number = buildNumberOffBuilder(walker, text, name, length, origin, std::nullopt))
        walker.bind(reader.id(), number);
    return true;
}

static bool buildParsedIntegerOfBuilder(Walker& walker, const BodyValue& reader, StringInBuilder& text, Reference name)
{
    auto call = reader.as<Opcode::Call>();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!text.liesInFrame || arguments.empty() || arguments.size() > 2 || !arguments[0] || walker.nameLastBoundTo(arguments[0]) != name || walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee)
        return false;
    TypeID known = walker.typeOf(callee);
    TypeID expected = findExpectation(walker, call.calleeOperand()).type;
    TypeID tested;
    bool isProven = false;
    bool parsesInteger = false;
    for (BuiltinID builtin : { Builtins::ParseInt, Builtins::Number }) {
        TypeID only = types.forBuiltin(walker.program().builtins().canonicalID(builtin));
        isProven = types.isSubset(known, only);
        if (!isProven && (!expected || !types.isSubset(expected, only) || !types.overlap(known, only)))
            continue;
        tested = only;
        parsesInteger = builtin == Builtins::ParseInt;
        break;
    }
    if (!tested || (!parsesInteger && arguments.size() != 1))
        return false;
    if (!isProven && (!callee.isValue() || !walker.isInLoop() || !walker.opensGuesses() || walker.standsInClosedArm()))
        return false;
    uint32_t radix = 0;
    if (arguments.size() == 2) {
        Reference written = arguments[1] ? walker.referenceOf(arguments[1]) : Reference();
        Type type = written ? types.at(walker.typeOf(written)) : Type();
        if (!written || !type.isInt32() || !type.integers.isSingleValue() || (type.integers.minimum && (type.integers.minimum < 2 || type.integers.minimum > 36)))
            return false;
        radix = static_cast<uint32_t>(type.integers.minimum);
    }
    Origin origin = walker.originOf(reader);
    origin.isTailCall = false;
    Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin);
    if (!length)
        return true;
    if (text.stringOfState && text.blockOfState == walker.currentBlock())
        return false;
    walker.count(parsesInteger ? StatisticID::IntegersParsedInBuilders : StatisticID::NumbersReadInBuilders);
    auto parse = [&](std::span<Reference> results) {
        results[0] = buildNumberOffBuilder(walker, text, name, length, origin, parsesInteger ? std::optional(radix) : std::nullopt);
    };
    std::array<Reference, 1> parsed;
    bool isBuilt = true;
    if (isProven)
        parse(parsed);
    else {
        Draft test(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(callee);
        test.setTestedType(tested);
        walker.recordSpeculation();
        isBuilt = buildArmsForStrings(walker, walker.build(WTF::move(test)), BlockFrequency::Rare, ArmForStrings(parse), ArmForStrings([&](std::span<Reference> results) {
            Reference string = materializeStringInBuilder(walker, text, name, origin);
            if (!string)
                return;
            walker.bind(arguments[0], string);
            Draft asWritten = walker.draftOf(reader);
            walker.chooseConservativeFields(asWritten);
            results[0] = walker.build(WTF::move(asWritten));
            walker.bind(arguments[0], name);
        }), parsed);
    }
    text.stringOfState = { };
    text.lengthOfState = { };
    if (isBuilt && parsed[0])
        walker.bind(reader.id(), parsed[0]);
    return true;
}

bool readsTextWithoutString(Walker& walker, const BodyValue& reader, StringInBuilder& text, Reference name)
{
    Opcode opcode = reader.opcode();
    if (opcode == Opcode::Call)
        return buildParsedIntegerOfBuilder(walker, reader, text, name);
    if (opcode == Opcode::ToNumber || opcode == Opcode::ToNumeric)
        return buildNumberOfBuilder(walker, reader, text, name);
    if (!readersOfLengthInTextBuilderReads.contains(opcode))
        return false;
    auto namesText = [&](ValueID operand) {
        return operand && walker.nameLastBoundTo(operand) == name;
    };

    Origin origin = walker.originOf(reader);
    switch (opcode) {
    case Opcode::PropGet: {
        if (reader.isFreeName() || reader.key() != walker.program().keyOf(BuiltinNames::length) || !namesText(reader.objectOperand()))
            return false;
        if (Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin))
            walker.bind(reader.id(), length);
        return true;
    }
    case Opcode::ToBoolean: {
        if (!namesText(reader.valueOperand()))
            return false;
        if (Reference truth = buildTruthinessOfStringInBuilder(walker, text, name, origin))
            walker.bind(reader.id(), truth);
        return true;
    }
    case Opcode::StrictEq:
    case Opcode::Eq: {
        ValueID left = reader.operand(OperandIndex::StrictEq::left);
        ValueID right = reader.operand(OperandIndex::StrictEq::right);
        bool comparesWithEmptyText = (namesText(left) && isEmptyTextInTextBuilderReads(walker, walker.referenceOf(right))) || (namesText(right) && isEmptyTextInTextBuilderReads(walker, walker.referenceOf(left)));
        if (!comparesWithEmptyText) {
            if (namesText(left) == namesText(right))
                return false;
            return buildEqualityAfterLengthCheck(walker, reader, text, name, walker.referenceOf(namesText(left) ? right : left), origin);
        }
        Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin);
        if (!length)
            return true;
        Draft isEmpty(Opcode::StrictEq, origin);
        isEmpty.inputValueGiven = reader.id();
        isEmpty.setLeftOperand(length);
        isEmpty.setRightOperand(walker.constant(Draft(Opcode::Integer, origin)));
        if (Reference answer = buildTestOfLengthInTextBuilderReads(walker, WTF::move(isEmpty)))
            walker.bind(reader.id(), answer);
        return true;
    }
    default:
        return false;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
