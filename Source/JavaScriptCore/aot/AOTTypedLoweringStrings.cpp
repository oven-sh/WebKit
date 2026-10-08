/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3BasicBlockInlines.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMap.h"
#include "JSSet.h"
#include "TypeInfoBlob.h"
#include <unicode/utf16.h>
#include <wtf/text/ASCIIFastPath.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// Strings, maps and sets.
//
// A string points to a StringImpl, which has its characters, or it is a rope: the word that would point there is marked, and the string is
// a part of another string (a substring) or up to three strings one after the other. A rope is made flat when someone needs it to be,
// once. From then on it points to a StringImpl like any other string.
//
// What the back end is told:
//
// The word is read as if it never changed (fiberOf()), so that it is read once, before a loop, and all that is read through it with it.
// That is sound. Without the mark it is as good as any later one: a string that is made a property key of comes to point to another
// StringImpl, which has the same characters, and the engine keeps the old one for as long as the collector finds the string on the stack
// (JSString::swapToAtomString()). So after every look at the characters the string has to be found still: ensureStillAliveHere(). With the
// mark it only says that the string was a rope: the path for ropes reads the word again, as part of the state of the run time.
//
// Length, width and place of the characters of a StringImpl never change. They are loaded from something that is always there where the
// string is a rope, so that the loads do not depend on the test and can be moved. How long a string is a rope says itself, in a place
// that stays as it is when the rope is made flat.
//
// In the place of `undefined` and `null` the empty string is loaded from. Whatever has to be inside a string is outside that one, so that
// one test tells both, and the path for what is outside tells them apart.
//
// A substring is looked at where its characters are, in the string it is a part of. It is not made flat for that.

// As long as the text of a comparison that is carried out by loads and immediates.
static constexpr size_t maxLengthOfTextComparedInPlace = 32;

Lowering::StringReceiver Lowering::stringReceiver(CellPointer string)
{
    return { string, safeIfReaches(string, sizeof(JSString)), { }, { } };
}

// The value is a string, or `undefined` or `null` if its type says so.
Lowering::StringReceiver Lowering::stringReceiver(ValueID id)
{
    Operand operand = get(id);
    bool mayBeNil = operand.rep == Rep::JSValue && typeOf(id).mayBeUndefinedOrNull();
    StringReceiver receiver = stringReceiver(operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>()));
    if (receiver.flags & IsSafe)
        return receiver;
    // By the type it was given. What a test has shown says nothing of the value above that test, which is where a load may be moved to.
    auto& given = m_program.at(m_function.at(id).type);
    bool isStringIfCell = !given.hasObjects() && !(given.primitives & (Primitive::Symbol | Primitive::BigInt));
    RELEASE_ASSERT(isStringIfCell || !mayBeNil);
    if (!isStringIfCell || !(mayBeNil || m_isInLoop))
        return receiver;
    // A variable that holds nothing yet reads as no bits, which pass for a cell.
    Boxed value(receiver.string.value());
    receiver.string = CellPointer(m_out.select(logicalAnd(isCell(value), isNotEmpty(value)).value(), value.value(), emptyString()));
    receiver.flags = IsSafe;
    if (mayBeNil) {
        receiver.isString = isCell(value);
        receiver.value = value;
    }
    return receiver;
}

void Lowering::turnAwayNil(const StringReceiver& receiver, StringID name)
{
    if (!receiver.isString)
        return;
    LBasicBlock isNil = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(receiver.isString.value(), usually(continuation), rarely(isNil));
    m_out.appendTo(isNil);
    throwNullish(receiver.value, name);
    m_out.appendTo(continuation);
}

Pointer Lowering::fiberOf(const StringReceiver& receiver)
{
    return loadField<Pointer>(receiver.string, m_heaps.JSRopeString_fiber0, receiver.flags | IsFixed);
}

Integer Lowering::stringLength(const StringReceiver& receiver)
{
    Pointer fiber = fiberOf(receiver);
    Pointer rope(m_out.add(receiver.string.value(), m_out.constIntPtr(JSRopeString::offsetOfLength() - StringImpl::lengthMemoryOffset())));
    Integer length = zeroExtended(loadField<Word32>(select(lacksBits(fiber, JSString::isRopeInPointer), fiber, rope), m_heaps.StringImpl_length, IsSafe));
    ensureStillAliveHere(receiver.string);
    return length;
}

