/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTLimits.h"
#include "AOTOperatorEmitter.h"
#include "AOTRepresentation.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "JSString.h"
#include "PureNaN.h"
#include <limits>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT { namespace Typed {

std::optional<Condition> StringBuiltinEmitter::isUndefinedArgument(const InlineCall& call, unsigned index)
{
    ValueID argument = call.argumentOrNull(index);
    if (!argument)
        return std::nullopt;
    const Type& type = typeOf(argument);
    if (!(type.primitives & Primitive::Undefined) || type.hasOnlyPrimitives(Primitive::Undefined))
        return std::nullopt;
    Operand operand = get(argument);
    if (operand.is<Boxed>())
        return equal(operand.as<Boxed>(), undefined());
    return emitter<ConversionEmitter>().isMarker(operand);
}

std::optional<Number> StringBuiltinEmitter::numberArgument(const InlineCall& call, unsigned index, double whenUndefined)
{
    ValueID argument = call.argumentOrNull(index);
    if (!argument)
        return std::nullopt;
    const Type& type = typeOf(argument);
    if (type.hasOnlyPrimitives(Primitive::Undefined))
        return std::nullopt;
    RELEASE_ASSERT(type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined));

    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Operand operand = get(argument);
    Number passed = operand.is<Boxed>() ? numberToDouble(operand.as<Boxed>())
        : operand.is<OptionalInteger>() ? conversions.toNumber(conversions.withoutMarkers(operand).as<Integer>(), type.integers)
        : operand.is<OptionalNumber>() ? conversions.withoutMarkers(operand).as<Number>()
        : numberOperand(argument);
    if (std::optional<Condition> isUndefined = isUndefinedArgument(call, index))
        return select(*isUndefined, number(whenUndefined), passed);
    return passed;
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

