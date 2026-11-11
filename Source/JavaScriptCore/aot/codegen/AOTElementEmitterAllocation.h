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
    bool isLeftToSpreadRightBehind(BlockID, ValueID literal, const Operation&, std::span<const ValueID> behind);
    void bearLiteralHoldingList(const ValueContext& spread, LValue iterable, StubKey appendsAsWritten);
    CellPointer newArrayOf(const OperationContext&, std::span<const ValueID> elements, const Type& array);
    CellPointer newArrayOfNumbers(const OperationContext&, std::span<const Number>);
    CellPointer newArrayOfHoles(const OperationContext&, Integer length, const ArrayStorageForm&, BuiltinID calledBuiltin);
    enum class ElementsBelowLength : bool { Cleared, NotCleared };
    enum class LengthIsSetBy : bool { Allocation, Caller };
    CellPointer allocateArrayOfHoles(const OperationContext&, Integer length, const ArrayStorageForm&, LBasicBlock slowPath, ElementsBelowLength = ElementsBelowLength::Cleared, LengthIsSetBy = LengthIsSetBy::Allocation);
    std::optional<Pointer> profileOfListsOfLengthAtSite(Origin, const ArrayStorageForm& seed);
    CellPointer allocateArrayOfHolesAtSite(const ValueContext&, Integer length, const ArrayStorageForm& seed, Pointer profile, LBasicBlock slowPath);
    CellPointer bearArrayOfLength(const OperationContext&, const ArrayStorageForm&, Integer length);
    CellPointer allocateEmptyArrayWithRoom(const OperationContext&, Integer room, const ArrayStorageForm&, LBasicBlock slowPath);
    void allocateListLike(const OperationContext&, CellPointer source, Integer length, std::optional<Integer> roomOfEmptyList, LBasicBlock slowPath, const ScopedLambda<void(CellPointer)>&, const ArrayStorageForm* inPlaceOfAnyValue = nullptr);
    void setLengthOfNewArray(CellPointer, Integer length);

    CellPointer allocateArrayCell(Integer header, Pointer butterfly, LBasicBlock slowPath);

private:
    using RawElements = Vector<Operand, 16>;

    CellPointer newLiteralWithHoles(const ValueContext&);
    std::optional<Integer> takeRoomAskedRightBehind(const ValueContext&);
    ArrayStorage storageThatLiteralNames(ValueID literal, const Operation&);
    struct AppendedBehindSpread {
        uint32_t room { 0 };
        bool holdsWhatIsNoNumber { false };
    };
    std::optional<AppendedBehindSpread> findWhatIsAppendedBehindSpread(BlockID, ValueID literal, ValueID spread);
    CellPointer newLiteralWithRoom(const OperationContext&, std::span<const ValueID> elements, const ArrayStorageForm&, Integer room);
    CellPointer newLiteralAtSite(const ValueContext&, const ArrayStorageForm& seed, std::span<const uint32_t> site);
    CellPointer newArrayInForm(const OperationContext&, std::span<const ValueID> elements, const ArrayStorageForm&);
    CellPointer newArrayOutOfLine(const OperationContext&, std::span<const ValueID> elements, const ArrayStorageForm&);
    const ArrayStorageForm& formOfLiteral(const OperationContext&, std::span<const ValueID> elements, const Type& array, ArrayStorage named, const ArrayStorage* declaredByArray = nullptr);
    const ArrayStorageForm* findFormOfLiteral(std::span<const ValueID> elements, const Type& array, ArrayStorage named, const ArrayStorage* declaredByArray);
    CellPointer newDoubleLiteral(const OperationContext&, std::span<const Number> numbersOrMarkers, uint32_t isNumberMask);
    CellPointer callNewEmptyArray(const OperationContext&, EmptyArray);
    CellPointer callNewArrayWithLength(const OperationContext&, const ArrayStorageForm&, Integer lengthAndCapacity);

    RawElements rawElementsOf(std::span<const ValueID> elements, const ArrayStorageForm&, LBasicBlock doesNotFit);
    void storeElements(const OperationContext&, CellPointer newArray, const ArrayStorageForm&, std::span<const Operand> rawElements, std::span<const uint32_t> indexes);
    Number numberOrMarker(ValueID);
    bool isNumberOrMarker(ValueID);
};

static_assert(sizeof(ArrayAllocationEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