Lowering::Characters Lowering::charactersOf(const StringReceiver& receiver)
{
    static_assert(sizeof(Instance) >= sizeof(StringImpl));
    LBasicBlock rope = newColdBlock();
    LBasicBlock substring = newColdBlock();
    LBasicBlock notSubstring = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> data;
    Vector<ValueFromBlock, 3> flags;
    auto yield = [&](Pointer first, Word32 flagsOfImpl) {
        data.append(m_out.anchor(first.value()));
        flags.append(m_out.anchor(flagsOfImpl.value()));
    };

    Pointer fiber = fiberOf(receiver);
    Word32 isFlat = lacksBits(fiber, JSString::isRopeInPointer);
    Pointer impl = select(isFlat, fiber, Pointer(m_instance));
    yield(loadField<Pointer>(impl, m_heaps.StringImpl_data, IsSafe), loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsSafe | IsFixed));
    m_out.branch(isFlat.value(), usually(continuation), rarely(rope));

    // Through a pointer of its own: a load is taken for an earlier one from the same place that is said never to change.
    m_out.appendTo(rope);
    CellPointer string(m_out.opaque(receiver.string.value()));
    Pointer current = loadField<Pointer>(string, m_heaps.JSRopeString_fiber0);
    m_out.branch(lacksBits(current, JSRopeString::isSubstringInPointer).value(), unsure(notSubstring), unsure(substring));

    // Nothing comes between these loads and the look at the characters that could make the substring flat, and so let go of its base.
    m_out.appendTo(substring);
    LValue lengthAndBaseLow = m_out.load64(string.value(), m_heaps.JSRopeString_fiber1);
    LValue baseHighAndOffset = m_out.load64(string.value(), m_heaps.JSRopeString_fiber2);
    CellPointer base(m_out.bitOr(m_out.lShr(lengthAndBaseLow, m_out.constInt32(32)), m_out.shl(m_out.bitAnd(baseHighAndOffset, m_out.constInt64(0xffff)), m_out.constInt32(32))));
    Integer offset(m_out.lShr(baseHighAndOffset, m_out.constInt32(16)));
    Pointer baseImpl = loadField<Pointer>(base, m_heaps.JSRopeString_fiber0);
    Word32 width = bitAnd(loadField<Word32>(baseImpl, m_heaps.StringImpl_hashAndFlags, IsFixed), word32(StringImpl::flagIs8Bit()));
    LValue offsetInBytes = m_out.shl(offset.value(), select(isZero(width), word32(1), word32(0)).value());
    yield(Pointer(m_out.add(loadField<Pointer>(baseImpl, m_heaps.StringImpl_data).value(), offsetInBytes)), width);
    m_out.jump(continuation);

    m_out.appendTo(notSubstring);
    Pointer flattened = eitherPointer(lacksBits(current, JSString::isRopeInPointer), [&] {
        return current;
    }, [&] {
        return AOT_TYPED_OPERATION(Flatten, receiver.string);
    });
    yield(loadField<Pointer>(flattened, m_heaps.StringImpl_data), loadField<Word32>(flattened, m_heaps.StringImpl_hashAndFlags, IsFixed));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return { Pointer(m_out.phi(Int64, data)), Word32(m_out.phi(Int32, flags)), receiver.string };
}

// The index is inside.
Word32 Lowering::codeUnitAt(const Characters& characters, Integer index)
{
    LBasicBlock narrow = m_out.newBlock();
    LBasicBlock wide = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(hasBits(characters.flags, StringImpl::flagIs8Bit()).value(), usually(narrow), rarely(wide));
    m_out.appendTo(narrow);
    ValueFromBlock fromNarrow = m_out.anchor(marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(characters.data, index, 0).value())), IsFixed));
    m_out.jump(continuation);
    m_out.appendTo(wide);
    ValueFromBlock fromWide = m_out.anchor(marked(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters16.atAnyIndex(), offsetBy(characters.data, index, 1).value())), IsFixed));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    Word32 unit(m_out.phi(Int32, fromNarrow, fromWide));
    ensureStillAliveHere(characters.owner);
    return unit;
}

