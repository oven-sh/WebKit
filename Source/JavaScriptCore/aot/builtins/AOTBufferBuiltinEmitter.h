/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "TypedArrayType.h"
#include "Width.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class BufferBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit BufferBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_BUFFER_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

    CellPointer newBigInt(const OperationContext&, Integer bits, bool isSigned);
    // The number modulo 2^64.
    Integer low64Of(CellPointer bigInt);
    Condition isNegativeBigInt(CellPointer);

private:
    struct ByteOrder {
        std::optional<bool> isLittle;
        Condition isLittleAtRunTime;
    };

    ByteOrder byteOrderOf(const InlineCall&, unsigned argumentIndex, unsigned size);
    bool mayBeNegative(ValueID) const;
    // A view whose length follows its buffer goes the other way. So does a view of a detached buffer, which has no elements.
    Pointer vectorHoldingElement(CellPointer view, Integer index, LBasicBlock otherwise);
    Pointer findBytes(CellPointer view, Integer offset, bool mayBeNegative, unsigned size, LBasicBlock otherwise);
    LValue byteSwapped(LValue bits, unsigned size);
    LValue inByteOrder(LValue bits, unsigned size, const ByteOrder&);
    Operand loadBytes(const OperationContext&, Pointer, TypedArrayType, const ByteOrder&);
    void storeBytes(Pointer, TypedArrayType, Operand, const ByteOrder&);
    Operand valueToStoreOrElse(ValueID, TypedArrayType, LBasicBlock otherwise);
    // A number with a fraction, NaN and what lies beyond int64 go the other way.
    Integer offsetOrElse(ValueID, LBasicBlock otherwise);
    // What the element held, in the low bits of the width. Above them lies anything. Null for a store.
    LValue emitAtomicAccess(AtomicsOperation, std::span<const LValue> operands, LValue address, Width);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