void StringBuiltinEmitter::generateIndexedRead(const InlineCall& call, IndexedRead read)
{
    const OperationContext& context = call.context();
    const Type& stringType = types().at(TypeTable::string);
    const Type& undefinedType = types().at(TypeTable::undefined);
    Representation representation = resultRepresentationOf(call);

    CharacterView view = strings().viewOfString(context, stringOperand(call.receiver()));
    ValueID positionOperand = call.argumentOrNull(0);
    Type positionType = positionOperand ? typeOf(positionOperand) : undefinedType;
    bool readsFirst = positionType.hasOnlyPrimitives(Primitive::Undefined) || (positionType.isInteger() && positionType.integers.isSubsetOf({ 0, 0 }));
    Integer index = readsFirst ? integer(0) : strings().indexInString(numberArgument(call, 0).value_or(number(0)), view.length, read == IndexedRead::At);

    auto integerTypeUpTo = [&](int64_t maximum) {
        return types().at(types().integer({ 0, maximum }));
    };
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
        Condition isInside = readsFirst ? isNotZero(view.length) : greaterThanOrEqual(index, integer(0));
        ScopeFork fork = m_out.forkScope();
        m_out.branch(isInside.value(), normalEdge(inside), unlikelyEdge(outside));
        m_out.appendToArm(inside, fork);
        join.add(inResultRepresentation(readInside(), insideType));
        m_out.appendToArm(outside, fork);
        join.add(inResultRepresentation(readOutside(), outsideType));
        finishInlineCall(call, join.finishBehind(fork));
    };
    auto readCharacter = [&]() -> Operand {
        Word32 codeUnit = strings().codeUnitAt(view, low32(index));
        return strings().stringFromCodeUnit(context, codeUnit);
    };
    auto readUndefined = [&]() -> Operand {
        return undefined();
    };

    switch (read) {
    case IndexedRead::CharCodeAt:
        finishWith(integerTypeUpTo(UINT16_MAX), [&]() -> Operand {
            return strings().codeUnitAt(view, low32(index));
        }, types().at(types().number(PNaN)), [&]() -> Operand {
            return number(PNaN);
        });
        return;
    case IndexedRead::CodePointAt:
        finishWith(integerTypeUpTo(0x10ffff), [&]() -> Operand {
            return strings().codePointAt(view, low32(index));
        }, undefinedType, readUndefined);
        return;
    case IndexedRead::CharAt:
        finishWith(stringType, readCharacter, stringType, [&]() -> Operand {
            return emptyString();
        });
        return;
    case IndexedRead::At:
        finishWith(stringType, readCharacter, undefinedType, readUndefined);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StringBuiltinEmitter::generateRange(const InlineCall& call, StringRangeMethod method)
{
    CellPointer string = stringOperand(call.receiver());
    Word32 length = strings().lengthOf(string);
    Number first = numberArgument(call, 0).value_or(number(0));
    CodeUnitRange range = strings().clampRange(method, length, first, numberArgument(call, 1, std::numeric_limits<double>::infinity()));
    finish(call, strings().substring(call.context(), string, range));
}

void StringBuiltinEmitter::generateStringFromCharCode(const InlineCall& call)
{
    auto codeUnitOf = [&](unsigned index) -> Word32 {
        Word32 word = emitter<ConversionEmitter>().toInt32(*numberArgument(call, index));
        return bitAnd(word, word32(UINT16_MAX));
    };
    unsigned count = call.numberOfArguments();
    if (count == 1) {
        finish(call, strings().stringFromCodeUnit(call.context(), codeUnitOf(0)));
        return;
    }
    Integer codeUnits = zeroExtended(codeUnitOf(0));
    for (unsigned index = 1; index < count; ++index)
        codeUnits = bitOr(codeUnits, Integer(m_out.shl(zeroExtended(codeUnitOf(index)).value(), static_cast<int32_t>(index * Limits::bitsOfCodeUnit))));
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::StringOfCodeUnits, { codeUnits, word32(static_cast<int32_t>(count)) }));
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
            return emitter<ElementAccessEmitter>().indexOrElse(argument, false, miss);
        default:
            break;
        }
        Boxed boxed = boxedOperand(call.context(), argument);
        branchUnless(isInt32(boxed), miss);
        return signExtended(unboxInt32(boxed));
    }();
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
        const TypeTestPlan& isString = typeTestPlanFor(typeIDOf(encoding), representationOf(encoding), TypeTable::string);
        branchUnless(logicalNot(hasType(get(encoding), isString)), missBlockOf(call));
    }
    CharacterView view = strings().viewOfString(call.context(), stringOperand(call.argument(0)));
    Integer byteLength = strings().selectByWidth<Integer>(view.is8Bit, [&](bool is8Bit) {
        return callLeaf<Integer>(is8Bit ? RuntimeOperation::StringUTF8Length8 : RuntimeOperation::StringUTF8Length16, { view.characters, view.length });
    }, Machine::EdgeClass::Normal);
    ensureStillAliveHere(view.owner);
    finish(call, byteLength);
}

Integer StringBuiltinEmitter::countArgument(const InlineCall& call, unsigned index)
{
    ValueID argument = call.argument(index);
    if (typeOf(argument).isInteger())
        return integerOperand(argument);
    Number count = numberOperand(argument);
#if !CPU(ARM64)
    count = select(equal(count, count), count, 0);
#endif
    return emitter<ConversionEmitter>().toInteger(count);
}

void StringBuiltinEmitter::generateStringRepeat(const InlineCall& call)
{
    LBasicBlock miss = missBlockOf(call);
    CellPointer string = stringOperand(call.receiver());
    Integer count = countArgument(call, 0);
    Integer length = zeroExtended(strings().lengthOf(string));
    Integer limit = integer(Limits::maxLengthOfStringMadeFlat);
    branchUnless(belowOrEqual(count, limit), miss);
    branchUnless(belowOrEqual(mul(count, length), limit), miss);
    finish(call, callOperation<CellPointer>(call.context(), RuntimeOperation::StringRepeat, { string, low32(count) }));
}

