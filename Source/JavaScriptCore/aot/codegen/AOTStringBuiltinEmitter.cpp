/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTOperatorEmitter.h"
#include "AOTRepresentation.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "JSString.h"
#include "PureNaN.h"
#include "SmallStrings.h"
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr int64_t maxFlatStringLength = 1024;

static constexpr bool trimsStart(TrimmedEnds ends)
{
    return ends != TrimmedEnds::End;
}

static constexpr bool trimsEnd(TrimmedEnds ends)
{
    return ends != TrimmedEnds::Start;
}

StringBuiltinEmitter::StringBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

Condition StringBuiltinEmitter::is8Bit(CellPointer string)
{
    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isNarrow = selectByBranch<Condition>(hasNoBits(fiber, JSString::isRopeInPointer), [&] {
        return hasBits(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags), StringImpl::flagIs8Bit());
    }, [&] {
        return logicalNot(hasNoBits(fiber, JSRopeString::is8BitInPointer));
    });
    ensureStillAliveHere(string);
    return isNarrow;
}

std::optional<Condition> StringBuiltinEmitter::isUndefinedArgument(const InlineCall& call, unsigned index)
{
    ValueID argument = call.argumentOrNull(index);
    if (!argument || !(typeOf(argument).primitives & Primitive::Undefined) || typeOf(argument).hasOnlyPrimitives(Primitive::Undefined))
        return std::nullopt;
    Operand operand = get(argument);
    if (operand.is<Boxed>())
        return equal(operand.as<Boxed>(), undefined());
    return emitter<ConversionEmitter>().isMarker(operand);
}

std::optional<Number> StringBuiltinEmitter::numberArgument(const InlineCall& call, unsigned index, double whenUndefined)
{
    ValueID argument = call.argumentOrNull(index);
    if (!argument || typeOf(argument).hasOnlyPrimitives(Primitive::Undefined))
        return std::nullopt;
    std::optional<Condition> isUndefined = isUndefinedArgument(call, index);
    if (!isUndefined)
        return numberOperand(argument);

    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Operand operand = get(argument);
    Number passed = operand.is<Boxed>() ? numberToDouble(operand.as<Boxed>())
        : operand.is<OptionalInteger>() ? conversions.toNumber(conversions.withoutMarkers(operand).as<Integer>(), typeOf(argument).integers)
        : conversions.withoutMarkers(operand).as<Number>();
    return select(*isUndefined, number(whenUndefined), passed);
}

Word32 StringBuiltinEmitter::positionArgument(const InlineCall& call, unsigned index, Word32 length, Word32 whenAbsent)
{
    std::optional<Number> position = numberArgument(call, index);
    if (!position)
        return whenAbsent;
    Word32 passed = strings().clampRange(StringRangeMethod::Substring, length, *position, std::nullopt).start;
    if (std::optional<Condition> isUndefined = isUndefinedArgument(call, index))
        return select(*isUndefined, whenAbsent, passed);
    return passed;
}

void StringBuiltinEmitter::finish(const InlineCall& call, Operand naturalResult)
{
    BuiltinFamilyEmitter::finish(call, naturalResult, resultTypeOf(call));
}

void StringBuiltinEmitter::generateStringLength(const InlineCall& call)
{
    finish(call, strings().lengthOf(stringOperand(call.receiver())));
}

