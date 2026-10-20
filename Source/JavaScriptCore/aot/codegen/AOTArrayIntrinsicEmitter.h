/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ArrayIntrinsicEmitter final : public BuiltinFamilyEmitter {
public:
    explicit ArrayIntrinsicEmitter(CodeGenerator&);

    void generateIsPristineArray(const ValueContext&);
    void generateArrayStorageKind(const ValueContext&);
    void generateHasContiguousIndex(const ValueContext&);
    void generateGetContiguousIndex(const ValueContext&);
    void generateGetDoubleIndex(const ValueContext&);
    void generatePutContiguousIndex(const ValueContext&);
    void generatePutDoubleIndex(const ValueContext&);
    void generateNewContiguousArray(const ValueContext&);
    void generateNewDoubleArray(const ValueContext&);
    void generateNewArrayLike(const ValueContext&);
    void generateNewArrayWithCapacity(const ValueContext&);
    void generateTryPutByValDirect(const ValueContext&);
    void generateTryPutIndex(const ValueContext&);
    void generateGrowNewArray(const ValueContext&);
    void generateTruncateNewArray(const ValueContext&);

private:
    Condition isPristineArrayValue(ValueID);
    Boxed contiguousElementOrEmpty(CellPointer array, Integer index);
    void putInBounds(const ValueContext&, const ArrayStorageForm&);
    void newArrayOfHoles(const ValueContext&, Integer length, ValueID source, const ArrayStorageForm&, uint32_t flags);
    const ArrayStorageForm& formToConstruct(const ValueContext&) const;
    CellPointer sourceOrNull(ValueID);
    Condition tryStore(const ValueContext&, const ArrayStorageForm&, CellPointer array, Integer index, ValueID stored);
};

static_assert(sizeof(ArrayIntrinsicEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
