/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "TypedArrayType.h"
#include "Width.h"
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class BufferBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit BufferBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_BUFFER_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

    CellPointer newBigInt(const OperationContext&, Integer bits, bool isSigned);
    Integer low64Of(CellPointer bigInt);
    Condition isNegativeBigInt(CellPointer);

private:
    struct ByteOrder {
        std::optional<bool> isLittle;
        Condition isLittleAtRunTime;
    };

    ByteOrder byteOrderOf(const InlineCall&, unsigned argumentIndex, unsigned size);
    bool mayBeNegative(ValueID) const;
    Pointer findBytes(CellPointer view, Integer offset, bool mayBeNegative, unsigned size, LBasicBlock otherwise);
    LValue byteSwapped(LValue bits, unsigned size);
    LValue inByteOrder(LValue bits, unsigned size, const ByteOrder&);
    Operand loadBytes(const OperationContext&, Pointer, TypedArrayType, const ByteOrder&);
    void storeBytes(Pointer, TypedArrayType, Operand, const ByteOrder&);
    Operand valueToStoreOrElse(ValueID, TypedArrayType, LBasicBlock otherwise);
    Integer offsetOrElse(ValueID, LBasicBlock otherwise);
    LValue emitAtomicAccess(AtomicsOperation, std::array<LValue, 2> operands, LValue address, Width);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
