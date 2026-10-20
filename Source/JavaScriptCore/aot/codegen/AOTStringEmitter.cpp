/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTStubsStrings.h"
#include "B3MemoryValue.h"
#include "JSString.h"
#include "SmallStrings.h"
#include <unicode/utf16.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned widthOfFiber1Lower = JSRopeString::CompactFibers::widthOfFiber1LowerInLengthWord;
static constexpr unsigned shiftOfFiber2 = JSRopeString::CompactFibers::shiftOfFiber2InUpperWord;

namespace {

// The flags meet, not the answer of a test on them: a branch on the width then tests the bit itself.
class CharacterSourceJoin {
public:
    explicit CharacterSourceJoin(Output& out)
        : m_out(out)
        , m_continuation(out.newBlock())
    {
    }

    void add(const CharacterSource& source)
    {
        m_characters.append(m_out.anchor(source.characters.value()));
        m_lengths.append(m_out.anchor(source.length.value()));
        m_implFlags.append(m_out.anchor(source.implFlags.value()));
        m_out.jump(m_continuation);
    }

    CharacterSource finish()
    {
        m_out.appendTo(m_continuation);
        Pointer characters(m_out.phi(b3TypeOf(Pointer::representation), m_characters));
        Word32 length(m_out.phi(b3TypeOf(Word32::representation), m_lengths));
        Word32 implFlags(m_out.phi(b3TypeOf(Word32::representation), m_implFlags));
        return { characters, length, implFlags };
    }

private:
    Output& m_out;
    LBasicBlock m_continuation;
    Vector<ValueFromBlock, 2> m_characters;
    Vector<ValueFromBlock, 2> m_lengths;
    Vector<ValueFromBlock, 2> m_implFlags;
};

} // anonymous namespace

static LValue markImmutable(LValue load)
{
    load->as<B3::MemoryValue>()->setReadsMutability(B3::Mutability::Immutable);
    return load;
}

StringEmitter::StringEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Word32 StringEmitter::word32Of(ValueID id)
{
    Operand operand = get(id);
    if (operand.is<Word32>())
        return operand.as<Word32>();
    return low32(operand.as<Integer>());
}

Integer StringEmitter::truncatedPositionOf(ValueID id)
{
    Operand operand = get(id);
    if (operand.is<Number>())
        return truncateToInteger(operand.as<Number>());
    return integerOf(id);
}

String StringEmitter::textOf(StringID id) const
{
    String text = m_program.strings.toWTFString(id);
    if (!text.is8Bit() && text.containsOnlyLatin1())
        return String::make8Bit(text.span16());
    return text;
}

Pointer StringEmitter::loadImmutableField(Pointer base, const B3::AbstractHeap& heap)
{
    return Pointer(markImmutable(m_out.loadPtr(base.value(), heap)));
}

Word32 StringEmitter::loadImmutableWord(Pointer base, const B3::AbstractHeap& heap)
{
    return Word32(markImmutable(m_out.load32(base.value(), heap)));
}

CharacterSource StringEmitter::sourceInImpl(Pointer impl, bool isImmutable)
{
    Pointer characters = loadField<Pointer>(impl, m_heaps.StringImpl_data);
    Word32 length = loadField<Word32>(impl, m_heaps.StringImpl_length);
    Word32 flags = isImmutable ? loadImmutableWord(impl, m_heaps.StringImpl_hashAndFlags) : loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags);
    return { characters, length, flags };
}

// The cell stays flat, so wherever B3 moves the load it gives an impl, and the loads from that impl may leave a loop that runs no iteration.
Pointer StringEmitter::implOfFlatString(CellPointer flat)
{
    Pointer impl = loadImmutableField(asPointer(flat), m_heaps.JSString_value);
    m_proc.setKnownDereferenceableBytes(impl.value(), sizeof(StringImpl));
    return impl;
}

CharacterView StringEmitter::viewOfSource(const CharacterSource& source, CellPointer owner)
{
    return { source.characters, source.length, hasBits(source.implFlags, StringImpl::flagIs8Bit()), owner };
}

// The value word is not immutable here: the cell may have been a rope before the caller's test, and B3 hoists an immutable load above that test.
CharacterView StringEmitter::viewOfFlatString(CellPointer flat)
{
    return viewOfSource(sourceInImpl(loadField<Pointer>(flat, m_heaps.JSString_value), false), flat);
}

