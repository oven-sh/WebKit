/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTTypedCompactTable.h"
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
// Length, width and place of the characters of a StringImpl never change. How long a string is a rope says itself, in a place that stays
// as it is when the rope is made flat.
//
// A substring is looked at where its characters are, in the string it is a part of. It is not made flat for that.

// As long as the text of a comparison that is carried out by loads and immediates.
static constexpr size_t maxLengthOfTextComparedInPlace = 32;

Lowering::StringReceiver Lowering::stringReceiver(CellPointer string)
{
    return { string, safeIfReaches(string, sizeof(JSString)), { }, { } };
}

// The value is a string, or `undefined` or `null` if its type says so: then nothing is loaded from it before `isString` has been looked at.
Lowering::StringReceiver Lowering::stringReceiver(ValueID id)
{
    Operand operand = get(id);
    if (operand.rep == Rep::Cell) {
        StringReceiver receiver = stringReceiver(operand.as<CellPointer>());
        receiver.id = id;
        return receiver;
    }
    Boxed value = operand.as<Boxed>();
    auto& type = typeOf(id);
    StringReceiver receiver = stringReceiver(asCell(value));
    receiver.id = id;
    if (type.mayBeUndefinedOrNull()) {
        receiver.flags = 0;
        receiver.isString = isCell(value, type);
        receiver.isCellOrNil = !(type.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull));
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
    throwNullish(receiver.value, name, receiver.isCellOrNil ? receiver.string : CellPointer());
    m_out.appendTo(continuation);
}

// The strings that loops read and do not define. Each is opened in front of the outermost loop that does not define it, by every block that enters that loop.
void Lowering::findStringsReadInLoops()
{
    // What is defined ahead of the entry of a handler is not there behind it.
    if (m_loops.isEmpty() || !m_catchEntrypoints.isEmpty())
        return;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        if (m_innermostLoop[b] == noLoop)
            continue;
        for (ValueID id : m_function.blocks[b].values) {
            auto& value = m_function.at(id);
            bool readsCharacters = true;
            if (value.opcode == Opcode::CallBuiltin) {
                auto& implementation = m_builtins.at(value.builtin()).implementation;
                if (implementation.kind != BuiltinImplementation::Inline)
                    continue;
                switch (implementation.inlined) {
                case InlineBuiltin::StringLength:
                    readsCharacters = false;
                    break;
                case InlineBuiltin::StringCharCodeAt:
                case InlineBuiltin::StringCodePointAt:
                case InlineBuiltin::StringCharAt:
                case InlineBuiltin::StringAt:
                    break;
                default:
                    continue;
                }
            } else if (value.opcode != Opcode::GetByVal || keyedAccessOf(m_program, m_function, value) != KeyedAccess::String)
                continue;
            ValueID string = stringDefinedBy(value.child(0));
            auto& type = m_program.at(m_function.at(string).type);
            if (!(type.primitives & Primitive::String) || !type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull))
                continue;
            // An outer loop comes first.
            for (unsigned i = 0; i < m_loops.size(); ++i) {
                auto& loop = m_loops[i];
                if (!loop.blocks.get(b) || !loop.header.index() || !isLoopInvariant(loop, string))
                    continue;
                if (m_stringsOpenedInFrontOfLoops.isEmpty())
                    m_stringsOpenedInFrontOfLoops.grow(m_loops.size());
                auto& opened = m_stringsOpenedInFrontOfLoops[i];
                size_t index = opened.findIf([&](auto& one) { return one.string == string; });
                if (index == notFound) {
                    index = opened.size();
                    opened.append(OpenedString { string, m_proc.addVariable(Int64), m_proc.addVariable(Int64), m_proc.addVariable(Int32) });
                }
                opened[index].isOpened |= readsCharacters;
                break;
            }
        }
    }
}

void Lowering::openStringsInFrontOfLoopsEnteredBy(const Terminator& terminator)
{
    if (m_stringsOpenedInFrontOfLoops.isEmpty())
        return;
    for (auto& target : successorsOf(terminator)) {
        for (unsigned i = 0; i < m_loops.size(); ++i) {
            if (m_loops[i].header != target.block || m_loops[i].blocks.get(m_currentBlockID.index()))
                continue;
            for (auto& opened : m_stringsOpenedInFrontOfLoops[i]) {
                StringReceiver receiver = stringReceiver(opened.string);
                LBasicBlock continuation = m_out.newBlock();
                // The read in the loop turns `undefined` and `null` away before it looks at any of this.
                if (receiver.isString) {
                    LBasicBlock isString = m_out.newBlock();
                    m_out.branch(receiver.isString.value(), usually(isString), rarely(continuation));
                    m_out.appendTo(isString);
                }
                auto set = [&](B3::Variable* variable, LValue value) {
                    m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), variable, value);
                };
                Pointer fiber = fiberOf(receiver);
                if (!opened.isOpened) {
                    set(opened.length, zeroExtended(either<Word32>(lacksBits(fiber, JSString::isRopeInPointer), [&] {
                        return loadField<Word32>(fiber, m_heaps.StringImpl_length);
                    }, [&] {
                        return loadField<Word32>(receiver.string, m_heaps.JSRopeString_length);
                    })).value());
                    ensureStillAliveHere(receiver.string);
                    m_out.jump(continuation);
                    m_out.appendTo(continuation);
                    continue;
                }
                // Every rope is made flat: a substring that is looked at in its base lets go of the base when something in the loop makes it flat.
                Pointer impl = eitherPointer(lacksBits(fiber, JSString::isRopeInPointer), [&] {
                    return fiber;
                }, [&] {
                    return AOT_TYPED_OPERATION(Flatten, receiver.string);
                });
                set(opened.data, loadField<Pointer>(impl, m_heaps.StringImpl_data).value());
                set(opened.length, zeroExtended(loadField<Word32>(impl, m_heaps.StringImpl_length)).value());
                set(opened.flags, loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
                ensureStillAliveHere(receiver.string);
                m_out.jump(continuation);
                m_out.appendTo(continuation);
            }
        }
    }
}

