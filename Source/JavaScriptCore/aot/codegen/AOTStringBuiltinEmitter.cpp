/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinFamilyEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTOperatorEmitter.h"
#include "AOTRepresentation.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsStrings.h"
#include "AOTThreshold.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "JSString.h"
#include "PureNaN.h"
#include "SmallStrings.h"
#include <wtf/text/StringImpl.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(int64_t, maxFlatStringLength, 1024);

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
        if (representation != Representation::Void && !isLegalRepresentation(outsideType, representation)) {
            generator().fail(context, InternalErrorKind::OpcodeWithoutCode);
            finishInlineCall(call, emitter<ConstantEmitter>().zeroOf(representation));
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

void StringBuiltinEmitter::generateRange(const InlineCall& call, StringRangeMethod method)
{
    CellPointer string = stringOperand(call.receiver());
    Word32 length = strings().lengthOf(string);
    Number first = numberArgument(call, 0).value_or(number(0));
    // Without an end the range goes on to the end of the string, as it does for an end past it.
    CodeUnitRange range = strings().clampRange(method, length, first, numberArgument(call, 1, std::numeric_limits<double>::infinity()));
    finish(call, strings().substring(call.context(), string, range));
}

void StringBuiltinEmitter::generateStringFromCharCode(const InlineCall& call)
{
    Word32 word = emitter<ConversionEmitter>().toInt32(*numberArgument(call, 0));
    finish(call, strings().stringFromCodeUnit(call.context(), bitAnd(word, word32(UINT16_MAX))));
}

void StringBuiltinEmitter::generateStringFromCodePoint(const InlineCall& call)
{
    LBasicBlock miss = missBlockOf(call);
    ValueID argument = call.argument(0);
    Integer codePoint = [&] {
        switch (representationOf(argument)) {
        case Representation::Int32:
        case Representation::Int64:
        case Representation::Double:
        case Representation::DoubleWithPayload:
        case Representation::ImpureDouble:
            return emitter<ElementAccessEmitter>().indexOrElse(argument, false, miss);
        default:
            break;
        }
        Boxed boxed = in<Boxed>(argument);
        branchUnless(isInt32(boxed), miss);
        return signExtended(unboxInt32(boxed));
    }();
    // Up to here a code point is one code unit, half of a pair too. A negative number is far above.
    branchUnless(belowOrEqual(codePoint, integer(UINT16_MAX)), miss);
    finish(call, strings().stringFromCodeUnit(call.context(), low32(codePoint)));
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
        Condition isNoString = logicalNot(hasType(get(encoding), isString));
        branchUnless(isNoString, missBlockOf(call));
    }
    CharacterView view = strings().viewOfString(call.context(), stringOperand(call.argument(0)));
    Integer byteLength = strings().selectByWidth<Integer>(view.is8Bit, [&](bool is8Bit) {
        return callLeaf<Integer>(is8Bit ? RuntimeOperation::StringUTF8Length8 : RuntimeOperation::StringUTF8Length16, { view.characters, view.length });
    }, B3::FrequencyClass::Normal);
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
    Condition repeatsFewTimes = belowOrEqual(count, limit);
    branchUnless(logicalAnd(repeatsFewTimes, belowOrEqual(mul(count, length), limit)), miss);
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

void StringBuiltinEmitter::generateCase(const InlineCall& call, RuntimeOperation operation)
{
    finish(call, callOperation<CellPointer>(call.context(), operation, { stringOperand(call.receiver()) }));
}

void StringBuiltinEmitter::generateTrim(const InlineCall& call, TrimmedEnds ends)
{
    CellPointer string = stringOperand(call.receiver());
    LBasicBlock byCall = newBlock();
    Join<CellPointer> join(m_out);

    continueIf(logicalNot(isRopeString(string)), byCall);
    CharacterView view = strings().viewOfFlatString(string);
    continueIf(view.is8Bit, byCall);
    continueIf(isNotZero(view.length), byCall);
    CharacterView narrowView { view.characters, view.length, boolean(true), view.owner };
    auto isPrintableASCII = [&](Word32 index) {
        return belowOrEqual(add(strings().codeUnitAt(narrowView, index), -0x21), 0x7e - 0x21);
    };
    if (ends != TrimmedEnds::End)
        continueIf(isPrintableASCII(word32(0)), byCall);
    if (ends != TrimmedEnds::Start)
        continueIf(isPrintableASCII(sub(view.length, word32(1))), byCall);
    join.add(string);

    m_out.appendTo(byCall);
    join.add(callOperation<CellPointer>(call.context(), RuntimeOperation::StringTrim, { string, word32(static_cast<int32_t>(ends)) }));
    finish(call, join.finish());
}

void StringBuiltinEmitter::generateStringLocaleCompare(const InlineCall& call)
{
    CellPointer string = stringOperand(call.receiver());
    CellPointer other = stringOperand(call.argument(0));
    finish(call, low32(Integer(callStub(call.context(), stringLocaleCompare(), { string.value(), other.value() }).first)));
}

void StringBuiltinEmitter::generateStringConcat(const InlineCall& call)
{
    Vector<CellPointer, 3> parts;
    parts.append(stringOperand(call.receiver()));
    for (ValueID argument : call.arguments())
        parts.append(stringOperand(argument));
    finish(call, strings().concatenate(call.context(), parts.span()));
}

using SplitResults = Vector<Operand, 33>;

// By whether all are counted, whether the separator has several units, and whether the string is wide.
static constexpr RuntimeOperation separatorOperations[2][2][2] = {
    { { RuntimeOperation::FindSeparators8, RuntimeOperation::FindSeparators16 }, { RuntimeOperation::FindLongSeparators8, RuntimeOperation::FindLongSeparators16 } },
    { { RuntimeOperation::CountSeparators8, RuntimeOperation::CountSeparators16 }, { RuntimeOperation::CountLongSeparators8, RuntimeOperation::CountLongSeparators16 } },
};

void StringBuiltinEmitter::generateStringSplitPrefix(const InlineCall& call)
{
    RELEASE_ASSERT(call.value());
    const ValueContext& value = *call.value();
    bool isBuiltinCall = value.opcode() == Opcode::CallBuiltin;
    CellPointer string = stringOperand(call.receiver());
    unsigned count = isBuiltinCall ? *value->prefixLength() : call.parameterWord(0);
    if (StringID literal = isBuiltinCall ? value->literalSeparator() : StringID()) {
        splitAtLiteral(call, string, literal, count);
        return;
    }

    Pointer bounds = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(count);
    SplitResults results;
    results.append(callOperation<Integer>(call.context(), RuntimeOperation::StringSplitPrefix, { string, stringOperand(call.argument(0)), word32(count), bounds }));
    for (unsigned i = 0; i < 2 * count; ++i)
        results.append(Word32(m_out.load32(m_out.address(m_heaps.argumentBuffer, bounds.value(), i * sizeof(uint32_t)))));
    finishWithResults(call, results.span());
}

Integer StringBuiltinEmitter::searchSeparators(const CharacterView& view, const Separator& separator, Pointer positions, unsigned wanted)
{
    bool isLong = separator.length > 1;
    Integer result = strings().selectByWidth<Integer>(view.is8Bit, [&](bool is8Bit) {
        Vector<Operand, 5> arguments { view.characters, view.length, separator.codeUnitOrText };
        if (wanted) {
            arguments.append(positions);
            arguments.append(word32(wanted));
        }
        RuntimeOperation operation = separatorOperations[!wanted][isLong][!is8Bit];
        Operand answer = isLong ? callWithoutSite(operation, arguments.span(), Integer::representation, { }) : callLeaf(operation, arguments.span(), Integer::representation);
        return answer.as<Integer>();
    }, B3::FrequencyClass::Normal);
    ensureStillAliveHere(view.owner);
    return result;
}

void StringBuiltinEmitter::splitAtLiteral(const InlineCall& call, CellPointer string, StringID literal, unsigned count)
{
    const OperationContext& context = call.context();
    String text = strings().textOf(literal);
    RELEASE_ASSERT(!text.isEmpty());
    Separator separator { word32(text.length() == 1 ? text[0] : literal.index()), text.length() };
    Word32 size = word32(separator.length);
    CharacterView view = strings().viewOfString(context, string);

    SplitResults results;
    if (!count) {
        results.append(add(searchSeparators(view, separator), 1));
        finishWithResults(call, results.span());
        return;
    }

    Pointer buffer = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer((count + 1) / 2);
    Word32 found = low32(searchSeparators(view, separator, buffer, count));
    Vector<Word32, 16> positions;
    for (unsigned i = 0; i < count; ++i)
        positions.append(Word32(m_out.load32(m_out.address(m_heaps.argumentBuffer, buffer.value(), i * sizeof(uint32_t)))));

    Integer numberOfSeparators = selectByBranch<Integer>(below(found, word32(count)), [&] {
        return zeroExtended(found);
    }, [&] {
        CharacterView rest = strings().subview(view, { add(positions[count - 1], size), view.length });
        Integer found = integer(count);
        return add(found, searchSeparators(rest, separator));
    });

    results.append(add(numberOfSeparators, integer(1)));
    for (unsigned i = 0; i < count; ++i) {
        CodeUnitRange piece { i ? add(positions[i - 1], size) : word32(0), select(below(word32(i), found), positions[i], view.length) };
        Condition exists = belowOrEqual(word32(i), found);
        results.append(select(exists, piece.start, word32(absentBound)));
        results.append(select(exists, piece.end, word32(absentBound)));
    }
    finishWithResults(call, results.span());
}

void StringBuiltinEmitter::generateHasAt(const InlineCall& call, LiteralEnd end)
{
    CellPointer string = stringOperand(call.receiver());
    CellPointer other = stringOperand(call.argument(0));
    Word32 length = strings().lengthOf(string);
    Integer otherLength = zeroExtended(strings().lengthOf(other));

    Integer at;
    Condition hasRoom;
    switch (end) {
    case LiteralEnd::Start: {
        at = zeroExtended(positionArgument(call, 1, length, word32(0)));
        Integer endOfOther = add(at, otherLength);
        hasRoom = lessThanOrEqual(endOfOther, zeroExtended(length));
        break;
    }
    case LiteralEnd::End:
        at = sub(zeroExtended(positionArgument(call, 1, length, length)), otherLength);
        hasRoom = greaterThanOrEqual(at, integer(0));
        break;
    }

    finish(call, selectByBranch<Condition>(hasRoom, [&] {
        return hasCharactersAt(call, string, other, at);
    }, [&] {
        return boolean(false);
    }));
}

// There is room for other at that place in string.
Condition StringBuiltinEmitter::hasCharactersAt(const InlineCall& call, CellPointer string, CellPointer other, Integer at)
{
    constexpr unsigned maxLengthComparedInLine = 2 * sizeof(uint64_t);
    const B3::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
    LBasicBlock generic = newBlock();
    Join<Condition> join(m_out);
    auto load = [&](unsigned width, Pointer pointer) {
        return loadBytes(m_out, width, TypedPointer(characters, pointer.value()));
    };

    continueIf(logicalNot(isRopeString(string)), generic);
    continueIf(logicalNot(isRopeString(other)), generic);
    CharacterView subject = strings().viewOfFlatString(string);
    CharacterView sought = strings().viewOfFlatString(other);
    continueIf(logicalAnd(subject.is8Bit, sought.is8Bit), generic);
    continueIf(belowOrEqual(sought.length, word32(maxLengthComparedInLine)), generic);
    Pointer here = offsetBy(subject.characters, at, 0);
    Integer size = zeroExtended(sought.length);
    for (unsigned width : { 8u, 4u, 2u }) {
        LBasicBlock compare = newBlock();
        LBasicBlock shorter = newBlock();
        m_out.branch(aboveOrEqual(sought.length, word32(width)).value(), normalEdge(compare), normalEdge(shorter));

        m_out.appendTo(compare);
        Integer lastOffset = sub(size, integer(width));
        LValue firstHere = load(width, here);
        LValue firstDifference = m_out.bitXor(firstHere, load(width, sought.characters));
        LValue lastHere = load(width, offsetBy(here, lastOffset, 0));
        LValue lastDifference = m_out.bitXor(lastHere, load(width, offsetBy(sought.characters, lastOffset, 0)));
        LValue difference = m_out.bitOr(firstDifference, lastDifference);
        join.add(Condition(width == 8 ? m_out.isZero64(difference) : m_out.isZero32(difference)));

        m_out.appendTo(shorter);
    }
    LBasicBlock oneCharacter = newBlock();
    LBasicBlock noCharacter = newBlock();
    m_out.branch(isNotZero(sought.length).value(), normalEdge(oneCharacter), normalEdge(noCharacter));

    m_out.appendTo(oneCharacter);
    LValue characterHere = load(1, here);
    join.add(Condition(m_out.equal(characterHere, load(1, sought.characters))));

    m_out.appendTo(noCharacter);
    join.add(boolean(true));

    m_out.appendTo(generic);
    join.add(isNotZero(low32(callOperation<Integer>(call.context(), RuntimeOperation::StringHasAt, { string, other, low32(at) }))));
    return join.finish();
}

void StringBuiltinEmitter::generateSearch(const InlineCall& call, SearchDirection direction, bool resultIsBoolean)
{
    CellPointer string = stringOperand(call.receiver());
    CellPointer other = stringOperand(call.argument(0));

    RuntimeOperation operationOnCells = direction == SearchDirection::Forward ? RuntimeOperation::StringIndexOfFrom : RuntimeOperation::StringLastIndexOf;

    Word32 position;
    switch (direction) {
    case SearchDirection::Forward: {
        Word32 length = strings().lengthOf(string);
        position = positionArgument(call, 1, length, word32(0));
        break;
    }
    case SearchDirection::Backward: {
        Word32 fromEnd = word32(INT32_MAX);
        position = fromEnd;
        if (std::optional<Number> argument = numberArgument(call, 1)) {
            Condition isNotNaN = equal(*argument, *argument);
            position = select(isNotNaN, positionArgument(call, 1, fromEnd, fromEnd), fromEnd);
        }
        break;
    }
    }

    LBasicBlock generic = newBlock();
    Join<Word32> join(m_out);

    continueIf(logicalNot(isRopeString(string)), generic);
    continueIf(logicalNot(isRopeString(other)), generic);
    CharacterView subject = strings().viewOfFlatString(string);
    CharacterView sought = strings().viewOfFlatString(other);
    continueIf(equal(subject.is8Bit, sought.is8Bit), generic);
    join.add(strings().find(subject, sought, position, direction));

    m_out.appendTo(generic);
    join.add(low32(callOperation<Integer>(call.context(), operationOnCells, { string, other, position })));

    Word32 found = join.finish();
    if (resultIsBoolean) {
        finish(call, notEqual(found, word32(-1)));
        return;
    }
    finish(call, found);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
