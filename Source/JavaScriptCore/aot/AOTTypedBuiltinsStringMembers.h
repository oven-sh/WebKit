/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

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
    struct StackStringBuilder {
        B3::Variable* length;
        B3::Variable* prefix;
        B3::Air::StackSlot* buffer;
        B3::Variable* whole;
        B3::Variable* lengthOfWhole;
    };
    static constexpr unsigned maxStringBuilderInlineDigits = 8;
    StringBuilderAnalysis m_stringBuilderAnalysis;
    Vector<StackStringBuilder> m_stringBuilders;
    void prepareStringBuilders();
    bool isResidentInStringBuilder(ValueID id) const { return m_stringBuilderAnalysis.isState(id) && !m_stringBuilderAnalysis.isFinalState.get(id.index()); }
    const StackStringBuilder& stringBuilderFor(ValueID id) const { return m_stringBuilders[m_stringBuilderAnalysis.builderForValue[id.index()]]; }
    bool stringBuilderMayBeEmpty(ValueID id) const { return m_stringBuilderAnalysis.mayBeEmpty.get(m_stringBuilderAnalysis.builderForValue[id.index()]); }
    Integer stringBuilderLength(const StackStringBuilder&);
    void setStringBuilderLength(const StackStringBuilder&, Integer);
    LValue stringBuilderPrefix(const StackStringBuilder&);
    Pointer stringBuilderBuffer(const StackStringBuilder&);
    void resetStringBuilder(const StackStringBuilder&);
    void flushStringBuilder(const StackStringBuilder&, Boxed operand);
    void flushStringBuilderIfOverCapacity(const StackStringBuilder&);
    void appendConstantToStringBuilder(const StackStringBuilder&, std::span<const uint8_t>);
    void appendNumberToStringBuilder(const StackStringBuilder&, ValueID);
    void appendStringToStringBuilder(const StackStringBuilder&, CellPointer);
    void appendStringOrNilToStringBuilder(const StackStringBuilder&, Boxed);
    void copyLatin1Characters(Pointer destination, Pointer source, Integer length);
    void appendToStringBuilder(const StackStringBuilder&, std::span<const ValueID> operands, bool isFinal);
    CellPointer finishStringBuilder(const StackStringBuilder&, bool mayBeEmpty);
    void initializeStringBuildersOnEdge(const Target&);
    void materializeFinalStringBuilderStates(const Block&);
    std::optional<Operand> tryLowerWithStringBuilder(ValueID, const Value&);
    bool isConcatenation(const Value&) const;
    bool collectPartsOfConcatenation(const BitVector& isInBlock, ValueID, bool isLast, PartsOfConcatenation& parts, PartsOfConcatenation& lookedThrough);
    void findConcatenationsWithNumbers();
    std::optional<Operand> lowerAsPartOfConcatenation(ValueID, const Value&);
    CellPointer concatenateInOnePiece(const PartsOfConcatenation&);
    BitVector m_sunkKeyConcatenations;
    ValueID m_keyBeingMaterialized;
    std::optional<Operand> tryLookupWithSunkKey(ValueID key, const Type&, const ScopedLambda<Boxed(Word32, Pointer)>& ask, const ScopedLambda<Operand()>& slowPath);
    CellPointer concatenateAsRopes(std::span<const ValueID> parts);
    CellPointer concatenateByStub(std::span<const ValueID> parts);
    CellPointer concatenateByStub(CellPointer, CellPointer);
    CellPointer concatenateWhereNeeded(const Value&);
    bool isNamedLastIndex(StringID) const;
    // Goes on where the value is a RegExp object, which it yields.
    CellPointer regExpObjectOrElse(Boxed, LBasicBlock otherwise);
    Boxed getLastIndex(Boxed base, StringID name, bool isCalled);
    void putLastIndex(Boxed base, StringID name, Boxed value);
    void emitCachedPutById(Boxed base, StringID name, Boxed value);
    void emitCachedPutByIdOnItsOwn(Boxed base, StringID name, Boxed value, bool onlyThroughStub = false, unsigned flagsOfSite = 0);
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
    };
    UncheckedKeyHashMap<uint32_t, IterationOfString, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_iterationsOfStrings;
    void findIterationsOfStrings();
    BitVector valuesUsedOtherwiseThanAsIteratorsOfSteps() const;
    UncheckedKeyHashMap<uint32_t, B3::Air::StackSlot*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_iterationsInFrame;
    void findIterationsInFrame();
    bool m_spreadIsLeftToStub { false };
    struct ElementsTakenAtOnce {
        B3::Air::StackSlot* slot;
        unsigned count;
    };
    UncheckedKeyHashMap<uint32_t, ElementsTakenAtOnce, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_elementsTakenAtOnce;
    void findElementsTakenAtOnce();
    const ElementsTakenAtOnce* elementsTakenAtOnce(ValueID iterator) const;
    std::optional<Pointer> recordInFrameOf(ValueID iterator);
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
        bool mayBeNotKept { false };
    };
    static constexpr int64_t codeUnitOfUndefined = INT64_MIN;
    static constexpr int64_t codeUnitOfEmptyString = INT64_MIN + 1;
    static constexpr int64_t codeUnitIsNotKept = INT64_MIN + 2;
    UncheckedKeyHashMap<uint32_t, CodeUnitOfString, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_codeUnitsOfStrings;
    void didMakeStringOfCodeUnit(ValueID, Operand string, const Vector<ValueFromBlock, 2>& codeUnits);
    Operand valueOfArrayOrRecord(ValueID result, const Value&);
    std::optional<Integer> codeUnitOfString(ValueID, bool mayBeNotKept = false);
    ValueID stringConstantOf(ValueID) const;
    std::optional<char16_t> constantCodeUnit(ValueID) const;
    std::optional<Word32> areKeptCodeUnitsEqual(ValueID, ValueID);
