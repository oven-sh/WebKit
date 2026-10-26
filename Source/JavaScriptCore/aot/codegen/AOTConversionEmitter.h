/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsArithmetic.h"

namespace JSC { namespace AOT { namespace Typed {

class ConversionEmitter final : public OperationEmitter {
public:
    explicit ConversionEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    // B3 moves code without branches out of a loop and leaves a branch where it stands.
    enum class NumberUnboxing : uint8_t { Branches, ComputesBothSides };

    Boxed box(const OperationContext& context, Operand operand, const Type& type) { return box(context, operand, type, isSeldomEntered(context)); }
    // In the arm of an operation that seldom runs, however often its block is entered: a number is boxed by the stub.
    Boxed boxInArm(const OperationContext& context, Operand operand, const Type& type) { return box(context, operand, type, true); }
    Operand unbox(Boxed, const Type&, Representation, NumberUnboxing = NumberUnboxing::Branches);
    Operand toOptional(Operand, const Type&, Representation);
    // For the code of an operation whose result is produced in one representation and wanted in another.
    Operand inRepresentation(const OperationContext&, Operand produced, const Type& producedType, Representation wanted);

    // A number kept as Double, DoubleWithPayload or ImpureDouble.
    Boxed boxNumber(Operand);
    Boxed boxDouble(Number value) { return Boxed(m_out.sub(m_out.bitCast(value.value(), B3::Int64), m_numberTag)); }
    Boxed boxNumberByStub(Number value) { return boxByStub(BoxedNumberForm::Double, value.value()); }
    Boxed boxInteger(Integer, Range);
    Number toNumber(Integer, Range);
    Integer toInteger(Number);
#if CPU(ARM64)
    // NaN is 0, and a number beyond int64 is the nearest int64.
    Integer toNearestInteger(Number);
#endif
    // The result, without rounding, of the integer arithmetic that made a number.
    std::optional<Integer> exactIntegerBehind(Number);
    Integer widen(Word32, Range);
    Word32 narrow(Integer value) { return low32(value); }
    Word32 toInt32(Number);
    // What a read of bytes as a number answers: every NaN is the one NaN.
    Number purified(Number);
    // A NaN keeps the sign that arithmetic gave it and none of its other bits.
    Number withoutPayload(Number);
    Number withoutBitsOfAnyNaN(Operand operand) { return operand.is<ImpureNumber>() ? purified(Number(operand.value)) : withoutPayload(Number(operand.as<NumberWithPayload>().value())); }

    Operand decode(NarrowReference, const Type&, Representation);
    NarrowReference compress(Operand, const Type&);

    Integer bitsOf(Operand);
    Operand fromBits(Integer, Representation);
    static uint64_t markerBits(uint16_t primitive, Representation);
    Condition isMarker(Operand);
    Condition isMarker(Operand operand, uint64_t marker) { return equal(bitsOf(operand), integer(static_cast<int64_t>(marker))); }
    Condition isAbsent(Operand);
    Operand withoutMarkers(Operand);

    // Code on an edge runs as seldom as the rarer of its ends and has the more compact of their shapes, whatever the context of the terminator says.
    struct PlaceOnEdge {
        BlockFrequency frequency;
        CodeShape shape;
    };
    // Answers with the place that was set before.
    std::optional<PlaceOnEdge> setPlaceOnEdge(std::optional<PlaceOnEdge> place) { return std::exchange(m_placeOnEdge, place); }

    LiteralStringWord literalStringConstant(StringID);
    LiteralStringWord toLiteralString(CellPointer string);
    LiteralStringWord toLiteralString(Operand, const Type&);
    Boxed boxLiteralString(const OperationContext&, LiteralStringWord, const Type&);
    Condition isStringCell(LiteralStringWord);

private:
    struct ExactInteger;
    std::optional<ExactInteger> findExactIntegerBehind(LValue number);
    Operand convertMarkers(Operand source, const Type&, Representation to, std::optional<Operand> unmarked = std::nullopt);
    Integer integerPartOf(Operand, const Type&);
    Number numberPartOf(Operand, const Type&);
    Boxed box(const OperationContext&, Operand, const Type&, bool isSeldomEntered);
    Boxed boxOptional(Operand, const Type&, bool isSeldomEntered);
    bool isSeldomEntered(const OperationContext&) const;
    CodeShape shapeAt(const OperationContext& context) const { return m_placeOnEdge ? m_placeOnEdge->shape : context.shape(); }
    Boxed boxByStub(BoxedNumberForm, LValue);
    Integer quietNaNWithSignOf(Integer bits) { return Integer(m_out.shl(m_out.bitOr(m_out.lShr(bits.value(), m_out.constInt32(51)), m_out.constInt64(1)), m_out.constInt32(51))); }
    Operand unboxOptional(Boxed, const Type&, Representation, NumberUnboxing);

    std::optional<PlaceOnEdge> m_placeOnEdge;

    using PrimitiveEmitter::compress;
    using PrimitiveEmitter::decode;
    using PrimitiveEmitter::toNumber;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
