/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTStubsStringLiterals.h"
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

// Whoever reads through `characters` ends with ensureStillAliveHere(owner).
struct CharacterView {
    Pointer characters;
    Word32 length;
    Condition is8Bit;
    CellPointer owner;
};

struct CharacterSource {
    Pointer characters;
    Word32 length;
    Word32 implFlags;
};

// The impl of a string that is no rope. Without a source each read is emitted where it is asked for, so that it becomes the memory operand of the compare that uses it.
struct FlatImpl {
    Pointer impl;
    CellPointer owner;
    bool isImmutable;
    std::optional<CharacterSource> source { };
};

struct CodeUnitRange {
    Word32 start;
    Word32 end;
};

// A position after ToIntegerOrInfinity. The range holds where the position is used, not wherever B3 computes the value: it chooses what is emitted and is never made a known range of the B3 value.
struct StringPosition {
    Integer value;
    Range range { Range::full() };
    // The conversion has made the least int64 of NaN and of whatever lies beyond int64. Whoever finds the value negative asks the number.
    std::optional<Number> numberBehindBareConversion { };
};

enum class LiteralEnd : uint8_t { Start, End };
enum class SearchDirection : uint8_t { Forward, Backward };

uint64_t packStringIntoRegister(StringView);

class StringEmitter final : public OperationEmitter {
public:
    explicit StringEmitter(CodeGenerator&);

    void generateSubstring(const ValueContext&);
    void generateSubstringOrUndefined(const ValueContext&);
    void generateFlattenString(const ValueContext&);
    void generateStringRange(const ValueContext&);
    void generateStringOrder(const ValueContext&);
    void generateStringOrderToCharacter(const ValueContext&);
    void generateStringEqualsLiteral(const ValueContext&);
    void generateValueEqualsLiteral(const ValueContext&);
    void generateValueLooselyEqualsLiteral(const ValueContext&);
    void generateStringStartsWithLiteral(const ValueContext&);
    void generateStringEndsWithLiteral(const ValueContext&);
    void generateStringIndexOfLiteral(const ValueContext&);
    void generateStringIndexOfCharacter(const ValueContext&);
    void generateStringLastIndexOfLiteral(const ValueContext&);

    Word32 word32Of(ValueID);
    // ToIntegerOrInfinity of a position that arrives as an integer or as a double, saturated beyond the length of every string.
    StringPosition truncatedPositionOf(ValueID, StringRangeMethod);
    Integer truncateToIndex(Number position);
    String textOf(StringID) const;

    Word32 lengthOf(ValueID string);
    Word32 lengthOf(CellPointer string);
    CharacterView viewOf(const OperationContext&, ValueID string);
    CharacterView viewOfFlatString(CellPointer flat);
    CharacterView viewOfString(const OperationContext&, CellPointer string);
    CharacterView subview(const CharacterView&, CodeUnitRange);

    Word32 codeUnitAt(const CharacterView&, Word32 index);
    Word32 codePointAt(const CharacterView&, Word32 index);
    CellPointer stringFromCodeUnit(const OperationContext&, Word32 codeUnit);
    CellPointer substring(const OperationContext&, CellPointer string, CodeUnitRange);
    CodeUnitRange clampRange(StringRangeMethod, Word32 length, StringPosition first, std::optional<StringPosition> second);
    CodeUnitRange clampRange(StringRangeMethod, Word32 length, Number first, std::optional<Number> second);
    Integer indexInString(Integer position, Word32 length, bool isRelativeToEnd);
    Integer indexInString(Number position, Word32 length, bool isRelativeToEnd) { return indexInString(truncateToInteger(position), length, isRelativeToEnd); }

