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

    void generateArrayFrom(const InlineCall&);
    void generateTypedArrayLength(const InlineCall&);
    void generateTypedArrayByteLength(const InlineCall&);
    void generateTypedArrayByteOffset(const InlineCall&);
    void generateTypedArrayWithLength(const InlineCall&);

    Condition hasFixedLength(CellPointer view);
    Integer lengthOf(CellPointer view);
    Integer byteOffsetOf(CellPointer view);
    Pointer vectorOf(CellPointer view);
    Pointer findTypedArrayElement(CellPointer view, Integer index, LBasicBlock otherwise, LBasicBlock outside);
    Operand loadElement(CellPointer view, Pointer vector, Integer index, TypedArrayType);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