// For the block that is being lowered.
const Lowering::OpenedString* Lowering::openedString(ValueID id) const
{
    if (m_stringsOpenedInFrontOfLoops.isEmpty() || !id || m_innermostLoop[m_currentBlockID.index()] == noLoop)
        return nullptr;
    id = stringDefinedBy(id);
    for (unsigned i = 0; i < m_loops.size(); ++i) {
        if (!m_loops[i].blocks.get(m_currentBlockID.index()))
            continue;
        for (auto& opened : m_stringsOpenedInFrontOfLoops[i]) {
            if (opened.string == id)
                return &opened;
        }
    }
    return nullptr;
}

std::optional<Lowering::Characters> Lowering::charactersOfOpenedString(const StringReceiver& receiver)
{
    auto* opened = openedString(receiver.id);
    if (!opened || !opened->isOpened)
        return std::nullopt;
    auto get = [&](B3::Variable* variable) {
        return m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), variable);
    };
    return Characters { Pointer(get(opened->data)), Word32(get(opened->flags)), Integer(get(opened->length)), receiver.string };
}

Pointer Lowering::fiberOf(const StringReceiver& receiver)
{
    return loadField<Pointer>(receiver.string, m_heaps.JSRopeString_fiber0, receiver.flags | IsFixed);
}

Integer Lowering::stringLength(const StringReceiver& receiver)
{
    if (auto* opened = openedString(receiver.id))
        return Integer(m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), opened->length));
    Pointer fiber = fiberOf(receiver);
    Integer length = zeroExtended(either<Word32>(lacksBits(fiber, JSString::isRopeInPointer), [&] {
        return loadField<Word32>(fiber, m_heaps.StringImpl_length);
    }, [&] {
        return loadField<Word32>(receiver.string, m_heaps.JSRopeString_length);
    }));
    ensureStillAliveHere(receiver.string);
    return length;
}

// A rope that is no substring is made flat, or left to `ifRope`.
Lowering::Characters Lowering::charactersOf(const StringReceiver& receiver, LBasicBlock ifRope)
{
    if (auto characters = charactersOfOpenedString(receiver))
        return *characters;
    LBasicBlock flat = m_out.newBlock();
    LBasicBlock rope = newColdBlock();
    LBasicBlock substring = newColdBlock();
    LBasicBlock notSubstring = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> data;
    Vector<ValueFromBlock, 3> flags;
    Vector<ValueFromBlock, 3> lengths;
    auto yield = [&](Pointer first, Word32 flagsOfImpl, Word32 length) {
        data.append(m_out.anchor(first.value()));
        flags.append(m_out.anchor(flagsOfImpl.value()));
        lengths.append(m_out.anchor(length.value()));
        m_out.jump(continuation);
    };
    auto yieldAllOf = [&](Pointer impl) {
        yield(loadField<Pointer>(impl, m_heaps.StringImpl_data), loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags, IsFixed), loadField<Word32>(impl, m_heaps.StringImpl_length));
    };

    Pointer fiber = fiberOf(receiver);
    m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(rope));
    m_out.appendTo(flat);
    yieldAllOf(fiber);

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
    yield(Pointer(m_out.add(loadField<Pointer>(baseImpl, m_heaps.StringImpl_data).value(), offsetInBytes)), width, Word32(m_out.castToInt32(lengthAndBaseLow)));

    m_out.appendTo(notSubstring);
    Word32 isFlatByNow = lacksBits(current, JSString::isRopeInPointer);
    if (ifRope) {
        orElse(isFlatByNow, ifRope);
        yieldAllOf(current);
    } else {
        yieldAllOf(eitherPointer(isFlatByNow, [&] {
            return current;
        }, [&] {
            return AOT_TYPED_OPERATION(Flatten, receiver.string);
        }));
    }

    m_out.appendTo(continuation);
    return { Pointer(m_out.phi(Int64, data)), Word32(m_out.phi(Int32, flags)), zeroExtended(Word32(m_out.phi(Int32, lengths))), receiver.string };
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
Word32 Lowering::codePointAt(const Characters& characters, Integer index)
{
    LBasicBlock narrow = m_out.newBlock();
    LBasicBlock wide = newColdBlock();
    LBasicBlock isLead = newColdBlock();
    LBasicBlock hasNext = newColdBlock();
    LBasicBlock isPair = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    auto isSurrogate = [&](Word32 unit, uint32_t first) {
        return equal(bitAnd(unit, word32(~0x3ff)), word32(first));
    };
    auto wideUnitAt = [&](Integer at) {
        return Word32(marked(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters16.atAnyIndex(), offsetBy(characters.data, at, 1).value())), IsFixed));
    };
    Vector<ValueFromBlock, 5> results;
    m_out.branch(hasBits(characters.flags, StringImpl::flagIs8Bit()).value(), usually(narrow), rarely(wide));
    m_out.appendTo(narrow);
    results.append(m_out.anchor(marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(characters.data, index, 0).value())), IsFixed)));
    m_out.jump(continuation);
    m_out.appendTo(wide);
    Word32 lead = wideUnitAt(index);
    results.append(m_out.anchor(lead.value()));
    m_out.branch(isSurrogate(lead, 0xd800).value(), unsure(isLead), unsure(continuation));
    m_out.appendTo(isLead);
    Integer next = add(index, integer(1));
    results.append(m_out.anchor(lead.value()));
    m_out.branch(below(next, characters.length).value(), usually(hasNext), rarely(continuation));
    m_out.appendTo(hasNext);
    Word32 trail = wideUnitAt(next);
    results.append(m_out.anchor(lead.value()));
    m_out.branch(isSurrogate(trail, 0xdc00).value(), usually(isPair), rarely(continuation));
    m_out.appendTo(isPair);
    results.append(m_out.anchor(add(add(shiftLeft(lead, word32(10)), trail), word32(-U16_SURROGATE_OFFSET)).value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    Word32 codePoint(m_out.phi(Int32, results));
    ensureStillAliveHere(characters.owner);
    return codePoint;
}

CellPointer Lowering::stringOfCodeUnit(Word32 unit)
{
    return either<CellPointer>(belowOrEqual(unit, word32(maxSingleCharacterString)), [&] {
        return CellPointer(singleCharacterString(unit.value()));
    }, [&] {
        return AOT_TYPED_OPERATION(CharToString, unit);
    });
}

// The index is inside.
CellPointer Lowering::singleCharacterStringAt(const Characters& characters, Integer index, Word32* codeUnit)
{
    static_assert(maxSingleCharacterString == std::numeric_limits<Latin1Character>::max());
    Vector<ValueFromBlock, 2> codeUnits;
    CellPointer string = either<CellPointer>(hasBits(characters.flags, StringImpl::flagIs8Bit()), [&] {
        LValue narrow = marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(characters.data, index, 0).value())), IsFixed);
        if (codeUnit)
            codeUnits.append(m_out.anchor(narrow));
        return CellPointer(singleCharacterString(narrow));
    }, [&] {
        LValue wide = marked(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters16.atAnyIndex(), offsetBy(characters.data, index, 1).value())), IsFixed);
        CellPointer made = stringOfCodeUnit(Word32(wide));
        if (codeUnit)
            codeUnits.append(m_out.anchor(wide));
        return made;
    });
    if (codeUnit)
        *codeUnit = Word32(m_out.phi(Int32, codeUnits));
    ensureStillAliveHere(characters.owner);
    return string;
}