void StringBuiltinEmitter::generateIndexedRead(const InlineCall& call, InlineBuiltin builtin)
{
    const OperationContext& context = call.context();
    const Type& stringType = m_program.at(TypeTable::string);
    const Type& undefinedType = m_program.at(TypeTable::undefined);
    Representation representation = resultRepresentationOf(call);

    CharacterView view = strings().viewOfString(context, stringOperand(call.receiver()));
    Number position = numberArgument(call, 0).value_or(number(0));
    Integer index = strings().indexInString(position, view.length, builtin == InlineBuiltin::StringAt);

    auto inResultRepresentation = [&](Operand natural, const Type& type) {
        return inRepresentation(context, representation, natural, type);
    };
    auto finishWith = [&](const Type& insideType, const auto& readInside, const Type& outsideType, const auto& readOutside) {
        // A representation without room for the result of a position outside the string comes from a type that rules such a position out.
        bool mayBeOutside = representation == Representation::Void || isLegalRepresentation(outsideType, representation);
        if (!mayBeOutside) {
            finishInlineCall(call, inResultRepresentation(readInside(), insideType));
            return;
        }
        LBasicBlock inside = newBlock();
        LBasicBlock outside = newBlock();
        OperandJoin join(m_out, representation);
        m_out.branch(greaterThanOrEqual(index, integer(0)).value(), normalEdge(inside), unlikelyEdge(outside));
        m_out.appendTo(inside);
        join.add(inResultRepresentation(readInside(), insideType));
        m_out.appendTo(outside);
        join.add(inResultRepresentation(readOutside(), outsideType));
        finishInlineCall(call, join.finish());
    };
    auto readCharacter = [&]() -> Operand {
        Word32 codeUnit = strings().codeUnitAt(view, low32(index));
        return strings().stringFromCodeUnit(context, codeUnit);
    };
    auto readUndefined = [&]() -> Operand {
        return undefined();
    };

    switch (builtin) {
    case InlineBuiltin::StringCharCodeAt:
        finishWith(Type::integer({ 0, UINT16_MAX }), [&]() -> Operand {
            return strings().codeUnitAt(view, low32(index));
        }, Type::number(PNaN), [&]() -> Operand {
            return number(PNaN);
        });
        return;
    case InlineBuiltin::StringCodePointAt:
        finishWith(Type::integer({ 0, 0x10ffff }), [&]() -> Operand {
            return strings().codePointAt(view, low32(index));
        }, undefinedType, readUndefined);
        return;
    case InlineBuiltin::StringCharAt:
        finishWith(stringType, readCharacter, stringType, [&]() -> Operand {
            return emptyString();
        });
        return;
    case InlineBuiltin::StringAt:
        finishWith(stringType, readCharacter, undefinedType, readUndefined);
        return;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StringBuiltinEmitter::generateStringAt(const InlineCall& call)
{
    generateIndexedRead(call, InlineBuiltin::StringAt);
}

void StringBuiltinEmitter::generateStringCharAt(const InlineCall& call)
{
    generateIndexedRead(call, InlineBuiltin::StringCharAt);
}

void StringBuiltinEmitter::generateStringCharCodeAt(const InlineCall& call)
{
    generateIndexedRead(call, InlineBuiltin::StringCharCodeAt);
}

void StringBuiltinEmitter::generateStringCodePointAt(const InlineCall& call)
{
    generateIndexedRead(call, InlineBuiltin::StringCodePointAt);
}

void StringBuiltinEmitter::generateRange(const InlineCall& call, StringRangeMethod method)
{
    CellPointer string = stringOperand(call.receiver());
    Word32 length = strings().lengthOf(string);
    Number first = numberArgument(call, 0).value_or(number(0));
    // Without an end the range goes on to the end of the string, as it does for an end past it.
    CodeUnitRange range = strings().clampRange(method, length, first, numberArgument(call, 1, std::numeric_limits<double>::infinity()));
    finish(call, strings().substring(call.context(), string, range));
}

void StringBuiltinEmitter::generateStringSlice(const InlineCall& call)
{
    generateRange(call, StringRangeMethod::Slice);
}

void StringBuiltinEmitter::generateStringSubstring(const InlineCall& call)
{
    generateRange(call, StringRangeMethod::Substring);
}

void StringBuiltinEmitter::generateStringSubstr(const InlineCall& call)
{
    generateRange(call, StringRangeMethod::Substr);
}

void StringBuiltinEmitter::generateStringFromCharCode(const InlineCall& call)
{
    Word32 word = emitter<ConversionEmitter>().toInt32(*numberArgument(call, 0));
    finish(call, strings().stringFromCodeUnit(call.context(), bitAnd(word, word32(UINT16_MAX))));
}

void StringBuiltinEmitter::generateStringFromCodePoint(const InlineCall& call)
{
    LBasicBlock miss = missBlockOf(call);
    Integer codePoint = integerOperand(call.argument(0));
    branchUnless(belowOrEqual(codePoint, integer(maxSingleCharacterString)), miss);
    finish(call, singleCharacterString(low32(codePoint)));
}

void StringBuiltinEmitter::generateCallStringConstructor(const InlineCall& call)
{
    ValueID argument = call.argument(0);
    finish(call, emitter<OperatorEmitter>().toString(call.context(), get(argument), typeOf(argument)));
}

void StringBuiltinEmitter::generateStringUTF8ByteLength(const InlineCall& call)
{
    if (call.canMiss()) {
        ValueID encoding = call.argument(1);
        const TypeTestPlan& isString = typeTestPlanFor(m_function.at(encoding).type, representationOf(encoding), TypeTable::string);
        branchUnless(logicalNot(hasType(get(encoding), isString)), missBlockOf(call));
    }
    CharacterView view = strings().viewOfString(call.context(), stringOperand(call.argument(0)));
    Integer byteLength = selectByBranch<Integer>(view.is8Bit, [&] {
        return callLeaf<Integer>(RuntimeOperation::StringUTF8Length8, { view.characters, view.length });
    }, [&] {
        return callLeaf<Integer>(RuntimeOperation::StringUTF8Length16, { view.characters, view.length });
    });
    ensureStillAliveHere(view.owner);
    finish(call, byteLength);
}

void StringBuiltinEmitter::generateStringRepeat(const InlineCall& call)
{
    LBasicBlock miss = missBlockOf(call);
    CellPointer string = stringOperand(call.receiver());
    Integer count = strings().truncateToIndex(numberOperand(call.argument(0)));
    Integer length = zeroExtended(strings().lengthOf(string));
    Integer limit = integer(maxFlatStringLength);
    branchUnless(logicalAnd(belowOrEqual(count, limit), belowOrEqual(mul(count, length), limit)), miss);
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::StringRepeat, { string, low32(count) }));
}