    enum class StringIdentity : bool { Unknown, Distinct };
    Condition areEqual(const OperationContext&, CellPointer, CellPointer, StringIdentity = StringIdentity::Unknown);
    Word32 order(const OperationContext&, CellPointer, CellPointer);
    Word32 orderToCharacter(const CharacterView&, char16_t codeUnit);
    Condition equalsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, bool subjectIsFlat = false, bool everyCellIsString = false);
    void branchOnEqualsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets, bool subjectIsFlat = false, bool everyCellIsString = false);
    // For a subject that the comparison in place cannot answer: a rope, a 16-bit string.
    void branchOnEqualsLiteralByStub(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets);
    // ToNumber of the literal is NaN.
    Condition looselyEqualsLiteral(const OperationContext&, Boxed, StringID);
    Condition hasTextAt(const CharacterView&, Word32 offset, StringID);
    void branchOnTextAt(const CharacterView&, Word32 offset, StringID, BranchTargets);
    bool canCompareInline(StringID) const;

    Word32 find(const CharacterView& subject, const CharacterView& sought, Word32 position, SearchDirection);
    Word32 findCodeUnit(const CharacterView& subject, Word32 codeUnit, Word32 position, SearchDirection);

    CellPointer concatenate(const OperationContext&, std::span<const CellPointer>);
    CellPointer concatenate(const OperationContext& context, std::initializer_list<CellPointer> strings) { return concatenate(context, std::span<const CellPointer>(strings.begin(), strings.size())); }
    CellPointer concatenateValues(const OperationContext&, std::span<const Boxed> primitives, bool isLikelyKey);
    CellPointer integerToString(const OperationContext&, Word32);
    CellPointer integerToString(const OperationContext&, Integer);
    CellPointer numberToString(const OperationContext&, Number);

private:
    Pointer loadImmutableField(Pointer base, const B3::AbstractHeap&);
    Word32 loadImmutableWord(Pointer base, const B3::AbstractHeap&);
    CharacterSource sourceInImpl(Pointer impl, bool isImmutable);
    Pointer implOfFlatString(CellPointer flat);
    CharacterSource sourceOfFlatString(CellPointer flat);
    FlatImpl flatImplOf(CellPointer string, bool isFlat);
    CharacterSource sourceOfRope(const OperationContext&, CellPointer rope, Pointer fiber);
    CharacterView viewOfSource(const CharacterSource&, CellPointer owner);
    CellPointer substringBaseOf(CellPointer rope);
    Word32 loadCodeUnit(Pointer characters, Word32 index, bool is8Bit);
    Word32 lengthIn(const FlatImpl&);
    Condition is8BitIn(const FlatImpl&);
    Pointer charactersIn(const FlatImpl&);
    // The 8-bit side falls through.
    template<typename T> T selectByWidth(Condition is8Bit, NOESCAPE const Invocable<T()> auto& whenNarrow, NOESCAPE const Invocable<T()> auto& whenWide);

    void generateEqualsLiteral(const ValueContext&, ComparedOperand, LValue subject, bool subjectIsFlat, bool everyCellIsString = false);
    void generateEndsLiteral(const ValueContext&, LiteralEnd);
    void generateFindLiteral(const ValueContext&, SearchDirection);
    bool comparesLiteralInPlace(const OperationContext&, const String& text) const;
    void branchOnBytesAt(Pointer address, std::span<const uint8_t> bytes, const B3::AbstractHeap&, CellPointer owner, BranchTargets);
    Integer truncateToInteger(Number position);
    StringPosition truncatedPositionOf(Number position, StringRangeMethod);
    Word32 clampToLength(const StringPosition&, Word32 length, bool isRelativeToEnd);

    struct OpenedString {
        Pointer impl;
        CharacterSource source;
    };
    // What FlattenString has read where it made the flat string, which is in front of every reader of it.
    UncheckedKeyHashMap<LValue, OpenedString> m_openedStrings;
};

template<typename T>
T StringEmitter::selectByWidth(Condition is8Bit, NOESCAPE const Invocable<T()> auto& whenNarrow, NOESCAPE const Invocable<T()> auto& whenWide)
{
    if (is8Bit.value()->hasInt())
        return is8Bit.value()->asInt() ? whenNarrow() : whenWide();

    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    Join<T> join(m_out);
    m_out.branch(is8Bit.value(), normalEdge(narrow), unlikelyEdge(wide));
    m_out.appendTo(narrow);
    join.add(whenNarrow());
    m_out.appendTo(wide);
    join.add(whenWide());
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