Operand Lowering::getStringElement(const StringReceiver& receiver, ValueID key, const Type& type, LBasicBlock otherwise)
{
    Rep rep = repOf(type);
    LBasicBlock outside = otherwise ? otherwise : newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    turnAwayNil(receiver, { });
    Integer index = indexOrElse(key, outside);
    Characters characters = charactersOf(receiver);
    orElse(below(index, characters.length), outside);
    // Not in an arm of a test on what is expected: what is defined there is not there behind it.
    bool keepsCodeUnit = !otherwise && keepsCodeUnitsOfStrings();
    Word32 codeUnit;
    CellPointer string = singleCharacterStringAt(characters, index, keepsCodeUnit ? &codeUnit : nullptr);
    Vector<ValueFromBlock, 2> codeUnits;
    if (keepsCodeUnit)
        codeUnits.append(m_out.anchor(zeroExtended(codeUnit).value()));
    join.add(ensure(string, m_program.at(m_program.types.string), type));
    if (otherwise)
        return join.finish();

    m_out.appendTo(outside);
    if (auto nothing = absentElement(type)) {
        if (keepsCodeUnit)
            codeUnits.append(m_out.anchor(m_out.constInt64(codeUnitOfUndefined)));
        join.add(*nothing);
    }
    Operand joined = join.finish();
    if (keepsCodeUnit)
        didMakeStringOfCodeUnit(m_currentBlock->values[m_currentIndexInBlock], joined, codeUnits);
    return joined;
}

