/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ElementAccessEmitter;
struct ArrayElements;

class ArrayIntrinsicEmitter final : public BuiltinFamilyEmitter {
public:
    explicit ArrayIntrinsicEmitter(CodeGenerator& generator) : BuiltinFamilyEmitter(generator) { }

    void generateIsPristineArray(const ValueContext&);
    void generateArrayStorageKind(const ValueContext&);
    void generateHasContiguousIndex(const ValueContext&);
    void generateGetContiguousIndex(const ValueContext&);
    void generateGetOwnContiguousIndex(const ValueContext&);
    void generateGetOwnDoubleIndex(const ValueContext&);
    void generateGetOwnInt32Index(const ValueContext&);
    void generatePutOwnContiguousIndex(const ValueContext&);
    void generatePutOwnDoubleIndex(const ValueContext&);
    void generatePutOwnInt32Index(const ValueContext&);
    void generateGetDoubleIndex(const ValueContext&);
    void generatePutContiguousIndex(const ValueContext& value) { putInBounds(value, arrayStorageFormOf(ArrayStorage::Contiguous)); }
    void generatePutDoubleIndex(const ValueContext& value) { putInBounds(value, arrayStorageFormOf(ArrayStorage::Double)); }
    void generateNewContiguousArray(const ValueContext& value) { newArrayOfForm(value, arrayStorageFormOf(ArrayStorage::Contiguous)); }
    void generateNewDoubleArray(const ValueContext& value) { newArrayOfForm(value, arrayStorageFormOf(ArrayStorage::Double)); }
    void generateNewArrayLike(const ValueContext&);
    void generateNewArrayWithCapacity(const ValueContext&);
    void generateTryPutByValDirect(const ValueContext&);
    void generateTryPutIndex(const ValueContext&);
    void generateTryCopyFilledElements(const ValueContext&);
    void generateTryPutElements(const ValueContext&);
    void generateTryFillNewArray(const ValueContext&);
    void generateGrowNewArray(const ValueContext&);
    void generateTruncateNewArray(const ValueContext&);

private:
    static constexpr const ArrayStorageForm& int32s = arrayStorageFormOf(ArrayStorage::Int32);
    static constexpr const ArrayStorageForm& doubles = arrayStorageFormOf(ArrayStorage::Double);
    static constexpr const ArrayStorageForm& contiguous = arrayStorageFormOf(ArrayStorage::Contiguous);

    ElementAccessEmitter& elements() { return emitter<ElementAccessEmitter>(); }
    Boxed contiguousElementOrEmpty(const ValueContext& access, CellPointer array, Integer index);
    // Null: the read does not stand in the arm of the last HasContiguousIndex that a Branch asked, of the same array and position, with nothing but names in between.
    LValue elementFoundByTestInFront(const ValueContext& read);
    void putInBounds(const ValueContext&, const ArrayStorageForm&);
    template<typename Functor> void forEachPositionBelow(Integer count, const Functor&);
    Condition isOneOfBoxedForms(Word32 mode, IndexingType ofIntegers, IndexingType ofValues);
    void newArrayOfForm(const ValueContext&, const ArrayStorageForm&);
    void newArrayOfHoles(const ValueContext&, Integer length, ValueID source, const ArrayStorageForm&, uint32_t flags);
    const ArrayStorageForm& formToConstruct(const ValueContext&) const;
    Integer lengthOfArrayStoredInto(const ValueContext&, const ArrayElements&);
    CellPointer sourceOrNull(ValueID);
};

static_assert(sizeof(ArrayIntrinsicEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
