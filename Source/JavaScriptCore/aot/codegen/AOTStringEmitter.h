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

struct CodeUnitRange {
    Word32 start;
    Word32 end;
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
    Integer truncatedPositionOf(ValueID);
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
    CodeUnitRange clampRange(StringRangeMethod, Word32 length, Integer first, std::optional<Integer> second);
    CodeUnitRange clampRange(StringRangeMethod, Word32 length, Number first, std::optional<Number> second);
    Integer indexInString(Integer position, Word32 length, bool isRelativeToEnd);
    Integer indexInString(Number position, Word32 length, bool isRelativeToEnd) { return indexInString(truncateToInteger(position), length, isRelativeToEnd); }

    Condition areEqual(const OperationContext&, CellPointer, CellPointer);
    Word32 order(const OperationContext&, CellPointer, CellPointer);
    Condition equalsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID);
    void branchOnEqualsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets);
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
    CharacterSource sourceOfRope(const OperationContext&, CellPointer rope, Pointer fiber);
    CharacterView viewOfSource(const CharacterSource&, CellPointer owner);
    CellPointer substringBaseOf(CellPointer rope);
    Word32 loadCodeUnit(Pointer characters, Word32 index, bool is8Bit);

    void generateEqualsLiteral(const ValueContext&, ComparedOperand, LValue subject);
    void generateEndsLiteral(const ValueContext&, LiteralEnd);
    void generateFindLiteral(const ValueContext&, SearchDirection);
    Condition hasBytesAt(Pointer address, std::span<const uint8_t> bytes, const B3::AbstractHeap&);
    Condition has8BitTextAt(Pointer characters, Word32 offset, StringView);
    Condition has16BitTextAt(Pointer characters, Word32 offset, StringView);
    Integer truncateToInteger(Number position);
    Word32 clampToLength(Integer position, Word32 length, bool isRelativeToEnd);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