// ToIntegerOrInfinity of a number: what is not a number is zero. What is too large to be kept as an integer stays outside every string.
Integer Lowering::positionOf(ValueID id)
{
    Operand operand = get(id);
    if (operand.rep == Rep::Int64)
        return operand.as<Integer>();
    Number asNumber = operand.as<Number>();
#if CPU(ARM64)
    // The conversion yields 0 for what is not a number.
    return truncateToInteger(asNumber);
#else
    return select(equal(asNumber, asNumber), truncateToInteger(asNumber), integer(0));
#endif
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
#if CPU(ARM64)
        // Likewise, and the least or the greatest integer for what does not fit.
        x = truncateToInteger(operand.as<Number>());
#else
        // No string is as long as the limit.
        Number limit = number(static_cast<double>(1ll << 32));
        Number given = operand.as<Number>();
        given = select(equal(given, given), given, number(0));
        given = select(greaterThan(given, limit), limit, given);
        given = select(lessThan(given, neg(limit)), neg(limit), given);
        x = truncateToInteger(given);
#endif
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
    LBasicBlock flat = m_out.newBlock();
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
    join.add(singleCharacterStringAt(charactersOf(receiver), from));

    m_out.appendTo(several);
    m_out.branch(equal(span, length).value(), rarely(whole), usually(part));
    m_out.appendTo(whole);
    join.add(receiver.string);

    // A substring of a string that is no rope. A part of a substring is a part of what that is a part of.
    m_out.appendTo(part);
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    Pointer fiber = fiberOf(receiver);
    m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(rope));
    m_out.appendTo(flat);
    ValueFromBlock flatBase = m_out.anchor(receiver.string.value());
    ValueFromBlock flatStart = m_out.anchor(from.value());
    ValueFromBlock flatFlags = m_out.anchor(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
    m_out.jump(make);

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

// Of a call of a built-in: its first argument is a text that is compared or looked for by loads and immediates. The cell of the constant is asked for in the general arm alone: see hasTextAt() and lowerStringSearch().
bool Lowering::looksForConstantWithoutItsCell(const Value& value) const
{
    if (value.arguments.isEmpty() || m_function.at(value.arguments[0]).opcode != Opcode::String)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    if (implementation.kind != BuiltinImplementation::Inline)
        return false;
    auto text = constantASCIIText(value.arguments[0]);
    if (!text)
        return false;
    // There the text goes by its number.
    auto isOutsideLoops = [&] {
        BlockID block = m_definingBlock[&value - m_function.values.span().data()];
        return block && !m_loopBlocks.get(block.index());
    };
    switch (implementation.inlined) {
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
        return text->size() <= maxLengthOfTextComparedInPlace || (value.arguments.size() == 1 && isOutsideLoops());
    case InlineBuiltin::StringIncludes:
    case InlineBuiltin::StringIndexOf:
    case InlineBuiltin::StringLastIndexOf:
        return text->size() == 1 || (!text->empty() && isOutsideLoops());
    default:
        return false;
    }
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
    LBasicBlock flat = m_out.newBlock();
    LBasicBlock rope = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer fiber = fiberOf(stringReceiver(string));
    m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), unsure(flat), unsure(rope));
    m_out.appendTo(flat);
    ValueFromBlock flatLength = m_out.anchor(loadField<Word32>(fiber, m_heaps.StringImpl_length).value());
    ValueFromBlock flatFlags = m_out.anchor(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
    m_out.jump(continuation);
    m_out.appendTo(rope);
    ValueFromBlock ropeLength = m_out.anchor(loadField<Word32>(string, m_heaps.JSRopeString_length).value());
    ValueFromBlock ropeFlags = m_out.anchor(m_out.castToInt32(fiber.value()));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    StringPart part { string, zeroExtended(Word32(m_out.phi(Int32, flatLength, ropeLength))), Word32(m_out.phi(Int32, flatFlags, ropeFlags)) };
    ensureStillAliveHere(string);
    return part;
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
CellPointer Lowering::concatenateAsRopes(std::span<const ValueID> parts)
{
    if (parts.empty())
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

CellPointer Lowering::lowerStrcat(const Value& value)
{
    return concatenateAsRopes(value.arguments.span());
}

// Of strings and numbers: it runs no code of the program.
bool Lowering::isConcatenation(const Value& value) const
{
    auto isStringOrNumber = [&](ValueID id) {
        auto& type = m_program.at(m_function.at(id).type);
        return type.isString() || type.isNumber();
    };
    switch (value.opcode) {
    case Opcode::ToString:
        return isStringOrNumber(value.child(0));
    case Opcode::Add:
        return isStringOrNumber(value.child(0)) && isStringOrNumber(value.child(1)) && m_program.at(value.type).isString();
    case Opcode::Strcat:
        for (ValueID part : value.arguments) {
            if (!isStringOrNumber(part))
                return false;
        }
        return true;
    default:
        return false;
    }
}

// Yields whether all parts are numbers and constants.
bool Lowering::collectPartsOfConcatenation(const BitVector& isInBlock, ValueID id, bool isLast, PartsOfConcatenation& parts, PartsOfConcatenation& lookedThrough)
{
    auto& value = m_function.at(id);
    if (isLast || (isInBlock.get(id.index()) && numberOfUsesOf(id) == 1 && isConcatenation(value))) {
        size_t numberOfParts = parts.size();
        size_t numberLookedThrough = lookedThrough.size();
        if (!isLast)
            lookedThrough.append(id);
        bool hasOnlyNumbersAndConstants = true;
        auto collect = [&](ValueID part) {
            hasOnlyNumbersAndConstants &= collectPartsOfConcatenation(isInBlock, part, false, parts, lookedThrough);
        };
        if (value.opcode == Opcode::Strcat) {
            for (ValueID part : value.arguments)
                collect(part);
        } else {
            collect(value.child(0));
            if (value.opcode == Opcode::Add)
                collect(value.child(1));
        }
        // To make a string of a number or of a string cannot fail, whenever it is done. To put strings together can, where they are too long for one: that stays where it is.
        if (isLast || value.opcode == Opcode::ToString || hasOnlyNumbersAndConstants)
            return hasOnlyNumbersAndConstants;
        parts.shrink(numberOfParts);
        lookedThrough.shrink(numberLookedThrough);
    }
    parts.append(id);
    return value.opcode == Opcode::String || m_program.at(value.type).isNumber();
}

// `a.id + "," + b.id`: the parts of the last Add or Strcat are what stands behind the ToString, Add and Strcat of its block that nothing else uses. Those are lowered nowhere, like a constant that is fetched where it is needed.
// What a Map, a Set or an object is asked by in this function, and what is handed on to that through casts and the parameters of blocks.
BitVector Lowering::valuesUsedAsKeys() const
{
    BitVector keys;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode == Opcode::GetByVal || value.opcode == Opcode::PutByVal) {
                keys.set(value.child(1).index());
                continue;
            }
            if (value.opcode != Opcode::CallBuiltin || value.arguments.isEmpty())
                continue;
            auto& implementation = m_builtins.at(value.builtin()).implementation;
            if (implementation.kind != BuiltinImplementation::Inline)
                continue;
            switch (implementation.inlined) {
            case InlineBuiltin::MapGet:
            case InlineBuiltin::MapSet:
            case InlineBuiltin::MapHas:
            case InlineBuiltin::MapDelete:
            case InlineBuiltin::SetAdd:
            case InlineBuiltin::SetHas:
            case InlineBuiltin::SetDelete:
                keys.set(value.arguments[0].index());
                break;
            default:
                break;
            }
        }
    }
    for (bool changed = !keys.isEmpty(); changed;) {
        changed = false;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::Cast && keys.get(id.index()))
                    changed |= !keys.set(value.child(0).index());
            }
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                for (unsigned i = 0; i < std::min(parameters.size(), target.arguments.size()); ++i) {
                    if (keys.get(parameters[i].index()))
                        changed |= !keys.set(target.arguments[i].index());
                }
            }
        }
    }
    return keys;
}

// Made in one piece a text costs a malloc, a hash and a destructor, which pays where it is looked up by at once, and nowhere else: a rope is cheaper to make.
void Lowering::findConcatenationsWithNumbers()
{
    // A handler may ask for whatever is defined ahead of it.
    if (!m_catchEntrypoints.isEmpty())
        return;
    std::optional<BitVector> keys;
    BitVector isInBlock;
    for (auto& block : m_function.blocks) {
        bool hasConcatenation = false;
        for (ValueID id : block.values) {
            Opcode opcode = m_function.at(id).opcode;
            hasConcatenation |= opcode == Opcode::Add || opcode == Opcode::Strcat;
        }
        if (!hasConcatenation)
            continue;
        isInBlock.clearAll();
        for (ValueID id : block.values)
            isInBlock.set(id.index());
        // The last one comes first.
        for (size_t i = block.values.size(); i--;) {
            ValueID id = block.values[i];
            auto& value = m_function.at(id);
            if ((value.opcode != Opcode::Add && value.opcode != Opcode::Strcat) || value.rep != Rep::Cell || isFetchedWhereNeeded(id) || !isConcatenation(value))
                continue;
            if (!keys)
                keys = valuesUsedAsKeys();
            if (!keys->get(id.index()))
                continue;
            PartsOfConcatenation parts;
            PartsOfConcatenation lookedThrough;
            collectPartsOfConcatenation(isInBlock, id, true, parts, lookedThrough);
            if (parts.size() < 2 || parts.size() > 8)
                continue;
            for (ValueID part : lookedThrough)
                m_isFetchedLate.set(part.index());
            m_partsOfConcatenations.add(id.index(), WTF::move(parts));
        }
    }
}