// The index is inside.
Word32 Lowering::codePointAt(const Characters& characters, Integer index, Integer length)
{
    LBasicBlock isLead = newColdBlock();
    LBasicBlock hasNext = newColdBlock();
    LBasicBlock isPair = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    auto isSurrogate = [&](Word32 unit, uint32_t first) {
        return equal(bitAnd(unit, word32(~0x3ff)), word32(first));
    };
    Word32 lead = codeUnitAt(characters, index);
    Vector<ValueFromBlock, 4> results;
    results.append(m_out.anchor(lead.value()));
    m_out.branch(isSurrogate(lead, 0xd800).value(), rarely(isLead), usually(continuation));
    m_out.appendTo(isLead);
    Integer next = add(index, integer(1));
    results.append(m_out.anchor(lead.value()));
    m_out.branch(below(next, length).value(), usually(hasNext), rarely(continuation));
    m_out.appendTo(hasNext);
    Word32 trail = codeUnitAt(characters, next);
    results.append(m_out.anchor(lead.value()));
    m_out.branch(isSurrogate(trail, 0xdc00).value(), usually(isPair), rarely(continuation));
    m_out.appendTo(isPair);
    results.append(m_out.anchor(add(add(shiftLeft(lead, word32(10)), trail), word32(-U16_SURROGATE_OFFSET)).value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return Word32(m_out.phi(Int32, results));
}

CellPointer Lowering::stringOfCodeUnit(Word32 unit)
{
    return either<CellPointer>(belowOrEqual(unit, word32(maxSingleCharacterString)), [&] {
        return CellPointer(singleCharacterString(unit.value()));
    }, [&] {
        return AOT_TYPED_OPERATION(CharToString, unit);
    });
}

Operand Lowering::getStringElement(const StringReceiver& receiver, ValueID key, const Type& type, LBasicBlock otherwise)
{
    Rep rep = repOf(type);
    LBasicBlock outside = otherwise ? otherwise : newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    Integer index = indexOrElse(key, outside);
    orElse(below(index, stringLength(receiver)), outside);
    join.add(ensure(stringOfCodeUnit(codeUnitAt(charactersOf(receiver), index)), m_program.at(m_program.types.string), type));
    if (otherwise)
        return join.finish();

    m_out.appendTo(outside);
    turnAwayNil(receiver, { });
    if (auto nothing = absentElement(type))
        join.add(*nothing);
    return join.finish();
}

// ToIntegerOrInfinity of a number: what is not a number is zero. What is too large to be kept as an integer stays outside every string.
Integer Lowering::positionOf(ValueID id)
{
    Operand operand = get(id);
    if (operand.rep == Rep::Int64)
        return operand.as<Integer>();
    Number asNumber = operand.as<Number>();
    return select(equal(asNumber, asNumber), truncateToInteger(asNumber), integer(0));
}

// An argument of `slice` or `substring`, which is a number, as a place in a string of the length: not outside, and for `slice` counted
// from the end if negative.
Integer Lowering::placeInString(ValueID id, Integer length, bool isRelativeToEnd)
{
    auto& type = typeOf(id);
    Integer x;
    if (Operand operand = get(id); operand.rep == Rep::Int64)
        x = operand.as<Integer>();
    else {
        // No string is as long as the limit.
        Number limit = number(static_cast<double>(1ll << 32));
        Number given = operand.as<Number>();
        given = select(equal(given, given), given, number(0));
        given = select(greaterThan(given, limit), limit, given);
        given = select(lessThan(given, neg(limit)), neg(limit), given);
        x = truncateToInteger(given);
    }
    Integer capped = select(greaterThan(x, length), length, x);
    if (type.isInteger() && type.integers.least >= 0)
        return capped;
    Integer zero = integer(0);
    Integer fromEnd = zero;
    if (isRelativeToEnd) {
        Integer sum = add(length, x);
        fromEnd = select(lessThan(sum, zero), zero, sum);
    }
    return select(lessThan(x, zero), fromEnd, capped);
}

// With its header. The three words that make it a rope are for whoever asks to store, followed by fenceAfterInitialization().
CellPointer Lowering::allocateRope(LBasicBlock slowPath)
{
    LValue cell = allocateHeapCell(fixedPointer(Instance::offsetOfRopeStringAllocator()), slowPath);
    applyEffects(cell->as<PatchpointValue>(), { Writes::RuntimeState, false });
    storeHeader(cell, fixed32(Instance::offsetOfStringStructureID()), TypeInfoBlob(NonArray, TypeInfo(StringType, JSString::StructureFlags)).blob());
    return CellPointer(cell);
}

// From the one place to the other, neither of which is outside. Nothing, if they are the wrong way round.
CellPointer Lowering::partOfString(const StringReceiver& receiver, Integer length, Integer from, Integer to)
{
    LBasicBlock atMostOne = m_out.newBlock();
    LBasicBlock none = m_out.newBlock();
    LBasicBlock one = m_out.newBlock();
    LBasicBlock several = m_out.newBlock();
    LBasicBlock whole = m_out.newBlock();
    LBasicBlock part = m_out.newBlock();
    LBasicBlock rope = newColdBlock();
    LBasicBlock substring = newColdBlock();
    LBasicBlock make = m_out.newBlock();
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Cell, Int64);

    Integer span = sub(to, from);
    m_out.branch(greaterThan(span, integer(1)).value(), usually(several), rarely(atMostOne));

    m_out.appendTo(atMostOne);
    m_out.branch(equal(span, integer(1)).value(), unsure(one), unsure(none));
    m_out.appendTo(none);
    join.add(CellPointer(emptyString()));
    m_out.appendTo(one);
    join.add(stringOfCodeUnit(codeUnitAt(charactersOf(receiver), from)));

    m_out.appendTo(several);
    m_out.branch(equal(span, length).value(), rarely(whole), usually(part));
    m_out.appendTo(whole);
    join.add(receiver.string);

    // A substring of a string that is no rope. A part of a substring is a part of what that is a part of.
    m_out.appendTo(part);
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    Pointer fiber = fiberOf(receiver);
    Word32 isFlat = lacksBits(fiber, JSString::isRopeInPointer);
    ValueFromBlock flatBase = m_out.anchor(receiver.string.value());
    ValueFromBlock flatStart = m_out.anchor(from.value());
    ValueFromBlock flatFlags = m_out.anchor(loadField<Word32>(select(isFlat, fiber, Pointer(m_instance)), m_heaps.StringImpl_hashAndFlags, IsSafe | IsFixed).value());
    m_out.branch(isFlat.value(), usually(make), rarely(rope));

    m_out.appendTo(rope);
    CellPointer string(m_out.opaque(receiver.string.value()));
    Pointer current = loadField<Pointer>(string, m_heaps.JSRopeString_fiber0);
    m_out.branch(lacksBits(current, JSRopeString::isSubstringInPointer).value(), unsure(slowPath), unsure(substring));
    m_out.appendTo(substring);
    LValue lengthAndBaseLow = m_out.load64(string.value(), m_heaps.JSRopeString_fiber1);
    LValue baseHighAndOffset = m_out.load64(string.value(), m_heaps.JSRopeString_fiber2);
    ValueFromBlock baseOfSubstring = m_out.anchor(m_out.bitOr(m_out.lShr(lengthAndBaseLow, m_out.constInt32(32)), m_out.shl(m_out.bitAnd(baseHighAndOffset, m_out.constInt64(0xffff)), m_out.constInt32(32))));
    ValueFromBlock startInBase = m_out.anchor(m_out.add(from.value(), m_out.lShr(baseHighAndOffset, m_out.constInt32(16))));
    ValueFromBlock flagsOfSubstring = m_out.anchor(m_out.castToInt32(current.value()));
    m_out.jump(make);

    m_out.appendTo(make);
    LValue base = m_out.phi(Int64, flatBase, baseOfSubstring);
    LValue start = m_out.phi(Int64, flatStart, startInBase);
    Word32 flags(m_out.phi(Int32, flatFlags, flagsOfSubstring));
    CellPointer made = allocateRope(slowPath);
    m_out.store64(m_out.bitOr(m_out.constInt64(JSString::isRopeInPointer | JSRopeString::isSubstringInPointer), zeroExtended(bitAnd(flags, word32(StringImpl::flagIs8Bit()))).value()), made.value(), m_heaps.JSRopeString_fiber0);
    m_out.store64(m_out.bitOr(span.value(), m_out.shl(base, m_out.constInt32(32))), made.value(), m_heaps.JSRopeString_fiber1);
    m_out.store64(m_out.bitOr(m_out.lShr(base, m_out.constInt32(32)), m_out.shl(start, m_out.constInt32(16))), made.value(), m_heaps.JSRopeString_fiber2);
    fenceAfterInitialization();
    join.add(made);

    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(Substring, receiver.string, low32(from), low32(to)));
    return join.finish().as<CellPointer>();
}

