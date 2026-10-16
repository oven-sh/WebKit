/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    IndexingType indexingTypeAtBirth(const Type& array) const;
    static int64_t holeIn(IndexingType);
    const B3::AbstractHeap& elementsOf(IndexingType);
    LValue allocateStorage(LValue size, LBasicBlock slowPath);
    CellPointer allocateArrayWithHoles(Integer length, IndexingType, LBasicBlock slowPath, bool isInTheMaking = false);
    IndexingType indexingTypeOfLiteral(const Arguments& elements, const Type&) const;
    LValue allocateArrayOfNumbers(IndexingType, LValue lengthAndCapacity, LBasicBlock slowPath);
    CellPointer newArrayOfType(const Arguments& elements, const Type&);
    CellPointer newArrayLiteral(ValueID);
    CellPointer newArrayLiteralWithAllocationSite(ValueID);
    bool isAvailableAtCurrentValue(ValueID) const;
    struct CountedLoop {
        unsigned index;
        ValueID counter;
        size_t position;
        ValueID bound;
        ValueID iteratedArray;
    };
    std::optional<CountedLoop> countedLoopContaining(BlockID) const;
    Integer arrayLengthOrBail(ValueID array, LBasicBlock otherwise);
    struct InitialCapacity {
        Vector<ValueID, 2> arrays;
        ValueID count;
        int64_t constant { 0 };
    };
    std::optional<InitialCapacity> inferInitialCapacity(ValueID literal);
    bool m_didFindArrayPushes { false };
    UncheckedKeyHashMap<uint32_t, Vector<std::pair<BlockID, unsigned>, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_arrayPushesByReceiver;
    UncheckedKeyHashMap<uint32_t, Vector<std::pair<BlockID, ValueID>, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_arrayStoresByReceiver;
    BitVector m_blocksWithAppendSpread;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_spreadsCarriedOutAtBirth;
    bool m_isFillingArrayAtBirth { false };
    void updateVectorLengthHint(CellPointer array, ValueID receiver);
    Type arrayTypeAtBirth(ValueID) const;
    ValueID emptyLiteralBehind(ValueID receiver);
    UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_emptyLiteralsBehindParameters;
    Word32 turnsLeftInLoop();
    LValue whatIsExpectedOfArrayStoredTo(ValueID receiver, ValueID key, bool isNumberAmongNumbers);
    std::optional<Word32> mostPushedInLoopOverCollection(ValueID literal);
    unsigned numberOfElementsPushedTo(ValueID array);
    LBasicBlock appendSpread(const Value&);
    LBasicBlock appendSpreadOfTypedArray(const Value&);
    Boxed callArrayMethodOnSlowPath(const Value&, CellPointer array);
    Operand lowerArrayConcat(const Value&, const Type&);
    Operand lowerArrayFromTypedArray(ValueID result, const Value&, const Type&);
    Operand lowerArrayShift(const Value&, const Type&);
    void addIfNumber(Join&, Boxed element, const Type&);
    Operand elementOfDoubles(Number, const Type&);
    Operand lowerArrayAt(const Value&, const Type&);
    Operand lowerArrayJoin(const Value&, const Type&, ValueID separator);
    Operand lowerArraySlice(const Value&, const Type&);
    Operand lowerArraySplice(ValueID result, const Value&, const Type&);
    Integer indexOfNumber(CellPointer array, Number);
    std::optional<Operand> lowerArraySearchAsOneCall(ValueID result, const Value&);
