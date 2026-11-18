/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTStubsStringLiterals.h"
#include "AOTStubsStrings.h"
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

struct CharacterView {
    Pointer characters;
    Word32 length;
    Condition is8Bit;
    CellPointer owner;
    ValueID possibleRope { };
    const OperationContext* resolvesAt { nullptr };
    Word32 implFlags { };
};

struct CharacterSource {
    Pointer characters;
    Word32 length;
    Word32 implFlags;
};

struct OwnedCharacters {
    CharacterSource source;
    CellPointer owner;
    Word32 start;
};

struct SubstringBounds {
    Word32 start;
    Word32 length;
};

struct CodeUnitRange {
    Word32 start;
    Word32 end;
};

struct StringPosition {
    Integer value;
    Range range { Range::full() };
    std::optional<Number> numberBehindBareConversion { };
};

enum class LiteralEnd : uint8_t { Start, End };
enum class SearchDirection : uint8_t { Forward, Backward };

uint64_t packStringIntoRegister(StringView);
LValue loadBytes(Output&, unsigned width, TypedPointer);

class StringEmitter;

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
    void generateBoundsOfStringSplit(const ValueContext&);
    void generateNewStringSplitArray(const ValueContext&);
    void generateFlattenString(const ValueContext&);
    void generateTryResolveRope(const ValueContext&);
    void generateOpenString(const ValueContext&);
    void generateStringRange(const ValueContext&);
    void generateStringOrder(const ValueContext&);
    void generateWordOfText(const ValueContext&);
    void generateReverseWordOfText(const ValueContext&);
    void generateHasTextInCell(const ValueContext&);
    void generateLocaleCompareToWord(const ValueContext&);
    void generateStringOrderToCharacter(const ValueContext&);
    void generateStringEqualsLiteral(const ValueContext&);
    void generateValueEqualsLiteral(const ValueContext&);
    void generateStringStartsWithLiteral(const ValueContext& value) { generateEndsLiteral(value, LiteralEnd::Start); }
    void generateStringEndsWithLiteral(const ValueContext& value) { generateEndsLiteral(value, LiteralEnd::End); }
    void generateStringIndexOfLiteral(const ValueContext& value) { generateFindLiteral(value, SearchDirection::Forward); }
    void generateStringIndexOfCharacter(const ValueContext&);
    void generateStringLastIndexOfLiteral(const ValueContext& value) { generateFindLiteral(value, SearchDirection::Backward); }
    void generateStringRangeEqualsLiteral(const ValueContext&);
    void generateStringRangeToNumber(const ValueContext&);
    void generateStringRangeParseInt(const ValueContext&);
    void generateFindStringRangeInMap(const ValueContext&);
    CharacterView viewOfRange(const ValueContext&);
    void holdRangeInside(const ValueContext&, Word32 start, Word32 end);

    void continueIf(Condition, BlockEdge otherwise);
    template<typename T> T selectByWidth(Condition is8Bit, NOESCAPE const Invocable<T(bool is8Bit)> auto& read, Machine::EdgeClass frequencyOfWide = Machine::EdgeClass::Unlikely);

    Word32 word32Of(ValueID);
    String textOf(StringID) const;
    void recordInlined(StringID);
    void recordInlined(char16_t codeUnit);

    Word32 lengthOf(ValueID string);
    void addOrderByFirstDifference(ValueID left, ValueID right, LBasicBlock byCall, Join<Word32>& order);
    Word32 lengthOf(CellPointer string);
    CharacterView viewOf(const OperationContext&, ValueID string);
    CharacterView viewOfFlatString(CellPointer flat);
    bool widthIsFoundAtRun(ValueID flatString);
    std::optional<Condition> findWidth(ValueID flatString, bool isNarrow);
    void forgetWidthsFound()
    {
        m_flagsFoundNarrow.shrink(0);
        m_flagsFoundWide.shrink(0);
    }
    std::optional<FlatStringCell> flatStringByType(ValueID string);
    Pointer implOfFlatString(FlatStringCell);
    CharacterView viewOfString(const OperationContext&, CellPointer string);
    CharacterView viewWithoutResolving(const OperationContext&, CellPointer string, BlockEdge otherwise);
    CharacterView viewOfCharacters(const OwnedCharacters& characters) { return viewOfSource(characters.source, characters.owner); }
    OwnedCharacters loadCharacters(const OperationContext&, CellPointer string, std::optional<SubstringBounds> = std::nullopt);
    CellPointer makeSubstring(const OperationContext&, const OwnedCharacters&);
    CellPointer makeCarriedSubstring(const ValueContext& cut, const CodeGenerator::CarriedSubstring&);

    Word32 codeUnitAt(const CharacterView&, Word32 index, std::optional<Integer> indexInWholeNumber = std::nullopt);
    Word32 codeUnitOfPossibleRope(const CharacterView&, Word32 index);
    CharacterView viewForReadOfCodeUnit(const OperationContext&, ValueID string);
    void passOpenedRope(ValueID string, BlockID from, LValue characters, LValue implFlags);
    Word32 codePointAt(const CharacterView&, Word32 index);
    CellPointer stringFromCodeUnit(const OperationContext&, Word32 codeUnit);
    CellPointer substring(const OperationContext&, CellPointer string, CodeUnitRange);
    CodeUnitRange clampRange(StringRangeMethod, Word32 length, StringPosition first, std::optional<StringPosition> second);
    CodeUnitRange clampRange(StringRangeMethod, Word32 length, Number first, std::optional<Number> second);
    Integer indexInString(Integer position, Word32 length, bool isRelativeToEnd);
    Integer indexInString(Number position, Word32 length, bool isRelativeToEnd);

    enum class StringIdentity : bool { Unknown, Distinct };
    Condition areEqual(const OperationContext&, CellPointer, CellPointer, StringIdentity = StringIdentity::Unknown);
    void branchOnEqualityOfDistinctStrings(const OperationContext&, CellPointer a, CellPointer b, BranchTargets);
    Word32 order(const OperationContext&, CellPointer, CellPointer);
    Word32 orderToCharacter(const CharacterView&, char16_t codeUnit);
    Condition equalsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, std::optional<FlatStringCell> flatSubject = std::nullopt, bool everyCellIsString = false, std::optional<Word32> lengthOfSubject = std::nullopt);
    void branchOnEqualsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets, std::optional<FlatStringCell> flatSubject = std::nullopt, bool everyCellIsString = false, std::optional<Word32> lengthOfSubject = std::nullopt);
    void branchOnEqualsLiteralByStub(const OperationContext&, ComparedOperand, LValue subject, StringID, BranchTargets);
    bool mayBeNameInLoop(const OperationContext&, const String& text);
    Condition holdsHeaderOf(CellPointer, StringID);
    void branchOnHeaderOf(CellPointer, StringID, BlockEdge isEqual);
    Condition holdsName(CellPointer);
    void makeCellsHoldAtomOf(StringID);
    void makeCellsHoldAtomOf(StringID, const String& text);
    void branchOnTextAt(const CharacterView&, Word32 offset, StringID, BranchTargets);
    bool canCompareInline(StringID) const;
    static bool canCompareInline(const String& text) { return text.length() <= maxLengthComparedInPlace; }

    Word32 find(const CharacterView& subject, const CharacterView& sought, Word32 position, SearchDirection);
    Word32 findCodeUnit(const CharacterView& subject, Word32 codeUnit, Word32 position, SearchDirection);

    struct StringPart {
        StringPart(CellPointer cell)
            : cell(cell)
        {
        }
        StringPart(StringID constant)
            : constant(constant)
        {
        }
        static StringPart toConvert(Boxed value, ConversionInConcatenation conversion)
        {
            StringPart part { StringID() };
            part.unconverted = value;
            part.conversion = conversion;
            return part;
        }
        static StringPart ofInteger(Integer value)
        {
            StringPart part { StringID() };
            part.integer = value;
            return part;
        }
        bool isConstant() const { return !cell && !unconverted && !integer; }

        std::optional<CellPointer> cell { };
        std::optional<Boxed> unconverted { };
        ConversionInConcatenation conversion { ConversionInConcatenation::ToString };
        std::optional<Integer> integer { };
        StringID constant { };
    };
    static std::optional<ConstantsOfConcatenation> chooseConstants(std::span<const StringPart>);
    StringID findNumberedStringConstant(const OperationContext&, ValueID);
    CellPointer concatenate(const OperationContext&, std::span<const StringPart>);
    CellPointer concatenate(const OperationContext& context, std::initializer_list<StringPart> parts) { return concatenate(context, std::span<const StringPart>(parts.begin(), parts.size())); }
    enum class KeyUse : uint8_t { AnyKey, NameOfProperty };
    CellPointer concatenateKey(const OperationContext&, std::span<const Boxed> primitives, KeyUse);
    CellPointer integerToString(const OperationContext& context, Word32 value) { return integerToString(context, signExtended(value)); }
    CellPointer integerToString(const OperationContext&, Integer);
    CellPointer numberToString(const OperationContext&, Number);