std::optional<Operand> Lowering::lowerAsPartOfConcatenation(ValueID id, const Value& value)
{
    if (m_partsOfConcatenations.isEmpty() || (value.opcode != Opcode::Add && value.opcode != Opcode::Strcat && value.opcode != Opcode::ToString))
        return std::nullopt;
    if (isFetchedWhereNeeded(id))
        return Operand(value.rep, nullptr);
    auto found = m_partsOfConcatenations.find(id.index());
    if (found == m_partsOfConcatenations.end())
        return std::nullopt;
    return convert(concatenateInOnePiece(found->value), typeOf(id), value.rep);
}

// For get(): one that was looked through, by itself.
CellPointer Lowering::concatenateWhereNeeded(const Value& value)
{
    if (value.opcode == Opcode::ToString)
        return toStringCell(value.child(0));
    if (value.opcode == Opcode::Strcat)
        return lowerStrcat(value);
    RELEASE_ASSERT(value.opcode == Opcode::Add);
    StringPart first = stringPartOf(value.child(0));
    return concatenate(first, stringPartOf(value.child(1))).string;
}

// Flat and hashed, where it is short: as a rule it is the key of a Map. See operationAOTTypedConcatenateWithIntegers.
CellPointer Lowering::concatenateInOnePiece(const PartsOfConcatenation& parts)
{
    static constexpr int64_t maxLength = 64;
    static constexpr int64_t maxLengthOfInt32 = 11;
    int64_t room = maxLength;
    unsigned numberOfNumbers = 0;
    bool hasConstant = false;
    bool hasOtherStrings = false;
    for (ValueID part : parts) {
        if (typeOf(part).isNumber()) {
            room -= maxLengthOfInt32;
            ++numberOfNumbers;
        } else if (auto text = constantASCIIText(part)) {
            room -= text->size();
            hasConstant |= !text->empty();
        } else
            hasOtherStrings = true;
    }
    if (room < (hasOtherStrings ? 8 : 0))
        return concatenateAsRopes(parts.span());

    LBasicBlock asRopes = hasOtherStrings ? m_out.newBlock() : nullptr;
    Join join(m_out, Rep::Cell, Int64);
    auto require = [&](Word32 condition) {
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition.value(), unsure(next), unsure(asRopes));
        m_out.appendTo(next);
    };
    Vector<Boxed, 8> passed;
    Integer lengthOfOtherStrings;
    Word32 flags;
    for (ValueID part : parts) {
        auto& type = typeOf(part);
        Operand operand = get(part);
        if (type.isNumber()) {
            if (operand.rep == Rep::Double) {
                // Minus zero reads "0".
                Number number = operand.as<Number>();
                Word32 low = low32(truncateToInteger(number));
                passed.append(select(equal(toNumber(low), number), boxInt32(low), box(purified(number))));
            } else
                passed.append(convertTo<Boxed>(operand, type));
            continue;
        }
        CellPointer string = operand.as<CellPointer>();
        passed.append(box(string));
        if (constantASCIIText(part))
            continue;
        Pointer fiber = fiberOf(stringReceiver(string));
        require(lacksBits(fiber, JSString::isRopeInPointer));
        Integer length = zeroExtended(loadField<Word32>(fiber, m_heaps.StringImpl_length));
        Word32 flagsOfImpl = loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed);
        lengthOfOtherStrings = lengthOfOtherStrings ? add(lengthOfOtherStrings, length) : length;
        flags = flags ? bitAnd(flags, flagsOfImpl) : flagsOfImpl;
    }
    if (hasOtherStrings) {
        require(logicalAnd(belowOrEqual(lengthOfOtherStrings, integer(room)), hasBits(flags, StringImpl::flagIs8Bit())));
        // Of one number by itself the engine keeps the string. There is one empty string, and the operation makes a new one.
        if (numberOfNumbers <= 1 && !hasConstant)
            require(logicalNot(isZero(lengthOfOtherStrings)));
    }
    LValue buffer = m_out.lockedStackSlot(passed.size() * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < passed.size(); ++i)
        m_out.store64(passed[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    join.add(AOT_TYPED_OPERATION(ConcatenateWithIntegers, word32(passed.size()), Pointer(buffer)));
    if (hasOtherStrings) {
        m_out.appendTo(asRopes);
        join.add(concatenateAsRopes(parts.span()));
    }
    return join.finish().as<CellPointer>();
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
    Word32 result;
    auto compare = [&](size_t offset, unsigned width) {
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
    unsigned width = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
    size_t offset = 0;
    for (; offset + width <= size; offset += width)
        compare(offset, width);
    // What is left, by the narrowest load that has room for it: its constant is the shortest. Where that is wider, with those before them once more.
    if (size_t left = size - offset) {
        unsigned last = left > 4 ? 8 : left > 2 ? 4 : left;
        compare(size - last, last);
    }
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

Word32 Lowering::areBytesEqual(Pointer p, Pointer q, Integer length)
{
    LBasicBlock isShort = m_out.newBlock();
    LBasicBlock byteLoop = m_out.newBlock();
    LBasicBlock nextByte = m_out.newBlock();
    LBasicBlock wordLoop = m_out.newBlock();
    LBasicBlock nextWord = m_out.newBlock();
    LBasicBlock lastWord = m_out.newBlock();
    LBasicBlock isSame = m_out.newBlock();
    LBasicBlock isDifferent = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    auto byteAt = [&](Pointer data, LValue index) {
        return marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data.value(), index))), IsFixed);
    };
    auto wordAt = [&](Pointer data, LValue index) {
        return marked(m_out.load64(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(data.value(), index))), IsFixed);
    };
    ValueFromBlock firstWord = m_out.anchor(m_out.int64Zero);
    m_out.branch(below(length, integer(8)).value(), unsure(isShort), unsure(wordLoop));

    m_out.appendTo(isShort);
    ValueFromBlock firstByte = m_out.anchor(m_out.int64Zero);
    m_out.branch(isZero(length).value(), rarely(isSame), usually(byteLoop));
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
    return join.finish().as<Word32>();
}

// The text is all ASCII and not empty. A few instructions at the site, and no cell for the text; what is in place is some sixty, and five fewer where it runs.
Word32 Lowering::isTextByStub(CellPointer string, ValueID constant)
{
    while (m_function.at(constant).opcode == Opcode::Cast)
        constant = m_function.at(constant).child(0);
    LValue number = m_out.constInt64(m_function.at(constant).string().index());
    return Word32(m_out.castToInt32(callAllocationStub(Stub::StringIsText, { { number, stubTemporaryGPRs[0] }, { string.value(), stubTemporaryGPRs[2] } }, { Writes::Nothing, false }, false)));
}

