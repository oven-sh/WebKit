/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsStrings.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "JSCJSValueInlines.h"
#include "JSString.h"
#include "SmallStrings.h"
#include <algorithm>
#include <cmath>
#include <unicode/utf16.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned widthOfFiber1Lower = JSRopeString::CompactFibers::widthOfFiber1LowerInLengthWord;
static constexpr unsigned shiftOfFiber2 = JSRopeString::CompactFibers::shiftOfFiber2InUpperWord;

void StringEmitter::continueIf(Condition condition, BlockEdge otherwise)
{
    LBasicBlock continuation = newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), otherwise);
    m_out.appendToContinuation(continuation);
}

Word32 StringEmitter::word32Of(ValueID id)
{
    Operand operand = get(id);
    if (operand.is<Word32>())
        return operand.as<Word32>();
    return low32(operand.as<Integer>());
}

String StringEmitter::textOf(StringID id) const
{
    Vector<char16_t> bufferForNonASCII;
    String text = m_generator.inputs().types.viewOf(id, bufferForNonASCII).toString();
    if (!text.is8Bit() && text.containsOnlyLatin1())
        return String::make8Bit(text.span16());
    return text;
}

Word32 StringEmitter::flagsInImpl(Pointer impl)
{
    return loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags);
}

CharacterSource StringEmitter::sourceInImpl(Pointer impl)
{
    Pointer characters = loadField<Pointer>(impl, m_heaps.StringImpl_data);
    Word32 length = loadField<Word32>(impl, m_heaps.StringImpl_length);
    return { characters, length, flagsInImpl(impl) };
}

std::optional<FlatStringCell> StringEmitter::flatStringByType(ValueID string)
{
    if (!typeOf(string).isOpaque(OpaqueKind::FlatString))
        return std::nullopt;
    return FlatStringCell(in<CellPointer>(string));
}

Pointer StringEmitter::implOfFlatString(FlatStringCell flatString)
{
    CellPointer flat = flatString.cell();
    if (auto opened = m_openedStrings.find(flat.value()); opened != m_openedStrings.end())
        return opened->value.impl;
    Pointer impl(m_out.loadPtr(asPointer(flat).value(), m_heaps.JSString_value));
    return impl;
}

CharacterView StringEmitter::viewOfSource(const CharacterSource& source, CellPointer owner)
{
    bool isFoundNarrow = m_flagsFoundNarrow.contains(source.implFlags.value());
    if (const OperationContext* context = generator().contextBeingGenerated(); context && (isFoundNarrow || m_flagsFoundWide.contains(source.implFlags.value())) && emitter<ElementAccessEmitter>().checksBounds()) {
        Word32 flags(m_out.opaque(source.implFlags.value()));
        emitter<ThrowEmitter>().requireOrCrash(*context, isFoundNarrow ? hasBits(flags, StringImpl::flagIs8Bit()) : hasNoBits(flags, StringImpl::flagIs8Bit()), std::nullopt, ProofSource::RememberedFact);
        build().statistics().count(StatisticID::UnrolledLoopTypeProofsChecked);
    }
    Condition is8Bit = m_flagsFoundNarrow.contains(source.implFlags.value()) ? boolean(true) : m_flagsFoundWide.contains(source.implFlags.value()) ? boolean(false) : hasBits(source.implFlags, StringImpl::flagIs8Bit());
    return { source.characters, source.length, is8Bit, owner };
}

bool StringEmitter::widthIsFoundAtRun(ValueID flatString)
{
    auto opened = m_openedStrings.find(in<CellPointer>(flatString).value());
    return opened != m_openedStrings.end() && !opened->value.source.implFlags.value()->hasInt();
}

std::optional<Condition> StringEmitter::findWidth(ValueID flatString, bool isNarrow)
{
    auto opened = m_openedStrings.find(in<CellPointer>(flatString).value());
    if (opened == m_openedStrings.end())
        return std::nullopt;
    Word32 flags = opened->value.source.implFlags;
    Vector<LValue, 2>& found = isNarrow ? m_flagsFoundNarrow : m_flagsFoundWide;
    if (found.contains(flags.value()))
        return boolean(true);
    Condition hasWidth = isNarrow ? hasBits(flags, StringImpl::flagIs8Bit()) : hasNoBits(flags, StringImpl::flagIs8Bit());
    if (hasWidth.value()->isInt(0))
        return std::nullopt;
    found.append(flags.value());
    return hasWidth;
}

