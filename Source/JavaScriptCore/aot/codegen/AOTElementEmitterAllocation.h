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
    const ArrayStorageForm& formOfLiteral(const OperationContext&, std::span<const ValueID> elements, const Type& array);

    CellPointer newLiteral(const ValueContext&);
    // NewArrayOfElements.
    CellPointer newLiteralOfElements(const ValueContext&);
    CellPointer newArrayOf(const OperationContext&, std::span<const ValueID> elements, const Type& array);
    // At most maxArrayElementsInRegisters, none of them NaN.
    CellPointer newArrayOfNumbers(const OperationContext&, std::span<const Number>);
    CellPointer newArrayOfHoles(const OperationContext&, Integer length, const ArrayStorageForm&, BuiltinID calledBuiltin);
    // Cleared: every element is a hole. Not cleared: whoever asks fills all of them before anything else runs, so nobody sees what lies there.
    enum class ElementsBelowLength : bool { Cleared, NotCleared };
    enum class LengthIsSetBy : bool { Allocation, Caller };
    CellPointer allocateArrayOfHoles(const OperationContext&, Integer length, const ArrayStorageForm&, LBasicBlock slowPath, ElementsBelowLength = ElementsBelowLength::Cleared, LengthIsSetBy = LengthIsSetBy::Allocation);
    // Without an element, and with room for `room` of them, which is no more than an empty list has anyway.
    CellPointer allocateEmptyArrayWithRoom(const OperationContext&, Integer room, const ArrayStorageForm&, LBasicBlock slowPath);
    void setLengthOfNewArray(CellPointer, Integer length);

    CellPointer allocateArrayCell(Integer header, Pointer butterfly, LBasicBlock slowPath);

private:
    using RawElements = Vector<Operand, 16>;

    CellPointer newLiteralWithHoles(const ValueContext&);
    // An empty array with the room that the lists of the site have come to need. It has the form, or a more general one that those lists have turned into.
    // `site` is a key of NumberSpace::ArrayProfile.
    CellPointer newLiteralAtSite(const ValueContext&, const ArrayStorageForm& seed, std::span<const uint32_t> site);
    CellPointer newArrayInForm(const OperationContext&, std::span<const ValueID> elements, const ArrayStorageForm&);
    CellPointer newArrayOutOfLine(const OperationContext&, std::span<const ValueID> elements, const ArrayStorageForm&);
    // `named` is NewArray::arrayStorage(): the kind that the walk chose, in whose form the elements arrive. The array is born with it unless a NaN arrives among doubles. Other: no kind is named.
    const ArrayStorageForm& formOfLiteral(const OperationContext&, std::span<const ValueID> elements, const Type& array, ArrayStorage named);
    const ArrayStorageForm* findFormOfLiteral(std::span<const ValueID> elements, const Type& array, ArrayStorage named);
    CellPointer newDoubleLiteral(const OperationContext&, std::span<const Number> numbersOrMarkers, uint32_t isNumberMask);
    CellPointer callNewEmptyArray(const OperationContext&, EmptyArray);
    CellPointer callNewArrayWithLength(const OperationContext&, const ArrayStorageForm&, Integer lengthAndCapacity);

    RawElements rawElementsOf(std::span<const ValueID> elements, const ArrayStorageForm&, LBasicBlock doesNotFit);
    // Behind it the code of the operation hands no cell on: the fence behind the stores may be left to a later allocation.
    void storeElements(const OperationContext&, CellPointer newArray, const ArrayStorageForm&, std::span<const Operand> rawElements, std::span<const uint32_t> indexes);
    Number numberOrMarker(ValueID);
    bool isNumberOrMarker(ValueID);
};

static_assert(sizeof(ArrayAllocationEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