std::optional<std::span<const uint8_t>> Lowering::constantASCIIText(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast)
        id = m_function.at(id).child(0);
    auto& value = m_function.at(id);
    if (value.opcode != Opcode::String)
        return std::nullopt;
    auto text = m_program.string(value.string());
    if (!charactersAreAllASCII(byteCast<Latin1Character>(text)))
        return std::nullopt;
    return text;
}

// Concatenation.

Lowering::StringPart Lowering::stringPartOf(CellPointer string)
{
    // A rope says how wide its characters are with the bit a StringImpl says it with. What it was made with holds for what it is made flat to.
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    StringReceiver receiver = stringReceiver(string);
    Pointer fiber = fiberOf(receiver);
    Word32 isFlat = lacksBits(fiber, JSString::isRopeInPointer);
    Word32 flagsOfImpl = loadField<Word32>(select(isFlat, fiber, Pointer(m_instance)), m_heaps.StringImpl_hashAndFlags, IsSafe | IsFixed);
    return { string, stringLength(receiver), select(isFlat, flagsOfImpl, Word32(m_out.castToInt32(fiber.value()))) };
}

// Of what is made a string of first, if it is none.
Lowering::StringPart Lowering::stringPartOf(ValueID id)
{
    CellPointer string = toStringCell(id);
    if (auto text = constantASCIIText(id))
        return { string, integer(text->size()), word32(StringImpl::flagIs8Bit()), text->empty() };
    return stringPartOf(string);
}

Lowering::StringPart Lowering::concatenate(const StringPart& first, const StringPart& second, const StringPart* third)
{
    Integer length = add(first.length, second.length);
    Word32 flags = bitAnd(first.flags, second.flags);
    bool mayBeEmpty = first.mayBeEmpty && second.mayBeEmpty;
    if (third) {
        length = add(length, third->length);
        flags = bitAnd(flags, third->flags);
        mayBeEmpty &= third->mayBeEmpty;
    }
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Cell, Int64);
    // Goes on where it is not.
    auto whereEmpty = [&](Integer lengthOfPart, CellPointer result) {
        LBasicBlock isEmpty = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        m_out.branch(isZero(lengthOfPart).value(), rarely(isEmpty), usually(isNot));
        m_out.appendTo(isEmpty);
        join.add(result);
        m_out.appendTo(isNot);
    };
    // There is one empty string, and no rope stands for it.
    if (third) {
        if (mayBeEmpty)
            whereEmpty(length, CellPointer(emptyString()));
    } else {
        if (first.mayBeEmpty)
            whereEmpty(first.length, second.string);
        if (second.mayBeEmpty)
            whereEmpty(second.length, first.string);
    }
    orElse(belowOrEqual(length, integer(JSString::MaxLength)), slowPath);
    CellPointer rope = allocateRope(slowPath);
    LValue mark = m_out.bitOr(m_out.constInt64(JSString::isRopeInPointer), zeroExtended(bitAnd(flags, word32(StringImpl::flagIs8Bit()))).value());
    m_out.store64(m_out.bitOr(first.string.value(), mark), rope.value(), m_heaps.JSRopeString_fiber0);
    m_out.store64(m_out.bitOr(length.value(), m_out.shl(second.string.value(), m_out.constInt32(32))), rope.value(), m_heaps.JSRopeString_fiber1);
    LValue secondHigh = m_out.lShr(second.string.value(), m_out.constInt32(32));
    m_out.store64(third ? m_out.bitOr(secondHigh, m_out.shl(third->string.value(), m_out.constInt32(16))) : secondHigh, rope.value(), m_heaps.JSRopeString_fiber2);
    fenceAfterInitialization();
    join.add(rope);

    m_out.appendTo(slowPath);
    CellPointer slow = AOT_TYPED_OPERATION(Concat, first.string, second.string);
    join.add(third ? AOT_TYPED_OPERATION(Concat, slow, third->string) : slow);
    return { join.finish().as<CellPointer>(), length, flags, mayBeEmpty };
}

// A rope has room for three.
CellPointer Lowering::lowerStrcat(const Value& value)
{
    auto& parts = value.arguments;
    if (parts.isEmpty())
        return CellPointer(emptyString());
    StringPart result = stringPartOf(parts[0]);
    for (unsigned i = 1; i < parts.size();) {
        StringPart second = stringPartOf(parts[i++]);
        if (i == parts.size()) {
            result = concatenate(result, second);
            break;
        }
        StringPart third = stringPartOf(parts[i++]);
        result = concatenate(result, second, &third);
    }
    return result.string;
}