void StringBuiltinEmitter::generatePad(const InlineCall& call, bool isAtStart)
{
    CellPointer string = stringOperand(call.receiver());
    Integer length = countArgument(call, 0);
    Integer stringLength = zeroExtended(strings().lengthOf(string));
    if (call.canMiss())
        branchUnless(belowOrEqual(length, integer(Limits::maxLengthOfStringMadeFlat)), missBlockOf(call));
    finish(call, selectByBranch<CellPointer>(lessThanOrEqual(length, stringLength), [&] {
        return string;
    }, [&] {
        CellPointer filler = call.numberOfArguments() > 1 ? stringOperand(call.argument(1)) : singleCharacterString(word32(' '));
        return callOperation<CellPointer>(call.context(), RuntimeOperation::StringPad, { string, low32(length), filler, word32(isAtStart) });
    }));
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

void StringBuiltinEmitter::generateStringIsWellFormed(const InlineCall& call)
{
    missUnless(call, is8Bit(stringOperand(call.receiver())));
    finish(call, boolean(true));
}

void StringBuiltinEmitter::generateStringToWellFormed(const InlineCall& call)
{
    CellPointer string = stringOperand(call.receiver());
    missUnless(call, is8Bit(string));
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

    ScopeFork fork = m_out.forkScope();
    CharacterView view = strings().viewWithoutResolving(call.context(), string, unlikelyEdge(byCall));
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
    finish(call, join.finishBehind(fork));
}

void StringBuiltinEmitter::generateStringLocaleCompare(const InlineCall& call)
{
    CellPointer string = stringOperand(call.receiver());
    CellPointer other = stringOperand(call.argument(0));
    finish(call, low32(callOperation<Integer>(call.context(), RuntimeOperation::StringLocaleCompare, { string, other })));
}

void StringBuiltinEmitter::generateStringConcat(const InlineCall& call)
{
    Vector<StringEmitter::StringPart, 3> parts;
    parts.append(stringOperand(call.receiver()));
    for (ValueID argument : call.arguments())
        parts.append(stringOperand(argument));
    finish(call, strings().concatenate(call.context(), parts.span()));
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

Condition StringBuiltinEmitter::hasCharactersAt(const InlineCall& call, CellPointer string, CellPointer other, Integer at)
{
    constexpr unsigned maxLengthComparedInLine = 2 * sizeof(uint64_t);
    const Machine::AbstractHeap& characters = m_heaps.characters8.atAnyIndex();
    LBasicBlock generic = newBlock();
    Join<Condition> join(m_out);
    auto load = [&](unsigned width, Pointer pointer) {
        return loadBytes(m_out, width, TypedPointer(characters, pointer.value()));
    };

    continueIf(logicalNot(isRopeString(other)), generic);
    CharacterView subject = strings().viewWithoutResolving(call.context(), string, unlikelyEdge(generic));
    CharacterView sought = strings().viewOfFlatString(other);
    continueIf(logicalAnd(subject.is8Bit, sought.is8Bit), generic);
    continueIf(belowOrEqual(sought.length, word32(maxLengthComparedInLine)), generic);
    Pointer here = offsetBy(subject.characters, at, 0);
    Integer size = zeroExtended(sought.length);
    auto addComparison = [&](Condition isSame) {
        ensureStillAliveHere(subject.owner);
        ensureStillAliveHere(sought.owner);
        join.add(isSame);
    };
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
        addComparison(Condition(width == 8 ? m_out.isZero64(difference) : m_out.isZero32(difference)));

        m_out.appendTo(shorter);
    }
    LBasicBlock oneCharacter = newBlock();
    LBasicBlock noCharacter = newBlock();
    m_out.branch(isNotZero(sought.length).value(), normalEdge(oneCharacter), normalEdge(noCharacter));

    m_out.appendTo(oneCharacter);
    LValue characterHere = load(1, here);
    addComparison(Condition(m_out.equal(characterHere, load(1, sought.characters))));

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

    ScopeFork fork = m_out.forkScope();
    continueIf(logicalNot(isRopeString(other)), generic);
    CharacterView subject = strings().viewWithoutResolving(call.context(), string, unlikelyEdge(generic));
    CharacterView sought = strings().viewOfFlatString(other);
    continueIf(equal(subject.is8Bit, sought.is8Bit), generic);
    join.add(strings().find(subject, sought, position, direction));

    m_out.appendTo(generic);
    join.add(low32(callOperation<Integer>(call.context(), operationOnCells, { string, other, position })));

    Word32 found = join.finishBehind(fork);
    if (resultIsBoolean) {
        finish(call, notEqual(found, word32(-1)));
        return;
    }
    finish(call, found);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
