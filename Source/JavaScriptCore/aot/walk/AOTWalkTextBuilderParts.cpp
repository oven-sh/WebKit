/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLimits.h"
#include "AOTTypeArena.h"
#include "AOTWalkStrings.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesTakenInTextBuilderParts = Primitive::All | Primitive::LiteralString | Primitive::LiteralSymbol;

static Opcode narrowestFormInTextBuilderParts(const Type& type)
{
    if (type.isInteger())
        return Opcode::StringBuilderAppendInteger;
    if (type.isNumber())
        return Opcode::StringBuilderAppendNumber;
    if (type.isBoolean())
        return Opcode::StringBuilderAppendBoolean;
    if (type.isAnyString())
        return Opcode::StringBuilderAppendString;
    if (type.hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull))
        return Opcode::StringBuilderAppendStringOrNullish;
    return Opcode::StringBuilderAppendValue;
}

bool partHasFormOfItsOwnInStringBuilder(const Type& type)
{
    return narrowestFormInTextBuilderParts(type) != Opcode::StringBuilderAppendValue || ((type.primitives & Primitive::Number) && !(type.primitives & Primitive::AnyString));
}

static Opcode formInFrameInTextBuilderParts(Opcode form)
{
    switch (form) {
    case Opcode::StringBuilderAppendConstant:
        return Opcode::StackStringBuilderAppendConstant;
    case Opcode::StringBuilderAppendInteger:
        return Opcode::StackStringBuilderAppendInteger;
    case Opcode::StringBuilderAppendNumber:
        return Opcode::StackStringBuilderAppendNumber;
    case Opcode::StringBuilderAppendBoolean:
        return Opcode::StackStringBuilderAppendBoolean;
    case Opcode::StringBuilderAppendCodeUnit:
        return Opcode::StackStringBuilderAppendCodeUnit;
    case Opcode::StringBuilderAppendString:
        return Opcode::StackStringBuilderAppendString;
    case Opcode::StringBuilderAppendStringOrNullish:
        return Opcode::StackStringBuilderAppendStringOrNullish;
    default:
        RELEASE_ASSERT(form == Opcode::StringBuilderAppendValue);
        return Opcode::StackStringBuilderAppendValue;
    }
}

StringID textOfKnownPartOfStringBuilder(Walker& walker, Reference part)
{
    if (!part || !part.isRecord())
        return { };
    const Operation& record = walker.code().at(part.id());
    if (record.opcode == Opcode::String)
        return record.string();

    TypeID id = walker.typeOf(part);
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (!type.isJavaScriptValue() || type.hasObjects())
        return { };
    const StringTable& strings = walker.program().strings;
    switch (type.primitives) {
    case Primitive::Undefined:
        return strings.textOfUndefined();
    case Primitive::Null:
        return strings.textOfNull();
    case Primitive::False:
        return strings.textOf(false);
    case Primitive::True:
        return strings.textOf(true);
    case Primitive::LiteralString:
        return singleStringLiteralOf(type);
    default:
        return { };
    }
}

static void buildPartInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Opcode form, Reference value, StringID constant, Origin origin)
{
    Draft part(form, origin);
    walker.chooseConservativeFields(part);
    part.setBuilderOperand(name);
    becomeOperationOfStringBuilderInFrame(walker, text, name, part, formInFrameInTextBuilderParts(form));
    if (walker.isUnreachable())
        return;
    if (constant)
        part.setString(constant);
    else
        part.setValueOperand(value);

    text.stringOfState = { };
    text.lengthOfState = { };
    walker.build(WTF::move(part));
}

using CodeUnitsOfText = Vector<Reference, Limits::maxPackedCodeUnits>;

static CodeUnitsOfText codeUnitsThatTextIsMadeOfInTextBuilderParts(Walker& walker, Reference part, Origin origin)
{
    Reference root = rootForStrings(walker, part);
    if (!root.isValue())
        return { };
    const Operation& made = walker.code().at(root.id());
    if (made.opcode != Opcode::CallInline || made.inlineBuiltin() != InlineBuiltin::StringFromCharCode)
        return { };
    CodeUnitsOfText codeUnits;
    for (ValueID argument : made.argumentsOperands()) {
        Reference number = referenceToRecordForStrings(walker, argument);
        if (!walker.types().at(walker.typeOf(number)).isNumber())
            return { };
        codeUnits.append(number);
    }
    for (Reference& codeUnit : codeUnits) {
        codeUnit = buildOperatorForStrings(walker, Opcode::BitAnd, codeUnit, integerForStrings(walker, UINT16_MAX), origin);
        if (!codeUnit)
            return { };
    }
    return codeUnits;
}

static bool appendCharacterByItsCodeUnitInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Reference part, const Type& type, Origin origin)
{
    if (!type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined))
        return false;
    Reference codeUnit = codePointOfCharacterInSight(walker, part);
    if (!codeUnit) {
        CodeUnitsOfText codeUnits = codeUnitsThatTextIsMadeOfInTextBuilderParts(walker, part, origin);
        if (codeUnits.size() > 1) {
            if (handlerReadsStringInBuilder(walker, text))
                return false;
            for (Reference each : codeUnits)
                buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendCodeUnit, each, { }, origin);
            walker.count(StatisticID::PackedCodeUnitsAppended);
            return true;
        }
        if (!codeUnits.isEmpty())
            codeUnit = codeUnits[0];
    }
    if (!codeUnit)
        return false;
    Range units = walker.types().at(walker.typeOf(codeUnit)).integers;
    if (units.isSubsetOf(Range { 0, UINT16_MAX })) {
        buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendCodeUnit, codeUnit, { }, origin);
        return true;
    }
    if (!units.isSubsetOf(Range { -1, UINT16_MAX }) || handlerReadsStringInBuilder(walker, text))
        return false;
    bool isUndefinedOutside = walker.types().at(walker.code().at(rootForStrings(walker, part).id()).type).primitives & Primitive::Undefined;
    Reference isInside = buildOperatorForStrings(walker, Opcode::LessEq, integerForStrings(walker, 0), codeUnit, origin);
    return buildArmsForStrings(walker, isInside, BlockFrequency::Unlikely, [&](std::span<Reference>) {
        buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendCodeUnit, buildProofBehindTestForStrings(walker, codeUnit, isInside, OperandIndex::LessEq::right, origin), { }, origin);
    }, [&](std::span<Reference>) {
        if (isUndefinedOutside)
            buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendConstant, { }, walker.program().strings.textOfUndefined(), origin);
    }, { });
}

static Reference buildHexadecimalDigitInTextBuilderParts(Walker& walker, Reference digit, Origin origin)
{
    static constexpr int64_t distanceOfLetters = 'a' - '0' - 10;
    Reference ofNine = buildOperatorForStrings(walker, Opcode::Sub, integerForStrings(walker, 9), digit, origin);
    Reference sign = ofNine ? buildOperatorForStrings(walker, Opcode::RShift, ofNine, integerForStrings(walker, 31), origin) : Reference();
    Reference distance = sign ? buildOperatorForStrings(walker, Opcode::BitAnd, sign, integerForStrings(walker, distanceOfLetters), origin) : Reference();
    Reference decimal = distance ? buildOperatorForStrings(walker, Opcode::Add, digit, integerForStrings(walker, '0'), origin) : Reference();
    Reference sum = decimal ? buildOperatorForStrings(walker, Opcode::Add, decimal, distance, origin) : Reference();
    return sum ? buildOperatorForStrings(walker, Opcode::BitAnd, sum, integerForStrings(walker, INT8_MAX), origin) : Reference();
}

static void appendHexadecimalDigitInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Reference digit, Origin origin)
{
    Reference codeUnit = digit ? buildHexadecimalDigitInTextBuilderParts(walker, digit, origin) : Reference();
    if (codeUnit && !walker.isUnreachable() && !walker.hasFailed())
        buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendCodeUnit, codeUnit, { }, origin);
}

static void appendDigitsOfByteInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Reference byte, bool hasTwoDigits, Origin origin)
{
    auto appendFirst = [&] {
        appendHexadecimalDigitInTextBuilderParts(walker, text, name, buildOperatorForStrings(walker, Opcode::RShift, byte, integerForStrings(walker, 4), origin), origin);
    };
    if (hasTwoDigits)
        appendFirst();
    else {
        Reference isLong = buildOperatorForStrings(walker, Opcode::Less, integerForStrings(walker, 15), byte, origin);
        if (!isLong)
            return;
        buildArmsForStrings(walker, isLong, BlockFrequency::Normal, [&](std::span<Reference>) {
            appendFirst();
        }, [&](std::span<Reference>) { }, { });
    }
    if (walker.isUnreachable() || walker.hasFailed())
        return;
    appendHexadecimalDigitInTextBuilderParts(walker, text, name, buildOperatorForStrings(walker, Opcode::BitAnd, byte, integerForStrings(walker, 15), origin), origin);
}