// The engine keeps the strings of the integers it was asked for last.
CellPointer Lowering::integerToString(Integer value, const Type& type)
{
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Cell, Int64);
    Word32 low = low32(value);
    if (!type.isInteger() || !type.integers.isWithin(Range::int32()))
        orElse(equal(signExtended(low), value), slowPath);
    join.add(CellPointer(int32ToString(low.value(), slowPath)));
    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(IntToString, value));
    return join.finish().as<CellPointer>();
}

// Comparison.

// Whether the 8-bit characters from there on, of which there are as many, are the text, which is not empty.
Word32 Lowering::hasText(Pointer data, std::span<const uint8_t> text)
{
    size_t size = text.size();
    unsigned width = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
    Word32 result;
    auto compare = [&](size_t offset) {
        uint64_t expected = 0;
        for (unsigned i = 0; i < width; ++i)
            expected |= static_cast<uint64_t>(text[offset + i]) << (8 * i);
        TypedPointer pointer = m_out.address(m_heaps.characters8.atAnyIndex(), data.value(), offset);
        Word32 isSame;
        switch (width) {
        case 8:
            isSame = equal(Integer(marked(m_out.load64(pointer), IsFixed)), integer(static_cast<int64_t>(expected)));
            break;
        case 4:
            isSame = equal(Word32(marked(m_out.load32(pointer), IsFixed)), word32(static_cast<int32_t>(expected)));
            break;
        case 2:
            isSame = equal(Word32(marked(m_out.load16ZeroExt32(pointer), IsFixed)), word32(static_cast<int32_t>(expected)));
            break;
        default:
            isSame = equal(Word32(marked(m_out.load8ZeroExt32(pointer), IsFixed)), word32(static_cast<int32_t>(expected)));
            break;
        }
        result = result ? logicalAnd(result, isSame) : isSame;
    };
    for (size_t offset = 0; offset + width <= size; offset += width)
        compare(offset);
    // The last ones once more, with those before them.
    if (size % width)
        compare(size - width);
    return result;
}

// Whether the characters of the string from the place on are the text, which is not empty and has room there. The value is the text as a string.
Word32 Lowering::hasTextAt(const StringReceiver& receiver, Integer place, ValueID constant, std::span<const uint8_t> text)
{
    auto generally = [&] {
        return AOT_TYPED_OPERATION(StringHasAt, receiver.string, in<CellPointer>(constant), low32(place));
    };
    if (text.size() > maxLengthOfTextComparedInPlace)
        return generally();
    Characters characters = charactersOf(receiver);
    return either<Word32>(hasBits(characters.flags, StringImpl::flagIs8Bit()), [&] {
        Word32 result = hasText(offsetBy(characters.data, place, 0), text);
        ensureStillAliveHere(characters.owner);
        return result;
    }, generally);
}

// `value === "text"`, for a value that is a string, `undefined` or `null`.
Word32 Lowering::isStringWithText(ValueID id, ValueID constant, std::span<const uint8_t> text)
{
    Boxed value = in<Boxed>(id);
    // There is one empty string.
    if (text.empty())
        return equal(value, box(CellPointer(emptyString())));
    LBasicBlock isSameCell = m_out.newBlock();
    LBasicBlock isOtherCell = m_out.newBlock();
    LBasicBlock isAsLong = m_out.newBlock();
    LBasicBlock isNotAsLong = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    StringReceiver receiver = stringReceiver(id);
    m_out.branch(equal(value, in<Boxed>(constant)).value(), unsure(isSameCell), unsure(isOtherCell));
    m_out.appendTo(isSameCell);
    join.add(boolean(true));
    m_out.appendTo(isOtherCell);
    m_out.branch(equal(stringLength(receiver), integer(text.size())).value(), unsure(isAsLong), unsure(isNotAsLong));
    m_out.appendTo(isNotAsLong);
    join.add(boolean(false));
    m_out.appendTo(isAsLong);
    join.add(hasTextAt(receiver, integer(0), constant, text));
    return join.finish().as<Word32>();
}

