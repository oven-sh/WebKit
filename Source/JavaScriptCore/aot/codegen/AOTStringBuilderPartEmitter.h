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
    explicit StringBuilderPartEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateStringBuilderAppendConstant(const ValueContext&);
    void generateStringBuilderAppendInteger(const ValueContext&);
    void generateStringBuilderAppendNumber(const ValueContext&);
    void generateStringBuilderAppendString(const ValueContext&);
    void generateStringBuilderAppendStringRange(const ValueContext&);
    void generateStringBuilderAppendBoolean(const ValueContext&);
    void generateStringBuilderAppendCodeUnit(const ValueContext&);
    void generateStringBuilderAppendStringOrNullish(const ValueContext&);
    void generateStringBuilderAppendValue(const ValueContext&);
    void generateStackStringBuilderAppendConstant(const ValueContext&);
    void generateStackStringBuilderAppendInteger(const ValueContext&);
    void generateStackStringBuilderAppendNumber(const ValueContext&);
    void generateStackStringBuilderAppendString(const ValueContext&);
    void generateStackStringBuilderAppendStringRange(const ValueContext&);
    void generateStackStringBuilderAppendBoolean(const ValueContext&);
    void generateStackStringBuilderAppendCodeUnit(const ValueContext&);
    void generateStackStringBuilderAppendStringOrNullish(const ValueContext&);
    void generateStackStringBuilderAppendValue(const ValueContext&);

private:
    struct PlaceInFrame {
        Pointer record;
        Pointer cursor;
    };

    StringBuilderEmitter& builders() { return emitter<StringBuilderEmitter>(); }
    PlaceInFrame placeInFrameOf(const ValueContext& value) { return { builders().recordOf(value), in<Pointer>(value->cursorOperand()) }; }
    void holdIntegerToldToHaveFewDigits(const ValueContext&, Integer appended);
    bool isProvenToHaveFewDigits(ValueID integer) const
    {
        Type type = typeOf(integer);
        return type.isInteger() && !type.integers.isEmpty() && type.integers.isSubsetOf(Range { 0, StringBuilderEmitter::firstIntegerWrittenByCall - 1 });
    }
    Pointer appendString(const OperationContext&, PlaceInFrame, CellPointer string);
    Pointer appendDigits(const OperationContext&, PlaceInFrame, Word32 value);
    Pointer appendPrimitive(const OperationContext&, PlaceInFrame, Boxed, bool isOnRarePath = false);
    void appendString(const OperationContext&, CellPointer builder, CellPointer string);
    void appendDigits(const OperationContext&, CellPointer builder, Word32 value);
    void appendNullish(const OperationContext&, CellPointer builder, Condition isUndefined);
    void appendPrimitive(const OperationContext&, CellPointer builder, Boxed, bool isOnRarePath = false);
    void appendInWordsOrByCall(const OperationContext&, CellPointer builder, int32_t bytesWritten, NOESCAPE const Invocable<Pointer(Pointer cursor)> auto& write, NOESCAPE const Invocable<Boxed()> auto& box);

    unsigned m_numberOfConstantsWrittenAhead { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