CharacterView StringEmitter::viewOfFlatString(CellPointer flat)
{
    return viewOfSource(sourceInImpl(loadField<Pointer>(flat, m_heaps.JSString_value)), flat);
}

CellPointer StringEmitter::substringBaseOf(CellPointer rope)
{
    static_assert(widthOfFiber1Lower == 32 && shiftOfFiber2 == 16, "load32 and load16 read the two parts of fiber 1");
    LValue lower = m_out.load32(m_out.address(m_heaps.JSRopeString_fiber1, rope.value(), JSRopeString::offsetOfFiber1Lower()));
    LValue upper = m_out.load16ZeroExt32(m_out.address(m_heaps.JSRopeString_fiber2, rope.value(), JSRopeString::offsetOfFiber2()));
    LValue lowerBits = m_out.zeroExtPtr(lower);
    return CellPointer(m_out.bitOr(lowerBits, m_out.shl(m_out.zeroExtPtr(upper), widthOfFiber1Lower)));
}

CharacterSource StringEmitter::sourceOfRope(const OperationContext& context, CellPointer rope, Pointer fiber)
{
    Word32 length = loadField<Word32>(rope, m_heaps.JSRopeString_length);
    Condition isNotSubstring = hasNoBits(fiber, JSRopeString::isSubstringInPointer);
    CellPointer base = substringBaseOf(rope);
    CellPointer flat = select(isNotSubstring, rope, base);
    Pointer characters(callStub(context, ropeCharacters(), { rope.value() }).first);
    Word32 flags = loadField<Word32>(loadField<Pointer>(flat, m_heaps.JSString_value), m_heaps.StringImpl_hashAndFlags);
    return { characters, length, flags };
}

CharacterView StringEmitter::viewOfString(const OperationContext& context, CellPointer string)
{
    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    LBasicBlock continuation = newBlock();
    Vector<ValueFromBlock, 2> characters;
    Vector<ValueFromBlock, 2> lengths;
    Vector<ValueFromBlock, 2> implFlags;
    auto join = [&](const CharacterSource& source) {
        characters.append(m_out.anchor(source.characters.value()));
        lengths.append(m_out.anchor(source.length.value()));
        implFlags.append(m_out.anchor(source.implFlags.value()));
        m_out.jump(continuation);
    };

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isFlat = hasNoBits(fiber, JSString::isRopeInPointer);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isFlat.value(), normalEdge(flat), unlikelyEdge(rope));

    m_out.appendToArm(flat, fork);
    join(sourceInImpl(fiber));

    m_out.appendToArm(rope, fork);
    join(sourceOfRope(context, string, fiber));

    m_out.appendToJoin(continuation, fork);
    Pointer joinedCharacters(m_out.phi(machineTypeOf(Pointer::representation), characters));
    Word32 joinedLength(m_out.phi(machineTypeOf(Word32::representation), lengths));
    Word32 joinedFlags(m_out.phi(machineTypeOf(Word32::representation), implFlags));
    return viewOfSource({ joinedCharacters, joinedLength, joinedFlags }, string);
}

CharacterView StringEmitter::viewOf(const OperationContext& context, ValueID string)
{
    std::optional<FlatStringCell> flat = flatStringByType(string);
    if (!flat)
        return viewOfString(context, in<CellPointer>(string));
    CellPointer cell = flat->cell();
    if (auto opened = m_openedStrings.find(cell.value()); opened != m_openedStrings.end())
        return viewOfSource(opened->value.source, cell);
    return viewOfSource(sourceInImpl(implOfFlatString(*flat)), cell);
}