// `a === b`, for values that are strings, `undefined` or `null`.
Word32 Lowering::areStringsEqual(ValueID a, ValueID b)
{
    if (auto text = constantASCIIText(b))
        return isStringWithText(a, b, *text);
    if (auto text = constantASCIIText(a))
        return isStringWithText(b, a, *text);

    Boxed x = in<Boxed>(a);
    Boxed y = in<Boxed>(b);
    StringReceiver first = stringReceiver(a);
    StringReceiver second = stringReceiver(b);
    Join join(m_out, Rep::Int32, Int32);
    // Goes on where the condition does not hold.
    auto when = [&](Word32 condition, Word32 answer) {
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition.value(), unsure(holds), unsure(next));
        m_out.appendTo(holds);
        join.add(answer);
        m_out.appendTo(next);
    };
    when(equal(x, y), boolean(true));
    Integer length = stringLength(first);
    when(notEqual(length, stringLength(second)), boolean(false));
    // What stands for `undefined` and `null` is as long as the empty string.
    Word32 areStrings = boolean(true);
    for (auto* receiver : { &first, &second }) {
        if (receiver->isString)
            areStrings = logicalAnd(areStrings, receiver->isString);
    }
    when(isZero(length), areStrings);
    Characters p = charactersOf(first);
    Characters q = charactersOf(second);
    when(equal(p.data, q.data), boolean(true));
    // There is one atom of a text.
    Word32 common = bitAnd(p.flags, q.flags);
    when(hasBits(common, StringImpl::flagIsAtom()), boolean(false));
    LBasicBlock slowPath = newColdBlock();
    orElse(hasBits(common, StringImpl::flagIs8Bit()), slowPath);

    LBasicBlock byteLoop = m_out.newBlock();
    LBasicBlock nextByte = m_out.newBlock();
    LBasicBlock wordLoop = m_out.newBlock();
    LBasicBlock nextWord = m_out.newBlock();
    LBasicBlock lastWord = m_out.newBlock();
    LBasicBlock isSame = m_out.newBlock();
    LBasicBlock isDifferent = m_out.newBlock();
    auto byteAt = [&](const Characters& characters, LValue index) {
        return marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(characters.data.value(), index))), IsFixed);
    };
    auto wordAt = [&](const Characters& characters, LValue index) {
        return marked(m_out.load64(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(characters.data.value(), index))), IsFixed);
    };
    ValueFromBlock firstByte = m_out.anchor(m_out.int64Zero);
    ValueFromBlock firstWord = m_out.anchor(m_out.int64Zero);
    m_out.branch(below(length, integer(8)).value(), unsure(byteLoop), unsure(wordLoop));

    m_out.appendTo(byteLoop);
    LValue byteIndex = m_out.phi(Int64, firstByte);
    m_out.branch(m_out.equal(byteAt(p, byteIndex), byteAt(q, byteIndex)), unsure(nextByte), unsure(isDifferent));
    m_out.appendTo(nextByte);
    LValue nextByteIndex = m_out.add(byteIndex, m_out.constInt64(1));
    m_out.addIncomingToPhi(byteIndex, m_out.anchor(nextByteIndex));
    m_out.branch(m_out.below(nextByteIndex, length.value()), unsure(byteLoop), unsure(isSame));

    m_out.appendTo(wordLoop);
    LValue wordIndex = m_out.phi(Int64, firstWord);
    m_out.branch(m_out.equal(wordAt(p, wordIndex), wordAt(q, wordIndex)), unsure(nextWord), unsure(isDifferent));
    m_out.appendTo(nextWord);
    LValue nextWordIndex = m_out.add(wordIndex, m_out.constInt64(8));
    m_out.addIncomingToPhi(wordIndex, m_out.anchor(nextWordIndex));
    m_out.branch(m_out.belowOrEqual(m_out.add(nextWordIndex, m_out.constInt64(8)), length.value()), unsure(wordLoop), unsure(lastWord));
    // The last ones once more, with those before them.
    m_out.appendTo(lastWord);
    LValue lastIndex = m_out.sub(length.value(), m_out.constInt64(8));
    join.add(Word32(m_out.equal(wordAt(p, lastIndex), wordAt(q, lastIndex))));

    m_out.appendTo(isSame);
    join.add(boolean(true));
    m_out.appendTo(isDifferent);
    join.add(boolean(false));
    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(StringEq, first.string, second.string));
    Word32 result = join.finish().as<Word32>();
    ensureStillAliveHere(p.owner);
    ensureStillAliveHere(q.owner);
    return result;
}

// The table.

