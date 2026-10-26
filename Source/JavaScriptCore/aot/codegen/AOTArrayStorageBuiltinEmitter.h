/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTElementAccessEmitter.h"
#include "AOTInlineCall.h"

namespace JSC { namespace AOT { namespace Typed {

class ArrayStorageBuiltinEmitter final : public OperationEmitter {
public:
    explicit ArrayStorageBuiltinEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateArrayPush(const InlineCall&);
    void generateArrayIsArray(const InlineCall&);
    void generateArrayLength(const InlineCall&);
    void generateArraySetLength(const InlineCall&);
    void generateArrayWithLength(const InlineCall&);
    const Type& typeOfArraysCreatedBy(const InlineCall&);
    void generateArrayOf(const InlineCall&);

private:
    ElementAccessEmitter& elements() { return emitter<ElementAccessEmitter>(); }
    Operand lengthResult(const InlineCall&, Integer length);
    void pushOne(const InlineCall&, CellPointer array);
    Operand pushOutOfLine(const InlineCall&, CellPointer array, ValueID pushed);
    void pushSeveral(const InlineCall&, CellPointer array);
};

static_assert(sizeof(ArrayStorageBuiltinEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