// The opening of a string that is wanted only if it is so long. The length is looked at first, of whatever kind the string is: it tells most strings from a text, and nothing else of them is loaded. Then a flat
// string, and a part of one of 8 bits, which is a rope by its bit and not rare (what slice, substring and trim yield), go one way. Any other rope goes to `otherwise`, and what is flat by now with it.
Lowering::Characters Lowering::charactersOfStringOfLength(const StringReceiver& receiver, size_t length, LBasicBlock isNotAsLong, LBasicBlock otherwise)
{
    // Goes on where the string is as long.
    auto look = [&](Word32 found, bool isCold) {
        LBasicBlock isAsLong = isCold ? newColdBlock() : m_out.newBlock();
        m_out.branch(equal(found, word32(static_cast<uint32_t>(length))).value(), unsure(isAsLong), unsure(isNotAsLong));
        m_out.appendTo(isAsLong);
    };
    if (auto characters = charactersOfOpenedString(receiver)) {
        look(low32(characters->length), false);
        return *characters;
    }
    static_assert(StringImpl::flagIs8Bit() == JSRopeString::is8BitInPointer);
    LBasicBlock isFlat = m_out.newBlock();
    LBasicBlock isRope = newColdBlock();
    LBasicBlock isPart = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Pointer fiber = fiberOf(receiver);
    m_out.branch(lacksBits(fiber, JSString::isRopeInPointer).value(), usually(isFlat), rarely(isRope));
    m_out.appendTo(isFlat);
    look(loadField<Word32>(fiber, m_heaps.StringImpl_length), false);
    ValueFromBlock flatData = m_out.anchor(loadField<Pointer>(fiber, m_heaps.StringImpl_data).value());
    ValueFromBlock flatFlags = m_out.anchor(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed).value());
    m_out.jump(continuation);

    m_out.appendTo(isRope);
    look(loadField<Word32>(receiver.string, m_heaps.JSRopeString_length), true);
    // Through a pointer of its own: a load is taken for an earlier one from the same place that is said never to change.
    CellPointer string(m_out.opaque(receiver.string.value()));
    Pointer current = loadField<Pointer>(string, m_heaps.JSRopeString_fiber0);
    constexpr uintptr_t isPartOf8Bits = JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer;
    m_out.branch(m_out.equal(m_out.bitAnd(current.value(), m_out.constIntPtr(isPartOf8Bits)), m_out.constIntPtr(isPartOf8Bits)), unsure(isPart), unsure(otherwise));
    // Nothing comes between these loads and the look at the characters that could make the substring flat, and so let go of its base.
    m_out.appendTo(isPart);
    LValue lengthAndBaseLow = m_out.load64(string.value(), m_heaps.JSRopeString_fiber1);
    LValue baseHighAndOffset = m_out.load64(string.value(), m_heaps.JSRopeString_fiber2);
    CellPointer base(m_out.bitOr(m_out.lShr(lengthAndBaseLow, m_out.constInt32(32)), m_out.shl(m_out.bitAnd(baseHighAndOffset, m_out.constInt64(0xffff)), m_out.constInt32(32))));
    LValue offset = m_out.lShr(baseHighAndOffset, m_out.constInt32(16));
    ValueFromBlock dataOfPart = m_out.anchor(m_out.add(loadField<Pointer>(loadField<Pointer>(base, m_heaps.JSRopeString_fiber0), m_heaps.StringImpl_data).value(), offset));
    ValueFromBlock flagsOfPart = m_out.anchor(m_out.castToInt32(current.value()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return { Pointer(m_out.phi(pointerType(), flatData, dataOfPart)), Word32(m_out.phi(Int32, flatFlags, flagsOfPart)), integer(length), receiver.string };
}

// The ways that are taken stand at the site as they do in a loop: the length, a flat string and a part of one, 8 bits, the characters against immediates. Any other rope and 16 bits are one call of the stub; so is a
// long text where the string is as long.
Word32 Lowering::isTextOfLengthByStub(const StringReceiver& receiver, ValueID constant, std::span<const uint8_t> text)
{
    LBasicBlock isNotAsLong = m_out.newBlock();
    LBasicBlock isAsLong = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    // `undefined` and `null` go where a string of another length goes: one block says no.
    if (receiver.isString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), unsure(isString), unsure(isNotAsLong));
        m_out.appendTo(isString);
    }
    if (text.size() > maxLengthOfTextComparedAtEverySite) {
        m_out.branch(equal(stringLength(receiver), integer(text.size())).value(), unsure(isAsLong), unsure(isNotAsLong));
        m_out.appendTo(isNotAsLong);
        join.add(boolean(false));
        m_out.appendTo(isAsLong);
        join.add(isTextByStub(receiver.string, constant));
        return join.finish().as<Word32>();
    }
    LBasicBlock byStub = newColdBlock();
    Characters characters = charactersOfStringOfLength(receiver, text.size(), isNotAsLong, byStub);
    m_out.jump(isAsLong);
    m_out.appendTo(isNotAsLong);
    join.add(boolean(false));
    m_out.appendTo(isAsLong);
    orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), byStub);
    Word32 isSame = hasText(characters.data, text);
    ensureStillAliveHere(characters.owner);
    // Branched on, the comparisons are one chain of flags.
    LBasicBlock isText = m_out.newBlock();
    m_out.branch(isSame.value(), unsure(isText), unsure(isNotAsLong));
    m_out.appendTo(isText);
    join.add(boolean(true));
    m_out.appendTo(byStub);
    join.add(isTextByStub(receiver.string, constant));
    return join.finish().as<Word32>();
}

