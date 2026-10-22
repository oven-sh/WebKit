/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTOperationEmitter.h"
#include "AOTStubsArrays.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class ArrayAllocationEmitter final : public OperationEmitter {
public:
    explicit ArrayAllocationEmitter(CodeGenerator&);

    const ArrayStorageForm& formAtAllocation(const Type& array) const;
    const ArrayStorageForm& formOfLiteral(std::span<const ValueID> elements, const Type& array) const;

    CellPointer newLiteral(const ValueContext&);
    CellPointer newArrayOf(const OperationContext&, std::span<const ValueID> elements, const Type& array);
    CellPointer newArrayOfNumbers(const OperationContext&, std::span<const Number>);
    CellPointer newArrayOfHoles(const OperationContext&, Integer length, const ArrayStorageForm&);
    CellPointer allocateArrayOfHoles(const OperationContext&, Integer length, const ArrayStorageForm&, LBasicBlock slowPath);

    CellPointer allocateEmptyArrayWithStorage(const OperationContext&, Integer header, Integer capacity, LBasicBlock slowPath);
    void moveStorageOutOfOwnCell(const OperationContext&, CellPointer array);
    CellPointer allocateArrayCell(Integer header, Pointer butterfly, LBasicBlock slowPath);
    Integer headerOfNewArray(const ArrayStorageForm&);
    Integer headerOfNewArrayWithShape(Word32 shape);

private:
    using RawElements = Vector<Operand, 16>;

    CellPointer newLiteralOnItsOwn(const ValueContext&);
    CellPointer newLiteralWithHoles(const ValueContext&);
    CellPointer newLiteralWithCapacity(const ValueContext&);
    CellPointer newLiteralAtSite(const ValueContext&, StaticsOffset);
    const ArrayStorageForm& formOfEmptyLiteral(ValueID literal);
    CellPointer newArrayOutOfLine(const OperationContext&, std::span<const ValueID> elements, const ArrayStorageForm&);
    CellPointer newDoubleLiteral(const OperationContext&, std::span<const Number> numbersOrMarkers, uint32_t isNumberMask);
    CellPointer callNewEmptyArray(const OperationContext&, EmptyArray);
    CellPointer callNewArrayWithLength(const OperationContext&, const ArrayStorageForm&, Integer lengthAndCapacity);

    RawElements rawElementsOf(std::span<const ValueID> elements, const ArrayStorageForm&, LBasicBlock doesNotFit);
    void storeElements(CellPointer newArray, const ArrayStorageForm&, std::span<const Operand> rawElements, std::span<const uint32_t> indexes);
    Number numberOrMarker(ValueID);
};

static_assert(sizeof(ArrayAllocationEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
