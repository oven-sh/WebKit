/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTRepresentationRules.h"
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

// A position after ToIntegerOrInfinity.
struct StringPosition {
    Integer value;
    // What the type says of the position where it is used. It chooses what is emitted.
    Range range { Range::full() };
    // The conversion has made the least int64 of NaN and of whatever lies beyond int64. Whoever finds the value negative asks the number.
    std::optional<Number> numberBehindBareConversion { };
};

enum class LiteralEnd : uint8_t { Start, End };
enum class SearchDirection : uint8_t { Forward, Backward };

uint64_t packStringIntoRegister(StringView);
LValue loadBytes(Output&, unsigned width, TypedPointer);

class StringEmitter;

// The cell of a string that is flat and stays so. Only StringEmitter makes one, from what the program says of the value.
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

    void generateTryStrcat(const ValueContext&);
    void generateSubstring(const ValueContext&);
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
    void generateStringRangeEqualsLiteral(const ValueContext&);
    void generateStringRangeToNumber(const ValueContext&);
    void generateStringRangeParseInt(const ValueContext&);
    // The units from the start operand to the end operand, which lie inside the string.
    CharacterView viewOfRange(const ValueContext&);

    // What follows is emitted where the condition holds.
    void continueIf(Condition, BlockEdge otherwise);
    // The 8-bit side falls through.
    template<typename T> T selectByWidth(Condition is8Bit, NOESCAPE const Invocable<T(bool is8Bit)> auto& read, Machine::EdgeClass frequencyOfWide = Machine::EdgeClass::Unlikely);

    Word32 word32Of(ValueID);
    String textOf(StringID) const;

    Word32 lengthOf(ValueID string);
    Word32 lengthOf(LiteralStringWord, const Type&);
    Word32 lengthOf(CellPointer string);
    CharacterView viewOf(const OperationContext&, ValueID string);
    CharacterView viewOfFlatString(CellPointer flat);
    // Of what FlattenString has made. With an answer, whoever reads the string until forgetStringsFoundNarrow() reads bytes: the caller branches on the answer and generates nothing where it may have failed.
    std::optional<Condition> findNarrow(ValueID flatString);
    void forgetStringsFoundNarrow() { m_flagsFoundNarrow.shrink(0); }
    // The result of FlattenString.
    std::optional<FlatStringCell> flatStringByType(ValueID string);
    Pointer implOfFlatString(FlatStringCell);
    CharacterView viewOfString(const OperationContext&, CellPointer string);

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
    // `lengthOfSubject`: how long the subject is, and -1 for a subject that is no string, where somebody has read that already.
    Condition equalsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, std::optional<FlatStringCell> flatSubject = std::nullopt, bool everyCellIsString = false, std::optional<Word32> lengthOfSubject = std::nullopt);
    void branchOnEqualsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets, std::optional<FlatStringCell> flatSubject = std::nullopt, bool everyCellIsString = false, std::optional<Word32> lengthOfSubject = std::nullopt);
    // Where the atom of a name of the image lies. In an image that may lie anywhere the atom is made on demand: `exists` says whether it has been.
    struct AtomOfName {
        Pointer address;
        std::optional<Condition> exists;
    };
    // `mentionOfName` is what m_mentions.add() gave for NumberSpace::Name or NameOfText.
    AtomOfName atomOfName(uint32_t mentionOfName);
    // For a subject that the comparison in place cannot answer: a rope, a 16-bit string.
    void branchOnEqualsLiteralByStub(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets);
    // Whatever cell the image makes of the text holds an atom, so a comparison with another text tells it apart by its address.
    void makeCellsHoldAtomOf(StringID);
    // The view has as many units behind the offset as the text has, and canCompareInline() holds of the text.
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
    static constexpr unsigned maxLengthComparedInPlace = maxLengthOfTextComparedInPlace;

    Word32 flagsInImpl(Pointer impl);
    CharacterSource sourceInImpl(Pointer impl);
    CharacterSource sourceOfRope(const OperationContext&, CellPointer rope, Pointer fiber);
    CharacterView viewOfSource(const CharacterSource&, CellPointer owner);
    CellPointer substringBaseOf(CellPointer rope);
    Word32 loadCodeUnit(Pointer characters, Word32 index, bool is8Bit);

    void generateEqualsLiteral(const ValueContext&, ComparedOperand, LValue subject, std::optional<FlatStringCell> flatSubject, bool everyCellIsString = false);
    void generateEndsLiteral(const ValueContext&, LiteralEnd);
    void generateFindLiteral(const ValueContext&, SearchDirection);
    bool comparesLiteralInPlace(const String& text) const;
    bool readsCharactersInLine(const OperationContext&);
    // Leaves Output where the pointer does not answer: with a cell that is not known to hold an atom, or while the text has none.
    CellPointer branchOnAtomOfLiteral(ComparedOperand, LValue subject, StringID, BranchTargets, bool isString, Machine::EdgeClass frequencyWithoutAtom);
    void branchOnBytesAt(Pointer address, std::span<const uint8_t> bytes, const Machine::AbstractHeap&, CellPointer owner, BranchTargets);
    Integer truncateToInteger(Number position);
    StringPosition truncatedPositionOf(Number position, StringRangeMethod);
    // Of a position that arrives as an integer or as a double.
    StringPosition truncatedPositionOf(ValueID, StringRangeMethod);
    Word32 clampToLength(const StringPosition&, Word32 length, bool isRelativeToEnd);

    struct OpenedString {
        Pointer impl;
        CharacterSource source;
    };
    // What FlattenString has read where it made the flat string, which is in front of every reader of it.
    UncheckedKeyHashMap<LValue, OpenedString> m_openedStrings;
    // The flags of strings that a test in front of the code being generated has found to be of 8 bits.
    Vector<LValue, 2> m_flagsFoundNarrow;
};

template<typename T>
T StringEmitter::selectByWidth(Condition is8Bit, NOESCAPE const Invocable<T(bool is8Bit)> auto& read, Machine::EdgeClass frequencyOfWide)
{
    if (is8Bit.value()->hasInt())
        return read(is8Bit.value()->asInt());

    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    Join<T> join(m_out);
    m_out.branch(is8Bit.value(), normalEdge(narrow), BlockEdge(wide, frequencyOfWide));
    m_out.appendTo(narrow);
    join.add(read(true));
    m_out.appendTo(wide);
    join.add(read(false));
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