// `value === "text"`, for a value that is a string, `undefined` or `null`.
Word32 Lowering::isStringWithText(ValueID id, ValueID constant, std::span<const uint8_t> text)
{
    Boxed value = in<Boxed>(id);
    // There is one empty string.
    if (text.empty())
        return equal(value, box(CellPointer(emptyString())));
    if (text.size() == 1) {
        if (auto codeUnit = codeUnitOfString(id))
            return equal(*codeUnit, integer(text[0]));
    }
    if (!m_isInLoop)
        return isTextOfLengthByStub(stringReceiver(id), constant, text);
    LBasicBlock isAsLong = m_out.newBlock();
    LBasicBlock isNotAsLong = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    StringReceiver receiver = stringReceiver(id);
    // Where the characters are compared with immediates, the cell of the constant is not fetched for a look at whether it is the same cell.
    if (text.size() > maxLengthOfTextComparedInPlace) {
        LBasicBlock isSameCell = m_out.newBlock();
        LBasicBlock isOtherCell = m_out.newBlock();
        m_out.branch(equal(value, in<Boxed>(constant)).value(), unsure(isSameCell), unsure(isOtherCell));
        m_out.appendTo(isSameCell);
        join.add(boolean(true));
        m_out.appendTo(isOtherCell);
    }
    if (receiver.isString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), unsure(isString), unsure(isNotAsLong));
        m_out.appendTo(isString);
    }
    m_out.branch(equal(stringLength(receiver), integer(text.size())).value(), unsure(isAsLong), unsure(isNotAsLong));
    m_out.appendTo(isNotAsLong);
    join.add(boolean(false));
    m_out.appendTo(isAsLong);
    join.add(hasTextAt(receiver, integer(0), constant, text));
    return join.finish().as<Word32>();
}

unsigned Lowering::numberOfPredecessorsOf(BlockID id)
{
    if (m_numberOfPredecessors.isEmpty()) {
        m_numberOfPredecessors.fill(0, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                ++m_numberOfPredecessors[target.block.index()];
        }
    }
    return m_numberOfPredecessors[id.index()];
}

std::optional<Lowering::ComparisonWithText> Lowering::comparisonWithTextBranchedOn(const Block& block)
{
    auto& terminator = block.terminator;
    if (terminator.kind != TerminatorKind::Branch || !block.values.contains(terminator.value) || numberOfUsesOf(terminator.value) != 1)
        return std::nullopt;
    auto& condition = m_function.at(terminator.value);
    constexpr uint16_t either = Primitive::True | Primitive::False;
    if (condition.opcode != Opcode::StrictEq || (m_program.at(condition.type).primitives & either) != either)
        return std::nullopt;
    for (unsigned i = 0; i < 2; ++i) {
        ValueID constant = condition.child(i);
        ValueID subject = condition.child(1 - i);
        auto text = constantASCIIText(constant);
        if (!text || text->empty() || text->size() > maxLengthOfTextComparedInPlace || m_function.at(constant).opcode != Opcode::String || !isFetchedWhereNeeded(constant))
            continue;
        // There is no value: the shape says what the field holds. See lowerStrictEq().
        if (m_pinnedReads.contains(subject.index()))
            continue;
        // Whatever may be a string: what is none is equal to no text.
        auto& type = m_program.at(m_function.at(subject).type);
        if (!(type.primitives & Primitive::String) || !holdsCells(m_function.at(subject).rep))
            continue;
        return ComparisonWithText { subject, constant, *text };
    }
    return std::nullopt;
}

// The comparison is carried out by the Branch.
std::optional<Operand> Lowering::deferToBranchOnText(ValueID id, const Value& value)
{
    if (value.opcode != Opcode::StrictEq || m_currentBlock->terminator.value != id)
        return std::nullopt;
    // There every value is looked at by itself.
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts()) [[unlikely]]
        return std::nullopt;
    auto comparison = comparisonWithTextBranchedOn(*m_currentBlock);
    if (!comparison || successorsOf(m_currentBlock->terminator).size() != 2)
        return std::nullopt;
    // Then it is a comparison of two integers.
    if (comparison->text.size() == 1 && codeUnitOfString(comparison->subject))
        return std::nullopt;
    m_isFetchedLate.set(id.index());
    return Operand(value.rep, nullptr);
}