private:
    static constexpr unsigned maxLengthComparedInPlace = maxLengthOfTextComparedInPlace;

    CellPointer cellOf(const OperationContext&, const StringPart&);
    std::optional<CellPointer> tryConcatenateInteger(const OperationContext&, std::span<const StringPart>);
    std::optional<CellPointer> tryConcatenateIntegers(const OperationContext&, std::span<const StringPart>);
    CellPointer concatenateTwoOrThree(const OperationContext&, std::span<const StringPart>);
    Word32 flagsInImpl(Pointer impl);
    struct ShortText {
        Integer word;
        Word32 length;
    };
    ShortText shortTextOf(CellPointer string, BlockEdge otherwise);
    CharacterSource sourceInImpl(Pointer impl);
    CharacterSource sourceOfRope(const OperationContext&, CellPointer rope);
    CharacterView viewOfSource(const CharacterSource&, CellPointer owner);
    Word32 loadCodeUnit(Pointer characters, Word32 index, bool is8Bit, std::optional<Integer> indexInWholeNumber = std::nullopt);

    void branchOnEqualsLiteral(const OperationContext&, ComparedOperand, LValue subject, StringID, const String& text, BranchTargets, std::optional<FlatStringCell> flatSubject, bool everyCellIsString, std::optional<Word32> lengthOfSubject);
    void branchOnEqualsLiteralByStub(const OperationContext&, ComparedOperand, LValue subject, StringID, const String& text, BranchTargets);
    bool expectsName(const OperationContext&, const String& text);
    void continueUnlessHoldsName(CellPointer, StringID, BranchTargets, bool isString);
    void generateEqualsLiteral(const ValueContext&, ComparedOperand, LValue subject, std::optional<FlatStringCell> flatSubject, bool everyCellIsString = false);
    void generateEndsLiteral(const ValueContext&, LiteralEnd);
    void generateFindLiteral(const ValueContext&, SearchDirection);
    bool comparesLiteralInPlace(const String& text) const;
    bool readsCharactersInLine(const OperationContext&);
    void branchOnBytesAt(Pointer address, std::span<const uint8_t> bytes, const Machine::AbstractHeap&, CellPointer owner, BranchTargets);
    Integer truncateToInteger(Number position);
    StringPosition truncatedPositionOf(Number position, StringRangeMethod);
    StringPosition truncatedPositionOf(ValueID, StringRangeMethod);
    Word32 clampToLength(const StringPosition&, Word32 length, bool isRelativeToEnd);

    struct OpenedString {
        Pointer impl;
        CharacterSource source;
        bool mayBeRope { false };
    };
    struct OpenedRope {
        ValueID string;
        BlockID block;
        Pointer characters;
        Word32 implFlags;
        bool isRead { false };
    };
    const OpenedRope& openedRopeAt(ValueID string, BlockID) const;
    CellPointer openPossibleRope(const ValueContext&, CellPointer string);
    CharacterView viewOfPossibleRope(ValueID string, BlockID, CellPointer);
    Vector<OpenedRope, 4> m_openedRopes;
    UncheckedKeyHashMap<LValue, OpenedString> m_openedStrings;
    Vector<LValue, 2> m_flagsFoundNarrow;
    Vector<LValue, 2> m_flagsFoundWide;
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
