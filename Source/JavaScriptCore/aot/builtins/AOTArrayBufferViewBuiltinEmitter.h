/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "TypedArrayType.h"

namespace JSC { namespace AOT { namespace Typed {

class ArrayBufferViewBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit ArrayBufferViewBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUFFER_VIEW_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

    Condition hasFixedLength(CellPointer view);
    Integer lengthOf(CellPointer view);
    Integer byteOffsetOf(CellPointer view);
    Pointer vectorOf(CellPointer view);
    Pointer findTypedArrayElement(CellPointer view, Integer index, LBasicBlock otherwise, LBasicBlock outside);
    Operand loadElement(CellPointer view, Pointer vector, Integer index, TypedArrayType);
    Condition haveFixedLength(CellPointer first, CellPointer second);
    // The ranges may overlap. There is at least one byte.
    void moveBytes(const OperationContext&, CellPointer target, Integer targetOffset, CellPointer source, Integer sourceOffset, Integer count);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