Word32 StringEmitter::lengthOf(CellPointer string)
{
    LBasicBlock flat = newBlock();
    LBasicBlock rope = newBlock();
    Join<Word32> join(m_out);

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isFlat = hasNoBits(fiber, JSString::isRopeInPointer);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isFlat.value(), normalEdge(flat), unlikelyEdge(rope));

    m_out.appendToArm(flat, fork);
    Word32 lengthInImpl = loadField<Word32>(fiber, m_heaps.StringImpl_length);
    ensureStillAliveHere(string);
    join.add(lengthInImpl);

    m_out.appendToArm(rope, fork);
    join.add(loadField<Word32>(string, m_heaps.JSRopeString_length));

    return join.finishBehind(fork);
}

Word32 StringEmitter::lengthOf(ValueID string)
{
    std::optional<FlatStringCell> flat = flatStringByType(string);
    if (!flat)
        return lengthOf(in<CellPointer>(string));
    CellPointer cell = flat->cell();
    if (auto opened = m_openedStrings.find(cell.value()); opened != m_openedStrings.end())
        return opened->value.source.length;
    Word32 length = loadField<Word32>(implOfFlatString(*flat), m_heaps.StringImpl_length);
    ensureStillAliveHere(cell);
    return length;
}

Word32 StringEmitter::loadCodeUnit(Pointer characters, Word32 index, bool is8Bit, std::optional<Integer> indexInWholeNumber)
{
    LValue offset = indexInWholeNumber ? indexInWholeNumber->value() : zeroExtended(index).value();
    if (is8Bit)
        return Word32(m_out.load8ZeroExt32(m_out.baseIndex(m_heaps.characters8, characters.value(), offset)));
    return Word32(m_out.load16ZeroExt32(m_out.baseIndex(m_heaps.characters16, characters.value(), offset)));
}

Word32 StringEmitter::codeUnitAt(const CharacterView& view, Word32 index, std::optional<Integer> indexInWholeNumber)
{
    return selectByWidth<Word32>(view.is8Bit, [&](bool is8Bit) {
        Word32 codeUnit = loadCodeUnit(view.characters, index, is8Bit, indexInWholeNumber);
        ensureStillAliveHere(view.owner);
        return codeUnit;
    });
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
        return equal(bitAnd(unit, ~0x3ff), first);
    };

    ScopeFork fork = m_out.forkScope();
    m_out.branch(view.is8Bit.value(), normalEdge(narrow), unlikelyEdge(wide));

    m_out.appendToArm(narrow, fork);
    join.add(loadCodeUnit(view.characters, index, true));

    m_out.appendToArm(wide, fork);
    Word32 lead = loadCodeUnit(view.characters, index, false);
    m_out.branch(isSurrogate(lead, 0xd800).value(), normalEdge(isLead), normalEdge(isSingleUnit));

    m_out.appendTo(isLead);
    Word32 next = add(index, word32(1));
    m_out.branch(below(next, view.length).value(), normalEdge(hasNext), unlikelyEdge(isSingleUnit));

    m_out.appendTo(hasNext);
    Word32 trail = loadCodeUnit(view.characters, next, false);
    m_out.branch(isSurrogate(trail, 0xdc00).value(), normalEdge(isPair), unlikelyEdge(isSingleUnit));

    m_out.appendTo(isPair);
    join.add(sub(add(shiftLeft(lead, 10), trail), U16_SURROGATE_OFFSET));

    m_out.appendTo(isSingleUnit);
    join.add(lead);

    Word32 codePoint = join.finishBehind(fork);
    ensureStillAliveHere(view.owner);
    return codePoint;
}

CellPointer StringEmitter::stringFromCodeUnit(const OperationContext& context, Word32 codeUnit)
{
    static_assert(maxSingleCharacterString == 0xff);
    LBasicBlock isLatin1 = newBlock();
    LBasicBlock isNotLatin1 = newBlock();
    Join<CellPointer> join(m_out);

    Condition fitsLatin1 = belowOrEqual(codeUnit, word32(maxSingleCharacterString));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(fitsLatin1.value(), normalEdge(isLatin1), unlikelyEdge(isNotLatin1));

    m_out.appendToArm(isLatin1, fork);
    join.add(singleCharacterString(codeUnit));

    m_out.appendToArm(isNotLatin1, fork);
    join.add(callOperation<CellPointer>(context, RuntimeOperation::CharToString, { codeUnit }));

    return join.finishBehind(fork);
}

