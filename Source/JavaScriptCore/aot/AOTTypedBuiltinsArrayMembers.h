/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    IndexingType indexingTypeAtBirth(const Type& array) const;
    LValue originalArrayStructureID(IndexingType);
    static int64_t holeIn(IndexingType);
    const B3::AbstractHeap& elementsOf(IndexingType);
    LValue allocateStorage(LValue size, LBasicBlock slowPath);
    CellPointer allocateArrayWithHoles(Integer length, IndexingType, LBasicBlock slowPath);
    IndexingType indexingTypeOfLiteral(const Arguments& elements, const Type&) const;
    LValue allocateArrayOfNumbers(IndexingType, LValue lengthAndCapacity, LBasicBlock slowPath);
    CellPointer newArrayOfType(const Arguments& elements, const Type&);
    CellPointer newArrayLiteral(ValueID);
    void updateVectorLengthHint(CellPointer array, ValueID receiver);
    Type arrayTypeAtBirth(ValueID) const;
    ValueID emptyLiteralBehind(ValueID receiver);
    Word32 turnsLeftInLoop();
    unsigned numberOfElementsPushedTo(ValueID array);
    LBasicBlock appendSpread(const Value&);
    Operand lowerArrayConcat(const Value&, const Type&);
    Operand lowerArrayShift(const Value&, const Type&);
    void addIfNumber(Join&, Boxed element, const Type&);
    Operand elementOfDoubles(Number, const Type&);
    Operand lowerArrayAt(const Value&, const Type&);
    Operand lowerArrayJoin(const Value&, const Type&, ValueID separator);
    Operand lowerArraySlice(const Value&, const Type&);
    Operand lowerArraySplice(ValueID result, const Value&, const Type&);
    Integer indexOfNumber(CellPointer array, Number);
