/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ArrayBufferViewBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit ArrayBufferViewBuiltinEmitter(CodeGenerator&);

    void generateTypedArrayLength(const InlineCall&);
    void generateTypedArrayByteLength(const InlineCall&);
    void generateTypedArrayByteOffset(const InlineCall&);
    void generateViewLength(const InlineCall&);
    void generateViewIsOutOfBounds(const InlineCall&);
    void generateTypedArrayWithLength(const InlineCall&);
    void generateTypedArraySet(const InlineCall&);
    void generateByteLiteral(const InlineCall&);

    Integer lengthOfViewInArgument(const InlineCall&);
    Condition hasFixedLength(CellPointer view);
    Integer lengthOf(CellPointer view);
    Integer byteOffsetOf(CellPointer view);
    Pointer vectorOf(CellPointer view);
    Condition haveFixedLength(CellPointer first, CellPointer second);
    CellPointer askedAnewWherePositionsAreTested(CellPointer view);
    void moveBytes(const OperationContext&, CellPointer target, Integer targetOffset, CellPointer source, Integer sourceOffset, Integer count);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
