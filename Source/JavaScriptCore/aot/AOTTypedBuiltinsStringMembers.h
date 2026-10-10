/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    void branchOnLatin1WhiteSpace(Word32 character, LBasicBlock isWhiteSpace, LBasicBlock isNot);
    std::pair<Integer, Integer> trimmedRange(const Characters&, bool trimsStart, bool trimsEnd);
    Word32 hasStringAt(const Characters&, Integer place, const Characters& other);
    Word32 lowerStartsOrEndsWith(bool isAtEnd, const StringReceiver&, ValueID other, std::optional<ValueID> position);
    Word32 lowerStringSearch(InlineBuiltin, const StringReceiver&, const Arguments&);
    Operand lowerStringBuiltin(InlineBuiltin, ValueID result, const Value&);
    // By the value a string is defined by: its length, and the block that read it. It holds wherever that block dominates.
    struct LengthOfString {
        BlockID block;
        LValue length;
    };
    UncheckedKeyHashMap<uint32_t, Vector<LengthOfString, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lengthsOfStrings;
    ValueID stringDefinedBy(ValueID) const;
    std::optional<Integer> lengthReadBefore(ValueID string) const;
    void didReadLength(ValueID string, Integer);
    // By the last Add or Strcat of a concatenation that has numbers among its parts and is looked up by: the parts. See findConcatenationsWithNumbers().
    using PartsOfConcatenation = Vector<ValueID, 8>;
    UncheckedKeyHashMap<uint32_t, PartsOfConcatenation, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_partsOfConcatenations;
    bool isConcatenation(const Value&) const;
    bool collectPartsOfConcatenation(const BitVector& isInBlock, ValueID, bool isLast, PartsOfConcatenation& parts, PartsOfConcatenation& lookedThrough);
    void findConcatenationsWithNumbers();
    BitVector valuesUsedAsKeys() const;
    std::optional<Operand> lowerAsPartOfConcatenation(ValueID, const Value&);
    CellPointer concatenateInOnePiece(const PartsOfConcatenation&);
    CellPointer concatenateAsRopes(std::span<const ValueID> parts);
    CellPointer concatenateWhereNeeded(const Value&);
    bool isNamedLastIndex(StringID) const;
    // Goes on where the value is a RegExp object, which it yields.
    CellPointer regExpObjectOrElse(Boxed, LBasicBlock otherwise);
    Boxed getLastIndex(Boxed base, StringID name, bool isCalled);
    void putLastIndex(Boxed base, StringID name, Boxed value);
    void emitCachedPutById(Boxed base, StringID name, Boxed value);
    // A string that is defined outside a loop and read in it: what its StringImpl had when the loop was entered. See findStringsReadInLoops().
    struct OpenedString {
        ValueID string;
        B3::Variable* data;
        B3::Variable* length;
        B3::Variable* flags;
        // The loop reads its characters. Else only its length, and only that is there: a rope is not made flat to be asked how long it is, and a function that asks no more changes nothing.
        bool isOpened { false };
    };
    // By loop, as m_loops.
    Vector<Vector<OpenedString, 2>> m_stringsOpenedInFrontOfLoops;
    void findStringsReadInLoops();
    void openStringsInFrontOfLoopsEnteredBy(const Terminator&);
    const OpenedString* openedString(ValueID) const;
    std::optional<Characters> charactersOfOpenedString(const StringReceiver&);
    // `s === "text"` that nothing uses but the Branch of its block: see lowerBranchOnText().
    struct ComparisonWithText {
        ValueID subject;
        ValueID constant;
        std::span<const uint8_t> text;
    };
    std::optional<ComparisonWithText> comparisonWithTextBranchedOn(const Block&);
    std::optional<Operand> deferToBranchOnText(ValueID, const Value&);
    // Outside loops: see Stub::StringIsText.
    static constexpr size_t maxLengthOfTextComparedAtEverySite = 32;
    Word32 isTextOfLengthByStub(const StringReceiver&, ValueID constant, std::span<const uint8_t> text);
    Word32 isTextByStub(CellPointer string, ValueID constant);
    Characters charactersOfStringOfLength(const StringReceiver&, size_t length, LBasicBlock isNotAsLong, LBasicBlock otherwise);
    void lowerBranchOnText();
    unsigned numberOfPredecessorsOf(BlockID);
    Vector<unsigned> m_numberOfPredecessors;
    BitVector m_blocksTakenIntoBranchOnText;
    bool looksForConstantWithoutItsCell(const Value&) const;
    // By the IteratorOpen of a string that stands for its own iterator: see findIterationsOfStrings().
    struct IterationOfString {
        // Where the iteration is, in code units.
        B3::Variable* position;
        // Of the StringImpl the string had when the iteration was opened.
        B3::Variable* data;
        B3::Variable* length;
        B3::Variable* flags;
        // The type says that it is a string. Otherwise it is expected to be one, and the iterator is looked at.
        bool isCertain;
        // In a function with handlers: four words on the stack that hold the same, for restoreIterationsOfStrings().
        LValue kept;
    };
    void setAndKeep(const IterationOfString&, B3::Variable*, LValue);
    void restoreIterationsOfStrings();
    UncheckedKeyHashMap<uint32_t, IterationOfString, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_iterationsOfStrings;
    void findIterationsOfStrings();
    const IterationOfString* iterationOfString(ValueID iterator) const;
    Word32 isStringStandingForItsIterator(ValueID iterator);
    Integer positionIn(const IterationOfString&);
    void setPositionIn(const IterationOfString&, Integer);
    Characters charactersOfIteratedString(const IterationOfString&, CellPointer);
    std::optional<Operand> castOfIteratedCharacter(const Value&);
    bool isKeptByNothing(ValueID literal, bool isGlobal);
    // `for (const [i, x] of array.entries())`: see findEntriesOfArrays().
    struct PairOfEntries {
        // The IteratorOpen of the loop, and the IteratorValue that stands for the pair.
        ValueID iterator;
        ValueID pair;
    };
    using PairsOfEntries = UncheckedKeyHashMap<uint32_t, PairOfEntries, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    // By the call of `entries`; by the IteratorOpen of the loop.
    BitVector m_entriesMadeAsRecords;
    BitVector m_iterationsOfEntries;
    // By the IteratorValue of the loop, by the casts of that, by the IteratorOpen of the pattern.
    PairsOfEntries m_pairsOfEntries;
    void findEntriesOfArrays();
    TypedPointer wordOfIterationRecord(CellPointer record, unsigned word);
    Word32 iteratesEntriesOfArray(ValueID iterator);
    CellPointer arrayOfEntries(ValueID iterator);
    std::optional<Operand> partOfEntriesOfArray(ValueID, const Value&);
    Operand openIterationOfString(const IterationOfString&, ValueID result, ValueID iterable);
    Word32 iteratedStringHasNext(const IterationOfString&, ValueID iterator);
    CellPointer nextOfIteratedString(const IterationOfString&, ValueID iterator, Word32& codePoint);
    // By the value: a string of one character that was made of a code unit, or what an access outside the string yields: the B3 value it stands for, and the code unit or the marker.
    struct CodeUnitOfString {
        LValue string;
        LValue codeUnit;
    };
    static constexpr int64_t codeUnitOfUndefined = INT64_MIN;
    static constexpr int64_t codeUnitOfEmptyString = INT64_MIN + 1;
    UncheckedKeyHashMap<uint32_t, CodeUnitOfString, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_codeUnitsOfStrings;
    bool keepsCodeUnitsOfStrings() const { return m_catchSpills.isEmpty(); }
    void didMakeStringOfCodeUnit(ValueID, Operand string, const Vector<ValueFromBlock, 2>& codeUnits);
    Operand valueOfArrayOrRecord(ValueID result, const Value&);
    std::optional<Integer> codeUnitOfString(ValueID);