static bool appendByteInHexadecimalInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Reference part, Origin origin)
{
    if (!walker.isInLoop() || handlerReadsStringInBuilder(walker, text))
        return false;
    std::optional<HexadecimalText> made = findHexadecimalText(walker, part);
    if (!made)
        return false;
    TypeArena& types = walker.types();
    Type type = types.at(walker.typeOf(made->number));
    if (type.isInteger() && type.integers.isSubsetOf(Range { 0, UINT8_MAX })) {
        walker.count(StatisticID::BytesAppendedInHexadecimal);
        appendDigitsOfByteInTextBuilderParts(walker, text, name, made->number, !!made->padding, origin);
        return true;
    }
    bool isWhole = type.isInteger();
    if (isWhole ? !type.isInt32() : walker.representationOf(made->number) != Representation::JSValue || !made->number.isValue())
        return false;

    walker.count(StatisticID::BytesAppendedInHexadecimal);
    BlockID inDigits = walker.addBlock();
    BlockID asMade = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    Reference integer = made->number;
    if (!isWhole) {
        Reference isInt32 = buildTypeTestForStrings(walker, made->number, TypeTable::int32, origin);
        if (!isInt32 || walker.hasFailed())
            return true;
        BlockID whole = walker.addBlock();
        walker.branch(isInt32, whole, asMade);
        walker.continueIn(whole);
        integer = buildProofBehindTestForStrings(walker, made->number, isInt32, OperandIndex::TypeTest::value, origin);
    }
    Reference bits = integer ? buildOperatorForStrings(walker, Opcode::URShift, integer, integerForStrings(walker, 0), origin) : Reference();
    Reference isByte = bits ? buildOperatorForStrings(walker, Opcode::LessEq, bits, integerForStrings(walker, UINT8_MAX), origin) : Reference();
    if (!isByte || walker.hasFailed())
        return true;
    Vector<Walker::JoinArm, 2> arms;
    walker.branch(isByte, inDigits, asMade);
    walker.continueIn(inDigits);
    if (Reference byte = buildOperatorForStrings(walker, Opcode::BitAnd, integer, integerForStrings(walker, UINT8_MAX), origin))
        appendDigitsOfByteInTextBuilderParts(walker, text, name, byte, !!made->padding, origin);
    if (!walker.isUnreachable() && !walker.hasFailed())
        arms.append({ walker.currentBlock(), { } });
    if (walker.hasFailed())
        return true;
    walker.continueIn(asMade);
    if (Reference string = buildHexadecimalTextAgain(walker, *made, origin))
        buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendString, string, { }, origin);
    if (!walker.isUnreachable() && !walker.hasFailed())
        arms.append({ walker.currentBlock(), { } });
    if (!walker.hasFailed() && !arms.isEmpty())
        walker.joinArms(arms.span(), meeting, { });
    return true;
}

static void appendPartInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Reference part, Origin origin)
{
    if (!part || !part.isRecord()) {
        walker.fail("a part of a string in a builder is not a record of Code"_s);
        return;
    }
    if (StringID known = textOfKnownPartOfStringBuilder(walker, part)) {
        if (known != walker.program().strings.emptyString())
            buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendConstant, { }, known, origin);
        return;
    }

    TypeID proven = walker.typeOf(part);
    Type type = proven ? walker.types().at(proven) : Type();
    if (!proven || !type.hasOnlyPrimitives(primitivesTakenInTextBuilderParts)) {
        walker.fail("a part of a string in a builder may run code"_s);
        return;
    }
    if (const Operation& record = walker.code().at(part.id()); record.opcode == Opcode::Double && !record.number()) {
        part = walker.constant(Draft(Opcode::Integer, origin));
        type = walker.types().at(walker.typeOf(part));
    }

    if (appendCharacterByItsCodeUnitInTextBuilderParts(walker, text, name, part, type, origin))
        return;
    Opcode form = narrowestFormInTextBuilderParts(type);
    if (const Operation& record = walker.code().at(part.id()); form == Opcode::StringBuilderAppendNumber && record.opcode == Opcode::Unbox && part.isValue()) {
        Reference boxed = Reference::value(record.valueOperand());
        if (walker.representationOf(boxed) == Representation::JSValue && walker.types().at(walker.typeOf(boxed)).isNumber())
            part = boxed;
    }
    if (form == Opcode::StringBuilderAppendString && (appendPieceWithoutCutting(walker, text, name, part, origin) || appendByteInHexadecimalInTextBuilderParts(walker, text, name, part, origin) || appendResultOfBuiltinStraight(walker, text, name, part, origin))) {
        text.stringOfState = { };
        text.lengthOfState = { };
        return;
    }
    buildPartInTextBuilderParts(walker, text, name, form, part, { }, origin);
}

void appendPartsToStringBuilder(Walker& walker, StringInBuilder& text, Reference name, std::span<const Reference> parts, Origin origin)
{
    origin.isTailCall = false;
    for (Reference part : parts) {
        if (walker.isUnreachable())
            return;
        appendPartInTextBuilderParts(walker, text, name, part, origin);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