void StringBuiltinEmitter::generatePad(const InlineCall& call, bool isAtStart)
{
    LBasicBlock miss = missBlockOf(call);
    CellPointer string = stringOperand(call.receiver());
    Integer length = strings().truncateToIndex(numberOperand(call.argument(0)));
    Integer stringLength = zeroExtended(strings().lengthOf(string));
    branchUnless(belowOrEqual(length, integer(maxFlatStringLength)), miss);
    finish(call, selectByBranch<CellPointer>(lessThanOrEqual(length, stringLength), [&] {
        return string;
    }, [&] {
        CellPointer filler = call.numberOfArguments() > 1 ? stringOperand(call.argument(1)) : singleCharacterString(word32(' '));
        return callOperation<CellPointer>(call.context(), RuntimeOperation::StringPad, { string, low32(length), filler, word32(isAtStart) });
    }));
}

void StringBuiltinEmitter::generateStringPadStart(const InlineCall& call)
{
    generatePad(call, true);
}

void StringBuiltinEmitter::generateStringPadEnd(const InlineCall& call)
{
    generatePad(call, false);
}

void StringBuiltinEmitter::generateStringIsWellFormed(const InlineCall& call)
{
    LBasicBlock miss = missBlockOf(call);
    branchUnless(is8Bit(stringOperand(call.receiver())), miss);
    finish(call, boolean(true));
}

void StringBuiltinEmitter::generateStringToWellFormed(const InlineCall& call)
{
    LBasicBlock miss = missBlockOf(call);
    CellPointer string = stringOperand(call.receiver());
    branchUnless(is8Bit(string), miss);
    finish(call, string);
}

void StringBuiltinEmitter::generateStringNormalize(const InlineCall& call)
{
    LBasicBlock miss = missBlockOf(call);
    CellPointer string = stringOperand(call.receiver());
    branchUnless(is8Bit(string), miss);
    finish(call, string);
}

void StringBuiltinEmitter::generateCase(const InlineCall& call, StringCase letterCase)
{
    RuntimeOperation operation = letterCase == StringCase::Lower ? RuntimeOperation::StringToLowerCase : RuntimeOperation::StringToUpperCase;
    finish(call, callOperation<CellPointer>(call.context(), operation, { stringOperand(call.receiver()) }));
}

void StringBuiltinEmitter::generateStringToLowerCase(const InlineCall& call)
{
    generateCase(call, StringCase::Lower);
}

void StringBuiltinEmitter::generateStringToUpperCase(const InlineCall& call)
{
    generateCase(call, StringCase::Upper);
}

void StringBuiltinEmitter::generateTrim(const InlineCall& call, TrimmedEnds ends)
{
    CellPointer string = stringOperand(call.receiver());
    LBasicBlock byCall = newBlock();
    Join<CellPointer> join(m_out);
    auto continueIf = [&](Condition condition) {
        LBasicBlock next = newBlock();
        m_out.branch(condition.value(), normalEdge(next), unlikelyEdge(byCall));
        m_out.appendTo(next);
    };

    continueIf(logicalNot(isRopeString(string)));
    CharacterView view = strings().viewOfFlatString(string);
    continueIf(view.is8Bit);
    continueIf(isNotZero(view.length));
    CharacterView narrowView { view.characters, view.length, boolean(true), view.owner };
    auto isPrintableASCII = [&](Word32 index) {
        return belowOrEqual(add(strings().codeUnitAt(narrowView, index), word32(-0x21)), word32(0x7e - 0x21));
    };
    if (trimsStart(ends))
        continueIf(isPrintableASCII(word32(0)));
    if (trimsEnd(ends))
        continueIf(isPrintableASCII(sub(view.length, word32(1))));
    join.add(string);

    m_out.appendTo(byCall);
    join.add(callOperation<CellPointer>(call.context(), RuntimeOperation::StringTrim, { string, word32(static_cast<int32_t>(ends)) }));
    finish(call, join.finish());
}

void StringBuiltinEmitter::generateStringTrim(const InlineCall& call)
{
    generateTrim(call, TrimmedEnds::Both);
}

void StringBuiltinEmitter::generateStringTrimStart(const InlineCall& call)
{
    generateTrim(call, TrimmedEnds::Start);
}

void StringBuiltinEmitter::generateStringTrimEnd(const InlineCall& call)
{
    generateTrim(call, TrimmedEnds::End);
}

void StringBuiltinEmitter::generateStringLocaleCompare(const InlineCall& call)
{
    CellPointer string = stringOperand(call.receiver());
    CellPointer other = stringOperand(call.argument(0));
    finish(call, low32(callOperation<Integer>(call.context(), RuntimeOperation::StringLocaleCompare, { string, other })));
}

void StringBuiltinEmitter::generateStringConcat(const InlineCall& call)
{
    Vector<CellPointer, 3> parts;
    parts.append(stringOperand(call.receiver()));
    for (ValueID argument : call.arguments())
        parts.append(stringOperand(argument));
    finish(call, strings().concatenate(call.context(), parts.span()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