CellPointer StringEmitter::substring(const OperationContext& context, CellPointer string, CodeUnitRange range)
{
    return CellPointer(callStub(context, AOT::substring(), { string.value(), zeroExtended(range.start).value(), zeroExtended(range.end).value() }).first);
}

static std::optional<LValue> integerBehindStringPosition(Number position)
{
    if (position.value()->opcode() != Machine::IToD)
        return std::nullopt;
    return position.value()->child(0);
}

Integer StringEmitter::truncateToInteger(Number position)
{
    if (position.value()->hasDouble()) {
        double constant = position.value()->asDouble();
        return integer(std::isnan(constant) ? 0 : static_cast<int64_t>(std::clamp(constant, -0x1p32, 0x1p32)));
    }
    if (std::optional<LValue> source = integerBehindStringPosition(position))
        return (*source)->type() == Machine::Int32 ? signExtended(Word32(*source)) : Integer(*source);
#if !CPU(ARM64)
    Number limit = number(static_cast<double>(1ll << 32));
    Number negativeLimit = number(-static_cast<double>(1ll << 32));
    position = select(equal(position, position), position, 0);
    position = select(greaterThan(position, limit), limit, position);
    position = select(lessThan(position, negativeLimit), negativeLimit, position);
    return emitter<ConversionEmitter>().toInteger(position);
#else
    return emitter<ConversionEmitter>().toNearestInteger(position);
#endif
}

StringPosition StringEmitter::truncatedPositionOf(Number position, StringRangeMethod method)
{
#if !CPU(ARM64)
    bool isClamped = method == StringRangeMethod::Slice || method == StringRangeMethod::Substring || method == StringRangeMethod::Substr;
    if (isClamped && !position.value()->hasDouble() && !integerBehindStringPosition(position))
        return { .value = emitter<ConversionEmitter>().toInteger(position), .numberBehindBareConversion = position };
#else
    UNUSED_PARAM(method);
#endif
    return { .value = truncateToInteger(position) };
}

StringPosition StringEmitter::truncatedPositionOf(ValueID id, StringRangeMethod method)
{
    Type type = typeOf(id);
    Range range = type.isInteger() ? type.integers : Range::full();
    Operand operand = get(id);
    if (!operand.is<Number>())
        return { .value = integerOf(id), .range = range };
    StringPosition position = truncatedPositionOf(operand.as<Number>(), method);
    position.range = range;
    return position;
}

Word32 StringEmitter::clampToLength(const StringPosition& position, Word32 length, bool isRelativeToEnd)
{
    Integer x = position.value;
    Integer limit = zeroExtended(length);
    auto countedFromStart = [&] {
        return low32(select(greaterThan(x, limit), limit, x));
    };
    auto countedFromEnd = [&] {
        if (!isRelativeToEnd)
            return word32(0);
        Integer sum = add(limit, x);
        return low32(select(lessThan(sum, 0), 0, sum));
    };

    Range range = x.value()->hasInt() ? Range::one(x.value()->asInt()) : position.range;
    if (range.minimum >= 0 || range.maximum < 0) {
        if (range == Range::one(0))
            return word32(0);
        return range.minimum >= 0 ? countedFromStart() : countedFromEnd();
    }

    LBasicBlock isNotNegative = newBlock();
    LBasicBlock isNegative = newBlock();
    Join<Word32> join(m_out);
    Condition positionIsNotNegative = greaterThanOrEqual(x, integer(0));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(positionIsNotNegative.value(), normalEdge(isNotNegative), unlikelyEdge(isNegative));
    m_out.appendToArm(isNotNegative, fork);
    join.add(countedFromStart());
    m_out.appendToArm(isNegative, fork);
    Word32 clamped = countedFromEnd();
    if (position.numberBehindBareConversion)
        clamped = select(greaterThan(*position.numberBehindBareConversion, number(0)), length, clamped);
    join.add(clamped);
    return join.finishBehind(fork);
}

