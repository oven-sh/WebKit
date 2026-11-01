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
LValue loadBytes(Output&, unsigned width, TypedPointer);

class StringEmitter;

// The cell of a string that is flat and stays so: wherever B3 moves a load from it, the load gives an impl. A test cannot promise that: the load may be moved above the test.
// Only StringEmitter makes one, from what the program says of the value.
class FlatStringCell {
public:
    CellPointer cell() const { return m_cell; }

private:
    friend class StringEmitter;
    explicit FlatStringCell(CellPointer cell)
        : m_cell(cell)
    {
    }

    CellPointer m_cell;
};

class StringEmitter final : public OperationEmitter {
public:
    explicit StringEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateSubstring(const ValueContext&);
    void generateSubstringOrUndefined(const ValueContext&);
    void generateFlattenString(const ValueContext&);
    void generateStringRange(const ValueContext&);
    void generateStringOrder(const ValueContext&);
    void generateStringOrderToCharacter(const ValueContext&);
    void generateStringEqualsLiteral(const ValueContext&);
    void generateValueEqualsLiteral(const ValueContext&);
    void generateValueLooselyEqualsLiteral(const ValueContext&);
    void generateStringStartsWithLiteral(const ValueContext& value) { generateEndsLiteral(value, LiteralEnd::Start); }
    void generateStringEndsWithLiteral(const ValueContext& value) { generateEndsLiteral(value, LiteralEnd::End); }
    void generateStringIndexOfLiteral(const ValueContext& value) { generateFindLiteral(value, SearchDirection::Forward); }
    void generateStringIndexOfCharacter(const ValueContext&);
    void generateStringLastIndexOfLiteral(const ValueContext& value) { generateFindLiteral(value, SearchDirection::Backward); }

    // What follows is emitted where the condition holds.
    void continueIf(Condition, B3::FrequentedBlock otherwise);
    // The 8-bit side falls through.
    template<typename T> T selectByWidth(Condition is8Bit, NOESCAPE const Invocable<T(bool is8Bit)> auto& read, B3::FrequencyClass frequencyOfWide = B3::FrequencyClass::Unlikely);

    Word32 word32Of(ValueID);
    // ToIntegerOrInfinity of a position that arrives as an integer or as a double, saturated beyond the length of every string.
    StringPosition truncatedPositionOf(ValueID, StringRangeMethod);
    Integer truncateToIndex(Number position);
    String textOf(StringID) const;

    Word32 lengthOf(ValueID string);
    Word32 lengthOf(LiteralStringWord, const Type&);
    Word32 lengthOf(CellPointer string);
    CharacterView viewOf(const OperationContext&, ValueID string);
    CharacterView viewOfFlatString(CellPointer flat);
    // The result of FlattenString.
    std::optional<FlatStringCell> flatStringByType(ValueID string);
    // A text of the image, written where it is used. The caller vouches that the cell is this value's: nothing here ties the two.
    std::optional<FlatStringCell> flatStringWrittenHere(ValueID string, CellPointer cellOfString);
    // For code that has no other road: what is neither stops the build.
    FlatStringCell flatStringAssumedBy(const OperationContext&, ValueID string);
    Pointer implOfFlatString(FlatStringCell);
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
    // Neither string is a rope.
    Condition areBoth8Bit(CellPointer, CellPointer);
    Word32 order(const OperationContext&, CellPointer, CellPointer);
    Word32 orderToCharacter(const CharacterView&, char16_t codeUnit);
    Condition equalsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, std::optional<FlatStringCell> flatSubject = std::nullopt, bool everyCellIsString = false, bool subjectIsComparedAgain = false);
    void branchOnEqualsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets, std::optional<FlatStringCell> flatSubject = std::nullopt, bool everyCellIsString = false, bool subjectIsComparedAgain = false);
    // Where the atom of a name of the image lies. In an image that may lie anywhere the atom is made on demand: `exists` says whether it has been.
    struct AtomOfName {
        Pointer address;
        std::optional<Condition> exists;
    };
    AtomOfName atomOfName(uint32_t identifierNumber);
    // For a subject that the comparison in place cannot answer: a rope, a 16-bit string.
    void branchOnEqualsLiteralByStub(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets);
    Condition hasTextAt(const CharacterView&, Word32 offset, StringID);
    void branchOnTextAt(const CharacterView&, Word32 offset, StringID, BranchTargets);
    bool canCompareInline(StringID) const;

    Word32 find(const CharacterView& subject, const CharacterView& sought, Word32 position, SearchDirection);
    Word32 findCodeUnit(const CharacterView& subject, Word32 codeUnit, Word32 position, SearchDirection);

    CellPointer concatenate(const OperationContext&, std::span<const CellPointer>);
    CellPointer concatenate(const OperationContext& context, std::initializer_list<CellPointer> strings) { return concatenate(context, std::span<const CellPointer>(strings.begin(), strings.size())); }
    enum class KeyUse : uint8_t { AnyKey, NameOfProperty };
    CellPointer concatenateKey(const OperationContext&, std::span<const Boxed> primitives, KeyUse);
    CellPointer integerToString(const OperationContext& context, Word32 value) { return integerToString(context, signExtended(value)); }
    CellPointer integerToString(const OperationContext&, Integer);
    CellPointer numberToString(const OperationContext&, Number);

private:
    Word32 flagsInImpl(Pointer impl, bool isImmutable);
    CharacterSource sourceInImpl(Pointer impl, bool isImmutable);
    CharacterSource sourceOfRope(const OperationContext&, CellPointer rope, Pointer fiber);
    CharacterView viewOfSource(const CharacterSource&, CellPointer owner);
    CellPointer substringBaseOf(CellPointer rope);
    Word32 loadCodeUnit(Pointer characters, Word32 index, bool is8Bit);

    void generateEqualsLiteral(const ValueContext&, ComparedOperand, LValue subject, std::optional<FlatStringCell> flatSubject, bool everyCellIsString = false);
    void generateEndsLiteral(const ValueContext&, LiteralEnd);
    void generateFindLiteral(const ValueContext&, SearchDirection);
    bool comparesLiteralInPlace(const OperationContext&, const String& text) const;
    bool isSubjectComparedAgain(const ValueContext&);
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
    // The comparisons with a literal whose subject another one compares too, in front of it or behind it on every way.
    std::optional<HashSet<ValueID>> m_comparisonsOfSharedSubjects;
};

template<typename T>
T StringEmitter::selectByWidth(Condition is8Bit, NOESCAPE const Invocable<T(bool is8Bit)> auto& read, B3::FrequencyClass frequencyOfWide)
{
    if (is8Bit.value()->hasInt())
        return read(is8Bit.value()->asInt());

    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    Join<T> join(m_out);
    m_out.branch(is8Bit.value(), normalEdge(narrow), B3::FrequentedBlock(wide, frequencyOfWide));
    m_out.appendTo(narrow);
    join.add(read(true));
    m_out.appendTo(wide);
    join.add(read(false));
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
