/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTSlotRepresentation.h"

namespace JSC { namespace AOT { namespace Typed {

class ConversionEmitter final : public OperationEmitter {
public:
    explicit ConversionEmitter(CodeGenerator&);

    // B3 moves code without branches out of a loop and leaves a branch where it stands.
    enum class NumberUnboxing : uint8_t { Branches, ComputesBothSides };

    Boxed box(const OperationContext&, Operand, const Type&);
    Operand unbox(Boxed, const Type&, Representation, NumberUnboxing = NumberUnboxing::Branches);
    Operand toOptional(Operand, const Type&, Representation);
    // For the code of an operation whose result is produced in one representation and wanted in another.
    Operand inRepresentation(const OperationContext&, Operand produced, const Type& producedType, Representation wanted);

    Boxed boxNumber(Number);
    Boxed boxInteger(Integer, Range);
    Number toNumber(Integer, Range);
    Integer toInteger(Number);
    // The result, without rounding, of the integer arithmetic that made a number. It is the number, with 0 for -0,
    // or both lie beyond 2^52 on the same side of zero: a sum of integers and at most one product of two is rounded nowhere else.
    std::optional<Integer> exactIntegerBehind(Number);
    Integer widen(Word32, Range);
    Word32 narrow(Integer);
    Word32 toInt32(Number);
    Number purified(Number);

    Operand decode(NarrowReference, const Type&, Representation);
    NarrowReference compress(Operand, const Type&);

    Integer bitsOf(Operand);
    Operand fromBits(Integer, Representation);
    static uint64_t markerBits(uint16_t primitive, Representation);
    Condition isMarker(Operand);
    Condition isMarker(Operand, uint64_t marker);
    Condition isAbsent(Operand);
    Operand withoutMarkers(Operand);

    LiteralStringWord literalStringConstant(StringID);
    LiteralStringWord toLiteralString(CellPointer string);
    LiteralStringWord toLiteralString(Operand, const Type&);
    Boxed boxLiteralString(const OperationContext&, LiteralStringWord, const Type&);
    Condition isStringCell(LiteralStringWord);

private:
    struct ExactInteger;
    std::optional<ExactInteger> findExactIntegerBehind(LValue number);
    Boxed boxOptional(Operand, const Type&);
    Operand unboxOptional(Boxed, const Type&, Representation, NumberUnboxing);

    using PrimitiveEmitter::compress;
    using PrimitiveEmitter::decode;
    using PrimitiveEmitter::toNumber;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
