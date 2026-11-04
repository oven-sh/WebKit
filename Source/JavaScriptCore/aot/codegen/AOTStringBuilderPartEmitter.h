/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringBuilderEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

// The appends of a string builder, one for each form in which a part arrives.
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
    void generateStringBuilderAppendStringOrNullish(const ValueContext&);
    void generateStringBuilderAppendValue(const ValueContext&);
    void generateStackStringBuilderAppendConstant(const ValueContext&);
    void generateStackStringBuilderAppendInteger(const ValueContext&);
    void generateStackStringBuilderAppendNumber(const ValueContext&);
    void generateStackStringBuilderAppendString(const ValueContext&);
    void generateStackStringBuilderAppendStringRange(const ValueContext&);
    void generateStackStringBuilderAppendBoolean(const ValueContext&);
    void generateStackStringBuilderAppendStringOrNullish(const ValueContext&);
    void generateStackStringBuilderAppendValue(const ValueContext&);

private:
    // A builder in the frame, and where its next character goes.
    struct PlaceInFrame {
        Pointer record;
        Pointer cursor;
    };

    StringBuilderEmitter& builders() { return emitter<StringBuilderEmitter>(); }
    PlaceInFrame placeInFrameOf(const ValueContext& value) { return { builders().recordOf(value), in<Pointer>(value->cursorOperand()) }; }
    bool isProvenToHaveFewDigits(ValueID integer) const
    {
        Type type = typeOf(integer);
        return type.isInteger() && !type.integers.isEmpty() && type.integers.isSubsetOf(Range { 0, StringBuilderEmitter::firstIntegerWrittenByCall - 1 });
    }
    // Each gives the cursor behind the part.
    Pointer appendString(const OperationContext&, PlaceInFrame, CellPointer string);
    Pointer appendDigits(const OperationContext&, PlaceInFrame, Word32 value);
    Pointer appendPrimitive(const OperationContext&, PlaceInFrame, Boxed, bool isOnRarePath = false);
    void appendString(const OperationContext&, CellPointer builder, CellPointer string);
    // The value is below StringBuilderEmitter::firstIntegerWrittenByCall.
    void appendDigits(const OperationContext&, CellPointer builder, Word32 value);
    void appendNullish(const OperationContext&, CellPointer builder, Condition isUndefined);
    // A primitive: what ToString makes of it. The operation runs no code of the program.
    void appendPrimitive(const OperationContext&, CellPointer builder, Boxed, bool isOnRarePath = false);
    void appendInWordsOrByCall(const OperationContext&, CellPointer builder, int32_t bytesWritten, NOESCAPE const Invocable<Pointer(Pointer cursor)> auto& write, NOESCAPE const Invocable<Boxed()> auto& box);

    // Texts that stand behind the one that was generated last, and were written with it.
    unsigned m_numberOfConstantsWrittenAhead { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