// `switch (s)` over texts, `if (s === "a") .. else if (s === "b") ..`: the blocks on the "not equal" side that do nothing but compare the same string with another text are taken in. The string is opened once; what tells
// most texts apart is how long they are.
void Lowering::lowerBranchOnText()
{
    struct Case {
        std::span<const uint8_t> text;
        ValueID constant;
        LBasicBlock taken;
    };
    Vector<Case, 8> cases;
    const Block* block = m_currentBlock;
    ValueID subject = comparisonWithTextBranchedOn(*block)->subject;
    for (;;) {
        auto comparison = *comparisonWithTextBranchedOn(*block);
        // The first block's own terminator has been seen to: see lowerTerminator().
        if (block != m_currentBlock)
            openStringsInFrontOfLoopsEnteredBy(block->terminator);
        cases.append(Case { comparison.text, comparison.constant, edgeTo(block->terminator.targets[0], true) });
        auto& next = block->terminator.targets[1];
        auto& following = m_function.at(next.block);
        if (!next.arguments.isEmpty() || !following.parameters.isEmpty() || numberOfPredecessorsOf(next.block) != 1 || cases.size() == 64)
            break;
        auto another = comparisonWithTextBranchedOn(following);
        if (!another || stringDefinedBy(another->subject) != stringDefinedBy(subject))
            break;
        bool onlyCompares = true;
        for (ValueID id : following.values)
            onlyCompares &= id == following.terminator.value || (m_function.at(id).opcode == Opcode::String && isFetchedWhereNeeded(id));
        if (!onlyCompares)
            break;
        m_blocksTakenIntoBranchOnText.set(next.block.index());
        block = &following;
    }
    LBasicBlock otherwise = edgeTo(block->terminator.targets[1], true);

    StringReceiver receiver = stringReceiver(subject);
    bool mayBeNoString = !typeOf(subject).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
    if (mayBeNoString) {
        receiver.flags = 0;
        if (!receiver.isString && get(subject).rep != Rep::Cell)
            receiver.isString = isCell(get(subject).as<Boxed>(), typeOf(subject));
    }
    if (receiver.isString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(receiver.isString.value(), unsure(isString), unsure(otherwise));
        m_out.appendTo(isString);
    }
    if (mayBeNoString) {
        LBasicBlock isString = m_out.newBlock();
        m_out.branch(equal(typeOfCell(receiver.string), word32(StringType)).value(), unsure(isString), unsure(otherwise));
        m_out.appendTo(isString);
    }
    Vector<size_t, 8> lengths;
    for (auto& one : cases) {
        if (!lengths.contains(one.text.size()))
            lengths.append(one.text.size());
    }
    // Outside loops, where all texts are as long as each other: the length first, of whatever kind the string is: nothing else of a string of another length is loaded.
    bool looksAtLengthFirst = !m_isInLoop && lengths.size() == 1;
    LBasicBlock wide = newColdBlock();
    // Outside loops a rope that is no part of a string is not made flat: it goes with 16 bits, through the texts that are as long by the stub.
    Characters characters = m_isInLoop ? charactersOf(receiver) : looksAtLengthFirst ? charactersOfStringOfLength(receiver, lengths[0], otherwise, wide) : charactersOf(receiver, wide);
    orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), wide);

    Vector<LBasicBlock, 8> blocksByLength;
    Vector<FTL::SwitchCase, 8> byLength;
    for (size_t length : lengths) {
        blocksByLength.append(m_out.newBlock());
        byLength.append(FTL::SwitchCase(m_out.constInt64(length), blocksByLength.last(), FTL::Weight()));
    }
    if (looksAtLengthFirst)
        m_out.jump(blocksByLength[0]);
    else if (lengths.size() == 1)
        m_out.branch(equal(characters.length, integer(lengths[0])).value(), unsure(blocksByLength[0]), unsure(otherwise));
    else
        m_out.switchInstruction(characters.length.value(), byLength, otherwise, FTL::Weight());
    for (unsigned i = 0; i < lengths.size(); ++i) {
        m_out.appendTo(blocksByLength[i]);
        unsigned left = 0;
        for (auto& one : cases)
            left += one.text.size() == lengths[i];
        for (auto& one : cases) {
            if (one.text.size() != lengths[i])
                continue;
            Word32 isSame = hasText(characters.data, one.text);
            ensureStillAliveHere(characters.owner);
            LBasicBlock next = --left ? m_out.newBlock() : otherwise;
            m_out.branch(isSame.value(), unsure(one.taken), unsure(next));
            if (left)
                m_out.appendTo(next);
        }
    }

    m_out.appendTo(wide);
    if (!m_isInLoop) {
        Integer length;
        if (!looksAtLengthFirst)
            length = stringLength(receiver);
        for (unsigned i = 0; i < cases.size(); ++i) {
            LBasicBlock next = i + 1 < cases.size() ? newColdBlock() : otherwise;
            if (!looksAtLengthFirst) {
                LBasicBlock isAsLong = newColdBlock();
                m_out.branch(equal(length, integer(cases[i].text.size())).value(), unsure(isAsLong), unsure(next));
                m_out.appendTo(isAsLong);
            }
            m_out.branch(isTextByStub(receiver.string, cases[i].constant).value(), unsure(cases[i].taken), unsure(next));
            if (i + 1 < cases.size())
                m_out.appendTo(next);
        }
        return;
    }
    for (unsigned i = 0; i < cases.size(); ++i) {
        LBasicBlock isAsLong = newColdBlock();
        LBasicBlock next = i + 1 < cases.size() ? newColdBlock() : otherwise;
        m_out.branch(equal(characters.length, integer(cases[i].text.size())).value(), unsure(isAsLong), unsure(next));
        m_out.appendTo(isAsLong);
        m_out.branch(AOT_TYPED_OPERATION(StringHasAt, receiver.string, in<CellPointer>(cases[i].constant), word32(0)).value(), unsure(cases[i].taken), unsure(next));
        if (i + 1 < cases.size())
            m_out.appendTo(next);
    }
}

// `a === b`, for values that are strings, `undefined` or `null`.
Word32 Lowering::areStringsEqual(ValueID a, ValueID b)
{
    if (auto text = constantASCIIText(b))
        return isStringWithText(a, b, *text);
    if (auto text = constantASCIIText(a))
        return isStringWithText(b, a, *text);
    if (auto first = codeUnitOfString(a)) {
        if (auto second = codeUnitOfString(b))
            return equal(*first, *second);
    }

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
    for (auto* receiver : { &first, &second }) {
        if (receiver->isString)
            when(logicalNot(receiver->isString), boolean(false));
    }
    Integer length = stringLength(first);
    when(notEqual(length, stringLength(second)), boolean(false));
    // Outside loops what has run so far is all that stands at the site: most strings that differ, differ in length. What is in place from here on is over a hundred instructions. See Stub::StringsAreEqual.
    if (!m_isInLoop) {
        join.add(Word32(m_out.castToInt32(callAllocationStub(Stub::StringsAreEqual, { { first.string.value(), stubTemporaryGPRs[2] }, { second.string.value(), stubTemporaryGPRs[3] } }, { Writes::Nothing, false }, false))));
        return join.finish().as<Word32>();
    }
    Characters p = charactersOf(first);
    Characters q = charactersOf(second);
    when(equal(p.data, q.data), boolean(true));
    // There is one atom of a text.
    Word32 common = bitAnd(p.flags, q.flags);
    when(hasBits(common, StringImpl::flagIsAtom()), boolean(false));
    LBasicBlock slowPath = newColdBlock();
    orElse(hasBits(common, StringImpl::flagIs8Bit()), slowPath);

    join.add(areBytesEqual(p.data, q.data, length));
    m_out.appendTo(slowPath);
    join.add(AOT_TYPED_OPERATION(StringEq, first.string, second.string));
    Word32 result = join.finish().as<Word32>();
    ensureStillAliveHere(p.owner);
    ensureStillAliveHere(q.owner);
    return result;
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
        Pointer compactTable = loadField<Pointer>(table, isMap ? m_heaps.JSMap_compactTable : m_heaps.JSSet_compactTable);
        return either<Integer>(Word32(m_out.notNull(compactTable.value())), [&] {
            return zeroExtended(Word32(m_out.sub(m_out.load32(compactTable.value(), m_heaps.AOTTypedCompactTable_used), m_out.load32(compactTable.value(), m_heaps.AOTTypedCompactTable_deleted))));
        }, [&] {
            return integer(0);
        });
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
            break;
        }

        callStub(isMap ? Stub::MapSet : Stub::SetAdd);
        return yieldsTable();
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