CellPointer StringEmitter::substringBaseOf(CellPointer rope)
{
    static_assert(widthOfFiber1Lower == 32 && shiftOfFiber2 == 16, "load32 and load16 read the two parts of fiber 1");
    LValue lower = m_out.load32(m_out.address(m_heaps.JSRopeString_fiber1, rope.value(), JSRopeString::offsetOfFiber1Lower()));
    LValue upper = m_out.load16ZeroExt32(m_out.address(m_heaps.JSRopeString_fiber2, rope.value(), JSRopeString::offsetOfFiber2()));
    return CellPointer(m_out.bitOr(m_out.zeroExtPtr(lower), m_out.shl(m_out.zeroExtPtr(upper), m_out.constInt32(widthOfFiber1Lower))));
}

CharacterSource StringEmitter::sourceOfRope(const OperationContext& context, CellPointer rope, Pointer fiber)
{
    Word32 length = loadField<Word32>(rope, m_heaps.JSRopeString_length);
    Condition isNotSubstring = hasNoBits(fiber, JSRopeString::isSubstringInPointer);
    CellPointer base = substringBaseOf(rope);
    CellPointer flat = select(isNotSubstring, rope, base);
    Pointer characters(callStub(context, ropeCharacters(), { rope.value() }).first);
    // Atomizing the base can replace its StringImpl with one of the other width, so a substring's own 8-bit flag may be stale.
    Word32 flags = loadField<Word32>(loadField<Pointer>(flat, m_heaps.JSString_value), m_heaps.StringImpl_hashAndFlags);
    return { characters, length, flags };
}

CharacterView StringEmitter::viewOfString(const OperationContext& context, CellPointer string)
{
    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    CharacterSourceJoin join(m_out);

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), normalEdge(flat), unlikelyEdge(rope));

    m_out.appendTo(flat);
    join.add(sourceInImpl(fiber, false));

    m_out.appendTo(rope);
    join.add(sourceOfRope(context, string, fiber));

    return viewOfSource(join.finish(), string);
}

CharacterView StringEmitter::viewOf(const OperationContext& context, ValueID string)
{
    CellPointer cell = in<CellPointer>(string);
    if (!typeOf(string).isOpaque(OpaqueKind::FlatString))
        return viewOfString(context, cell);
    return viewOfSource(sourceInImpl(implOfFlatString(cell), true), cell);
}

CharacterView StringEmitter::subview(const CharacterView& view, CodeUnitRange range)
{
    Integer offset(m_out.shl(zeroExtended(range.start).value(), logicalNot(view.is8Bit).value()));
    return { offsetBy(view.characters, offset, 0), sub(range.end, range.start), view.is8Bit, view.owner };
}

Word32 StringEmitter::lengthOf(CellPointer string)
{
    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    Join<Word32> join(m_out);

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), normalEdge(flat), unlikelyEdge(rope));

    m_out.appendTo(flat);
    join.add(loadField<Word32>(fiber, m_heaps.StringImpl_length));

    m_out.appendTo(rope);
    join.add(loadField<Word32>(string, m_heaps.JSRopeString_length));

    Word32 length = join.finish();
    ensureStillAliveHere(string);
    return length;
}

Word32 StringEmitter::lengthOf(ValueID string)
{
    CellPointer cell = in<CellPointer>(string);
    if (!typeOf(string).isOpaque(OpaqueKind::FlatString))
        return lengthOf(cell);
    Word32 length = loadField<Word32>(implOfFlatString(cell), m_heaps.StringImpl_length);
    ensureStillAliveHere(cell);
    return length;
}

Word32 StringEmitter::loadCodeUnit(Pointer characters, Word32 index, bool is8Bit)
{
    LValue offset = zeroExtended(index).value();
    if (is8Bit)
        return Word32(markImmutable(m_out.load8ZeroExt32(m_out.baseIndex(m_heaps.characters8, characters.value(), offset))));
    return Word32(markImmutable(m_out.load16ZeroExt32(m_out.baseIndex(m_heaps.characters16, characters.value(), offset))));
}

