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
    Integer low64Of(CellPointer bigInt);
    Condition isNegativeBigInt(CellPointer);

    struct ByteOrder {
        std::optional<bool> isLittle;
        Condition isLittleAtRunTime;
    };
    enum class NaNBits : bool { Purified, AsStored };
    Operand loadBytes(const OperationContext&, Pointer, TypedArrayType, const ByteOrder&, NaNBits = NaNBits::Purified);
    void storeBytes(Pointer, TypedArrayType, Operand, const ByteOrder&);

private:
    ByteOrder byteOrderOf(const InlineCall&, unsigned argumentIndex, unsigned size);
    void continueWhereIsNotNegative(const InlineCall&, ValueID, Integer position, LBasicBlock otherwise);
    Pointer vectorHoldingElement(CellPointer view, Integer index, LBasicBlock otherwise);
    Pointer findBytes(CellPointer view, Integer offset, unsigned size, LBasicBlock otherwise);
    LValue byteSwapped(LValue bits, unsigned size);
    LValue inByteOrder(LValue bits, unsigned size, const ByteOrder&);
    Operand valueToStoreOrElse(ValueID, TypedArrayType, LBasicBlock otherwise);
    Integer offsetOrElse(ValueID, LBasicBlock otherwise);
    LValue emitAtomicAccess(AtomicsOperation, std::span<const LValue> operands, LValue address, Width);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