CodeUnitRange StringEmitter::clampRange(StringRangeMethod method, Word32 length, Number first, std::optional<Number> second)
{
    std::optional<StringPosition> truncatedSecond;
    if (second)
        truncatedSecond = truncatedPositionOf(*second, method);
    return clampRange(method, length, truncatedPositionOf(first, method), truncatedSecond);
}

CodeUnitRange StringEmitter::clampRange(StringRangeMethod method, Word32 length, StringPosition first, std::optional<StringPosition> second)
{
    auto singleUnit = [&](bool isRelativeToEnd) -> CodeUnitRange {
        Word32 index = low32(indexInString(first.value, length, isRelativeToEnd));
        Word32 start = select(lessThan(index, 0), 0, index);
        return { start, add(index, word32(1)) };
    };

    switch (method) {
    case StringRangeMethod::Slice: {
        Word32 from = clampToLength(first, length, true);
        if (!second)
            return { from, length };
        Word32 to = clampToLength(*second, length, true);
        if (from.value()->isInt32(0))
            return { from, to };
        return { from, select(lessThan(to, from), from, to) };
    }
    case StringRangeMethod::Substring: {
        Word32 from = clampToLength(first, length, false);
        if (!second)
            return { from, length };
        Word32 to = clampToLength(*second, length, false);
        if (from.value()->isInt32(0))
            return { from, to };
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
    if (isRelativeToEnd) {
        Condition countsFromEnd = lessThan(x, 0);
        x = select(countsFromEnd, add(x, limit), x);
    }
    return select(below(x, limit), x, -1);
}

void StringEmitter::generateSubstring(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    if (ElementAccessEmitter& elements = emitter<ElementAccessEmitter>(); elements.checksBounds()) {
        elements.stopUnlessInside(value, belowOrEqual(start, end));
        elements.stopUnlessInside(value, belowOrEqual(end, lengthOf(string)));
    }
    define(value, substring(value, string, { start, end }));
}

void StringEmitter::generateSubstringOrUndefined(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    Word32 start = word32Of(value->startOperand());
    Word32 end = word32Of(value->endOperand());
    define(value, selectByBranch<Boxed>(logicalAnd(greaterThanOrEqual(start, word32(0)), greaterThanOrEqual(end, start)), [&] {
        if (ElementAccessEmitter& elements = emitter<ElementAccessEmitter>(); elements.checksBounds()) {
            elements.stopUnlessInside(value, belowOrEqual(start, end));
            elements.stopUnlessInside(value, belowOrEqual(end, lengthOf(string)));
        }
        return boxCell(substring(value, string, { start, end }));
    }, [&] {
        return undefined();
    }));
}

void StringEmitter::generateBoundsOfStringSplit(const ValueContext& value)
{
    uint32_t count = value->count();
    RELEASE_ASSERT(count < (1u << 31));
    CharacterView view = viewOf(value, value->stringOperand());
    static_assert(2 * sizeof(uint32_t) == sizeof(EncodedJSValue));
    Pointer bounds = emitter<RuntimeCallEmitter>().uninitializedArgumentBuffer(std::max(count, 1u));
    Word32 countAndWhetherAllAreCounted = word32(static_cast<int32_t>(count | (value->countsEveryPiece() ? 1u << 31 : 0)));
    String separator = textOf(value->literal());
    bool isOneCodeUnit = separator.length() == 1;
    Word32 sought = isOneCodeUnit ? word32(separator[0]) : numberOf(m_mentions.add(NumberSpace::String, value->literal()));
    Word32 numberOfPieces = selectByWidth<Word32>(view.is8Bit, [&](bool is8Bit) {
        RuntimeOperation operation = isOneCodeUnit
            ? (is8Bit ? RuntimeOperation::StringFindSeparators8 : RuntimeOperation::StringFindSeparators16)
            : (is8Bit ? RuntimeOperation::StringFindLongSeparators8 : RuntimeOperation::StringFindLongSeparators16);
        return low32(callLeaf<Integer>(operation, { view.characters, view.length, sought, countAndWhetherAllAreCounted, bounds }));
    });
    ensureStillAliveHere(view.owner);
    Vector<Operand, 9> results;
    for (uint32_t bound = 0; bound < 2 * count; ++bound)
        results.append(Word32(m_out.load32(m_out.address(m_heaps.argumentBuffer, bounds.value(), bound * sizeof(uint32_t)))));
    results.append(numberOfPieces);
    defineResults(value, results.span());
}

void StringEmitter::generateNewStringSplitArray(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::StringSplitString, { in<CellPointer>(value->stringOperand()), in<CellPointer>(value->separatorOperand()), word32(-1) }));
}

