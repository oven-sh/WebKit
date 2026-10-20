/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringBuilderEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class StringBuilderPartEmitter final : public OperationEmitter {
public:
    explicit StringBuilderPartEmitter(CodeGenerator&);

    void generateStringBuilderAppendConstant(const ValueContext&);
    void generateStringBuilderAppendInteger(const ValueContext&);
    void generateStringBuilderAppendNumber(const ValueContext&);
    void generateStringBuilderAppendBoolean(const ValueContext&);
    void generateStringBuilderAppendString(const ValueContext&);
    void generateStringBuilderAppendStringRange(const ValueContext&);
    void generateStringBuilderAppendStringOrNullish(const ValueContext&);
    void generateStringBuilderAppendValue(const ValueContext&);

private:
    void appendString(const OperationContext&, CellPointer builder, CellPointer string);
    void appendViewOfString(const OperationContext&, CellPointer builder, CellPointer string, const CharacterView&, LBasicBlock byCall);
    void appendView(CellPointer builder, const CharacterView&, LBasicBlock byCall);
    void appendViewOrCall(CellPointer builder, const CharacterView&, LBasicBlock byCall, NOESCAPE const Invocable<void()> auto& appendByCall);
    void appendConstant(const OperationContext&, CellPointer builder, StringID);
    void appendDigits(const OperationContext&, CellPointer builder, Word32 value);
    void appendNullish(const OperationContext&, CellPointer builder, Condition isUndefined);
    void appendPrimitive(const OperationContext&, CellPointer builder, Boxed primitive);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
