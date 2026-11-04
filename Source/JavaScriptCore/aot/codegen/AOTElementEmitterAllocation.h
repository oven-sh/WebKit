/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTOperationEmitter.h"
#include "AOTStubsArrays.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class ArrayAllocationEmitter final : public OperationEmitter {
public:
    explicit ArrayAllocationEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    const ArrayStorageForm& formAtAllocation(const Type& array) const;
    const ArrayStorageForm& formOfLiteral(const OperationContext&, std::span<const ValueID> elements, const Type& array, bool hasHoles = false);

    CellPointer newLiteral(const ValueContext&);
    // The one user of such a value is a literal that computes it behind its allocation, where the value goes straight into the array: no register holds it across the call.
    bool isComputedBehindAllocation(ValueID);
    CellPointer newArrayOf(const OperationContext&, std::span<const ValueID> elements, const Type& array);
    CellPointer newArrayOfNumbers(const OperationContext&, std::span<const Number>);
    CellPointer newArrayOfHoles(const OperationContext&, Integer length, const ArrayStorageForm&, BuiltinID calledBuiltin);
    // `[...source]`: the AppendElements that comes right behind an empty literal with storage for any value and a planned capacity. The literal is then made with those elements,
    // by the code of the literal, and the append has no code.
    std::optional<ValueID> appendThatFillsLiteral(BlockID, ValueID literal);
    std::optional<ValueID> membersThatFillLiteral(BlockID, ValueID literal);
    // Cleared: every element is a hole. Not cleared: whoever asks fills all of them before anything else runs, so nobody sees what lies there.
    enum class ElementsBelowLength : bool { Cleared, NotCleared };
    enum class LengthIsSetBy : bool { Allocation, Caller };
    CellPointer allocateArrayOfHoles(const OperationContext&, Integer length, const ArrayStorageForm&, LBasicBlock slowPath, ElementsBelowLength = ElementsBelowLength::Cleared, LengthIsSetBy = LengthIsSetBy::Allocation);
    void setLengthOfNewArray(CellPointer, Integer length);

    CellPointer allocateEmptyArrayWithStorage(const OperationContext&, Integer header, Integer capacity, LBasicBlock slowPath);
    CellPointer allocateArrayCell(Integer header, Pointer butterfly, LBasicBlock slowPath);
    Integer headerOfNewArray(const ArrayStorageForm&);

private:
    using RawElements = Vector<Operand, 16>;

    CellPointer newLiteralWithHoles(const ValueContext&);
    CellPointer newLiteralWithCapacity(const ValueContext&);
    CellPointer newLiteralAtSite(const ValueContext&, StaticsOffset);
    const ArrayStorageForm& formOfEmptyLiteral(ValueID literal, const Type& array);
    CellPointer newArrayOutOfLine(const OperationContext&, std::span<const ValueID> elements, const ArrayStorageForm&);
    const ArrayStorageForm* findFormOfLiteral(std::span<const ValueID> elements, const Type& array);
    bool storesElementsBehindAllocation(std::span<const ValueID> elements, const ArrayStorageForm&);
    CellPointer newArrayStoredBehindAllocation(const OperationContext&, std::span<const ValueID> elements, const ArrayStorageForm&);
    CellPointer newDoubleLiteral(const OperationContext&, std::span<const Number> numbersOrMarkers, uint32_t isNumberMask);
    CellPointer callNewEmptyArray(const OperationContext&, EmptyArray);
    CellPointer callNewArrayWithLength(const OperationContext&, const ArrayStorageForm&, Integer lengthAndCapacity);

    RawElements rawElementsOf(std::span<const ValueID> elements, const ArrayStorageForm&, LBasicBlock doesNotFit);
    void storeElements(CellPointer newArray, const ArrayStorageForm&, std::span<const Operand> rawElements, std::span<const uint32_t> indexes);
    Number numberOrMarker(ValueID);
    bool isNumberOrMarker(ValueID);
};

static_assert(sizeof(ArrayAllocationEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