void StringEmitter::generateFlattenString(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    if (ValueID isRead = value->isReadOperand())
        string = select(in<Condition>(isRead), string, emptyString());
    LBasicBlock isFlat = newBlock();
    LBasicBlock isRope = newBlock();
    LBasicBlock continuation = newBlock();

    Pointer fiber = loadField<Pointer>(string, m_heaps.JSString_value);
    Condition isAlreadyFlat = hasNoBits(fiber, JSString::isRopeInPointer);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(isAlreadyFlat.value(), normalEdge(isFlat), unlikelyEdge(isRope));

    m_out.appendToArm(isFlat, fork);
    ValueFromBlock cellWhenFlat = m_out.anchor(string.value());
    ValueFromBlock implWhenFlat = m_out.anchor(fiber.value());
    m_out.jump(continuation);

    m_out.appendToArm(isRope, fork);
    CellPointer flattened(callStub(value, flattenString(), { string.value() }).first);
    ValueFromBlock cellWhenRope = m_out.anchor(flattened.value());
    ValueFromBlock implWhenRope = m_out.anchor(loadField<Pointer>(flattened, m_heaps.JSString_value).value());
    m_out.jump(continuation);

    m_out.appendToJoin(continuation, fork);
    CellPointer flat(m_out.phi(machineTypeOf(CellPointer::representation), cellWhenFlat, cellWhenRope));
    Pointer impl(m_out.phi(machineTypeOf(Pointer::representation), implWhenFlat, implWhenRope));
    m_openedStrings.add(flat.value(), OpenedString { impl, sourceInImpl(impl) });
    define(value, flat);
}

void StringEmitter::generateStringRange(const ValueContext& value)
{
    StringRangeMethod method = value->method();
    Word32 length = word32Of(value->lengthOperand());
    StringPosition first = truncatedPositionOf(value->firstOperand(), method);
    std::optional<StringPosition> second;
    if (ValueID operand = value->secondOperand())
        second = truncatedPositionOf(operand, method);
    CodeUnitRange range = clampRange(method, length, first, second);
    defineResults(value, { range.start, range.end });
}

CellPointer StringEmitter::concatenate(const OperationContext& context, std::span<const CellPointer> strings)
{
    if (strings.empty())
        return emptyString();

    CellPointer result = strings[0];
    for (size_t i = 1; i < strings.size();) {
        Vector<LValue, 3> operands { result.value(), strings[i++].value() };
        if (i < strings.size())
            operands.append(strings[i++].value());
        result = CellPointer(callStub(context, AOT::concatenate(static_cast<unsigned>(operands.size())), operands.span()).first);
    }
    return result;
}

CellPointer StringEmitter::concatenateKey(const OperationContext& context, std::span<const Boxed> primitives, KeyUse keyUse)
{
    if (primitives.size() == 2)
        return CellPointer(callStub(context, concatenateKeyOfPair(keyUse == KeyUse::NameOfProperty ? ConcatenatedKeyForm::Name : ConcatenatedKeyForm::Key), { primitives[0].value(), primitives[1].value() }).first);
    UseOfJoinedText use = keyUse == KeyUse::NameOfProperty ? UseOfJoinedText::LikelyName : UseOfJoinedText::LikelyKey;
    return callOperation<CellPointer>(context, RuntimeOperation::Concatenate, { word32(static_cast<int32_t>(primitives.size())), argumentBuffer(primitives), word32(static_cast<int32_t>(use)) });
}

CellPointer StringEmitter::integerToString(const OperationContext& context, Integer value)
{
    return CellPointer(callStub(context, AOT::integerToString(), { value.value() }).first);
}

CellPointer StringEmitter::numberToString(const OperationContext& context, Number value)
{
    return callOperation<CellPointer>(context, RuntimeOperation::DoubleToString, { value });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