Word32 StringEmitter::codeUnitAt(const CharacterView& view, Word32 index)
{
    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    Join<Word32> join(m_out);

    m_out.branch(view.is8Bit.value(), normalEdge(narrow), normalEdge(wide));

    m_out.appendTo(narrow);
    join.add(loadCodeUnit(view.characters, index, true));

    m_out.appendTo(wide);
    join.add(loadCodeUnit(view.characters, index, false));

    Word32 codeUnit = join.finish();
    ensureStillAliveHere(view.owner);
    return codeUnit;
}

Word32 StringEmitter::codePointAt(const CharacterView& view, Word32 index)
{
    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    LBasicBlock isLead = newBlock();
    LBasicBlock hasNext = newBlock();
    LBasicBlock isPair = newBlock();
    LBasicBlock isSingleUnit = newBlock();
    Join<Word32> join(m_out);
    auto isSurrogate = [&](Word32 unit, int32_t first) {
        return equal(bitAnd(unit, word32(~0x3ff)), word32(first));
    };

    m_out.branch(view.is8Bit.value(), normalEdge(narrow), normalEdge(wide));

    m_out.appendTo(narrow);
    join.add(loadCodeUnit(view.characters, index, true));

    m_out.appendTo(wide);
    Word32 lead = loadCodeUnit(view.characters, index, false);
    m_out.branch(isSurrogate(lead, 0xd800).value(), normalEdge(isLead), normalEdge(isSingleUnit));

    m_out.appendTo(isLead);
    Word32 next = add(index, word32(1));
    m_out.branch(below(next, view.length).value(), normalEdge(hasNext), unlikelyEdge(isSingleUnit));

    m_out.appendTo(hasNext);
    Word32 trail = loadCodeUnit(view.characters, next, false);
    m_out.branch(isSurrogate(trail, 0xdc00).value(), normalEdge(isPair), unlikelyEdge(isSingleUnit));

    m_out.appendTo(isPair);
    join.add(sub(add(shiftLeft(lead, word32(10)), trail), word32(U16_SURROGATE_OFFSET)));

    m_out.appendTo(isSingleUnit);
    join.add(lead);

    Word32 codePoint = join.finish();
    ensureStillAliveHere(view.owner);
    return codePoint;
}

CellPointer StringEmitter::stringFromCodeUnit(const OperationContext& context, Word32 codeUnit)
{
    static_assert(maxSingleCharacterString == 0xff);
    LBasicBlock isLatin1 = newBlock();
    LBasicBlock isNotLatin1 = newBlock();
    Join<CellPointer> join(m_out);

    m_out.branch(belowOrEqual(codeUnit, word32(maxSingleCharacterString)).value(), normalEdge(isLatin1), unlikelyEdge(isNotLatin1));

    m_out.appendTo(isLatin1);
    join.add(singleCharacterString(codeUnit));

    m_out.appendTo(isNotLatin1);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::CharToString, { codeUnit }));

    return join.finish();
}

CellPointer StringEmitter::substring(const OperationContext& context, CellPointer string, CodeUnitRange range)
{
    return CellPointer(callStub(context, AOT::substring(), { string.value(), zeroExtended(range.start).value(), zeroExtended(range.end).value() }).first);
}

// IToD rounds only integers beyond 2^53, and those are out of range for every string before and after rounding.
static std::optional<LValue> integerBehindConversion(Number position)
{
    if (position.value()->opcode() != B3::IToD)
        return std::nullopt;
    return position.value()->child(0);
}

Integer StringEmitter::truncateToInteger(Number position)
{
    if (std::optional<LValue> source = integerBehindConversion(position))
        return (*source)->type() == B3::Int32 ? signExtended(Word32(*source)) : Integer(*source);
#if !CPU(ARM64)
    Number limit = number(static_cast<double>(1ll << 32));
    Number negativeLimit = number(-static_cast<double>(1ll << 32));
    position = select(equal(position, position), position, number(0));
    position = select(greaterThan(position, limit), limit, position);
    position = select(lessThan(position, negativeLimit), negativeLimit, position);
#endif
    return emitter<ConversionEmitter>().toInteger(position);
}

// A number beyond int64 becomes the least or the greatest int64, and read as unsigned neither is below a length.
Integer StringEmitter::truncateToIndex(Number position)
{
    if (std::optional<LValue> source = integerBehindConversion(position))
        return (*source)->type() == B3::Int32 ? signExtended(Word32(*source)) : Integer(*source);
#if !CPU(ARM64)
    position = select(equal(position, position), position, number(0));
#endif
    return emitter<ConversionEmitter>().toInteger(position);
}