Operand Lowering::lowerStringBuiltin(InlineBuiltin which, ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto& stringType = m_program.at(m_program.types.string);
    auto& booleanType = m_program.at(m_program.types.boolean);
    StringID name = m_program.stringOf(m_builtins.at(value.builtin()).nameName);
    StringReceiver receiver = stringReceiver(value.child(0));
    Integer length = stringLength(receiver);

    switch (which) {
    case InlineBuiltin::StringLength:
        turnAwayNil(receiver, name);
        return ensure(length, Type::integer(Range::length()), type);

    case InlineBuiltin::StringAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt: {
        Rep rep = repOf(type);
        LBasicBlock outside = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        Integer index = positionOf(arguments[0]);
        if (which == InlineBuiltin::StringAt)
            index = select(lessThan(index, integer(0)), add(index, length), index);
        orElse(below(index, length), outside);
        Characters characters = charactersOf(receiver);
        if (which == InlineBuiltin::StringCharCodeAt)
            join.add(ensure(zeroExtended(codeUnitAt(characters, index)), Type::integer({ 0, UINT16_MAX }), type));
        else if (which == InlineBuiltin::StringCodePointAt)
            join.add(ensure(zeroExtended(codePointAt(characters, index, length)), Type::integer({ 0, 0x10ffff }), type));
        else
            join.add(ensure(stringOfCodeUnit(codeUnitAt(characters, index)), stringType, type));

        m_out.appendTo(outside);
        turnAwayNil(receiver, name);
        if (which == InlineBuiltin::StringCharCodeAt)
            join.add(ensure(number(PNaN), m_program.at(m_program.types.number), type));
        else if (which == InlineBuiltin::StringCharAt)
            join.add(ensure(CellPointer(emptyString()), stringType, type));
        else
            join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
        return join.finish();
    }

    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr: {
        turnAwayNil(receiver, name);
        Integer from = placeInString(arguments[0], length, which != InlineBuiltin::StringSubstring);
        Integer to = length;
        if (arguments.size() > 1) {
            to = placeInString(arguments[1], length, which == InlineBuiltin::StringSlice);
            if (which == InlineBuiltin::StringSubstr) {
                Integer end = add(from, to);
                to = select(greaterThan(end, length), length, end);
            } else if (which == InlineBuiltin::StringSubstring) {
                Word32 isReversed = greaterThan(from, to);
                Integer least = select(isReversed, to, from);
                to = select(isReversed, from, to);
                from = least;
            }
        }
        return ensure(partOfString(receiver, length, from, to), stringType, type);
    }

    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith: {
        turnAwayNil(receiver, name);
        ValueID other = arguments[0];
        auto text = constantASCIIText(other);
        Integer otherLength = text ? integer(text->size()) : stringLength(stringReceiver(other));
        Integer place = arguments.size() > 1 ? placeInString(arguments[1], length, false) : which == InlineBuiltin::StringStartsWith ? integer(0) : length;
        if (which == InlineBuiltin::StringEndsWith)
            place = sub(place, otherLength);
        if (!text)
            return ensure(AOT_TYPED_OPERATION(StringHasAt, receiver.string, in<CellPointer>(other), low32(place)), booleanType, type);
        if (text->empty())
            return ensure(boolean(true), booleanType, type);
        LBasicBlock hasRoom = m_out.newBlock();
        LBasicBlock hasNoRoom = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(logicalAnd(greaterThanOrEqual(place, integer(0)), lessThanOrEqual(add(place, otherLength), length)).value(), unsure(hasRoom), unsure(hasNoRoom));
        m_out.appendTo(hasNoRoom);
        join.add(boolean(false));
        m_out.appendTo(hasRoom);
        join.add(hasTextAt(receiver, place, other, *text));
        return ensure(join.finish(), booleanType, type);
    }

    case InlineBuiltin::StringIncludes:
    case InlineBuiltin::StringIndexOf: {
        turnAwayNil(receiver, name);
        Integer place = arguments.size() > 1 ? placeInString(arguments[1], length, false) : integer(0);
        Word32 found = AOT_TYPED_OPERATION(StringIndexOf, receiver.string, in<CellPointer>(arguments[0]), low32(place));
        if (which == InlineBuiltin::StringIncludes)
            return ensure(notEqual(found, word32(-1)), booleanType, type);
        return ensure(signExtended(found), Type::integer({ -1, INT32_MAX }), type);
    }

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Maps and sets.
//
// What is in a map or a set only the engine and the code here look into: it is part of the state of the run time.

// As a key a number is an int32 wherever it can be one, and what is not a number is one key.
Boxed Lowering::mapKey(ValueID id)
{
    Operand operand = get(id);
    if (operand.rep != Rep::Double)
        return convertTo<Boxed>(operand, typeOf(id));
    Number number = operand.as<Number>();
    Integer whole = truncateToInteger(number);
    Word32 low = low32(whole);
    Word32 isInt32 = logicalAnd(equal(signExtended(low), whole), equal(convertToNumber(whole), number));
    return select(isInt32, boxInt32(low), box(purified(number)));
}

// Whether two keys of the type, as mapKey() yields them, are one key exactly if they are the same bits. A number that is kept boxed, or
// with room for `undefined`, may be a double that stands for an int32.
static bool isComparedByBits(const Type& key)
{
    if (key.primitives & (Primitive::String | Primitive::BigInt))
        return false;
    Rep rep = repOf(key);
    return !(key.primitives & Primitive::Number) || rep == Rep::Int64 || rep == Rep::Double;
}

// Of a key that is compared by its bits: rapidHashMix64().
Word32 Lowering::hashOfMapKey(Boxed key)
{
    LValue a = m_out.bitXor(key.value(), integer(static_cast<int64_t>(0x2d358dccaa6c78a5ULL)).value());
    LValue b = m_out.bitXor(key.value(), integer(static_cast<int64_t>(0x8bb84b93962eacc9ULL)).value());
    return Word32(m_out.castToInt32(m_out.bitXor(m_out.mul(a, b), m_out.uMulHigh(a, b))));
}

// For a key that is compared by its bits. Goes on where the table has it, and yields where: in a map the value comes next.
Pointer Lowering::findInMapOrSet(CellPointer table, bool isMap, Boxed key, Word32 hash, LBasicBlock absent)
{
    static_assert(JSMap::Helper::capacityIndex() == JSSet::Helper::capacityIndex() && JSMap::Helper::hashTableStartIndex() == JSSet::Helper::hashTableStartIndex());
    LBasicBlock hasStorage = m_out.newBlock();
    LBasicBlock loop = m_out.newBlock();
    LBasicBlock hasEntry = m_out.newBlock();
    LBasicBlock next = m_out.newBlock();
    LBasicBlock found = m_out.newBlock();
    auto at = [&](Pointer base, ptrdiff_t index) {
        return m_out.address(m_heaps.OrderedHashTableData, base.value(), index * sizeof(EncodedJSValue));
    };

    Pointer storage = loadField<Pointer>(table, isMap ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
    m_out.branch(m_out.notNull(storage.value()), usually(hasStorage), rarely(absent));

    // What is no key or value in there is an index or a count, boxed as an int32. No bits: the end of a chain.
    m_out.appendTo(hasStorage);
    Pointer data(m_out.add(storage.value(), m_out.constIntPtr(JSCellButterfly::offsetOfData())));
    Word32 capacity(m_out.load32(at(data, JSMap::Helper::capacityIndex())));
    Word32 bucket = add(bitAnd(hash, add(capacity, word32(-1))), word32(JSMap::Helper::hashTableStartIndex()));
    ValueFromBlock first = m_out.anchor(m_out.load64(at(offsetBy(data, zeroExtended(bucket), 3), 0)));
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue index = m_out.phi(Int64, first);
    m_out.branch(m_out.isZero64(index), unsure(absent), unsure(hasEntry));

    // What marks an entry as deleted is a cell no program gets hold of.
    m_out.appendTo(hasEntry);
    Pointer entry = offsetBy(data, zeroExtended(Word32(m_out.castToInt32(index))), 3);
    m_out.branch(equal(Boxed(m_out.load64(at(entry, 0))), key).value(), unsure(found), unsure(next));

    m_out.appendTo(next);
    m_out.addIncomingToPhi(index, m_out.anchor(m_out.load64(at(entry, isMap ? JSMap::Helper::ChainOffset : JSSet::Helper::ChainOffset))));
    m_out.jump(loop);

    m_out.appendTo(found);
    return entry;
}

Integer Lowering::sizeOfMapOrSet(CellPointer table, bool isMap)
{
    static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex());
    Pointer storage = loadField<Pointer>(table, isMap ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
    Integer size = either<Integer>(Word32(m_out.notNull(storage.value())), [&] {
        return zeroExtended(Word32(m_out.load32(m_out.address(m_heaps.OrderedHashTableData, storage.value(), JSMap::Helper::offsetOfAliveEntryCount()))));
    }, [&] {
        return integer(0);
    });
    ensureStillAliveHere(table);
    return size;
}

Operand Lowering::lowerMapOrSetBuiltin(InlineBuiltin which, ValueID result, const Value& value, Writes inPlace)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto& booleanType = m_program.at(m_program.types.boolean);
    if (which == InlineBuiltin::MapNew)
        return ensure(AOT_TYPED_OPERATION(NewMap), Type::engineClass(m_builtins.classes()[value.engineClass()]), type);

    CellPointer table = nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName));
    bool isMap = which != InlineBuiltin::SetAdd && which != InlineBuiltin::SetHas && which != InlineBuiltin::SetSize;
    if (which == InlineBuiltin::MapSize || which == InlineBuiltin::SetSize)
        return ensure(sizeOfMapOrSet(table, isMap), Type::integer(Range::length()), type);
    auto yieldsTable = [&] {
        return ensure(table, typeOf(value.child(0)).withoutPrimitives(Primitive::UndefinedOrNull), type);
    };
    Boxed key = mapKey(arguments[0]);

    // The stubs do strings in place that are no ropes and have been hashed, and leave big integers and doubles to the engine.
    if (!isComparedByBits(typeOf(arguments[0]))) {
        auto callStub = [&](Stub stub) {
            Vector<std::pair<LValue, GPRReg>, 4> operands { { table.value(), thisGPR }, { key.value(), argumentGPR(0) } };
            if (arguments.size() > 1)
                operands.append(std::pair { in<Boxed>(arguments[1]).value(), argumentGPR(1) });
            return Boxed(callICStub(stub, Int64, operands, std::nullopt, OperationEffects { inPlace, inPlace != Writes::Nothing }));
        };
        switch (which) {
        case InlineBuiltin::MapGet:
            return ensure(callStub(Stub::MapGet), type);
        case InlineBuiltin::MapHas:
            return ensure(unboxedBoolean(callStub(Stub::MapHas)), booleanType, type);
        case InlineBuiltin::SetHas:
            return ensure(unboxedBoolean(callStub(Stub::SetHas)), booleanType, type);
        default:
            callStub(isMap ? Stub::MapSet : Stub::SetAdd);
            return yieldsTable();
        }
    }

    Word32 hash = hashOfMapKey(key);
    LBasicBlock absent = m_out.newBlock();
    Pointer entry = findInMapOrSet(table, isMap, key, hash, absent);
    TypedPointer valueOfEntry = m_out.address(m_heaps.OrderedHashTableData, entry.value(), sizeof(EncodedJSValue));
    switch (which) {
    case InlineBuiltin::MapGet: {
        Rep rep = repOf(type);
        Join join(m_out, rep, b3TypeOf(rep));
        join.add(ensure(Boxed(m_out.load64(valueOfEntry)), type));
        m_out.appendTo(absent);
        join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
        Operand found = join.finish();
        ensureStillAliveHere(table);
        return found;
    }
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas: {
        Join join(m_out, Rep::Int32, Int32);
        join.add(boolean(true));
        m_out.appendTo(absent);
        join.add(boolean(false));
        Operand has = join.finish();
        ensureStillAliveHere(table);
        return ensure(has, booleanType, type);
    }
    default:
        break;
    }

    Join join(m_out, Rep::Void, Void);
    OperationEffects effectsOfAdding { Writes::RuntimeState, true };
    if (isMap) {
        didEmitWrite(Writes::RuntimeState);
        m_out.store64(in<Boxed>(arguments[1]).value(), valueOfEntry);
        if (typeOf(arguments[1]).hasCells())
            barrier(loadField<CellPointer>(table, m_heaps.JSMap_storage));
    }
    join.add({ });
    m_out.appendTo(absent);
    if (isMap)
        callOperation<operationAOTMapSet>(Entry::operationAOTMapSet, effectsOfAdding, table, key, in<Boxed>(arguments[1]), hash);
    else
        callOperation<operationAOTSetAdd>(Entry::operationAOTSetAdd, effectsOfAdding, table, key, hash);
    join.add({ });
    join.finish();
    return yieldsTable();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