Word32 StringEmitter::clampToLength(Integer x, Word32 length, bool isRelativeToEnd)
{
    Integer limit = zeroExtended(length);
    Integer zero = integer(0);
    Integer capped = select(greaterThan(x, limit), limit, x);
    Integer fromEnd = zero;
    if (isRelativeToEnd) {
        Integer sum = add(limit, x);
        fromEnd = select(lessThan(sum, zero), zero, sum);
    }
    return low32(select(lessThan(x, zero), fromEnd, capped));
}

CodeUnitRange StringEmitter::clampRange(StringRangeMethod method, Word32 length, Number first, std::optional<Number> second)
{
    std::optional<Integer> truncatedSecond;
    if (second)
        truncatedSecond = truncateToInteger(*second);
    return clampRange(method, length, truncateToInteger(first), truncatedSecond);
}

CodeUnitRange StringEmitter::clampRange(StringRangeMethod method, Word32 length, Integer first, std::optional<Integer> second)
{
    auto singleUnit = [&](bool isRelativeToEnd) -> CodeUnitRange {
        // Outside the string the index is -1, so the end is 0.
        Word32 index = low32(indexInString(first, length, isRelativeToEnd));
        Word32 start = select(lessThan(index, word32(0)), word32(0), index);
        return { start, add(index, word32(1)) };
    };

    switch (method) {
    case StringRangeMethod::Slice: {
        Word32 from = clampToLength(first, length, true);
        if (!second)
            return { from, length };
        Word32 to = clampToLength(*second, length, true);
        return { from, select(lessThan(to, from), from, to) };
    }
    case StringRangeMethod::Substring: {
        Word32 from = clampToLength(first, length, false);
        if (!second)
            return { from, length };
        Word32 to = clampToLength(*second, length, false);
        Condition isReversed = greaterThan(from, to);
        return { select(isReversed, to, from), select(isReversed, from, to) };
    }
    case StringRangeMethod::Substr: {
        Word32 from = clampToLength(first, length, true);
        Word32 available = sub(length, from);
        Word32 count = second ? clampToLength(*second, available, false) : available;
        return { from, add(from, count) };
    }
    case StringRangeMethod::SingleIndex:
        return singleUnit(false);
    case StringRangeMethod::SingleRelativeIndex:
        return singleUnit(true);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Integer StringEmitter::indexInString(Integer x, Word32 length, bool isRelativeToEnd)
{
    Integer limit = zeroExtended(length);
    if (isRelativeToEnd)
        x = select(lessThan(x, integer(0)), add(x, limit), x);
    return select(below(x, limit), x, integer(-1));
}

void StringEmitter::generateSubstring(const ValueContext& value)
{
    define(value, substring(value, in<CellPointer>(value->stringOperand()), { word32Of(value->startOperand()), word32Of(value->endOperand()) }));
}

void StringEmitter::generateSubstringOrUndefined(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    CodeUnitRange range { word32Of(value->startOperand()), word32Of(value->endOperand()) };
    Boxed result = either<Boxed>(greaterThanOrEqual(range.start, word32(0)), [&] {
        return boxCell(substring(value, string, range));
    }, [&] {
        return undefined();
    });
    define(value, emitter<ConversionEmitter>().inRepresentation(value, result, typeOf(value.id()), value.representation()));
}

void StringEmitter::generateFlattenString(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    // Without the phi, B3 could merge an immutable load from the flat value with a load that ran while the cell was a rope.
    define(value, either<CellPointer>(logicalNot(isRopeString(string)), [&] {
        return string;
    }, [&] {
        return CellPointer(callStub(value, flattenString(), { string.value() }).first);
    }));
}

void StringEmitter::generateStringRange(const ValueContext& value)
{
    std::optional<Integer> second;
    if (ValueID operand = value->secondOperand())
        second = truncatedPositionOf(operand);
    CodeUnitRange range = clampRange(value->method(), word32Of(value->lengthOperand()), truncatedPositionOf(value->firstOperand()), second);
    defineResults(value, { range.start, range.end });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
